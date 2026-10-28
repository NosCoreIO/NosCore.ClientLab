/* GDI bitmaps with real pixel memory, which Delphi's TBitmap (ScanLine, GetObject) depends on. */
#include "port.h"

#define RET(v) (c->eax = (uint32_t)(v))
#define BMP_BASE 0x00400000u /* handle range 0x04000000.. keeps clear of other fake handles */
#define BMP_HANDLE(i) (0x04000000u + (uint32_t)(i) * 4)
#define MAX_BMP 8192

typedef struct {
    int used;
    int32_t w, h; /* h < 0: top-down */
    uint32_t bpp, stride, bits, size, dib;
    uint32_t colors[256]; /* RGBQUAD colour table for 1/4/8 bpp */
    uint32_t ncolors;
} Bmp;
static Bmp bmps[MAX_BMP];

static Bmp* bmp_get(uint32_t h) {
    uint32_t k = (h - 0x04000000u) / 4;
    if (h < 0x04000000u || k >= MAX_BMP || !bmps[k].used) return 0;
    return &bmps[k];
}

static uint32_t bmp_new(int32_t w, int32_t h, uint32_t bpp, int dib) {
    for (int k = 1; k < MAX_BMP; k++)
        if (!bmps[k].used) {
            Bmp* b = &bmps[k];
            b->used = 1;
            b->w = w; b->h = h; b->bpp = bpp ? bpp : 32; b->dib = (uint32_t)dib;
            b->stride = ((uint32_t)(w < 0 ? -w : w) * b->bpp + 31) / 32 * 4;
            b->size = b->stride * (uint32_t)(h < 0 ? -h : h);
            b->bits = guest_alloc(b->size ? b->size : 16, 16);
            memset(GPTR(b->bits), 0, b->size);
            b->ncolors = b->bpp <= 8 ? 1u << b->bpp : 0;
            if (b->bpp == 1) { b->colors[0] = 0x000000; b->colors[1] = 0xffffff; }
            else for (uint32_t i = 0; i < b->ncolors; i++) {
                uint32_t v = i * 255 / (b->ncolors - 1);
                b->colors[i] = v << 16 | v << 8 | v;
            }
            return BMP_HANDLE(k);
        }
    port_fatal("out of GDI bitmaps");
    return 0;
}

/* Copies a BITMAPINFO colour table (RGBQUADs after the 40-byte header) into the bitmap. */
static void read_color_table(Bmp* b, uint32_t bmi) {
    if (b->bpp > 8 || !bmi) return;
    uint32_t used = M32(bmi + 32), n = used && used < b->ncolors ? used : b->ncolors;
    for (uint32_t i = 0; i < n; i++) b->colors[i] = M32(bmi + 40 + 4 * i) & 0xffffff;
}

uint32_t gdi_new_bitmap(int32_t w, int32_t h, uint32_t bpp) { return bmp_new(w, h, bpp, 1); }

int gdi_bitmap(uint32_t h, GdiBitmap* out) {
    Bmp* b = bmp_get(h);
    if (!b) return 0;
    out->colors = b->colors;
    out->ncolors = b->ncolors;
    out->w = b->w < 0 ? -b->w : b->w;
    out->h = b->h < 0 ? -b->h : b->h;
    out->top_down = b->h < 0;
    out->bpp = b->bpp; out->stride = b->stride; out->bits = b->bits;
    return 1;
}
int gdi_delete_bitmap(uint32_t h) {
    Bmp* b = bmp_get(h);
    if (!b) return 0;
    guest_free(b->bits);
    b->used = 0;
    return 1;
}

static void g_CreateDIBSection(CPU* c) {
    uint32_t bmi = ARG(1);
    int32_t w = (int32_t)M32(bmi + 4), h = (int32_t)M32(bmi + 8);
    uint32_t bpp = M16(bmi + 14);
    uint32_t hbm = bmp_new(w, h, bpp, 1);
    read_color_table(bmp_get(hbm), bmi);
    if (ARG(3)) M32(ARG(3)) = bmp_get(hbm)->bits;
    RET(hbm);
}
static void g_CreateBitmap(CPU* c) {
    uint32_t hbm = bmp_new((int32_t)ARG(0), (int32_t)ARG(1), ARG(3), 0);
    Bmp* b = bmp_get(hbm);
    if (ARG(4)) {
        uint32_t src_stride = ((ARG(0) * ARG(3) + 15) / 16) * 2, rows = ARG(1);
        for (uint32_t y = 0; y < rows; y++) memcpy(GPTR(b->bits + y * b->stride), GPTR(ARG(4) + y * src_stride), src_stride < b->stride ? src_stride : b->stride);
    }
    RET(hbm);
}
static void g_CreateCompatibleBitmap(CPU* c) { RET(bmp_new((int32_t)ARG(1), (int32_t)ARG(2), 32, 0)); }
static void g_CreateDIBitmap(CPU* c) {
    uint32_t bih = ARG(1);
    uint32_t hbm = bih ? bmp_new((int32_t)M32(bih + 4), (int32_t)M32(bih + 8), M16(bih + 14) ? M16(bih + 14) : 32, 0) : bmp_new(1, 1, 32, 0);
    Bmp* b = bmp_get(hbm);
    read_color_table(b, ARG(5));
    if ((ARG(2) & 4) && ARG(3)) memcpy(GPTR(b->bits), GPTR(ARG(3)), b->size);
    RET(hbm);
}

static void fill_bitmap(Bmp* b, uint32_t out, uint32_t size) {
    memset(GPTR(out), 0, size);
    M32(out + 4) = (uint32_t)(b->w < 0 ? -b->w : b->w);
    M32(out + 8) = (uint32_t)(b->h < 0 ? -b->h : b->h);
    M32(out + 12) = b->stride;
    M16(out + 16) = 1;
    M16(out + 18) = (uint16_t)b->bpp;
    M32(out + 20) = b->dib ? b->bits : 0;
    if (size >= 84) {
        uint32_t ih = out + 24;
        M32(ih) = 40; M32(ih + 4) = (uint32_t)b->w; M32(ih + 8) = (uint32_t)b->h; M16(ih + 12) = 1;
        M16(ih + 14) = (uint16_t)b->bpp; M32(ih + 20) = b->size;
    }
}

/* GetObject for a bitmap: BITMAP (24 bytes), or DIBSECTION (84) when asked for and the bitmap is a DIB section. */
int gdi_bitmap_object(uint32_t h, uint32_t size, uint32_t out) {
    Bmp* b = bmp_get(h);
    if (!b) return -1;
    uint32_t want = size >= 84 && b->dib ? 84 : 24;
    if (out) fill_bitmap(b, out, want <= size ? want : size);
    return (int)want;
}

/* GetDIBits / SetDIBits / SetDIBitsToDevice copy rows straight through; formats are assumed to match. */
static void g_GetDIBits(CPU* c) {
    Bmp* b = bmp_get(ARG(1));
    uint32_t start = ARG(2), lines = ARG(3), bits = ARG(4), bmi = ARG(5);
    if (!b) { RET(0); return; }
    if (!bits) {
        M32(bmi + 4) = (uint32_t)b->w; M32(bmi + 8) = (uint32_t)b->h; M16(bmi + 12) = 1;
        if (!M16(bmi + 14)) M16(bmi + 14) = (uint16_t)b->bpp;
        M32(bmi + 16) = 0; M32(bmi + 20) = b->size;
        for (uint32_t i = 0; i < b->ncolors && M16(bmi + 14) <= 8; i++) M32(bmi + 40 + 4 * i) = b->colors[i];
        RET(1);
        return;
    }
    for (uint32_t i = 0; i < b->ncolors && M16(bmi + 14) <= 8; i++) M32(bmi + 40 + 4 * i) = b->colors[i];
    uint32_t rows = (uint32_t)(b->h < 0 ? -b->h : b->h);
    if (start + lines > rows) lines = rows > start ? rows - start : 0;
    memcpy(GPTR(bits), GPTR(b->bits + start * b->stride), lines * b->stride);
    RET(lines);
}
static void g_SetDIBits(CPU* c) {
    Bmp* b = bmp_get(ARG(1));
    uint32_t start = ARG(2), lines = ARG(3), bits = ARG(4);
    if (!b) { RET(0); return; }
    uint32_t rows = (uint32_t)(b->h < 0 ? -b->h : b->h);
    if (start + lines > rows) lines = rows > start ? rows - start : 0;
    memcpy(GPTR(b->bits + start * b->stride), GPTR(bits), lines * b->stride);
    RET(lines);
}
static void g_GetBitmapBits(CPU* c) {
    Bmp* b = bmp_get(ARG(0));
    if (!b) { RET(0); return; }
    uint32_t n = ARG(1) < b->size ? ARG(1) : b->size;
    memcpy(GPTR(ARG(2)), GPTR(b->bits), n);
    RET(n);
}
static void g_SetBitmapBits(CPU* c) {
    Bmp* b = bmp_get(ARG(0));
    if (!b) { RET(0); return; }
    uint32_t n = ARG(1) < b->size ? ARG(1) : b->size;
    memcpy(GPTR(b->bits), GPTR(ARG(2)), n);
    RET(n);
}

static const Hle gdi[] = {
    {"gdi32.dll", "CreateDIBSection", 6, g_CreateDIBSection},
    {"gdi32.dll", "CreateBitmap", 5, g_CreateBitmap},
    {"gdi32.dll", "CreateCompatibleBitmap", 3, g_CreateCompatibleBitmap},
    {"gdi32.dll", "CreateDIBitmap", 6, g_CreateDIBitmap},
    {"gdi32.dll", "GetDIBits", 7, g_GetDIBits},
    {"gdi32.dll", "SetDIBits", 7, g_SetDIBits},
    {"gdi32.dll", "GetBitmapBits", 3, g_GetBitmapBits},
    {"gdi32.dll", "SetBitmapBits", 3, g_SetBitmapBits},
};

void hle_gdi_register(void) { hle_register(gdi, sizeof gdi / sizeof gdi[0]); }
