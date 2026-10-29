/* Miles Sound System on WebAudio. The client gives each sample handle a whole WAV or MP3 file image in memory;
   starting it posts those bytes to the page, which decodes and plays them. The page reports finished sounds through
   a SharedArrayBuffer (the worker never yields), which AIL_sample_status reads. Positional samples are mixed here:
   distance from the listener sets the gain and the listener's right axis sets the pan. */
#include "port.h"
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define MAX_SAMPLES 128
#define SAMPLE_BASE 0x00600000u
#define DRIVER 0x005f0000u

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
enum { TYPE_UNKNOWN = 0, TYPE_PCM_WAV = 1, TYPE_ADPCM_WAV = 2, TYPE_OTHER_WAV = 3, TYPE_MPEG_L3 = 13 };
enum { WAVE_PCM = 1, WAVE_IMA_ADPCM = 0x11 };

typedef struct {
    int used, playing, positional;
    uint32_t data, size, loops, generation, rate, native_rate;
    float volume, pan, pos[3], min_dist, max_dist;
    double start_ms, length_ms, stopped_at_ms;
} Sample;
static Sample samples[MAX_SAMPLES];

static float master_volume = 1, rolloff = 1;
static float listener[3], listener_front[3] = {0, 0, 1}, listener_up[3] = {0, 1, 0};

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
static void file_track(uint32_t ptr, uint32_t size) {
    for (int k = 0; k < 4096; k++)
        if (!files[k].ptr || files[k].ptr == ptr) { files[k].ptr = ptr; files[k].size = size; return; }
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, js_audio_play, (int id, int generation, const uint8_t* data, int size, int loops, float volume, float pan, float rate), {
    if (typeof postMessage !== 'function') return;   /* not in a worker (Node) */
    const bytes = HEAPU8.slice(data, data + size);
    postMessage({ type: 'sound', op: 'play', id, generation, bytes, loops, volume, pan, rate }, [bytes.buffer]);
});
EM_JS(void, js_audio_op, (int id, int op, float volume, float pan, float rate), {
    if (typeof postMessage === 'function') postMessage({ type: 'sound', op: op === 0 ? 'stop' : 'volume', id, volume, pan, rate });
});
EM_JS(int, js_audio_finished, (int id), {
    const done = globalThis.audioDone;
    return done ? Atomics.load(done, id) : 0;
});
#else
static void js_audio_play(int id, int generation, const uint8_t* data, int size, int loops, float volume, float pan,
                          float rate) {
    (void)id; (void)generation; (void)data; (void)size; (void)loops; (void)volume; (void)pan; (void)rate;
}
static void js_audio_op(int id, int op, float volume, float pan, float rate) {
    (void)id; (void)op; (void)volume; (void)pan; (void)rate;
}
static int js_audio_finished(int id) { (void)id; return 0; }
#endif

/* ---- WAV images ---- */

static uint32_t get16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    uint32_t format, channels, rate, block, bits, data_len;
    const uint8_t* data;
} Wav;

static int wav_parse(const uint8_t* w, uint32_t size, Wav* out) {
    const uint8_t* fmt = 0;
    memset(out, 0, sizeof *out);
    if (size < 12 || memcmp(w, "RIFF", 4) || memcmp(w + 8, "WAVE", 4)) return 0;
    for (uint32_t p = 12; p + 8 <= size;) {
        uint32_t len = get32(w + p + 4);
        if (!memcmp(w + p, "fmt ", 4) && len >= 16 && p + 24 <= size) fmt = w + p + 8;
        if (!memcmp(w + p, "data", 4)) { out->data = w + p + 8; out->data_len = len < size - p - 8 ? len : size - p - 8; }
        if (len > size - p - 8) break;
        p += 8 + len + (len & 1);
    }
    if (!fmt || !out->data) return 0;
    out->format = get16(fmt);
    out->channels = get16(fmt + 2);
    out->rate = get32(fmt + 4);
    out->block = get16(fmt + 12);
    out->bits = get16(fmt + 14);
    return out->channels && out->rate;
}
static uint32_t wav_size(uint32_t p, uint32_t size) { return size ? size : file_size_of(p); }

static uint32_t adpcm_samples_per_block(uint32_t block, uint32_t ch) { return (block - 4 * ch) * 2 / ch + 1; }

static uint32_t wav_frames(const Wav* w) {
    if (w->format == WAVE_IMA_ADPCM) {
        if (w->block < 4 * w->channels) return 0;
        uint32_t rest = w->data_len % w->block;
        uint32_t frames = w->data_len / w->block * adpcm_samples_per_block(w->block, w->channels);
        if (rest >= 4 * w->channels) frames += adpcm_samples_per_block(rest, w->channels);
        return frames;
    }
    uint32_t frame = w->block ? w->block : w->channels * ((w->bits + 7) / 8);
    return frame ? w->data_len / frame : 0;
}

/* ---- IMA ADPCM -> 16-bit PCM WAV (browsers decode PCM and MP3, not ADPCM) ---- */

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

/* Returns a malloc'd 16-bit PCM WAV image decoded from IMA ADPCM blocks. */
static uint8_t* ima_decode(const uint8_t* data, uint32_t data_len, uint32_t ch, uint32_t rate, uint32_t block,
                           uint32_t* out_size) {
    if (!ch || ch > 2 || block < 4 * ch) return 0;
    uint32_t per_block = adpcm_samples_per_block(block, ch), blocks = (data_len + block - 1) / block;
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

/* Returns a malloc'd PCM WAV for an IMA ADPCM WAV image, or 0 if the image is anything else. */
static uint8_t* adpcm_to_pcm(const uint8_t* w, uint32_t size, uint32_t* out_size) {
    Wav wav;
    if (!wav_parse(w, size, &wav) || wav.format != WAVE_IMA_ADPCM) return 0;
    return ima_decode(wav.data, wav.data_len, wav.channels, wav.rate, wav.block, out_size);
}

/* ---- MPEG layer III length ---- */

static double mp3_length(const uint8_t* p, uint32_t size, uint32_t* rate_out) {
    static const uint16_t kbps[2][15] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
                                         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
    static const uint32_t rates[3] = {44100, 48000, 32000};
    uint32_t i = 0, frames = 0, rate = 0;
    double samples = 0;
    if (size >= 10 && !memcmp(p, "ID3", 3))
        i = 10 + ((p[6] & 0x7f) << 21 | (p[7] & 0x7f) << 14 | (p[8] & 0x7f) << 7 | (p[9] & 0x7f));
    while (i + 4 <= size) {
        uint32_t h = (uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3];
        uint32_t version = (h >> 19) & 3, layer = (h >> 17) & 3, br = (h >> 12) & 15, sr = (h >> 10) & 3;
        if ((h >> 21) != 0x7ff || version == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) {
            if (frames) break;
            i++;
            continue;
        }
        int mpeg1 = version == 3;
        uint32_t r = rates[sr] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
        if (!rate) rate = r;
        samples += mpeg1 ? 1152 : 576;
        frames++;
        i += (mpeg1 ? 144000u : 72000u) * kbps[!mpeg1][br] / r + ((h >> 9) & 1);
    }
    *rate_out = rate;
    return rate ? samples * 1000.0 / rate : 0;
}

static double sound_length(const uint8_t* p, uint32_t size, uint32_t* rate) {
    Wav w;
    if (wav_parse(p, size, &w)) {
        *rate = w.rate;
        return wav_frames(&w) * 1000.0 / w.rate;
    }
    return mp3_length(p, size, rate);
}

/* ---- mixing ---- */

static float farg(CPU* c, int n) { uint32_t v = ARG(n); float f; memcpy(&f, &v, 4); return f; }

/* A positional sample is attenuated by distance (inverse rolloff from its minimum distance, silent past its
   maximum) and panned by where it sits along the listener's right axis. */
static void mix(const Sample* s, float* volume, float* pan) {
    *volume = s->volume * master_volume;
    *pan = s->pan;
    if (!s->positional) return;
    float d[3] = {s->pos[0] - listener[0], s->pos[1] - listener[1], s->pos[2] - listener[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist > s->max_dist) { *volume = 0; return; }
    if (dist > s->min_dist) *volume *= s->min_dist / (s->min_dist + rolloff * (dist - s->min_dist));
    const float *f = listener_front, *u = listener_up;
    float right[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    float len = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (dist > 0 && len > 0) *pan = 0.5f + 0.5f * (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (dist * len);
}
static float speed(const Sample* s) { return s->rate && s->native_rate ? (float)s->rate / (float)s->native_rate : 1.0f; }

static void update(Sample* s) {
    float volume, pan;
    if (!s->playing) return;
    mix(s, &volume, &pan);
    js_audio_op((int)sample_index(s), 1, volume, pan, speed(s));
}
static void update_all(void) {
    for (int k = 0; k < MAX_SAMPLES; k++)
        if (samples[k].used) update(&samples[k]);
}

static double position_ms(const Sample* s) {
    if (!s->playing) return s->stopped_at_ms;
    if (s->length_ms <= 0) return 0;
    double t = (port_now_ms() - s->start_ms) * speed(s);
    if (s->loops && t >= s->length_ms * s->loops) return s->length_ms;
    return fmod(t, s->length_ms);
}

/* ---- API ---- */

static void stop(Sample* s) {
    if (s->playing) {
        s->stopped_at_ms = position_ms(s);
        js_audio_op((int)sample_index(s), 0, 0, 0, 1);
    }
    s->playing = 0;
}
static void reset(Sample* s) {
    stop(s);
    s->loops = 1;
    s->volume = 1;
    s->pan = 0.5f;
    s->rate = 0;
    s->positional = 0;
    s->min_dist = 1;
    s->max_dist = 1e9f;
    s->stopped_at_ms = 0;
}

static void a_allocate_sample(CPU* c) {
    for (int k = 0; k < MAX_SAMPLES; k++)
        if (!samples[k].used) {
            uint32_t generation = samples[k].generation;
            memset(&samples[k], 0, sizeof samples[k]);
            samples[k].used = 1;
            samples[k].generation = generation;
            reset(&samples[k]);
            RET(SAMPLE_BASE + 16 * (uint32_t)k);
            return;
        }
    RET(0);
}
static void a_release_sample(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) { stop(s); s->used = 0; } RET(0); }
static void a_init_sample(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) reset(s); RET(0); }

static int load(Sample* s, uint32_t data, uint32_t size) {
    stop(s);
    s->data = data;
    s->size = size;
    s->stopped_at_ms = 0;
    s->native_rate = 0;
    s->length_ms = size ? sound_length((const uint8_t*)GPTR(data), size, &s->native_rate) : 0;
    return size ? 1 : 0;
}
static void a_set_sample_file(CPU* c) {
    Sample* s = sample_get(ARG(0));
    RET(s ? load(s, ARG(1), file_size_of(ARG(1))) : 0);
}
static void a_set_named_sample_file(CPU* c) {
    Sample* s = sample_get(ARG(0));
    RET(s ? load(s, ARG(2), ARG(3) ? ARG(3) : file_size_of(ARG(2))) : 0);
}

static void a_file_type(CPU* c) {
    uint32_t p = ARG(0);
    Wav w;
    if (!p) { RET(TYPE_UNKNOWN); return; }
    if (M32(p) == 0x46464952u) {
        if (!wav_parse((const uint8_t*)GPTR(p), wav_size(p, ARG(1)), &w)) RET(TYPE_OTHER_WAV);
        else RET(w.format == WAVE_PCM ? TYPE_PCM_WAV : w.format == WAVE_IMA_ADPCM ? TYPE_ADPCM_WAV : TYPE_OTHER_WAV);
        return;
    }
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
    float volume, pan;
    mix(s, &volume, &pan);
    uint32_t pcm_size = 0;
    uint8_t* pcm = adpcm_to_pcm((const uint8_t*)GPTR(s->data), s->size, &pcm_size);
    if (pcm) {
        js_audio_play((int)sample_index(s), (int)s->generation, pcm, (int)pcm_size, (int)s->loops, volume, pan, speed(s));
        free(pcm);
    } else
        js_audio_play((int)sample_index(s), (int)s->generation, (const uint8_t*)GPTR(s->data), (int)s->size,
                      (int)s->loops, volume, pan, speed(s));
    s->playing = 1;
    s->start_ms = port_now_ms();
    RET(0);
}
static void a_end_sample(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) stop(s); RET(0); }

static void a_sample_status(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (!s) { RET(SMP_FREE); return; }
    if (s->playing && (uint32_t)js_audio_finished((int)sample_index(s)) == s->generation) {
        s->playing = 0;
        s->stopped_at_ms = s->length_ms;
    }
    RET(s->playing ? SMP_PLAYING : SMP_DONE);
}

static void a_set_volume_pan(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) {
        s->volume = farg(c, 1);
        s->pan = farg(c, 2);
        update(s);
    }
    RET(0);
}
static void a_set_loop_count(CPU* c) { Sample* s = sample_get(ARG(0)); if (s) s->loops = ARG(1); RET(0); }
static void a_set_playback_rate(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) {
        double played = (port_now_ms() - s->start_ms) * speed(s);
        s->rate = ARG(1);
        if (s->playing) s->start_ms = port_now_ms() - played / speed(s);
        update(s);
    }
    RET(0);
}
static void a_sample_ms_position(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (ARG(1)) M32(ARG(1)) = s ? (uint32_t)s->length_ms : 0;
    if (ARG(2)) M32(ARG(2)) = s ? (uint32_t)position_ms(s) : 0;
    RET(0);
}

static void a_set_3D_position(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) {
        for (int k = 0; k < 3; k++) s->pos[k] = farg(c, 1 + k);
        s->positional = 1;
        update(s);
    }
    RET(0);
}
/* A point source sounds the same whichever way it faces; only the listener's orientation enters the mix. */
static void a_set_3D_orientation(CPU* c) { RET(0); }
static void a_set_3D_distances(CPU* c) {
    Sample* s = sample_get(ARG(0));
    if (s) {
        s->max_dist = farg(c, 1);
        s->min_dist = farg(c, 2) > 0 ? farg(c, 2) : 1;
        update(s);
    }
    RET(0);
}
static void a_set_listener_position(CPU* c) {
    for (int k = 0; k < 3; k++) listener[k] = farg(c, 1 + k);
    update_all();
    RET(0);
}
static void a_set_listener_orientation(CPU* c) {
    for (int k = 0; k < 3; k++) { listener_front[k] = farg(c, 1 + k); listener_up[k] = farg(c, 4 + k); }
    update_all();
    RET(0);
}
static void a_set_rolloff(CPU* c) { rolloff = farg(c, 1); update_all(); RET(0); }
static void a_set_master_volume(CPU* c) { master_volume = farg(c, 1); update_all(); RET(0); }

/* AILSOUNDINFO: format, data_ptr, data_len, rate, bits, channels, samples, block_size, initial_ptr. */
static void a_WAV_info(CPU* c) {
    uint32_t p = ARG(0), info = ARG(1);
    Wav w;
    if (!p || !wav_parse((const uint8_t*)GPTR(p), wav_size(p, 0), &w)) { RET(0); return; }
    M32(info) = w.format;
    M32(info + 4) = p + (uint32_t)(w.data - (const uint8_t*)GPTR(p));
    M32(info + 8) = w.data_len;
    M32(info + 12) = w.rate;
    M32(info + 16) = w.bits;
    M32(info + 20) = w.channels;
    M32(info + 24) = wav_frames(&w);
    M32(info + 28) = w.block;
    M32(info + 32) = p;
    RET(1);
}
/* Decodes the IMA ADPCM data an AILSOUNDINFO describes into a new PCM WAV image, freed with AIL_mem_free_lock. */
static void a_decompress_ADPCM(CPU* c) {
    uint32_t info = ARG(0), size = 0;
    if (M32(info) != WAVE_IMA_ADPCM) { RET(0); return; }
    uint8_t* pcm = ima_decode((const uint8_t*)GPTR(M32(info + 4)), M32(info + 8), M32(info + 20), M32(info + 12),
                              M32(info + 28), &size);
    if (!pcm) { RET(0); return; }
    uint32_t out = guest_alloc(size, 16);
    memcpy(GPTR(out), pcm, size);
    free(pcm);
    file_track(out, size);
    if (ARG(1)) M32(ARG(1)) = out;
    if (ARG(2)) M32(ARG(2)) = size;
    RET(1);
}

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
    file_track(buf, (uint32_t)n);
    RET(buf);
}
static void a_file_size(CPU* c) {
    char path[600];
    struct stat st;
    port_host_path(GSTR(ARG(0)), path, sizeof path);
    RET(stat(path, &st) ? 0xFFFFFFFFu : (uint32_t)st.st_size);
}
/* Frees what AIL_file_read or AIL_decompress_ADPCM allocated. */
static void a_mem_free_lock(CPU* c) {
    for (int k = 0; k < 4096; k++)
        if (files[k].ptr == ARG(0)) {
            files[k].ptr = 0;
            guest_free(ARG(0));
            break;
        }
    RET(0);
}

static int started;
static void release_all(void) {
    for (int k = 0; k < MAX_SAMPLES; k++)
        if (samples[k].used) { stop(&samples[k]); samples[k].used = 0; }
}
static void a_startup(CPU* c) { started = 1; RET(1); }
static void a_shutdown(CPU* c) { release_all(); started = 0; RET(0); }
static void a_open_driver(CPU* c) { RET(started ? DRIVER : 0); }
static void a_close_driver(CPU* c) { if (ARG(0) == DRIVER) release_all(); RET(0); }

#define A(n, a, f) {"mss32.dll", n, a, f}
static const Hle audio[] = {
    A("_AIL_startup@0", 0, a_startup), A("_AIL_shutdown@0", 0, a_shutdown),
    A("_AIL_open_digital_driver@16", 4, a_open_driver), A("_AIL_close_digital_driver@4", 1, a_close_driver),
    A("_AIL_set_3D_rolloff_factor@8", 2, a_set_rolloff),
    A("_AIL_set_listener_3D_position@16", 4, a_set_listener_position),
    A("_AIL_set_listener_3D_orientation@28", 7, a_set_listener_orientation),
    A("_AIL_allocate_sample_handle@4", 1, a_allocate_sample),
    A("_AIL_release_sample_handle@4", 1, a_release_sample), A("_AIL_file_read@8", 2, a_file_read),
    A("_AIL_file_size@4", 1, a_file_size), A("_AIL_mem_free_lock@4", 1, a_mem_free_lock),
    A("_AIL_init_sample@12", 3, a_init_sample), A("_AIL_set_named_sample_file@20", 5, a_set_named_sample_file),
    A("_AIL_set_sample_file@12", 3, a_set_sample_file), A("_AIL_file_type@8", 2, a_file_type),
    A("_AIL_WAV_info@8", 2, a_WAV_info), A("_AIL_decompress_ADPCM@12", 3, a_decompress_ADPCM),
    A("_AIL_sample_ms_position@12", 3, a_sample_ms_position),
    A("_AIL_set_sample_3D_position@16", 4, a_set_3D_position),
    A("_AIL_set_sample_3D_orientation@28", 7, a_set_3D_orientation),
    A("_AIL_set_sample_3D_distances@16", 4, a_set_3D_distances),
    A("_AIL_set_sample_volume_pan@12", 3, a_set_volume_pan), A("_AIL_set_sample_loop_count@8", 2, a_set_loop_count),
    A("_AIL_start_sample@4", 1, a_start_sample), A("_AIL_stop_sample@4", 1, a_end_sample),
    A("_AIL_end_sample@4", 1, a_end_sample), A("_AIL_sample_status@4", 1, a_sample_status),
    A("_AIL_set_sample_playback_rate@8", 2, a_set_playback_rate),
    A("_AIL_set_digital_master_volume_level@8", 2, a_set_master_volume),
};

void hle_audio_register(void) { hle_register(audio, sizeof audio / sizeof audio[0]); }
