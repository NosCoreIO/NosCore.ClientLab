/* Miles Sound System on WebAudio. The client gives each sample handle a whole WAV or MP3 file image in memory;
   starting it posts those bytes to the page, which decodes and plays them. The page reports finished sounds through
   a SharedArrayBuffer (the worker never yields), which AIL_sample_status reads. */
#include "port.h"
#include <stdlib.h>
#include <sys/stat.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define MAX_SAMPLES 128
#define SAMPLE_BASE 0x00600000u
#define DRIVER 0x005f0000u

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
enum { TYPE_UNKNOWN = 0, TYPE_PCM_WAV = 1, TYPE_MPEG_L3 = 13 };

typedef struct {
    int used, playing;
    uint32_t data, size, loops, generation;
    float volume, pan;
} Sample;
static Sample samples[MAX_SAMPLES];

/* AIL_file_read buffers, so a later AIL_set_sample_file knows the size of the image it is given. */
typedef struct { uint32_t ptr, size; } FileBuf;
static FileBuf files[4096];

static Sample* sample_get(uint32_t h) {
    uint32_t k = (h - SAMPLE_BASE) / 16;
    if (h < SAMPLE_BASE || k >= MAX_SAMPLES || !samples[k].used) return 0;
    return &samples[k];
}
static uint32_t sample_index(Sample* s) { return (uint32_t)(s - samples); }

static uint32_t file_size_of(uint32_t ptr) {
    for (int k = 0; k < 4096; k++)
        if (files[k].ptr == ptr) return files[k].size;
    if (M32(ptr) == 0x46464952u) return M32(ptr + 4) + 8;
    return 0;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, js_audio_play, (int id, int generation, const uint8_t* data, int size, int loops, float volume, float pan), {
    if (typeof postMessage !== 'function') return;   /* not in a worker (Node) */
    const bytes = HEAPU8.slice(data, data + size);
    postMessage({ type: 'sound', op: 'play', id, generation, bytes, loops, volume, pan }, [bytes.buffer]);
});
EM_JS(void, js_audio_op, (int id, int op, float volume, float pan), {
    if (typeof postMessage === 'function') postMessage({ type: 'sound', op: op === 0 ? 'stop' : 'volume', id, volume, pan });
});
EM_JS(int, js_audio_finished, (int id), {
    const done = globalThis.audioDone;
    return done ? Atomics.load(done, id) : 0;
});
#else
static void js_audio_play(int id, int generation, const uint8_t* data, int size, int loops, float volume, float pan) {
    (void)id; (void)generation; (void)data; (void)size; (void)loops; (void)volume; (void)pan;
}
static void js_audio_op(int id, int op, float volume, float pan) { (void)id; (void)op; (void)volume; (void)pan; }
static int js_audio_finished(int id) { (void)id; return 0; }
#endif

/* ---- IMA ADPCM WAV -> 16-bit PCM WAV (browsers decode PCM and MP3, not ADPCM) ---- */

static const int ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int16_t ima_nibble(int nib, int* pred, int* idx) {
    int step = ima_steps[*idx], diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    *pred += (nib & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += ima_index[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Returns a malloc'd PCM WAV for an IMA ADPCM WAV image, or 0 if the image is anything else. */
static uint8_t* adpcm_to_pcm(const uint8_t* w, uint32_t size, uint32_t* out_size) {
    if (size < 44 || memcmp(w, "RIFF", 4) || memcmp(w + 8, "WAVE", 4)) return 0;
    const uint8_t *fmt = 0, *data = 0;
    uint32_t data_len = 0;
    for (uint32_t p = 12; p + 8 <= size;) {
        uint32_t len = w[p + 4] | w[p + 5] << 8 | w[p + 6] << 16 | (uint32_t)w[p + 7] << 24;
        if (!memcmp(w + p, "fmt ", 4)) fmt = w + p + 8;
        if (!memcmp(w + p, "data", 4)) { data = w + p + 8; data_len = len < size - p - 8 ? len : size - p - 8; }
        p += 8 + len + (len & 1);
    }
    if (!fmt || !data || (fmt[0] | fmt[1] << 8) != 0x11) return 0;
    uint32_t ch = fmt[2] | fmt[3] << 8, rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
    uint32_t block = fmt[12] | fmt[13] << 8;
    if (!ch || ch > 2 || block < 4 * ch) return 0;
    uint32_t per_block = (block - 4 * ch) * 2 / ch + 1, blocks = (data_len + block - 1) / block;
    uint32_t pcm_bytes = blocks * per_block * ch * 2;
    uint8_t* out = malloc(44 + pcm_bytes);
    int16_t* pcm = (int16_t*)(out + 44);
    uint32_t n = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        const uint8_t* blk = data + b * block;
        uint32_t len = data_len - b * block < block ? data_len - b * block : block;
        if (len < 4 * ch) break;
        int pred[2], idx[2];
        for (uint32_t k = 0; k < ch; k++) {
            pred[k] = (int16_t)(blk[4 * k] | blk[4 * k + 1] << 8);
            idx[k] = blk[4 * k + 2] > 88 ? 88 : blk[4 * k + 2];
            pcm[n++] = (int16_t)pred[k];
        }
        const uint8_t* p = blk + 4 * ch;
        uint32_t left = len - 4 * ch;
        if (ch == 1) {
            for (uint32_t i = 0; i < left; i++) {
                pcm[n++] = ima_nibble(p[i] & 15, &pred[0], &idx[0]);
                pcm[n++] = ima_nibble(p[i] >> 4, &pred[0], &idx[0]);
            }
        } else {
            /* stereo: 4 bytes (8 samples) of left, then 4 bytes of right */
            for (uint32_t g = 0; g + 8 <= left; g += 8) {
                int16_t l[8], r[8];
                for (int i = 0; i < 4; i++) {
                    l[2 * i] = ima_nibble(p[g + i] & 15, &pred[0], &idx[0]);
                    l[2 * i + 1] = ima_nibble(p[g + i] >> 4, &pred[0], &idx[0]);
                    r[2 * i] = ima_nibble(p[g + 4 + i] & 15, &pred[1], &idx[1]);
                    r[2 * i + 1] = ima_nibble(p[g + 4 + i] >> 4, &pred[1], &idx[1]);
                }
                for (int i = 0; i < 8; i++) { pcm[n++] = l[i]; pcm[n++] = r[i]; }
            }
        }
    }
    pcm_bytes = n * 2;
    memcpy(out, "RIFF", 4); put32(out + 4, 36 + pcm_bytes); memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16); put16(out + 20, 1); put16(out + 22, ch); put32(out + 24, rate);
    put32(out + 28, rate * ch * 2); put16(out + 32, ch * 2); put16(out + 34, 16);
    memcpy(out + 36, "data", 4); put32(out + 40, pcm_bytes);
    *out_size = 44 + pcm_bytes;
    return out;
}

/* ---- API ---- */

static void a_one(CPU* c) { RET(1); }
static void a_zero(CPU* c) { RET(0); }
static void a_open_driver(CPU* c) { RET(DRIVER); }

static void a_allocate_sample(CPU* c) {
    for (int k = 0; k < MAX_SAMPLES; k++)
        if (!samples[k].used) {
            uint32_t generation = samples[k].generation;
            memset(&samples[k], 0, sizeof samples[k]);
            samples[k].used = 1;
            samples[k].generation = generation;
            samples[k].volume = 1;
            samples[k].pan = 0.5f;
            samples[k].loops = 1;
            RET(SAMPLE_BASE + 16 * (uint32_t)k);
            return;
        }
    RET(0);
}
static void stop(Sample* s) {
    if (s->playing) js_audio_op((int)sample_index(s), 0, 0, 0);
    s->playing = 0;
}
static void a_release_sample(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) { stop(s); s->used = 0; } RET(0); }
static void a_init_sample(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) { stop(s); s->loops = 1; s->volume = 1; s->pan = 0.5f; } RET(0); }

static void a_set_sample_file(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) { stop(s); s->data = ARG(1); s->size = file_size_of(ARG(1)); }
    RET(s && s->size ? 1 : 0);
}
static void a_set_named_sample_file(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) { stop(s); s->data = ARG(2); s->size = ARG(3) ? ARG(3) : file_size_of(ARG(2)); }
    RET(s && s->size ? 1 : 0);
}

/* Every WAV is reported as PCM: ADPCM is converted when it is played, so the client takes the plain path. */
static void a_file_type(CPU* c) {
    uint32_t p = ARG(0);
    if (!p) { RET(TYPE_UNKNOWN); return; }
    if (M32(p) == 0x46464952u) { RET(TYPE_PCM_WAV); return; }
    if ((M8(p) == 'I' && M8(p + 1) == 'D' && M8(p + 2) == '3') || (M8(p) == 0xFF && (M8(p + 1) & 0xE0) == 0xE0)) {
        RET(TYPE_MPEG_L3);
        return;
    }
    RET(TYPE_UNKNOWN);
}

static void a_start_sample(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (!s || !s->data || !s->size) { RET(0); return; }
    stop(s);
    s->generation++;
    uint32_t pcm_size = 0;
    uint8_t* pcm = adpcm_to_pcm((const uint8_t*)GPTR(s->data), s->size, &pcm_size);
    if (pcm) {
        js_audio_play((int)sample_index(s), (int)s->generation, pcm, (int)pcm_size, (int)s->loops, s->volume, s->pan);
        free(pcm);
    } else
        js_audio_play((int)sample_index(s), (int)s->generation, (const uint8_t*)GPTR(s->data), (int)s->size,
                      (int)s->loops, s->volume, s->pan);
    s->playing = 1;
    RET(0);
}
static void a_end_sample(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) stop(s); RET(0); }

static void a_sample_status(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (!s) { RET(SMP_FREE); return; }
    if (s->playing && (uint32_t)js_audio_finished((int)sample_index(s)) == s->generation) s->playing = 0;
    RET(s->playing ? SMP_PLAYING : SMP_DONE);
}

static float farg(CPU* c, int n) { uint32_t v = ARG(n); float f; memcpy(&f, &v, 4); return f; }
static void a_set_volume_pan(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) {
        s->volume = farg(c, 1);
        s->pan = farg(c, 2);
        if (s->playing) js_audio_op((int)sample_index(s), 1, s->volume, s->pan);
    }
    RET(0);
}
static void a_set_loop_count(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) s->loops = ARG(1); RET(0); }
static void a_sample_ms_position(CPU* c) { if (ARG(1)) M32(ARG(1)) = 0; if (ARG(2)) M32(ARG(2)) = 0; RET(0); }

static void a_file_read(CPU* c) {
    char path[600];
    port_host_path(GSTR(ARG(0)), path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) { RET(0); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t buf = ARG(1) ? ARG(1) : guest_alloc((uint32_t)n, 16);
    fread(GPTR(buf), 1, (size_t)n, f);
    fclose(f);
    for (int k = 0; k < 4096; k++)
        if (!files[k].ptr || files[k].ptr == buf) { files[k].ptr = buf; files[k].size = (uint32_t)n; break; }
    RET(buf);
}
static void a_file_size(CPU* c) {
    char path[600];
    struct stat st;
    port_host_path(GSTR(ARG(0)), path, sizeof path);
    RET(stat(path, &st) ? 0xFFFFFFFFu : (uint32_t)st.st_size);
}
static void a_mem_free_lock(CPU* c) {
    for (int k = 0; k < 4096; k++)
        if (files[k].ptr == ARG(0)) files[k].ptr = 0;
    RET(0);
}

#define A(n, a, f) {"mss32.dll", n, a, f}
static const Hle audio[] = {
    A("_AIL_startup@0", 0, a_one), A("_AIL_shutdown@0", 0, a_zero),
    A("_AIL_open_digital_driver@16", 4, a_open_driver), A("_AIL_close_digital_driver@4", 1, a_zero),
    A("_AIL_set_3D_rolloff_factor@8", 2, a_zero), A("_AIL_set_listener_3D_position@16", 4, a_zero),
    A("_AIL_set_listener_3D_orientation@28", 7, a_zero), A("_AIL_allocate_sample_handle@4", 1, a_allocate_sample),
    A("_AIL_release_sample_handle@4", 1, a_release_sample), A("_AIL_file_read@8", 2, a_file_read),
    A("_AIL_file_size@4", 1, a_file_size), A("_AIL_mem_free_lock@4", 1, a_mem_free_lock),
    A("_AIL_init_sample@12", 3, a_init_sample), A("_AIL_set_named_sample_file@20", 5, a_set_named_sample_file),
    A("_AIL_set_sample_file@12", 3, a_set_sample_file), A("_AIL_file_type@8", 2, a_file_type),
    A("_AIL_WAV_info@8", 2, a_zero), A("_AIL_decompress_ADPCM@12", 3, a_zero),
    A("_AIL_sample_ms_position@12", 3, a_sample_ms_position), A("_AIL_set_sample_3D_position@16", 4, a_zero),
    A("_AIL_set_sample_3D_orientation@28", 7, a_zero), A("_AIL_set_sample_3D_distances@16", 4, a_zero),
    A("_AIL_set_sample_volume_pan@12", 3, a_set_volume_pan), A("_AIL_set_sample_loop_count@8", 2, a_set_loop_count),
    A("_AIL_start_sample@4", 1, a_start_sample), A("_AIL_stop_sample@4", 1, a_end_sample),
    A("_AIL_end_sample@4", 1, a_end_sample), A("_AIL_sample_status@4", 1, a_sample_status),
    A("_AIL_set_sample_playback_rate@8", 2, a_zero), A("_AIL_set_digital_master_volume_level@8", 2, a_zero),
};

void hle_audio_register(void) { hle_register(audio, sizeof audio / sizeof audio[0]); }
