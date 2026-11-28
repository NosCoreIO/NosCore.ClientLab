/* Differential check: FillRect/PatBlt's solid-brush fast path against the per-pixel path it skips, over random
   rectangles (reversed, off-surface), clip rectangles, exclusions, bottom-up and top-down 16/24/32 bpp surfaces and
   solid/null brushes. Build with emcc (-sERROR_ON_UNDEFINED_SYMBOLS=0) and run under Node. */
#include "../../recomp/port/hle_dc.c"
#include <stdio.h>

static uint32_t seed = 2463534242u;
static uint32_t next(void) { seed ^= seed << 13; seed ^= seed >> 17; return seed ^= seed << 5; }
static int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }

static void per_pixel(Dc* d, GdiBitmap* b, int x0, int y0, int x1, int y1, uint32_t brush, uint32_t rop) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (clipped(d, b, x, y)) continue;
            uint32_t p = 0;
            int has = brush_pixel(d, brush, x, y, &p);
            if (!has && rop == 0xf00021) continue;
            px_set(b, x, y, rop == 0xf00021 ? p : rop3(rop, p, 0, px_get(b, x, y)));
        }
}

int main(void) {
    static const uint32_t formats[] = {32, 24, 16};
    static const uint32_t rops[] = {0xf00021, 0xf00021, 0xf00021, 0x5a0049, 0x550009};
    for (int i = 0; i < 20000; i++) {
        GdiBitmap fast = {0}, slow;
        fast.bpp = formats[next() % 3];
        fast.w = range(1, 40); fast.h = range(1, 40); fast.top_down = (int)(next() & 1);
        fast.stride = ((uint32_t)fast.w * fast.bpp / 8 + 3) & ~3u;
        size_t size = (size_t)fast.stride * (size_t)fast.h;
        uint8_t* a = malloc(size); uint8_t* b = malloc(size);
        for (size_t k = 0; k < size; k++) a[k] = b[k] = (uint8_t)next();
        slow = fast;
        fast.bits = (uint32_t)(uintptr_t)a; slow.bits = (uint32_t)(uintptr_t)b;

        Dc d = {0};
        d.s.clip_on = (int)(next() & 1);
        for (int k = 0; k < 4; k++) d.s.clip[k] = range(-10, 50);
        if (next() % 4 == 0) { d.s.nexcl = 1; for (int k = 0; k < 4; k++) d.s.excl[0][k] = range(-5, 45); }
        Brush* brush;
        uint32_t handle = brush_new(&brush);
        brush->style = next() % 5 == 0 ? BS_NULL : BS_SOLID;
        brush->color = next() & 0xffffff;
        uint32_t rop = rops[next() % 5];
        int x0 = range(-10, 50), y0 = range(-10, 50), x1 = range(-10, 50), y1 = range(-10, 50);

        fill_rect_dev(&d, &fast, x0, y0, x1, y1, handle, rop);
        per_pixel(&d, &slow, x0, y0, x1, y1, handle, rop);
        if (memcmp(a, b, size)) {
            printf("FAIL: iteration %d, %ubpp %dx%d, rect (%d,%d)-(%d,%d), rop %06x, clip %d\n", i, fast.bpp, fast.w, fast.h, x0, y0, x1, y1, rop, d.s.clip_on);
            return 1;
        }
        brush->used = 0;
        free(a); free(b);
    }
    printf("PASS: solid fills match the per-pixel path over 20000 random rectangles, clips and surfaces.\n");
    return 0;
}
