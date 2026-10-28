/* GDI device contexts, fonts and text. Glyphs are rasterised by the browser (OffscreenCanvas in the worker) and blended
   into the DIB selected in the DC, which is how the client's TBitmap-based widgets get their text. */
#include "port.h"
#include <stdio.h>
#include <stdlib.h>

#define RET(v) (c->eax = (uint32_t)(v))

#define FONT_BASE 0x05000000u
#define DC_BASE 0x05800000u
#define MAX_FONT 4096
#define MAX_DC 1024
#define STOCK_BASE 0x00100000u

typedef struct {
    int used, size, bold, italic;
    char face[32];
    char css[96];
} Font;
static Font fonts[MAX_FONT];

typedef struct {
    int used;
    uint32_t bmp, font, other;
    uint32_t text_color, bk_color, align;
    int bk_mode;
} Dc;
static Dc dcs[MAX_DC];

static Font default_font = {1, 12, 0, 0, "Tahoma", "12px Tahoma, Arial, sans-serif"};

static Font* font_get(uint32_t h) {
    uint32_t k = (h - FONT_BASE) / 4;
    if (h < FONT_BASE || k >= MAX_FONT || !fonts[k].used) return 0;
    return &fonts[k];
}
static Dc* dc_get(uint32_t h) {
    uint32_t k = (h - DC_BASE) / 4;
    if (h < DC_BASE || k >= MAX_DC || !dcs[k].used) return 0;
    return &dcs[k];
}
static Font* dc_font(Dc* d) {
    Font* f = d ? font_get(d->font) : 0;
    return f ? f : &default_font;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Text arrives as guest bytes (Windows-1252 for the A calls, UTF-16 for the W calls). */
EM_JS(void, js_text_measure, (const char* css, const void* text, int n, int wide, int* out), {
    const s = wide ? String.fromCharCode.apply(null, HEAPU16.subarray(text >> 1, (text >> 1) + n))
                   : String.fromCharCode.apply(null, HEAPU8.subarray(text, text + n));
    const font = UTF8ToString(css);
    if (typeof OffscreenCanvas === 'undefined') {
        HEAP32[out >> 2] = 7 * n; HEAP32[(out >> 2) + 1] = 13; HEAP32[(out >> 2) + 2] = 3;
        return;
    }
    const g = self.__textCtx || (self.__textCtx = new OffscreenCanvas(4, 4).getContext('2d', { willReadFrequently: true }));
    g.font = font;
    const m = g.measureText(s);
    const asc = Math.ceil(m.fontBoundingBoxAscent ?? m.actualBoundingBoxAscent ?? 10);
    const desc = Math.ceil(m.fontBoundingBoxDescent ?? m.actualBoundingBoxDescent ?? 3);
    HEAP32[out >> 2] = Math.round(m.width); HEAP32[(out >> 2) + 1] = asc; HEAP32[(out >> 2) + 2] = desc;
});
EM_JS(void, js_text_prefixes, (const char* css, const void* text, int n, int wide, int* out), {
    const s = wide ? String.fromCharCode.apply(null, HEAPU16.subarray(text >> 1, (text >> 1) + n))
                   : String.fromCharCode.apply(null, HEAPU8.subarray(text, text + n));
    if (typeof OffscreenCanvas === 'undefined') {
        for (let i = 0; i < n; i++) HEAP32[(out >> 2) + i] = 7 * (i + 1);
        return;
    }
    const g = self.__textCtx || (self.__textCtx = new OffscreenCanvas(4, 4).getContext('2d', { willReadFrequently: true }));
    g.font = UTF8ToString(css);
    for (let i = 0; i < n; i++) HEAP32[(out >> 2) + i] = Math.round(g.measureText(s.slice(0, i + 1)).width);
});
/* Coverage (0..255) of the string drawn at baseline 'asc' into a w x h box. Returns 0 when it cannot render. */
EM_JS(int, js_text_render, (const char* css, const void* text, int n, int wide, int w, int h, int asc, uint8_t* out), {
    if (typeof OffscreenCanvas === 'undefined' || w <= 0 || h <= 0) return 0;
    const s = wide ? String.fromCharCode.apply(null, HEAPU16.subarray(text >> 1, (text >> 1) + n))
                   : String.fromCharCode.apply(null, HEAPU8.subarray(text, text + n));
    let cv = self.__textCanvas;
    if (!cv || cv.width < w || cv.height < h) {
        cv = self.__textCanvas = new OffscreenCanvas(Math.max(w, cv ? cv.width : 0), Math.max(h, cv ? cv.height : 0));
        self.__textDraw = cv.getContext('2d', { willReadFrequently: true });
    }
    const g = self.__textDraw;
    g.clearRect(0, 0, w, h);
    g.font = UTF8ToString(css);
    g.fillStyle = '#fff';
    g.textBaseline = 'alphabetic';
    g.fillText(s, 0, asc);
    const px = g.getImageData(0, 0, w, h).data;
    for (let i = 0, j = 3; i < w * h; i++, j += 4) HEAPU8[out + i] = px[j];
    return 1;
});
#else
static void js_text_measure(const char* css, const void* text, int n, int wide, int* out) {
    (void)css; (void)text; (void)wide; out[0] = 7 * n; out[1] = 13; out[2] = 3;
}
static void js_text_prefixes(const char* css, const void* text, int n, int wide, int* out) {
    (void)css; (void)text; (void)wide; for (int i = 0; i < n; i++) out[i] = 7 * (i + 1);
}
static int js_text_render(const char* css, const void* text, int n, int wide, int w, int h, int asc, uint8_t* out) {
    (void)css; (void)text; (void)n; (void)wide; (void)w; (void)h; (void)asc; (void)out; return 0;
}
#endif

static void measure(Font* f, uint32_t text, int n, int wide, int* m) {
    js_text_measure(f->css, GPTR(text), n > 0 ? n : 0, wide, m);
}

/* ---- objects ---- */

static void g_CreateFontIndirectA(CPU* c) {
    uint32_t lf = ARG(0);
    for (int k = 1; k < MAX_FONT; k++) {
        if (fonts[k].used) continue;
        Font* f = &fonts[k];
        memset(f, 0, sizeof *f);
        f->used = 1;
        int32_t h = (int32_t)M32(lf);
        f->size = h < 0 ? -h : h > 0 ? h * 4 / 5 : 12;
        if (f->size < 6) f->size = 6;
        f->bold = (int32_t)M32(lf + 16) >= 600;
        f->italic = M8(lf + 20) != 0;
        snprintf(f->face, sizeof f->face, "%.31s", GSTR(lf + 28));
        snprintf(f->css, sizeof f->css, "%s%s%dpx \"%s\", Tahoma, Arial, sans-serif", f->italic ? "italic " : "",
                 f->bold ? "bold " : "", f->size, f->face[0] ? f->face : "Tahoma");
        RET(FONT_BASE + (uint32_t)k * 4);
        return;
    }
    port_fatal("out of fonts");
}

static void g_CreateCompatibleDC(CPU* c) {
    for (int k = 1; k < MAX_DC; k++) {
        if (dcs[k].used) continue;
        Dc* d = &dcs[k];
        memset(d, 0, sizeof *d);
        d->used = 1;
        d->bk_color = 0xffffff;
        d->bk_mode = 2;
        RET(DC_BASE + (uint32_t)k * 4);
        return;
    }
    port_fatal("out of device contexts");
}
static void g_DeleteDC(CPU* c) { Dc* d = dc_get(ARG(0)); if (d) d->used = 0; RET(1); }

static void g_SelectObject(CPU* c) {
    Dc* d = dc_get(ARG(0));
    uint32_t obj = ARG(1), old;
    GdiBitmap b;
    if (gdi_bitmap(obj, &b)) { old = d ? d->bmp : 0; if (d) d->bmp = obj; RET(old ? old : STOCK_BASE + 21); return; }
    if (font_get(obj) || obj == STOCK_BASE + 12 || obj == STOCK_BASE + 13 || obj == STOCK_BASE + 17) {
        old = d ? d->font : 0;
        if (d) d->font = obj;
        RET(old ? old : STOCK_BASE + 13);
        return;
    }
    old = d ? d->other : 0;
    if (d) d->other = obj;
    RET(old ? old : STOCK_BASE);
}

static void g_DeleteObject(CPU* c) {
    Font* f = font_get(ARG(0));
    if (f) f->used = 0;
    else gdi_delete_bitmap(ARG(0));
    RET(1);
}

static void g_SetTextColor(CPU* c) { Dc* d = dc_get(ARG(0)); uint32_t o = d ? d->text_color : 0; if (d) d->text_color = ARG(1); RET(o); }
static void g_SetBkColor(CPU* c) { Dc* d = dc_get(ARG(0)); uint32_t o = d ? d->bk_color : 0xffffff; if (d) d->bk_color = ARG(1); RET(o); }
static void g_GetBkColor(CPU* c) { Dc* d = dc_get(ARG(0)); RET(d ? d->bk_color : 0xffffff); }
static void g_SetBkMode(CPU* c) { Dc* d = dc_get(ARG(0)); int o = d ? d->bk_mode : 2; if (d) d->bk_mode = (int)ARG(1); RET(o); }
static void g_SetTextAlign(CPU* c) { Dc* d = dc_get(ARG(0)); uint32_t o = d ? d->align : 0; if (d) d->align = ARG(1); RET(o); }

/* ---- metrics ---- */

static void extent(CPU* c, int wide) {
    int m[3];
    measure(dc_font(dc_get(ARG(0))), ARG(1), (int)ARG(2), wide, m);
    M32(ARG(3)) = (uint32_t)m[0];
    M32(ARG(3) + 4) = (uint32_t)(m[1] + m[2]);
    RET(1);
}
static void g_GetTextExtentPoint32A(CPU* c) { extent(c, 0); }
static void g_GetTextExtentPoint32W(CPU* c) { extent(c, 1); }

static void g_GetTextExtentExPointA(CPU* c) {
    Font* f = dc_font(dc_get(ARG(0)));
    int n = (int)ARG(2), max = (int)ARG(3), m[3];
    uint32_t fit = ARG(4), dx = ARG(5), size = ARG(6);
    measure(f, ARG(1), n, 0, m);
    if (n > 0 && (fit || dx)) {
        int* pre = malloc(sizeof(int) * (size_t)n);
        js_text_prefixes(f->css, GPTR(ARG(1)), n, 0, pre);
        int k = 0;
        while (k < n && pre[k] <= max) k++;
        if (fit) M32(fit) = (uint32_t)k;
        if (dx) for (int i = 0; i < n; i++) M32(dx + 4 * (uint32_t)i) = (uint32_t)pre[i];
        free(pre);
    } else if (fit) M32(fit) = 0;
    if (size) { M32(size) = (uint32_t)m[0]; M32(size + 4) = (uint32_t)(m[1] + m[2]); }
    RET(1);
}

static void g_GetTextMetricsA(CPU* c) {
    Font* f = dc_font(dc_get(ARG(0)));
    uint32_t p = ARG(1);
    int m[3], x[3], w[3];
    measure(f, 0, 0, 0, m);
    static const char sample[] = "x", widest[] = "W";
    uint32_t tmp = guest_alloc(4, 4);
    memcpy(GPTR(tmp), sample, 1);
    js_text_measure(f->css, GPTR(tmp), 1, 0, x);
    memcpy(GPTR(tmp), widest, 1);
    js_text_measure(f->css, GPTR(tmp), 1, 0, w);
    guest_free(tmp);
    memset(GPTR(p), 0, 56);
    M32(p) = (uint32_t)(m[1] + m[2]);
    M32(p + 4) = (uint32_t)m[1];
    M32(p + 8) = (uint32_t)m[2];
    M32(p + 20) = (uint32_t)x[0];
    M32(p + 24) = (uint32_t)w[0];
    M32(p + 28) = f->bold ? 700 : 400;
    M32(p + 36) = 96; M32(p + 40) = 96;
    M8(p + 44) = 0x20; M8(p + 45) = 0xff; M8(p + 46) = 0x1f; M8(p + 47) = 0x20;
    M8(p + 48) = (uint8_t)f->italic;
    M8(p + 51) = 0x26;
    RET(1);
}

/* ---- drawing ---- */

static uint8_t* row_ptr(GdiBitmap* b, int y) {
    int r = b->top_down ? y : b->h - 1 - y;
    return (uint8_t*)GPTR(b->bits + (uint32_t)r * b->stride);
}

static void fill(GdiBitmap* b, int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->w) x1 = b->w;
    if (y1 > b->h) y1 = b->h;
    uint8_t r = color & 0xff, g = (color >> 8) & 0xff, bl = (color >> 16) & 0xff;
    int px = (int)b->bpp / 8;
    if (px < 3) return;
    for (int y = y0; y < y1; y++) {
        uint8_t* p = row_ptr(b, y) + x0 * px;
        for (int x = x0; x < x1; x++, p += px) {
            p[0] = bl; p[1] = g; p[2] = r;
            if (px == 4) p[3] = 0xff;
        }
    }
}

static void draw_text(Dc* d, int x, int y, uint32_t text, int n, int wide, uint32_t opts, uint32_t rect) {
    GdiBitmap b;
    if (!d || !d->bmp || !gdi_bitmap(d->bmp, &b) || b.bpp < 24) return;
    Font* f = dc_font(d);
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = b.w, clip_y1 = b.h;
    if (rect && (opts & 2))
        fill(&b, (int)M32(rect), (int)M32(rect + 4), (int)M32(rect + 8), (int)M32(rect + 12), d->bk_color);
    if (rect && (opts & 4)) {
        int rx0 = (int)M32(rect), ry0 = (int)M32(rect + 4), rx1 = (int)M32(rect + 8), ry1 = (int)M32(rect + 12);
        if (rx0 > clip_x0) clip_x0 = rx0;
        if (ry0 > clip_y0) clip_y0 = ry0;
        if (rx1 < clip_x1) clip_x1 = rx1;
        if (ry1 < clip_y1) clip_y1 = ry1;
    }
    if (n <= 0) return;
    int m[3];
    measure(f, text, n, wide, m);
    int w = m[0] + f->size / 2 + 2, h = m[1] + m[2];
    if ((d->align & 6) == 6) x -= m[0] / 2;
    else if (d->align & 2) x -= m[0];
    if ((d->align & 24) == 24) y -= m[1];
    else if (d->align & 8) y -= h;
    if (d->bk_mode == 2 && !(opts & 2)) fill(&b, x, y, x + m[0], y + h, d->bk_color);
    uint8_t* cov = malloc((size_t)w * (size_t)h);
    if (!js_text_render(f->css, GPTR(text), n, wide, w, h, m[1], cov)) { free(cov); return; }
    uint32_t tc = d->text_color;
    int cr = tc & 0xff, cg = (tc >> 8) & 0xff, cb = (tc >> 16) & 0xff, px = (int)b.bpp / 8;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < clip_y0 || yy >= clip_y1) continue;
        uint8_t* row = row_ptr(&b, yy);
        for (int i = 0; i < w; i++) {
            int xx = x + i, a = cov[j * w + i];
            if (!a || xx < clip_x0 || xx >= clip_x1) continue;
            uint8_t* p = row + xx * px;
            p[0] = (uint8_t)((p[0] * (255 - a) + cb * a) / 255);
            p[1] = (uint8_t)((p[1] * (255 - a) + cg * a) / 255);
            p[2] = (uint8_t)((p[2] * (255 - a) + cr * a) / 255);
            if (px == 4 && p[3] < a) p[3] = (uint8_t)a;
        }
    }
    free(cov);
}

static int text_logged;
static void log_text(const char* api, uint32_t hdc, uint32_t text, int n, int wide) {
    if (text_logged++ >= 12 || n <= 0) return;
    char s[80];
    int k = 0;
    for (; k < n && k < 79; k++) s[k] = (char)(wide ? M16(text + 2 * (uint32_t)k) : M8(text + (uint32_t)k));
    s[k] = 0;
    Dc* d = dc_get(hdc);
    port_log("%s dc=%08x bmp=%08x \"%s\" font=%s", api, hdc, d ? d->bmp : 0, s, dc_font(d)->css);
}

static void g_TextOutA(CPU* c) {
    log_text("TextOutA", ARG(0), ARG(3), (int)ARG(4), 0);
    draw_text(dc_get(ARG(0)), (int)ARG(1), (int)ARG(2), ARG(3), (int)ARG(4), 0, 0, 0);
    RET(1);
}
static void g_TextOutW(CPU* c) {
    log_text("TextOutW", ARG(0), ARG(3), (int)ARG(4), 1);
    draw_text(dc_get(ARG(0)), (int)ARG(1), (int)ARG(2), ARG(3), (int)ARG(4), 1, 0, 0);
    RET(1);
}
static void g_ExtTextOutA(CPU* c) {
    log_text("ExtTextOutA", ARG(0), ARG(5), (int)ARG(6), 0);
    draw_text(dc_get(ARG(0)), (int)ARG(1), (int)ARG(2), ARG(5), (int)ARG(6), 0, ARG(3), ARG(4));
    RET(1);
}

static void g_AddFontResourceA(CPU* c) {
    port_log("AddFontResourceA \"%s\"", ARG(0) ? GSTR(ARG(0)) : "");
    RET(1);
}

#define G(n, a) {"gdi32.dll", #n, a, g_##n}
static const Hle text[] = {
    G(CreateFontIndirectA, 1), G(CreateCompatibleDC, 1), G(DeleteDC, 1), G(SelectObject, 2), G(DeleteObject, 1),
    G(SetTextColor, 2), G(SetBkColor, 2), G(GetBkColor, 1), G(SetBkMode, 2), G(SetTextAlign, 2),
    G(GetTextExtentPoint32A, 4), G(GetTextExtentPoint32W, 4), G(GetTextExtentExPointA, 7), G(GetTextMetricsA, 2),
    G(TextOutA, 5), G(TextOutW, 5), G(ExtTextOutA, 8), G(AddFontResourceA, 1),
};

void hle_text_register(void) { hle_register(text, sizeof text / sizeof text[0]); }
