/* Software rasteriser behind the null Direct3D 9 device: enough of the fixed-function pipeline for NosTale's sprites
   (FVF vertices, world/view/projection + texture matrix, TFACTOR/diffuse modulation, alpha test and blending). */
#include "render.h"
#include <math.h>
#include <stdlib.h>

static int gl_on;

/* Per-frame profile logged every 5 seconds. The split into draw calls, uploads and submission reads the clock
   several times per draw, which costs more than the draws themselves, so it is only measured with PORT_PROFILE=1. */
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static double prof_now(void) { return emscripten_get_now(); }
#else
static double prof_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif
static int prof_detail = -1;
static double prof_mark(void) {
    if (prof_detail < 0) prof_detail = getenv("PORT_PROFILE") != 0;
    return prof_detail ? prof_now() : 0;
}
static double prof_draw, prof_upload, prof_frame_start, prof_frames_ms, prof_submit, prof_present, prof_fetch;
static uint32_t prof_frames, prof_draw_calls, prof_vertices;
double port_prof_net;
static void gl_start(uint32_t backbuffer);
static void gl_clear(int flags, uint32_t color, float z, int x, int y, int w, int h);

/* surface objects (see hle_d3d9.c): +12 width +16 height +20 format +28 bits +32 pitch; texture +24 levels +40 levels[] */
#define SW(s) M32((s) + 12)
#define SH(s) M32((s) + 16)
#define SFMT(s) M32((s) + 20)
#define SBITS(s) M32((s) + 28)
#define SPITCH(s) M32((s) + 32)

typedef struct { float m[16]; } Mat;

static struct {
    Mat world, view, proj, tex[8];
    uint32_t rs[256], tss[8][33], samp[8][14];
    uint32_t tex_obj[8];
    uint32_t vb, vb_offset, vb_stride, ib, fvf;
    uint32_t vp_x, vp_y, vp_w, vp_h;
    uint32_t target;
    int vp_set;
    float light[8][26];   /* D3DLIGHT9 as floats (type, diffuse, specular, ambient, position, direction, range, ...) */
    int light_on[8];
    float material[17];   /* D3DMATERIAL9: diffuse, ambient, specular, emissive, power */
} R;

static void identity(Mat* m) { memset(m, 0, sizeof *m); m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1; }

void r_init(uint32_t backbuffer) {
    identity(&R.world); identity(&R.view); identity(&R.proj);
    for (int k = 0; k < 8; k++) identity(&R.tex[k]);
    R.rs[7] = 1;          /* ZENABLE */
    R.rs[14] = 1;         /* ZWRITEENABLE */
    R.rs[23] = 4;         /* ZFUNC LESSEQUAL */
    R.rs[19] = 2;         /* SRCBLEND ONE */
    R.rs[20] = 1;         /* DESTBLEND ZERO */
    R.rs[25] = 8;         /* ALPHAFUNC ALWAYS */
    R.rs[60] = 0xffffffffu;
    R.rs[137] = 1;        /* LIGHTING */
    R.rs[141] = 1;        /* COLORVERTEX */
    R.rs[145] = 1;        /* DIFFUSEMATERIALSOURCE COLOR1 */
    R.rs[146] = 2;        /* SPECULARMATERIALSOURCE COLOR2 */
    for (int s = 0; s < 8; s++) {
        R.tss[s][1] = s ? 1 : 4; R.tss[s][2] = 2; R.tss[s][3] = 1;
        R.tss[s][4] = s ? 1 : 2; R.tss[s][5] = 2; R.tss[s][6] = 1;
        R.samp[s][1] = R.samp[s][2] = R.samp[s][3] = 1;
        R.samp[s][5] = R.samp[s][6] = 1;
    }
    R.target = backbuffer;
    gl_start(backbuffer);
}

void r_set_render_target(uint32_t surface) {
    static uint32_t seen[16];
    static int nseen;
    int known = 0;
    for (int k = 0; k < nseen; k++) known |= seen[k] == surface;
    if (!known && nseen < 16) {
        seen[nseen++] = surface;
        port_log("render target %08x: %ux%u format %u", surface, SW(surface), SH(surface), SFMT(surface));
    }
    R.target = surface;
    R.vp_set = 0;
}
static int wvp_dirty = 1;
void r_set_transform(uint32_t type, uint32_t addr) {
    Mat* m = type == 256 ? &R.world : type == 2 ? &R.view : type == 3 ? &R.proj : (type >= 16 && type < 24) ? &R.tex[type - 16] : 0;
    if (m) memcpy(m->m, GPTR(addr), 64);
    if (type == 256 || type == 2 || type == 3) wvp_dirty = 1;
}
void r_get_transform(uint32_t type, uint32_t addr) {
    Mat* m = type == 256 ? &R.world : type == 2 ? &R.view : type == 3 ? &R.proj : (type >= 16 && type < 24) ? &R.tex[type - 16] : 0;
    if (m) memcpy(GPTR(addr), m->m, 64);
    else { Mat i; identity(&i); memcpy(GPTR(addr), i.m, 64); }
}
void r_set_render_state(uint32_t s, uint32_t v) { if (s < 256) R.rs[s] = v; }
void r_set_light(uint32_t i, uint32_t p) {
    if (i >= 8) return;
    R.light[i][0] = (float)M32(p);
    for (int k = 1; k < 26; k++) R.light[i][k] = MF32(p + 4 * (uint32_t)k);
}
void r_get_light(uint32_t i, uint32_t p) {
    if (i >= 8) return;
    M32(p) = (uint32_t)R.light[i][0];
    for (int k = 1; k < 26; k++) MF32(p + 4 * (uint32_t)k) = R.light[i][k];
}
void r_light_enable(uint32_t i, uint32_t on) { if (i < 8) R.light_on[i] = on != 0; }
uint32_t r_light_enabled(uint32_t i) { return i < 8 ? (uint32_t)R.light_on[i] : 0; }
void r_set_material(uint32_t p) { for (int k = 0; k < 17; k++) R.material[k] = MF32(p + 4 * (uint32_t)k); }
void r_get_material(uint32_t p) { for (int k = 0; k < 17; k++) MF32(p + 4 * (uint32_t)k) = R.material[k]; }
uint32_t r_get_render_state(uint32_t s) { return s < 256 ? R.rs[s] : 0; }
void r_set_tss(uint32_t st, uint32_t t, uint32_t v) { if (st < 8 && t < 33) R.tss[st][t] = v; }
uint32_t r_get_tss(uint32_t st, uint32_t t) { return st < 8 && t < 33 ? R.tss[st][t] : 0; }
void r_set_sampler(uint32_t s, uint32_t t, uint32_t v) { if (s < 8 && t < 14) R.samp[s][t] = v; }
uint32_t r_get_sampler(uint32_t s, uint32_t t) { return s < 8 && t < 14 ? R.samp[s][t] : 0; }
void r_set_texture(uint32_t st, uint32_t tex) { if (st < 8) R.tex_obj[st] = tex; }
uint32_t r_get_texture(uint32_t st) { return st < 8 ? R.tex_obj[st] : 0; }
void r_set_stream(uint32_t vb, uint32_t offset, uint32_t stride) { R.vb = vb; R.vb_offset = offset; R.vb_stride = stride; }
void r_set_indices(uint32_t ib) { R.ib = ib; }
void r_set_fvf(uint32_t fvf) { R.fvf = fvf; }
void r_set_viewport(uint32_t vp) { R.vp_x = M32(vp); R.vp_y = M32(vp + 4); R.vp_w = M32(vp + 8); R.vp_h = M32(vp + 12); R.vp_set = 1; }

/* ---- pixels ---- */

static uint32_t expand(uint32_t v, int bits) { return bits == 0 ? 255 : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1); }

static uint32_t rgb565(uint32_t v) { return 0xff000000u | expand((v >> 11) & 31, 5) << 16 | expand((v >> 5) & 63, 6) << 8 | expand(v & 31, 5); }

static uint32_t dxt_texel(uint32_t s, uint32_t x, uint32_t y, uint32_t fmt) {
    uint32_t bw = (SW(s) + 3) / 4, bsize = fmt == 0x31545844u ? 8 : 16;
    uint32_t blk = SBITS(s) + ((y / 4) * bw + x / 4) * bsize, px = x & 3, py = y & 3;
    uint32_t cblk = fmt == 0x31545844u ? blk : blk + 8, alpha = 255;
    uint32_t c0 = M16(cblk), c1 = M16(cblk + 2), idx = (M32(cblk + 4) >> (2 * (py * 4 + px))) & 3;
    uint32_t a = rgb565(c0), b = rgb565(c1), col;
#define MIX(p, q, wp, wq, d) ((((((p) >> 16) & 255) * (wp) + (((q) >> 16) & 255) * (wq)) / (d)) << 16 | (((((p) >> 8) & 255) * (wp) + (((q) >> 8) & 255) * (wq)) / (d)) << 8 | ((((p) & 255) * (wp) + ((q) & 255) * (wq)) / (d)))
    if (idx == 0) col = a & 0xffffff;
    else if (idx == 1) col = b & 0xffffff;
    else if (c0 > c1 || fmt != 0x31545844u) col = idx == 2 ? MIX(a, b, 2, 1, 3) : MIX(a, b, 1, 2, 3);
    else if (idx == 2) col = MIX(a, b, 1, 1, 2);
    else { col = 0; alpha = 0; }
    if (fmt == 0x33545844u) alpha = expand((M16(blk + 2 * py) >> (4 * px)) & 15, 4);
    else if (fmt == 0x35545844u) {
        uint32_t a0 = M8(blk), a1 = M8(blk + 1);
        uint64_t bits = 0;
        for (int k = 0; k < 6; k++) bits |= (uint64_t)M8(blk + 2 + k) << (8 * k);
        uint32_t ai = (uint32_t)(bits >> (3 * (py * 4 + px))) & 7;
        if (ai == 0) alpha = a0;
        else if (ai == 1) alpha = a1;
        else if (a0 > a1) alpha = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
        else alpha = ai == 6 ? 0 : ai == 7 ? 255 : ((6 - ai) * a0 + (ai - 1) * a1) / 5;
    }
    return alpha << 24 | col;
}

static uint32_t texel(uint32_t s, uint32_t x, uint32_t y) {
    uint32_t fmt = SFMT(s), row = SBITS(s) + y * SPITCH(s), v;
    switch (fmt) {
    case 21: return M32(row + 4 * x);
    case 22: return M32(row + 4 * x) | 0xff000000u;
    case 26: v = M16(row + 2 * x); return expand(v >> 12, 4) << 24 | expand((v >> 8) & 15, 4) << 16 | expand((v >> 4) & 15, 4) << 8 | expand(v & 15, 4);
    case 25: v = M16(row + 2 * x); return (v & 0x8000 ? 0xff000000u : 0) | expand((v >> 10) & 31, 5) << 16 | expand((v >> 5) & 31, 5) << 8 | expand(v & 31, 5);
    case 24: v = M16(row + 2 * x); return 0xff000000u | expand((v >> 10) & 31, 5) << 16 | expand((v >> 5) & 31, 5) << 8 | expand(v & 31, 5);
    case 23: return rgb565(M16(row + 2 * x));
    case 28: return (uint32_t)M8(row + x) << 24 | 0xffffff;
    case 50: v = M8(row + x); return 0xff000000u | v << 16 | v << 8 | v;
    case 51: v = M16(row + 2 * x); return (v >> 8) << 24 | (v & 255) << 16 | (v & 255) << 8 | (v & 255);
    case 0x31545844u: case 0x33545844u: case 0x35545844u: return dxt_texel(s, x, y, fmt);
    default: return 0xffff00ffu;
    }
}

uint32_t r_texel(uint32_t surface, uint32_t x, uint32_t y) { return texel(surface, x, y); }

static int32_t wrap(int32_t i, int32_t n, int clamp) {
    if (clamp) return i < 0 ? 0 : i >= n ? n - 1 : i;
    i %= n;
    return i < 0 ? i + n : i;
}

static uint32_t sample(int st, uint32_t tex, float u, float v) {
    if (!tex) return 0xffffffffu;
    uint32_t s = M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;  /* texture -> level 0 surface */
    int32_t w = (int32_t)SW(s), h = (int32_t)SH(s);
    if (w <= 0 || h <= 0 || !SBITS(s)) return 0xffffffffu;
    int clamp_u = R.samp[st][1] == 3, clamp_v = R.samp[st][2] == 3;
    if (R.samp[st][5] != 2 && R.samp[st][6] != 2) {
        int32_t x = wrap((int32_t)floorf(u * w), w, clamp_u), y = wrap((int32_t)floorf(v * h), h, clamp_v);
        return texel(s, (uint32_t)x, (uint32_t)y);
    }
    float fx = u * w - 0.5f, fy = v * h - 0.5f;
    int32_t x0 = (int32_t)floorf(fx), y0 = (int32_t)floorf(fy);
    uint32_t ax = (uint32_t)((fx - x0) * 256), ay = (uint32_t)((fy - y0) * 256);
    int32_t xa = wrap(x0, w, clamp_u), xb = wrap(x0 + 1, w, clamp_u), ya = wrap(y0, h, clamp_v), yb = wrap(y0 + 1, h, clamp_v);
    uint32_t t00 = texel(s, (uint32_t)xa, (uint32_t)ya), t10 = texel(s, (uint32_t)xb, (uint32_t)ya);
    uint32_t t01 = texel(s, (uint32_t)xa, (uint32_t)yb), t11 = texel(s, (uint32_t)xb, (uint32_t)yb), out = 0;
    for (int k = 0; k < 32; k += 8) {
        uint32_t top = ((t00 >> k) & 255) * (256 - ax) + ((t10 >> k) & 255) * ax;
        uint32_t bot = ((t01 >> k) & 255) * (256 - ax) + ((t11 >> k) & 255) * ax;
        out |= ((top * (256 - ay) + bot * ay + 32768) >> 16) << k;
    }
    return out;
}

static uint32_t mul_color(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int k = 0; k < 32; k += 8) r |= (((a >> k) & 255) * ((b >> k) & 255) + 127) / 255 << k;
    return r;
}
static uint32_t add_color(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int k = 0; k < 32; k += 8) { uint32_t v = ((a >> k) & 255) + ((b >> k) & 255); r |= (v > 255 ? 255 : v) << k; }
    return r;
}

static uint32_t stage_arg(uint32_t arg, uint32_t tex, uint32_t diffuse, uint32_t current) {
    uint32_t v;
    switch (arg & 0xf) {
    case 0: v = diffuse; break;
    case 1: v = current; break;
    case 2: v = tex; break;
    case 3: v = R.rs[60]; break;
    default: v = 0xffffffffu; break;
    }
    if (arg & 0x10) v = ~v;
    if (arg & 0x20) { uint32_t a = v >> 24; v = a << 24 | a << 16 | a << 8 | a; }
    return v;
}
static uint32_t stage_op(uint32_t op, uint32_t a1, uint32_t a2, uint32_t current) {
    switch (op) {
    case 1: return current;
    case 2: return a1;
    case 3: return a2;
    case 4: return mul_color(a1, a2);
    case 5: return add_color(mul_color(a1, a2), mul_color(a1, a2));
    case 7: return add_color(a1, a2);
    default: return mul_color(a1, a2);
    }
}

#define STAGES 2

/* Fixed-function texture stages, evaluated until the first disabled colour op. */
static uint32_t shade(uint32_t diffuse, const float* uv) {
    uint32_t current = diffuse;
    for (int s = 0; s < STAGES; s++) {
        if (R.tss[s][1] == 1) break;
        uint32_t tex = R.tex_obj[s] ? sample(s, R.tex_obj[s], uv[2 * s], uv[2 * s + 1]) : 0xffffffffu;
        uint32_t color = stage_op(R.tss[s][1], stage_arg(R.tss[s][2], tex, diffuse, current),
                                  stage_arg(R.tss[s][3], tex, diffuse, current), current);
        uint32_t alpha = R.tss[s][4] == 1 ? current
                       : stage_op(R.tss[s][4], stage_arg(R.tss[s][5], tex, diffuse, current),
                                  stage_arg(R.tss[s][6], tex, diffuse, current), current);
        current = (alpha & 0xff000000u) | (color & 0xffffff);
    }
    return current;
}

static int alpha_pass(uint32_t a) {
    if (!R.rs[15]) return 1;
    uint32_t ref = R.rs[24] & 255;
    switch (R.rs[25]) {
    case 1: return 0;
    case 2: return a < ref;
    case 3: return a == ref;
    case 4: return a <= ref;
    case 5: return a > ref;
    case 6: return a != ref;
    case 7: return a >= ref;
    default: return 1;
    }
}

static uint32_t blend_factor(uint32_t f, uint32_t src, uint32_t dst, int ch) {
    uint32_t sa = src >> 24, da = dst >> 24, s = (src >> ch) & 255, d = (dst >> ch) & 255;
    switch (f) {
    case 1: return 0;
    case 2: return 255;
    case 3: return s;
    case 4: return 255 - s;
    case 5: return sa;
    case 6: return 255 - sa;
    case 7: return da;
    case 8: return 255 - da;
    case 9: return d;
    case 10: return 255 - d;
    default: return 255;
    }
}

static struct { uint32_t draws, tris, covered, written, skipped; } S;

/* Depth buffer for the render target, kept host-side. */
static float* zbuf;
static uint32_t zw, zh;

static float* depth_for_target(void) {
    uint32_t w = SW(R.target), h = SH(R.target);
    if (!zbuf || zw != w || zh != h) {
        free(zbuf);
        zbuf = malloc(sizeof(float) * w * h);
        zw = w; zh = h;
        for (uint32_t k = 0; k < w * h; k++) zbuf[k] = 1.0f;
    }
    return zbuf;
}

static int depth_pass(float z, float d) {
    switch (R.rs[23]) {
    case 1: return 0;
    case 2: return z < d;
    case 3: return z == d;
    case 4: return z <= d;
    case 5: return z > d;
    case 6: return z != d;
    case 7: return z >= d;
    default: return 1;
    }
}

static void put(uint32_t x, uint32_t y, float z, uint32_t src) {
    uint32_t t = R.target, addr = SBITS(t) + y * SPITCH(t) + 4 * x;
    S.covered++;
    if (!alpha_pass(src >> 24)) return;
    if (R.rs[7]) {
        float* zp = depth_for_target() + y * zw + x;
        if (!depth_pass(z, *zp)) return;
        if (R.rs[14]) *zp = z;
    }
    S.written++;
    if (!R.rs[27]) { M32(addr) = src; return; }
    uint32_t dst = M32(addr), out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        uint32_t sf = blend_factor(R.rs[19], src, dst, ch), df = blend_factor(R.rs[20], src, dst, ch);
        uint32_t v = (((src >> ch) & 255) * sf + ((dst >> ch) & 255) * df + 127) / 255;
        out |= (v > 255 ? 255 : v) << ch;
    }
    M32(addr) = out;
}

/* ---- vertices ---- */

typedef struct { float x, y, z, w, uv[2 * STAGES]; uint32_t diffuse; float fog; } Vtx;

static void xform(const Mat* m, const float in[4], float out[4]) {
    for (int j = 0; j < 4; j++) out[j] = in[0] * m->m[j] + in[1] * m->m[4 + j] + in[2] * m->m[8 + j] + in[3] * m->m[12 + j];
}

/* Fixed-function vertex lighting (world space; directional, point and spot lights treated as point). */
static uint32_t light_vertex(const float pos[3], const float nrm_in[3], int has_color, uint32_t vcolor) {
    float n[3];
    for (int j = 0; j < 3; j++) n[j] = nrm_in[0] * R.world.m[j] + nrm_in[1] * R.world.m[4 + j] + nrm_in[2] * R.world.m[8 + j];
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0) for (int j = 0; j < 3; j++) n[j] /= len;
    float vc[4] = {((vcolor >> 16) & 255) / 255.0f, ((vcolor >> 8) & 255) / 255.0f, (vcolor & 255) / 255.0f, (vcolor >> 24) / 255.0f};
    int colorvertex = R.rs[141] && has_color;
    const float* dsrc = colorvertex && R.rs[145] == 1 ? vc : R.material + 0;
    const float* asrc = colorvertex && R.rs[147] == 1 ? vc : R.material + 4;
    const float* esrc = colorvertex && R.rs[148] == 1 ? vc : R.material + 12;
    uint32_t amb = R.rs[139];
    float ga[3] = {((amb >> 16) & 255) / 255.0f, ((amb >> 8) & 255) / 255.0f, (amb & 255) / 255.0f};
    float out[3];
    for (int ch = 0; ch < 3; ch++) out[ch] = esrc[ch] + ga[ch] * asrc[ch];
    for (int i = 0; i < 8; i++) {
        if (!R.light_on[i]) continue;
        const float* L = R.light[i];
        float dir[3], atten = 1;
        if ((int)L[0] == 3) {
            for (int j = 0; j < 3; j++) dir[j] = -L[16 + j];
        } else {
            float d2 = 0;
            for (int j = 0; j < 3; j++) { dir[j] = L[13 + j] - pos[j]; d2 += dir[j] * dir[j]; }
            float d = sqrtf(d2);
            if (L[19] > 0 && d > L[19]) continue;
            float a = L[21] + L[22] * d + L[23] * d2;
            atten = a > 0 ? 1 / a : 1;
        }
        float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        float ndotl = dl > 0 ? (n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2]) / dl : 0;
        if (ndotl < 0) ndotl = 0;
        for (int ch = 0; ch < 3; ch++) out[ch] += atten * (L[9 + ch] * asrc[ch] + ndotl * L[1 + ch] * dsrc[ch]);
    }
    uint32_t r = 0;
    for (int ch = 0; ch < 3; ch++) {
        float v = out[ch] < 0 ? 0 : out[ch] > 1 ? 1 : out[ch];
        r |= (uint32_t)(v * 255 + 0.5f) << (16 - 8 * ch);
    }
    float a = dsrc[3] < 0 ? 0 : dsrc[3] > 1 ? 1 : dsrc[3];
    return r | (uint32_t)(a * 255 + 0.5f) << 24;
}

/* world * view * projection, rebuilt only when one of them changes */
static Mat wvp, wv;
static void mat_mul(const Mat* a, const Mat* b, Mat* out) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[4 * i + k] * b->m[4 * k + j];
            out->m[4 * i + j] = s;
        }
}
static void update_wvp(void) {
    if (!wvp_dirty) return;
    mat_mul(&R.world, &R.view, &wv);
    mat_mul(&wv, &R.proj, &wvp);
    wvp_dirty = 0;
}

/* Vertex layout and per-draw constants, worked out once per draw call instead of once per vertex. */
static struct {
    uint32_t pos, normal_at, diffuse_at, tex_at[4], tex_n[4];
    int lit, has_diffuse, pretransformed, stage_set[STAGES], stage_gen[STAGES], needs_camera_pos, needs_camera_normal;
    int fog_mode;   /* 0 none, 1 exp, 2 exp2, 3 linear */
    float fog_start, fog_end, fog_density;
    uint32_t ttff[STAGES];
    float vw, vh, vx, vy;
} LY;

static double fetch_t0;
static void layout_begin(void) {
    fetch_t0 = prof_mark();
    uint32_t fvf = R.fvf, pos = fvf & 0xe, off = pos == 4 ? 16 : 12;
    if (pos >= 6 && pos <= 0xe) off += 4 * ((pos - 4) / 2);
    LY.pos = pos;
    LY.pretransformed = pos == 4;
    LY.normal_at = off;
    if (fvf & 0x10) off += 12;
    if (fvf & 0x20) off += 4;
    LY.has_diffuse = (fvf & 0x40) != 0;
    LY.diffuse_at = off;
    if (fvf & 0x40) off += 4;
    if (fvf & 0x80) off += 4;
    uint32_t nsets = (fvf >> 8) & 15;
    for (uint32_t k = 0; k < 4; k++) {
        LY.tex_n[k] = 0;
        if (k >= nsets) continue;
        uint32_t tsize = (fvf >> (16 + 2 * k)) & 3;
        LY.tex_n[k] = tsize == 0 ? 2 : tsize == 1 ? 3 : tsize == 2 ? 4 : 1;
        LY.tex_at[k] = off;
        off += 4 * LY.tex_n[k];
    }
    LY.needs_camera_pos = LY.needs_camera_normal = 0;
    for (int st = 0; st < STAGES; st++) {
        LY.stage_set[st] = (int)(R.tss[st][11] & 3);
        LY.stage_gen[st] = (int)(R.tss[st][11] >> 16);   /* D3DTSS_TCI_*: 1 camera-space normal, 2 camera-space position */
        LY.ttff[st] = R.tss[st][24];
        if (LY.stage_gen[st] == 2) LY.needs_camera_pos = 1;
        if (LY.stage_gen[st] == 1) LY.needs_camera_normal = 1;
    }
    LY.lit = R.rs[137] && (fvf & 0x10) && pos != 4;
    /* D3D fog: table (pixel) mode if set, else vertex mode; both computed here from camera-space depth. */
    LY.fog_mode = R.rs[28] && pos != 4 ? (int)(R.rs[35] ? R.rs[35] : R.rs[140]) : 0;
    if (LY.fog_mode) {
        LY.needs_camera_pos = 1;
        memcpy(&LY.fog_start, &R.rs[36], 4);
        memcpy(&LY.fog_end, &R.rs[37], 4);
        memcpy(&LY.fog_density, &R.rs[38], 4);
    }
    LY.vw = (float)(R.vp_set ? R.vp_w : SW(R.target));
    LY.vh = (float)(R.vp_set ? R.vp_h : SH(R.target));
    LY.vx = (float)(R.vp_set ? R.vp_x : 0);
    LY.vy = (float)(R.vp_set ? R.vp_y : 0);
    if (!LY.pretransformed) update_wvp();
}

static void fetch(uint32_t p, Vtx* o) {
    float in[4] = {MF32(p), MF32(p + 4), MF32(p + 8), 1}, t[4];
    o->diffuse = LY.has_diffuse ? M32(p + LY.diffuse_at) : 0xffffffffu;
    if (LY.lit) {
        float wp[4], nrm[3] = {MF32(p + LY.normal_at), MF32(p + LY.normal_at + 4), MF32(p + LY.normal_at + 8)};
        xform(&R.world, in, wp);
        o->diffuse = light_vertex(wp, nrm, LY.has_diffuse, o->diffuse);
    }
    float cam_pos[4] = {0, 0, 0, 1}, cam_normal[4] = {0, 0, 0, 0};
    if (!LY.pretransformed && LY.needs_camera_pos) xform(&wv, in, cam_pos);
    o->fog = 1;
    if (LY.fog_mode) {
        float d = fabsf(cam_pos[2]);
        if (LY.fog_mode == 3) o->fog = LY.fog_end != LY.fog_start ? (LY.fog_end - d) / (LY.fog_end - LY.fog_start) : 1;
        else if (LY.fog_mode == 1) o->fog = expf(-LY.fog_density * d);
        else if (LY.fog_mode == 2) o->fog = expf(-(LY.fog_density * d) * (LY.fog_density * d));
        o->fog = o->fog < 0 ? 0 : o->fog > 1 ? 1 : o->fog;
    }
    if (!LY.pretransformed && LY.needs_camera_normal && (R.fvf & 0x10)) {
        float nrm[4] = {MF32(p + LY.normal_at), MF32(p + LY.normal_at + 4), MF32(p + LY.normal_at + 8), 0};
        xform(&wv, nrm, cam_normal);
    }
    for (int st = 0; st < STAGES; st++) {
        int set = LY.stage_set[st];
        float tc[4] = {0, 0, 1, 0};
        if (LY.stage_gen[st] == 2) memcpy(tc, cam_pos, sizeof tc);
        else if (LY.stage_gen[st] == 1) memcpy(tc, cam_normal, sizeof tc);
        else {
            uint32_t n = LY.tex_n[set], at = p + LY.tex_at[set];
            for (uint32_t j = 0; j < n; j++) tc[j] = MF32(at + 4 * j);
            if (n == 3) tc[3] = 1;
        }
        uint32_t ttff = LY.ttff[st];
        if (ttff & 7) {
            float r[4];
            xform(&R.tex[st], tc, r);
            float q = (ttff & 256) ? r[(ttff & 7) - 1] : 1;
            o->uv[2 * st] = r[0] / q; o->uv[2 * st + 1] = r[1] / q;
        } else {
            o->uv[2 * st] = tc[0]; o->uv[2 * st + 1] = tc[1];
        }
    }
    if (LY.pretransformed) { o->x = in[0]; o->y = in[1]; o->z = in[2]; o->w = 1; return; }
    xform(&wvp, in, t);
    float w = t[3] != 0 ? t[3] : 1;
    o->x = LY.vx + (t[0] / w + 1) * 0.5f * LY.vw;
    o->y = LY.vy + (1 - t[1] / w) * 0.5f * LY.vh;
    o->z = t[2] / w;
    o->w = w;
}

static void triangle(const Vtx* a, const Vtx* b, const Vtx* c) {
    S.tris++;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f) return;
    int32_t tw = (int32_t)SW(R.target), th = (int32_t)SH(R.target);
    int32_t x0 = (int32_t)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int32_t)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int32_t y0 = (int32_t)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int32_t)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    int flat = (a->diffuse == b->diffuse && b->diffuse == c->diffuse);
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++) {
            float px = (float)x, py = (float)y; /* D3D9 samples pixels at integer coordinates */
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) / area;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float uv[2 * STAGES];
            for (int k = 0; k < 2 * STAGES; k++) uv[k] = w0 * a->uv[k] + w1 * b->uv[k] + w2 * c->uv[k];
            uint32_t d = a->diffuse;
            if (!flat) {
                d = 0;
                for (int ch = 0; ch < 32; ch += 8) {
                    float f = w0 * ((a->diffuse >> ch) & 255) + w1 * ((b->diffuse >> ch) & 255) + w2 * ((c->diffuse >> ch) & 255);
                    d |= (uint32_t)(f + 0.5f) << ch;
                }
            }
            put((uint32_t)x, (uint32_t)y, w0 * a->z + w1 * b->z + w2 * c->z, shade(d, uv));
        }
}

static void log_state_combo(void) {
    static uint32_t seen[64][8];
    static int nseen;
    uint32_t tex = R.tex_obj[0], s = tex && M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;
    uint32_t key[8] = {s ? SFMT(s) : 0, s ? SW(s) << 16 | SH(s) : 0, R.rs[27] << 16 | R.rs[19] << 8 | R.rs[20], R.rs[171],
                       R.tss[0][1] << 16 | R.tss[0][2] << 8 | R.tss[0][3], R.tss[0][4] << 16 | R.tss[0][5] << 8 | R.tss[0][6],
                       R.tss[1][1] << 16 | (R.tex_obj[1] != 0), R.fvf};
    for (int k = 0; k < nseen; k++) if (!memcmp(seen[k], key, sizeof key)) return;
    if (nseen >= 64) return;
    memcpy(seen[nseen++], key, sizeof key);
    port_log("draw combo: fmt %u %ux%u blend %u src %u dst %u op %u | c %u(%u,%u) a %u(%u,%u) | st1 op %u tex %u | fvf %x target fmt %u",
             key[0], key[1] >> 16, key[1] & 0xffff, R.rs[27], R.rs[19], R.rs[20], R.rs[171], R.tss[0][1], R.tss[0][2],
             R.tss[0][3], R.tss[0][4], R.tss[0][5], R.tss[0][6], R.tss[1][1], R.tex_obj[1] != 0, R.fvf,
             R.target ? SFMT(R.target) : 0);
}

static void gl_draw_list(uint32_t prim, Vtx* v, uint32_t n);

/* Frame dump (F9 in the page): every draw of the next frame with its state, to debug a scene that renders wrong. */
static int dump_state;   /* 1: armed, dump from the next Present; 2: dumping this frame */
static uint32_t dump_draws;
void r_dump_next_frame(void) { dump_state = 1; }

static void dump_draw(uint32_t prim, Vtx* v, uint32_t n) {
    uint32_t tex = R.tex_obj[0], s = tex && M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;
    port_log("draw %u: prim %u n %u tex %ux%u fmt %u | blend %u %u->%u atest %u/%u ref %u | z %u/%u/%u fog %u light %u "
             "| s0 %u(%x,%x) a%u s1 %u tex1 %u | v0 (%.1f, %.1f, %.4f, w %.2f) diffuse %08x uv %.3f,%.3f",
             dump_draws++, prim, n, s ? SW(s) : 0, s ? SH(s) : 0, s ? SFMT(s) : 0, R.rs[27], R.rs[19], R.rs[20], R.rs[15],
             R.rs[25], R.rs[24] & 255, R.rs[7], R.rs[14], R.rs[23], R.rs[28], R.rs[137], R.tss[0][1], R.tss[0][2],
             R.tss[0][3], R.tss[0][4], R.tss[1][1], R.tex_obj[1] != 0, v[0].x, v[0].y, v[0].z, v[0].w, v[0].diffuse,
             v[0].uv[0], v[0].uv[1]);
}

static void draw_list(uint32_t prim, uint32_t count, Vtx* v, uint32_t n) {
    if (dump_state == 2 && n) dump_draw(prim, v, n);
    prof_fetch += prof_mark() - fetch_t0;
    S.draws++;
    if (gl_on) { gl_draw_list(prim, v, n); return; }
    log_state_combo();
    if (!R.target || !SBITS(R.target) || (SFMT(R.target) != 21 && SFMT(R.target) != 22)) { S.skipped++; return; }
    if (prim == 4) for (uint32_t k = 0; k < count && 3 * k + 2 < n; k++) triangle(&v[3 * k], &v[3 * k + 1], &v[3 * k + 2]);
    else if (prim == 5) for (uint32_t k = 0; k < count && k + 2 < n; k++) triangle(&v[k], &v[k + 1], &v[k + 2]);
    else if (prim == 6) for (uint32_t k = 0; k < count && k + 2 < n; k++) triangle(&v[0], &v[k + 1], &v[k + 2]);
}

static uint32_t vertex_count(uint32_t prim, uint32_t count) { return prim == 4 ? 3 * count : prim == 5 || prim == 6 ? count + 2 : 0; }

/* Vertex scratch, grown on demand: a large map's terrain is a single draw of well over 64k vertices. */
static Vtx* vbuf;
static uint32_t vbuf_cap;
static Vtx* vcache;
static uint32_t vcache_cap;

static int reserve(Vtx** buf, uint32_t* cap, uint32_t n) {
    if (n <= *cap) return 1;
    Vtx* grown = realloc(*buf, sizeof(Vtx) * n);
    if (!grown) return 0;
    *buf = grown;
    *cap = n;
    return 1;
}

static void draw_impl_draw(uint32_t prim, uint32_t start, uint32_t count) {
    layout_begin();
    uint32_t n = vertex_count(prim, count);
    if (!R.vb || !n || !R.vb_stride || !reserve(&vbuf, &vbuf_cap, n)) return;
    uint32_t base = M32(R.vb + 28) + R.vb_offset;
    for (uint32_t k = 0; k < n; k++) fetch(base + (start + k) * R.vb_stride, &vbuf[k]);
    draw_list(prim, count, vbuf, n);
}

static void draw_impl_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride) {
    layout_begin();
    uint32_t n = vertex_count(prim, count);
    if (!n || !reserve(&vbuf, &vbuf_cap, n)) return;
    for (uint32_t k = 0; k < n; k++) fetch(data + k * stride, &vbuf[k]);
    draw_list(prim, count, vbuf, n);
}

/* Indexed meshes share vertices between triangles: each referenced vertex is transformed and lit once, then the
   index list picks from that cache. */

static void draw_indexed_cached(uint32_t prim, uint32_t count, uint32_t n, uint32_t vb, uint32_t stride,
                                uint32_t indices, int idx32, int32_t base_vertex) {
    uint32_t lo = 0xffffffffu, hi = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = idx32 ? M32(indices + 4 * k) : M16(indices + 2 * k);
        if (i < lo) lo = i;
        if (i > hi) hi = i;
    }
    if (hi < lo || !reserve(&vcache, &vcache_cap, hi - lo + 1)) return;
    for (uint32_t i = lo; i <= hi; i++) fetch(vb + (uint32_t)((int32_t)i + base_vertex) * stride, &vcache[i - lo]);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = idx32 ? M32(indices + 4 * k) : M16(indices + 2 * k);
        vbuf[k] = vcache[i - lo];
    }
    draw_list(prim, count, vbuf, n);
}

static void draw_impl_draw_indexed(uint32_t prim, int32_t base_vertex, uint32_t start_index, uint32_t count) {
    layout_begin();
    uint32_t n = vertex_count(prim, count);
    if (!R.vb || !R.ib || !n || !reserve(&vbuf, &vbuf_cap, n)) return;
    int idx32 = M32(R.ib + 20) == 102;
    uint32_t ib = M32(R.ib + 28) + start_index * (idx32 ? 4 : 2);
    draw_indexed_cached(prim, count, n, M32(R.vb + 28) + R.vb_offset, R.vb_stride, ib, idx32, base_vertex);
}

static void draw_impl_draw_indexed_up(uint32_t prim, uint32_t count, uint32_t indices, uint32_t idx_fmt, uint32_t data, uint32_t stride) {
    layout_begin();
    uint32_t n = vertex_count(prim, count);
    if (!n || !reserve(&vbuf, &vbuf_cap, n)) return;
    draw_indexed_cached(prim, count, n, data, stride, indices, idx_fmt == 102, 0);
}

void r_clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z) {
    uint32_t t = R.target;
    if (gl_on) {
        gl_clear((int)flags, color, z, (int)(R.vp_set ? R.vp_x : 0), (int)(R.vp_set ? R.vp_y : 0),
                 (int)(R.vp_set ? R.vp_w : SW(t)), (int)(R.vp_set ? R.vp_h : SH(t)));
        return;
    }
    if ((flags & 2) && t) {
        float* zp = depth_for_target();
        for (uint32_t k = 0; k < zw * zh; k++) zp[k] = z;
    }
    if (!(flags & 1) || !t || !SBITS(t) || (SFMT(t) != 21 && SFMT(t) != 22)) return;
    for (uint32_t y = 0; y < SH(t); y++)
        for (uint32_t x = 0; x < SW(t); x++) M32(SBITS(t) + y * SPITCH(t) + 4 * x) = color;
    (void)count; (void)rects;
}

void r_dump_bmp(uint32_t surface, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t w = SW(surface), h = SH(surface);
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t size = 54 + w * h * 4;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    int32_t ih = -(int32_t)h;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &ih, 4);
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    for (uint32_t y = 0; y < h; y++) fwrite(GPTR(SBITS(surface) + y * SPITCH(surface)), 4, w, f);
    fclose(f);
}

void r_prof_frame(void) {
    if (dump_state == 2) { dump_state = 0; port_log("frame dump: %u draws", dump_draws); }
    else if (dump_state == 1) { dump_state = 2; dump_draws = 0; }
    double now = prof_now();
    if (prof_frame_start > 0) prof_frames_ms += now - prof_frame_start;
    prof_frame_start = now;
    if (++prof_frames < 2 || prof_frames_ms < 5000) return;
    if (!prof_detail)
        port_log("profile: %.1f ms/frame (%.0f fps), %u draw calls, %u prims; PORT_PROFILE=1 splits the frame",
                 prof_frames_ms / prof_frames, 1000.0 * prof_frames / prof_frames_ms, prof_draw_calls / prof_frames,
                 prof_vertices / prof_frames);
    else port_log("profile: %.1f ms/frame (%.0f fps): draw calls %.1f ms (%u calls, %u prims; GL submit %.1f), texture uploads %.1f, "
             "present %.1f, game %.1f; vertex fetch %.1f, network relay %.1f",
             prof_frames_ms / prof_frames, 1000.0 * prof_frames / prof_frames_ms, prof_draw / prof_frames,
             prof_draw_calls / prof_frames, prof_vertices / prof_frames, prof_submit / prof_frames, prof_upload / prof_frames,
             prof_present / prof_frames, (prof_frames_ms - prof_draw - prof_upload - prof_present) / prof_frames,
             prof_fetch / prof_frames, port_prof_net / prof_frames);
    port_report_api_time(prof_frames);
    prof_frames = prof_draw_calls = prof_vertices = 0;
    prof_frames_ms = prof_draw = prof_upload = prof_submit = prof_present = prof_fetch = port_prof_net = 0;
}

void r_stats(void) {
    port_log("render: %u draws (%u skipped), %u triangles, %u pixels covered, %u written; target %ux%u fmt %u; sample v0 x=%.1f y=%.1f u=%.3f v=%.3f",
             S.draws, S.skipped, S.tris, S.covered, S.written, R.target ? SW(R.target) : 0, R.target ? SH(R.target) : 0,
             R.target ? SFMT(R.target) : 0, vbuf[0].x, vbuf[0].y, vbuf[0].uv[0], vbuf[0].uv[1]);
    static int dumped;
    if (!dumped++) {
        const Mat* ms[4] = {&R.world, &R.view, &R.proj, &R.tex[0]};
        const char* names[4] = {"world", "view", "proj", "tex0"};
        for (int k = 0; k < 4; k++)
            port_log("  %s: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", names[k], ms[k]->m[0], ms[k]->m[1], ms[k]->m[2],
                     ms[k]->m[3], ms[k]->m[4], ms[k]->m[5], ms[k]->m[6], ms[k]->m[7], ms[k]->m[8], ms[k]->m[9], ms[k]->m[10],
                     ms[k]->m[11], ms[k]->m[12], ms[k]->m[13], ms[k]->m[14], ms[k]->m[15]);
        if (R.vb) {
            uint32_t p = M32(R.vb + 28) + R.vb_offset;
            port_log("  vb %08x stride %u fvf %08x first vertex: %g %g %g | %g %g %g | %g %g %g", R.vb, R.vb_stride, R.fvf,
                     MF32(p), MF32(p + 4), MF32(p + 8), MF32(p + 12), MF32(p + 16), MF32(p + 20), MF32(p + 24), MF32(p + 28), MF32(p + 32));
        }
    }
    memset(&S, 0, sizeof S);
}

#include "render_gl.inc"

void r_draw(uint32_t prim, uint32_t start, uint32_t count) {
    double t = prof_mark();
    draw_impl_draw(prim, start, count);
    prof_draw += prof_mark() - t;
    prof_draw_calls++;
    prof_vertices += count;
}

void r_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride) {
    double t = prof_mark();
    draw_impl_draw_up(prim, count, data, stride);
    prof_draw += prof_mark() - t;
    prof_draw_calls++;
    prof_vertices += count;
}

void r_draw_indexed(uint32_t prim, int32_t base_vertex, uint32_t start_index, uint32_t count) {
    double t = prof_mark();
    draw_impl_draw_indexed(prim, base_vertex, start_index, count);
    prof_draw += prof_mark() - t;
    prof_draw_calls++;
    prof_vertices += count;
}

void r_draw_indexed_up(uint32_t prim, uint32_t count, uint32_t indices, uint32_t idx_fmt, uint32_t data, uint32_t stride) {
    double t = prof_mark();
    draw_impl_draw_indexed_up(prim, count, indices, idx_fmt, data, stride);
    prof_draw += prof_mark() - t;
    prof_draw_calls++;
    prof_vertices += count;
}
