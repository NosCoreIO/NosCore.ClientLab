/* Image files from 32-bit pixels (0xAARRGGBB, top row first), in the D3DXIMAGE_FILEFORMAT set: BMP, baseline
   JPEG, TGA, PNG (stored deflate), DDS, PPM, DIB, Radiance HDR and PFM. */
#include "image_file.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void le16(FILE* f, uint32_t v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void le32(FILE* f, uint32_t v) { le16(f, v & 0xffff); le16(f, v >> 16); }
static void be32(FILE* f, uint32_t v) { fputc((int)(v >> 24), f); fputc((int)(v >> 16 & 255), f); fputc((int)(v >> 8 & 255), f); fputc((int)(v & 255), f); }

static int opaque(int w, int h, const uint32_t* px) {
    for (long k = 0; k < (long)w * h; k++)
        if (px[k] >> 24 != 255) return 0;
    return 1;
}

static void write_dib(FILE* f, int w, int h, const uint32_t* px, int file_header) {
    int bpp = opaque(w, h, px) ? 24 : 32;
    uint32_t stride = ((uint32_t)w * (uint32_t)bpp / 8 + 3) & ~3u, bits = stride * (uint32_t)h;
    if (file_header) { fputc('B', f); fputc('M', f); le32(f, 54 + bits); le32(f, 0); le32(f, 54); }
    le32(f, 40); le32(f, (uint32_t)w); le32(f, (uint32_t)h); le16(f, 1); le16(f, (uint32_t)bpp);
    le32(f, 0); le32(f, bits); le32(f, 2835); le32(f, 2835); le32(f, 0); le32(f, 0);
    for (int y = h - 1; y >= 0; y--) {
        uint32_t n = 0;
        for (int x = 0; x < w; x++, n += (uint32_t)bpp / 8) {
            uint32_t p = px[(long)y * w + x];
            fputc((int)(p & 255), f); fputc((int)(p >> 8 & 255), f); fputc((int)(p >> 16 & 255), f);
            if (bpp == 32) fputc((int)(p >> 24), f);
        }
        for (; n < stride; n++) fputc(0, f);
    }
}

static void write_tga(FILE* f, int w, int h, const uint32_t* px) {
    uint8_t hdr[18] = {0, 0, 2};
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8); hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;
    fwrite(hdr, 1, 18, f);
    for (long k = 0; k < (long)w * h; k++) le32(f, px[k]);
}

static void write_ppm(FILE* f, int w, int h, const uint32_t* px) {
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (long k = 0; k < (long)w * h; k++) { fputc((int)(px[k] >> 16 & 255), f); fputc((int)(px[k] >> 8 & 255), f); fputc((int)(px[k] & 255), f); }
}

static void write_dds(FILE* f, int w, int h, const uint32_t* px) {
    fwrite("DDS ", 1, 4, f);
    le32(f, 124); le32(f, 0x100f); le32(f, (uint32_t)h); le32(f, (uint32_t)w); le32(f, 4u * (uint32_t)w); le32(f, 0); le32(f, 0);
    for (int k = 0; k < 11; k++) le32(f, 0);
    le32(f, 32); le32(f, 0x41); le32(f, 0); le32(f, 32);
    le32(f, 0x00ff0000); le32(f, 0x0000ff00); le32(f, 0x000000ff); le32(f, 0xff000000);
    le32(f, 0x1000); le32(f, 0); le32(f, 0); le32(f, 0); le32(f, 0);
    for (long k = 0; k < (long)w * h; k++) le32(f, px[k]);
}

static void write_hdr(FILE* f, int w, int h, const uint32_t* px) {
    fprintf(f, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", h, w);
    for (long k = 0; k < (long)w * h; k++) {
        double r = (px[k] >> 16 & 255) / 255.0, g = (px[k] >> 8 & 255) / 255.0, b = (px[k] & 255) / 255.0;
        double m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (m < 1e-32) { le32(f, 0); continue; }
        int e;
        double scale = frexp(m, &e) * 256.0 / m;
        fputc((int)(r * scale), f); fputc((int)(g * scale), f); fputc((int)(b * scale), f); fputc(e + 128, f);
    }
}

static void write_pfm(FILE* f, int w, int h, const uint32_t* px) {
    fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(long)y * w + x];
            float rgb[3] = {(p >> 16 & 255) / 255.0f, (p >> 8 & 255) / 255.0f, (p & 255) / 255.0f};
            for (int k = 0; k < 3; k++) { uint32_t v; memcpy(&v, &rgb[k], 4); le32(f, v); }
        }
}

/* ---- PNG: RGBA rows with filter 0, deflated as stored blocks ---- */

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    if (!crc_table[1])
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t v = k;
            for (int b = 0; b < 8; b++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[k] = v;
        }
    c = ~c;
    for (size_t k = 0; k < n; k++) c = crc_table[(c ^ p[k]) & 255] ^ (c >> 8);
    return ~c;
}
static void png_chunk(FILE* f, const char* type, const uint8_t* data, size_t n) {
    be32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    be32(f, crc(crc(0, (const uint8_t*)type, 4), data, n));
}
static void write_png(FILE* f, int w, int h, const uint32_t* px) {
    size_t raw_n = (size_t)h * (1 + 4 * (size_t)w);
    uint8_t* raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        uint8_t* r = raw + (size_t)y * (1 + 4 * (size_t)w);
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(long)y * w + x];
            r[1 + 4 * x] = (uint8_t)(p >> 16); r[2 + 4 * x] = (uint8_t)(p >> 8); r[3 + 4 * x] = (uint8_t)p; r[4 + 4 * x] = (uint8_t)(p >> 24);
        }
    }
    size_t blocks = raw_n / 65535 + 1, z_n = 2 + raw_n + 5 * blocks + 4, o = 0;
    uint8_t* z = malloc(z_n);
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t at = 0;;) {
        size_t n = raw_n - at > 65535 ? 65535 : raw_n - at;
        z[o++] = at + n == raw_n;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + at, n);
        for (size_t k = 0; k < n; k++) { a = (a + raw[at + k]) % 65521; b = (b + a) % 65521; }
        o += n; at += n;
        if (at == raw_n) break;
    }
    uint32_t adler = b << 16 | a;
    z[o++] = (uint8_t)(adler >> 24); z[o++] = (uint8_t)(adler >> 16); z[o++] = (uint8_t)(adler >> 8); z[o++] = (uint8_t)adler;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, o);
    png_chunk(f, "IEND", 0, 0);
    free(raw);
    free(z);
}

/* ---- baseline JPEG, 4:4:4, the standard tables at quality 90 ---- */

static const uint8_t zigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
                                   41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                   30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t q_luma[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57,
                                   69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64,
                                   81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t q_chroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                                     99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
static const uint8_t dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct { uint16_t code[256]; uint8_t len[256]; } Huff;
static void huff_build(Huff* t, const uint8_t* bits, const uint8_t* vals) {
    uint32_t code = 0, k = 0;
    for (int l = 1; l <= 16; l++, code <<= 1)
        for (int n = 0; n < bits[l - 1]; n++, code++, k++) { t->code[vals[k]] = (uint16_t)code; t->len[vals[k]] = (uint8_t)l; }
}

typedef struct { FILE* f; uint32_t acc; int n; } Bits;
static void put_bits(Bits* b, uint32_t v, int n) {
    b->acc = b->acc << n | (v & ((1u << n) - 1));
    b->n += n;
    while (b->n >= 8) {
        int byte = (int)(b->acc >> (b->n - 8) & 255);
        fputc(byte, b->f);
        if (byte == 255) fputc(0, b->f);
        b->n -= 8;
    }
}
static int magnitude(int v, uint32_t* bits) {
    int a = v < 0 ? -v : v, n = 0;
    while (a >> n) n++;
    *bits = v < 0 ? (uint32_t)(v - 1) : (uint32_t)v;
    return n;
}

static void fdct(const float* in, float* out) {
    static float cs[8][8];
    if (cs[0][0] == 0)
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++) cs[u][x] = (float)((u ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * x + 1) * u * 3.14159265358979 / 16));
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) s += cs[u][x] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) s += cs[v][y] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

static void encode_block(Bits* b, const float* in, const uint8_t* q, const Huff* dc, const Huff* ac, int* prev) {
    float co[64];
    int z[64];
    uint32_t bits;
    fdct(in, co);
    for (int k = 0; k < 64; k++) z[k] = (int)lrintf(co[zigzag[k]] / q[zigzag[k]]);
    int diff = z[0] - *prev, n = magnitude(diff, &bits);
    *prev = z[0];
    put_bits(b, dc->code[n], dc->len[n]);
    if (n) put_bits(b, bits, n);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        if (!z[k]) { run++; continue; }
        while (run > 15) { put_bits(b, ac->code[0xf0], ac->len[0xf0]); run -= 16; }
        n = magnitude(z[k], &bits);
        int sym = run << 4 | n;
        put_bits(b, ac->code[sym], ac->len[sym]);
        put_bits(b, bits, n);
        run = 0;
    }
    if (run) put_bits(b, ac->code[0], ac->len[0]);
}

static void write_jpeg(FILE* f, int w, int h, const uint32_t* px) {
    uint8_t ql[64], qc[64];
    for (int k = 0; k < 64; k++) {
        int a = (q_luma[k] * 20 + 50) / 100, c = (q_chroma[k] * 20 + 50) / 100;
        ql[k] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
        qc[k] = (uint8_t)(c < 1 ? 1 : c > 255 ? 255 : c);
    }
    static const uint8_t app0[18] = {0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    fputc(0xff, f); fputc(0xd8, f);
    fwrite(app0, 1, 18, f);
    for (int t = 0; t < 2; t++) {
        fputc(0xff, f); fputc(0xdb, f); fputc(0, f); fputc(67, f); fputc(t, f);
        for (int k = 0; k < 64; k++) fputc((t ? qc : ql)[zigzag[k]], f);
    }
    uint8_t sof[19] = {0xff, 0xc0, 0, 17, 8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3,
                       1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1};
    fwrite(sof, 1, 19, f);
    const uint8_t* bits[4] = {dc_luma_bits, ac_luma_bits, dc_chroma_bits, ac_chroma_bits};
    const uint8_t* vals[4] = {dc_vals, ac_luma_vals, dc_vals, ac_chroma_vals};
    const uint8_t ids[4] = {0x00, 0x10, 0x01, 0x11};
    Huff hf[4];
    for (int t = 0; t < 4; t++) {
        int n = 0;
        for (int k = 0; k < 16; k++) n += bits[t][k];
        fputc(0xff, f); fputc(0xc4, f); fputc((19 + n) >> 8, f); fputc((19 + n) & 255, f); fputc(ids[t], f);
        fwrite(bits[t], 1, 16, f);
        fwrite(vals[t], 1, (size_t)n, f);
        memset(&hf[t], 0, sizeof hf[t]);
        huff_build(&hf[t], bits[t], vals[t]);
    }
    static const uint8_t sos[14] = {0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    fwrite(sos, 1, 14, f);
    Bits b = {f, 0, 0};
    int prev[3] = {0, 0, 0};
    float blk[3][64];
    for (int by = 0; by < h; by += 8)
        for (int bx = 0; bx < w; bx += 8) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int X = bx + x < w ? bx + x : w - 1, Y = by + y < h ? by + y : h - 1;
                    uint32_t p = px[(long)Y * w + X];
                    float r = (float)(p >> 16 & 255), g = (float)(p >> 8 & 255), bl = (float)(p & 255);
                    blk[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * bl - 128;
                    blk[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * bl;
                    blk[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * bl;
                }
            encode_block(&b, blk[0], ql, &hf[0], &hf[1], &prev[0]);
            encode_block(&b, blk[1], qc, &hf[2], &hf[3], &prev[1]);
            encode_block(&b, blk[2], qc, &hf[2], &hf[3], &prev[2]);
        }
    if (b.n) put_bits(&b, 0x7f, 8 - b.n);
    fputc(0xff, f); fputc(0xd9, f);
}

int image_file_write(const char* path, int format, int w, int h, const uint32_t* px) {
    if (w <= 0 || h <= 0 || format < 0 || format > IMAGE_PFM) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    switch (format) {
    case IMAGE_BMP: write_dib(f, w, h, px, 1); break;
    case IMAGE_JPG: write_jpeg(f, w, h, px); break;
    case IMAGE_TGA: write_tga(f, w, h, px); break;
    case IMAGE_PNG: write_png(f, w, h, px); break;
    case IMAGE_DDS: write_dds(f, w, h, px); break;
    case IMAGE_PPM: write_ppm(f, w, h, px); break;
    case IMAGE_DIB: write_dib(f, w, h, px, 0); break;
    case IMAGE_HDR: write_hdr(f, w, h, px); break;
    case IMAGE_PFM: write_pfm(f, w, h, px); break;
    }
    int ok = !ferror(f);
    return fclose(f) == 0 && ok;
}
