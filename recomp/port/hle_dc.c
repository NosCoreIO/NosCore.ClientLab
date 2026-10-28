/* GDI device contexts and everything drawn through them: fonts and text (glyphs come from the browser's font engine
   via OffscreenCanvas), brushes, pens, palettes, blits with raster operations, user32's painting helpers (system
   colours, edges, frame controls, DrawText), icons and cursors (the active cursor becomes the page's CSS cursor),
   window DCs backed by bitmaps, and enhanced/Windows metafiles. */
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define RET(v) (c->eax = (uint32_t)(v))

#define FONT_BASE 0x05000000u
#define DC_BASE 0x05800000u
#define BRUSH_BASE 0x06000000u
#define PEN_BASE 0x06400000u
#define PAL_BASE 0x06800000u
#define ICON_BASE 0x06c00000u
#define EMF_BASE 0x07000000u
#define STOCK_BASE 0x00100000u
#define MAX_FONT 4096
#define MAX_DC 1024
#define MAX_BRUSH 4096
#define MAX_PEN 1024
#define MAX_PAL 256
#define MAX_ICON 1024
#define MAX_EMF 256

/* ---- objects ---- */

typedef struct {
    int used, size, bold, italic;
    char face[32];
    char css[96];
    uint8_t logfont[60];
} Font;

enum { BS_SOLID = 0, BS_NULL = 1, BS_HATCHED = 2, BS_PATTERN = 3 };
typedef struct { int used, style; uint32_t color, hatch, pattern; } Brush;
typedef struct { int used, style, width; uint32_t color; } Pen;
typedef struct { int used; uint32_t count, entries[256]; } Palette;

enum { DC_MEMORY, DC_WINDOW, DC_META };
typedef struct {
    uint32_t bmp, font, brush, pen, palette;
    uint32_t text_color, bk_color, align;
    int bk_mode, rop2, stretch_mode, map_mode;
    int org_x, org_y, vp_x, vp_y, brush_org_x, brush_org_y, pos_x, pos_y;
    int clip_on, clip[4], nexcl, excl[8][4];
} DcState;
typedef struct {
    int used, kind;
    uint32_t hwnd, meta, meta_file;
    DcState s;
    DcState saved[16];
    int nsaved;
} Dc;

typedef struct {
    int used, is_icon, w, h, hot_x, hot_y;
    uint32_t color, mask;   /* 32bpp colour (alpha used when present), 1bpp AND mask (1 = transparent) */
    int has_alpha;
} Icon;

typedef struct {
    int used;
    uint32_t bmp, desc;   /* recorded picture, description (guest string, two NUL-terminated parts) */
    int frame[4];         /* .01 mm */
} Emf;

static Font fonts[MAX_FONT];
static Dc dcs[MAX_DC];
static Brush brushes[MAX_BRUSH];
static Pen pens[MAX_PEN];
static Palette palettes[MAX_PAL];
static Icon icons[MAX_ICON];
static Emf emfs[MAX_EMF];

static Font default_font = {1, 12, 0, 0, "Tahoma", "12px Tahoma, Arial, sans-serif", {0}};

#define GETTER(name, type, base, max, arr) \
    static type* name(uint32_t h) { \
        uint32_t k = (h - base) / 4; \
        if (h < base || k >= max || !arr[k].used) return 0; \
        return &arr[k]; \
    }
GETTER(font_get, Font, FONT_BASE, MAX_FONT, fonts)
GETTER(dc_get, Dc, DC_BASE, MAX_DC, dcs)
GETTER(brush_get, Brush, BRUSH_BASE, MAX_BRUSH, brushes)
GETTER(pen_get, Pen, PEN_BASE, MAX_PEN, pens)
GETTER(pal_get, Palette, PAL_BASE, MAX_PAL, palettes)
GETTER(icon_get, Icon, ICON_BASE, MAX_ICON, icons)
GETTER(emf_get, Emf, EMF_BASE, MAX_EMF, emfs)

#define ALLOC(name, type, base, max, arr) \
    static uint32_t name(type** out) { \
        for (int k = 1; k < max; k++) \
            if (!arr[k].used) { memset(&arr[k], 0, sizeof arr[k]); arr[k].used = 1; *out = &arr[k]; return base + 4u * (uint32_t)k; } \
        port_fatal("out of GDI objects (" #type ")"); \
        return 0; \
    }
ALLOC(font_new, Font, FONT_BASE, MAX_FONT, fonts)
ALLOC(dc_new_slot, Dc, DC_BASE, MAX_DC, dcs)
ALLOC(brush_new, Brush, BRUSH_BASE, MAX_BRUSH, brushes)
ALLOC(pen_new, Pen, PEN_BASE, MAX_PEN, pens)
ALLOC(pal_new, Palette, PAL_BASE, MAX_PAL, palettes)
ALLOC(icon_new, Icon, ICON_BASE, MAX_ICON, icons)
ALLOC(emf_new, Emf, EMF_BASE, MAX_EMF, emfs)

static Font* dc_font(Dc* d) {
    Font* f = d ? font_get(d->s.font) : 0;
    return f ? f : &default_font;
}

/* ---- system colours (Windows 7 defaults) ---- */

static const uint32_t sys_colors[31] = {
    0xc8c8c8, 0x000000, 0xd1b499, 0xdbcdbf, 0xf0f0f0, 0xffffff, 0x646464, 0x000000, 0x000000, 0x000000, 0xb4b4b4,
    0xfcf7f4, 0xababab, 0xff9933, 0xffffff, 0xf0f0f0, 0xa0a0a0, 0x6d6d6d, 0x000000, 0x544e43, 0xffffff, 0x696969,
    0xe3e3e3, 0x000000, 0xe1ffff, 0xb5b5b5, 0xcc6600, 0xead1b9, 0xf2e4d7, 0xff9933, 0xf0f0f0};
static uint32_t sys_color(uint32_t i) { return i < 31 ? sys_colors[i] : 0; }
static void u_GetSysColor(CPU* c) { RET(sys_color(ARG(0))); }
static uint32_t sys_brushes[31];
static void u_GetSysColorBrush(CPU* c) {
    uint32_t i = ARG(0);
    if (i >= 31) { RET(0); return; }
    if (!sys_brushes[i]) {
        Brush* b;
        sys_brushes[i] = brush_new(&b);
        b->style = BS_SOLID;
        b->color = sys_colors[i];
    }
    RET(sys_brushes[i]);
}

/* Stock objects: brushes 0-5 and 18, pens 6-8 and 19, fonts 10-14/16/17, palette 15. */
static int stock_brush(uint32_t h, int* style, uint32_t* color) {
    static const uint32_t greys[5] = {0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0x000000};
    uint32_t n = h - STOCK_BASE;
    if (h < STOCK_BASE || n > 19) return 0;
    if (n <= 4) { *style = BS_SOLID; *color = greys[n]; return 1; }
    if (n == 5) { *style = BS_NULL; *color = 0; return 1; }
    if (n == 18) { *style = BS_SOLID; *color = 0xffffff; return 1; }
    return 0;
}
static int stock_pen(uint32_t h, int* style, uint32_t* color) {
    uint32_t n = h - STOCK_BASE;
    if (h < STOCK_BASE) return 0;
    if (n == 6 || n == 19) { *style = 0; *color = n == 6 ? 0xffffff : 0; return 1; }
    if (n == 7) { *style = 0; *color = 0; return 1; }
    if (n == 8) { *style = 5; *color = 0; return 1; }
    return 0;
}
static int stock_font(uint32_t h) {
    uint32_t n = h - STOCK_BASE;
    return h >= STOCK_BASE && ((n >= 10 && n <= 14) || n == 16 || n == 17);
}

static void g_GetStockObject(CPU* c) { RET(ARG(0) <= 19 && ARG(0) != 9 ? STOCK_BASE + ARG(0) : 0); }

/* A FillRect brush may be a system colour index + 1. */
static int brush_info(uint32_t h, int* style, uint32_t* color, uint32_t* hatch, uint32_t* pattern) {
    *hatch = *pattern = 0;
    Brush* b = brush_get(h);
    if (b) { *style = b->style; *color = b->color; *hatch = b->hatch; *pattern = b->pattern; return 1; }
    if (stock_brush(h, style, color)) return 1;
    if (h >= 1 && h <= 31) { *style = BS_SOLID; *color = sys_color(h - 1); return 1; }
    return 0;
}

/* ---- browser text engine ---- */

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
/* Registers a font file with the worker's font set under its family name. */
EM_JS(int, js_font_add, (const char* family, const uint8_t* data, int n), {
    if (typeof FontFace === 'undefined' || !self.fonts) return 0;
    try {
        const name = UTF8ToString(family);
        const face = new FontFace(name, HEAPU8.slice(data, data + n).buffer);
        self.fonts.add(face);
        (self.__addedFonts || (self.__addedFonts = {}))[name] = face;
        return face.status === 'error' ? 0 : 1;
    } catch (e) { return 0; }
});
EM_JS(int, js_font_remove, (const char* family), {
    const name = UTF8ToString(family), faces = self.__addedFonts || {};
    if (!faces[name] || !self.fonts) return 0;
    self.fonts.delete(faces[name]);
    delete faces[name];
    return 1;
});
/* The page shows the cursor: an RGBA image with its hot spot, or none to hide it. */
EM_JS(void, js_set_cursor, (const uint8_t* rgba, int w, int h, int hx, int hy, int visible), {
    if (typeof postMessage === 'function') postMessage({ type: 'cursor', visible: !!visible, w, h, hx, hy, rgba: rgba ? HEAPU8.slice(rgba, rgba + w * h * 4) : null });
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
static int js_font_add(const char* family, const uint8_t* data, int n) { (void)family; (void)data; (void)n; return 0; }
static int js_font_remove(const char* family) { (void)family; return 0; }
static void js_set_cursor(const uint8_t* rgba, int w, int h, int hx, int hy, int visible) {
    (void)rgba; (void)w; (void)h; (void)hx; (void)hy; (void)visible;
}
#endif

static void measure(Font* f, uint32_t text, int n, int wide, int* m) {
    js_text_measure(f->css, GPTR(text), n > 0 ? n : 0, wide, m);
}

/* ---- pixels ---- */

static uint8_t* row_ptr(GdiBitmap* b, int y) {
    int r = b->top_down ? y : b->h - 1 - y;
    return (uint8_t*)GPTR(b->bits + (uint32_t)r * b->stride);
}
static uint32_t rgbquad_to_colorref(uint32_t q) { return (q >> 16 & 0xff) | (q & 0xff00) | (q & 0xff) << 16; }

/* Pixels as COLORREF (0x00BBGGRR). */
static uint32_t px_get(GdiBitmap* b, int x, int y) {
    uint8_t* r = row_ptr(b, y);
    switch (b->bpp) {
    case 32: case 24: { uint8_t* p = r + x * (int)(b->bpp / 8); return p[2] | p[1] << 8 | (uint32_t)p[0] << 16; }
    case 16: {
        uint32_t v = r[2 * x] | r[2 * x + 1] << 8;
        return ((v >> 10 & 31) * 255 / 31) | ((v >> 5 & 31) * 255 / 31) << 8 | ((v & 31) * 255 / 31) << 16;
    }
    case 8: return rgbquad_to_colorref(b->colors[r[x]]);
    case 4: return rgbquad_to_colorref(b->colors[(x & 1 ? r[x / 2] : r[x / 2] >> 4) & 15]);
    case 1: return rgbquad_to_colorref(b->colors[r[x / 8] >> (7 - (x & 7)) & 1]);
    default: return 0;
    }
}
static uint32_t nearest_index(GdiBitmap* b, uint32_t color) {
    uint32_t best = 0, best_d = 0xffffffffu;
    for (uint32_t i = 0; i < b->ncolors; i++) {
        uint32_t q = rgbquad_to_colorref(b->colors[i]);
        int dr = (int)(q & 0xff) - (int)(color & 0xff), dg = (int)(q >> 8 & 0xff) - (int)(color >> 8 & 0xff);
        int db = (int)(q >> 16 & 0xff) - (int)(color >> 16 & 0xff);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}
static void px_set(GdiBitmap* b, int x, int y, uint32_t color) {
    uint8_t* r = row_ptr(b, y);
    switch (b->bpp) {
    case 32: case 24: {
        uint8_t* p = r + x * (int)(b->bpp / 8);
        p[0] = (uint8_t)(color >> 16); p[1] = (uint8_t)(color >> 8); p[2] = (uint8_t)color;
        if (b->bpp == 32) p[3] = 0xff;
        break;
    }
    case 16: {
        uint32_t v = ((color & 0xff) >> 3) << 10 | ((color >> 8 & 0xff) >> 3) << 5 | (color >> 16 & 0xff) >> 3;
        r[2 * x] = (uint8_t)v; r[2 * x + 1] = (uint8_t)(v >> 8);
        break;
    }
    case 8: r[x] = (uint8_t)nearest_index(b, color); break;
    case 4: {
        uint8_t i = (uint8_t)nearest_index(b, color);
        r[x / 2] = x & 1 ? (r[x / 2] & 0xf0) | i : (uint8_t)((r[x / 2] & 0x0f) | i << 4);
        break;
    }
    case 1: {
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        if (nearest_index(b, color)) r[x / 8] |= bit; else r[x / 8] &= (uint8_t)~bit;
        break;
    }
    }
}

/* ---- DCs ---- */

/* Each window gets an off-screen surface; the game presents through Direct3D, so nothing composites these. */
typedef struct { uint32_t hwnd, bmp; } WindowSurface;
static WindowSurface surfaces[64];

static uint32_t window_surface(uint32_t hwnd) {
    uint32_t w = 1920, h = 1080;
    if (hwnd) hle_window_client_size(hwnd, &w, &h);
    if (!w) w = 1;
    if (!h) h = 1;
    int free_slot = -1;
    for (int k = 0; k < 64; k++) {
        if (surfaces[k].bmp && surfaces[k].hwnd == hwnd) {
            GdiBitmap b;
            if (gdi_bitmap(surfaces[k].bmp, &b) && (uint32_t)b.w == w && (uint32_t)b.h == h) return surfaces[k].bmp;
            gdi_delete_bitmap(surfaces[k].bmp);
            surfaces[k].bmp = gdi_new_bitmap((int32_t)w, -(int32_t)h, 32);
            return surfaces[k].bmp;
        }
        if (!surfaces[k].bmp && free_slot < 0) free_slot = k;
    }
    if (free_slot < 0) free_slot = 0, gdi_delete_bitmap(surfaces[0].bmp);
    surfaces[free_slot].hwnd = hwnd;
    surfaces[free_slot].bmp = gdi_new_bitmap((int32_t)w, -(int32_t)h, 32);
    return surfaces[free_slot].bmp;
}

static uint32_t dc_create(int kind) {
    Dc* d;
    uint32_t h = dc_new_slot(&d);
    d->kind = kind;
    d->s.bk_color = 0xffffff;
    d->s.bk_mode = 2;          /* OPAQUE */
    d->s.rop2 = 13;            /* R2_COPYPEN */
    d->s.stretch_mode = 1;     /* BLACKONWHITE */
    d->s.map_mode = 1;         /* MM_TEXT */
    d->s.brush = STOCK_BASE + 0;
    d->s.pen = STOCK_BASE + 7;
    d->s.font = STOCK_BASE + 13;
    d->s.palette = STOCK_BASE + 15;
    return h;
}

static int dc_target(Dc* d, GdiBitmap* b) {
    if (!d) return 0;
    if (d->kind == DC_WINDOW) d->s.bmp = window_surface(d->hwnd);
    return d->s.bmp && gdi_bitmap(d->s.bmp, b);
}

static int lx(Dc* d, int x) { return x - d->s.org_x + d->s.vp_x; }
static int ly(Dc* d, int y) { return y - d->s.org_y + d->s.vp_y; }

static int clipped(Dc* d, GdiBitmap* b, int x, int y) {
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return 1;
    if (d->s.clip_on && (x < d->s.clip[0] || y < d->s.clip[1] || x >= d->s.clip[2] || y >= d->s.clip[3])) return 1;
    for (int k = 0; k < d->s.nexcl; k++)
        if (x >= d->s.excl[k][0] && y >= d->s.excl[k][1] && x < d->s.excl[k][2] && y < d->s.excl[k][3]) return 1;
    return 0;
}

/* ---- raster operations ---- */

/* Any ternary ROP from its truth table: bit (P<<2 | S<<1 | D) of the code's middle byte. */
static uint32_t rop3(uint32_t rop, uint32_t p, uint32_t s, uint32_t dst) {
    uint32_t code = rop >> 16 & 0xff, r = 0;
    for (int i = 0; i < 8; i++)
        if (code >> i & 1) r |= (i & 4 ? p : ~p) & (i & 2 ? s : ~s) & (i & 1 ? dst : ~dst);
    return r & 0xffffff;
}
/* R2_* binary raster ops for pens. */
static uint32_t rop2(int mode, uint32_t pen, uint32_t dst) {
    switch (mode) {
    case 1: return 0;
    case 2: return ~(pen | dst) & 0xffffff;
    case 3: return dst & ~pen & 0xffffff;
    case 4: return ~pen & 0xffffff;
    case 5: return pen & ~dst & 0xffffff;
    case 6: return ~dst & 0xffffff;
    case 7: return (pen ^ dst) & 0xffffff;
    case 8: return ~(pen & dst) & 0xffffff;
    case 9: return pen & dst;
    case 10: return ~(pen ^ dst) & 0xffffff;
    case 11: return dst;
    case 12: return (~pen | dst) & 0xffffff;
    case 14: return (pen | ~dst) & 0xffffff;
    case 15: return (pen | dst) & 0xffffff;
    case 16: return 0xffffff;
    default: return pen;
    }
}

static const uint8_t hatches[6][8] = {
    {0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01}, {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},
    {0x08, 0x08, 0x08, 0xff, 0x08, 0x08, 0x08, 0x08}, {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81}};

/* The brush colour at device pixel (x, y); returns 0 for a null brush or a transparent hatch background. */
static int brush_pixel(Dc* d, uint32_t brush, int x, int y, uint32_t* out) {
    int style;
    uint32_t color, hatch, pattern;
    if (!brush_info(brush, &style, &color, &hatch, &pattern) || style == BS_NULL) return 0;
    if (style == BS_HATCHED) {
        int bx = (x - d->s.brush_org_x) & 7, by = (y - d->s.brush_org_y) & 7;
        if (hatches[hatch < 6 ? hatch : 0][by] & (0x80 >> bx)) { *out = color; return 1; }
        if (d->s.bk_mode == 1) return 0;
        *out = d->s.bk_color;
        return 1;
    }
    if (style == BS_PATTERN) {
        GdiBitmap p;
        if (!gdi_bitmap(pattern, &p) || !p.w || !p.h) return 0;
        int px = ((x - d->s.brush_org_x) % p.w + p.w) % p.w, py = ((y - d->s.brush_org_y) % p.h + p.h) % p.h;
        uint32_t v = px_get(&p, px, py);
        if (p.bpp == 1) v = v ? d->s.bk_color : d->s.text_color;
        *out = v;
        return 1;
    }
    *out = color;
    return 1;
}

static void fill_rect_dev(Dc* d, GdiBitmap* b, int x0, int y0, int x1, int y1, uint32_t brush, uint32_t rop) {
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

static void pen_info(uint32_t h, int* style, uint32_t* color, int* width) {
    Pen* p = pen_get(h);
    *width = 1;
    if (p) { *style = p->style; *color = p->color; *width = p->width > 0 ? p->width : 1; return; }
    if (!stock_pen(h, style, color)) { *style = 0; *color = 0; }
}

static void plot_pen(Dc* d, GdiBitmap* b, int x, int y, uint32_t color, int width) {
    int r = width / 2;
    for (int j = -r; j <= (width - 1) - r; j++)
        for (int i = -r; i <= (width - 1) - r; i++)
            if (!clipped(d, b, x + i, y + j)) px_set(b, x + i, y + j, rop2(d->s.rop2, color, px_get(b, x + i, y + j)));
}

/* Bresenham, excluding the end point as LineTo does. */
static void line_dev(Dc* d, GdiBitmap* b, int x0, int y0, int x1, int y1) {
    int style, width;
    uint32_t color;
    pen_info(d->s.pen, &style, &color, &width);
    if (style == 5) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy, step = 0;
    while (x0 != x1 || y0 != y1) {
        int on = style == 0 || (style == 1 && (step / 6) % 2 == 0) || (style == 2 && (step / 2) % 2 == 0) ||
                 (style == 3 && step % 9 < 6) || (style == 4 && step % 12 < 6);
        if (on) plot_pen(d, b, x0, y0, color, width);
        step++;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Copies a source rectangle into a destination one (scaled when sizes differ), applying a ROP, with Windows'
   mono conversion rules: colour -> mono maps the source background colour to 1; mono -> colour maps 0 to the
   destination's text colour and 1 to its background colour. */
static void blit(Dc* dd, GdiBitmap* db, int dx, int dy, int dw, int dh, Dc* sd, GdiBitmap* sb, int sx, int sy, int sw,
                 int sh, uint32_t rop, GdiBitmap* mask, int mx, int my, uint32_t back_rop) {
    if (!dw || !dh || (sb && (!sw || !sh))) return;
    for (int j = 0; j < (dh < 0 ? -dh : dh); j++)
        for (int i = 0; i < (dw < 0 ? -dw : dw); i++) {
            int x = dx + (dw < 0 ? -i - 1 : i), y = dy + (dh < 0 ? -j - 1 : j);
            if (clipped(dd, db, x, y)) continue;
            uint32_t s = 0, p = 0;
            int si = sb ? (int)((int64_t)i * (sw < 0 ? -sw : sw) / (dw < 0 ? -dw : dw)) : 0;
            int sj = sb ? (int)((int64_t)j * (sh < 0 ? -sh : sh) / (dh < 0 ? -dh : dh)) : 0;
            if (sb) {
                int xx = sx + (sw < 0 ? -si - 1 : si), yy = sy + (sh < 0 ? -sj - 1 : sj);
                if (xx < 0 || yy < 0 || xx >= sb->w || yy >= sb->h) continue;
                s = px_get(sb, xx, yy);
                if (sb->bpp == 1 && db->bpp != 1) s = s ? dd->s.bk_color : dd->s.text_color;
                else if (sb->bpp != 1 && db->bpp == 1) s = s == (sd ? sd->s.bk_color : 0xffffff) ? 0xffffff : 0;
            }
            brush_pixel(dd, dd->s.brush, x, y, &p);
            uint32_t use = rop;
            if (mask) {
                int mxx = mx + i, myy = my + j;
                int bit = mxx >= 0 && myy >= 0 && mxx < mask->w && myy < mask->h && px_get(mask, mxx, myy);
                use = bit ? rop : back_rop;
            }
            px_set(db, x, y, rop3(use, p, s, px_get(db, x, y)));
        }
}

/* ---- object creation and selection ---- */

static void font_from_logfont(Font* f, uint32_t lf) {
    memcpy(f->logfont, GPTR(lf), 60);
    int32_t h = (int32_t)M32(lf);
    f->size = h < 0 ? -h : h > 0 ? h * 4 / 5 : 12;
    if (f->size < 6) f->size = 6;
    f->bold = (int32_t)M32(lf + 16) >= 600;
    f->italic = M8(lf + 20) != 0;
    snprintf(f->face, sizeof f->face, "%.31s", GSTR(lf + 28));
    snprintf(f->css, sizeof f->css, "%s%s%dpx \"%s\", Tahoma, Arial, sans-serif", f->italic ? "italic " : "",
             f->bold ? "bold " : "", f->size, f->face[0] ? f->face : "Tahoma");
}
static void g_CreateFontIndirectA(CPU* c) {
    Font* f;
    uint32_t h = font_new(&f);
    font_from_logfont(f, ARG(0));
    RET(h);
}
static void g_CreateSolidBrush(CPU* c) {
    Brush* b;
    uint32_t h = brush_new(&b);
    b->style = BS_SOLID;
    b->color = ARG(0) & 0xffffff;
    RET(h);
}
static void g_CreateBrushIndirect(CPU* c) {
    uint32_t lb = ARG(0);
    Brush* b;
    uint32_t h = brush_new(&b);
    b->style = (int)M32(lb);
    b->color = M32(lb + 4) & 0xffffff;
    b->hatch = M32(lb + 8);
    if (b->style == BS_PATTERN) b->pattern = b->hatch;
    if (b->style > BS_PATTERN) b->style = BS_SOLID;
    RET(h);
}
static void g_CreatePenIndirect(CPU* c) {
    uint32_t lp = ARG(0);
    Pen* p;
    uint32_t h = pen_new(&p);
    p->style = (int)(M32(lp) & 0xf);
    p->width = (int)M32(lp + 4);
    p->color = M32(lp + 12) & 0xffffff;
    RET(h);
}
static void g_CreatePalette(CPU* c) {
    uint32_t lp = ARG(0);
    Palette* p;
    uint32_t h = pal_new(&p);
    p->count = M16(lp + 2) > 256 ? 256 : M16(lp + 2);
    for (uint32_t i = 0; i < p->count; i++) p->entries[i] = M32(lp + 4 + 4 * i);
    RET(h);
}
/* The standard 256-colour halftone palette: a 6x6x6 colour cube plus greys. */
static void g_CreateHalftonePalette(CPU* c) {
    Palette* p;
    uint32_t h = pal_new(&p);
    uint32_t n = 0;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) p->entries[n++] = (uint32_t)(r * 51) | (uint32_t)(g * 51) << 8 | (uint32_t)(b * 51) << 16;
    for (int k = 0; n < 256; k++, n++) { uint32_t v = (uint32_t)(k * 255 / 39); p->entries[n] = v | v << 8 | v << 16; }
    p->count = 256;
    RET(h);
}

static void g_CreateCompatibleDC(CPU* c) { RET(dc_create(DC_MEMORY)); }
static void g_DeleteDC(CPU* c) { Dc* d = dc_get(ARG(0)); if (d) d->used = 0; RET(d != 0); }

static void g_SelectObject(CPU* c) {
    Dc* d = dc_get(ARG(0));
    uint32_t obj = ARG(1), old = 0;
    int style, width;
    uint32_t color, hatch, pattern;
    GdiBitmap b;
    if (!d) { RET(0); return; }
    if (gdi_bitmap(obj, &b)) { old = d->s.bmp ? d->s.bmp : STOCK_BASE + 21; d->s.bmp = obj; }
    else if (font_get(obj) || stock_font(obj)) { old = d->s.font; d->s.font = obj; }
    else if (pen_get(obj) || stock_pen(obj, &style, &color)) { old = d->s.pen; d->s.pen = obj; }
    else if (brush_get(obj) || stock_brush(obj, &style, &color)) { old = d->s.brush; d->s.brush = obj; }
    else if (obj == STOCK_BASE + 21) { old = d->s.bmp; d->s.bmp = 0; }
    (void)width; (void)hatch; (void)pattern;
    RET(old);
}

static void g_DeleteObject(CPU* c) {
    uint32_t h = ARG(0);
    Font* f = font_get(h);
    Brush* b = brush_get(h);
    Pen* p = pen_get(h);
    Palette* pal = pal_get(h);
    if (f) f->used = 0;
    else if (b) b->used = 0;
    else if (p) p->used = 0;
    else if (pal) pal->used = 0;
    else if (!gdi_delete_bitmap(h) && !(h >= STOCK_BASE && h <= STOCK_BASE + 21)) { RET(0); return; }
    RET(1);
}

static void g_GetObjectA(CPU* c) {
    uint32_t h = ARG(0), size = ARG(1), out = ARG(2);
    int n = gdi_bitmap_object(h, size, out);
    if (n >= 0) { RET(n); return; }
    Font* f = font_get(h);
    int style, width;
    uint32_t color, hatch, pattern;
    if (f || stock_font(h)) {
        uint8_t lf[60];
        if (f) memcpy(lf, f->logfont, 60);
        else { memset(lf, 0, 60); lf[0] = (uint8_t)-13; lf[1] = lf[2] = lf[3] = 0xff; memcpy(lf + 28, "Tahoma", 7); }
        if (out) memcpy(GPTR(out), lf, size < 60 ? size : 60);
        RET(60);
        return;
    }
    if (brush_info(h, &style, &color, &hatch, &pattern) && (brush_get(h) || h >= STOCK_BASE)) {
        if (out && size >= 12) { M32(out) = (uint32_t)style; M32(out + 4) = color; M32(out + 8) = style == BS_PATTERN ? pattern : hatch; }
        RET(12);
        return;
    }
    if (pen_get(h) || stock_pen(h, &style, &color)) {
        pen_info(h, &style, &color, &width);
        if (out && size >= 16) { M32(out) = (uint32_t)style; M32(out + 4) = (uint32_t)width; M32(out + 8) = 0; M32(out + 12) = color; }
        RET(16);
        return;
    }
    Palette* pal = pal_get(h);
    if (pal || h == STOCK_BASE + 15) {
        if (out && size >= 2) M16(out) = (uint16_t)(pal ? pal->count : 20);
        RET(2);
        return;
    }
    RET(0);
}

static void g_GetObjectType(CPU* c) {
    uint32_t h = ARG(0);
    int style;
    uint32_t color;
    GdiBitmap b;
    Dc* d = dc_get(h);
    if (pen_get(h) || stock_pen(h, &style, &color)) RET(1);
    else if (brush_get(h) || stock_brush(h, &style, &color)) RET(2);
    else if (d) RET(d->kind == DC_META ? 12 : d->kind == DC_MEMORY ? 10 : 3);
    else if (pal_get(h) || h == STOCK_BASE + 15) RET(5);
    else if (font_get(h) || stock_font(h)) RET(6);
    else if (gdi_bitmap(h, &b)) RET(7);
    else if (emf_get(h)) RET(13);
    else RET(0);
}

static void g_UnrealizeObject(CPU* c) { RET(1); }
static void g_SelectPalette(CPU* c) {
    Dc* d = dc_get(ARG(0));
    uint32_t old = d ? d->s.palette : 0;
    if (d) d->s.palette = ARG(1);
    RET(old);
}
/* True-colour display: realising a palette maps nothing into a system palette. */
static void g_RealizePalette(CPU* c) { RET(0); }
static void g_GetPaletteEntries(CPU* c) {
    Palette* p = pal_get(ARG(0));
    uint32_t start = ARG(1), n = ARG(2), out = ARG(3), count = p ? p->count : 0;
    static const uint32_t defaults[20] = {0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000,
                                          0xc0c0c0, 0xc0dcc0, 0xf0caa6, 0xf0fbff, 0xa4a0a0, 0x808080, 0x0000ff,
                                          0x00ff00, 0x00ffff, 0xff0000, 0xff00ff, 0xffff00, 0xffffff};
    if (!p && ARG(0) == STOCK_BASE + 15) count = 20;
    if (!out) { RET(count); return; }
    uint32_t k = 0;
    for (; k < n && start + k < count; k++) M32(out + 4 * k) = p ? p->entries[start + k] : defaults[start + k];
    RET(k);
}
/* No system palette on a true-colour device. */
static void g_GetSystemPaletteEntries(CPU* c) { RET(0); }

static void g_GetDIBColorTable(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    uint32_t start = ARG(1), n = ARG(2), out = ARG(3), k = 0;
    if (!dc_target(d, &b) || b.bpp > 8) { RET(0); return; }
    for (; k < n && start + k < b.ncolors; k++) M32(out + 4 * k) = b.colors[start + k];
    RET(k);
}
static void g_SetDIBColorTable(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    uint32_t start = ARG(1), n = ARG(2), in = ARG(3), k = 0;
    if (!dc_target(d, &b) || b.bpp > 8) { RET(0); return; }
    for (; k < n && start + k < b.ncolors; k++) b.colors[start + k] = M32(in + 4 * k) & 0xffffff;
    RET(k);
}

/* ---- DC state ---- */

#define DC_SETTER(name, field, cast) \
    static void g_##name(CPU* c) { \
        Dc* d = dc_get(ARG(0)); \
        if (!d) { RET(0); return; } \
        uint32_t old = (uint32_t)d->s.field; \
        d->s.field = (cast)ARG(1); \
        RET(old); \
    }
DC_SETTER(SetTextColor, text_color, uint32_t)
DC_SETTER(SetBkColor, bk_color, uint32_t)
DC_SETTER(SetBkMode, bk_mode, int)
DC_SETTER(SetTextAlign, align, uint32_t)
DC_SETTER(SetROP2, rop2, int)
DC_SETTER(SetStretchBltMode, stretch_mode, int)
DC_SETTER(SetMapMode, map_mode, int)
static void g_GetBkColor(CPU* c) { Dc* d = dc_get(ARG(0)); RET(d ? d->s.bk_color : 0xffffffffu); }

static void put_point(uint32_t p, int x, int y) { if (p) { M32(p) = (uint32_t)x; M32(p + 4) = (uint32_t)y; } }
static void g_SetWindowOrgEx(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    put_point(ARG(3), d->s.org_x, d->s.org_y);
    d->s.org_x = (int)ARG(1); d->s.org_y = (int)ARG(2);
    RET(1);
}
static void g_GetWindowOrgEx(CPU* c) { Dc* d = dc_get(ARG(0)); if (d) put_point(ARG(1), d->s.org_x, d->s.org_y); RET(d != 0); }
static void g_SetViewportOrgEx(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    put_point(ARG(3), d->s.vp_x, d->s.vp_y);
    d->s.vp_x = (int)ARG(1); d->s.vp_y = (int)ARG(2);
    RET(1);
}
static void g_SetBrushOrgEx(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    put_point(ARG(3), d->s.brush_org_x, d->s.brush_org_y);
    d->s.brush_org_x = (int)ARG(1); d->s.brush_org_y = (int)ARG(2);
    RET(1);
}
static void g_GetBrushOrgEx(CPU* c) { Dc* d = dc_get(ARG(0)); if (d) put_point(ARG(1), d->s.brush_org_x, d->s.brush_org_y); RET(d != 0); }
/* Screen position of the DC's origin: the window's position for a window DC. */
static void g_GetDCOrgEx(CPU* c) {
    Dc* d = dc_get(ARG(0));
    int x = 0, y = 0;
    if (d && d->kind == DC_WINDOW && d->hwnd) hle_window_origin(d->hwnd, &x, &y);
    if (d) put_point(ARG(1), x, y);
    RET(d != 0);
}
static void g_MoveToEx(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    put_point(ARG(3), d->s.pos_x, d->s.pos_y);
    d->s.pos_x = (int)ARG(1); d->s.pos_y = (int)ARG(2);
    RET(1);
}
static void g_GetCurrentPositionEx(CPU* c) { Dc* d = dc_get(ARG(0)); if (d) put_point(ARG(1), d->s.pos_x, d->s.pos_y); RET(d != 0); }

static void g_SaveDC(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d || d->nsaved >= 16) { RET(0); return; }
    d->saved[d->nsaved++] = d->s;
    RET(d->nsaved);
}
static void g_RestoreDC(CPU* c) {
    Dc* d = dc_get(ARG(0));
    int32_t n = (int32_t)ARG(1);
    if (!d) { RET(0); return; }
    int level = n < 0 ? d->nsaved + n + 1 : n;
    if (level < 1 || level > d->nsaved) { RET(0); return; }
    d->s = d->saved[level - 1];
    d->nsaved = level - 1;
    RET(1);
}

/* Clip region: a rectangle minus up to eight excluded rectangles (device coordinates). */
static int region_type(Dc* d) {
    if (d->s.clip_on && (d->s.clip[0] >= d->s.clip[2] || d->s.clip[1] >= d->s.clip[3])) return 1;
    return d->s.nexcl ? 3 : 2;
}
static void dc_bounds(Dc* d, int r[4]) {
    GdiBitmap b;
    int w = 0, h = 0;
    if (dc_target(d, &b)) { w = b.w; h = b.h; }
    r[0] = 0; r[1] = 0; r[2] = w; r[3] = h;
    if (d->s.clip_on) {
        if (d->s.clip[0] > r[0]) r[0] = d->s.clip[0];
        if (d->s.clip[1] > r[1]) r[1] = d->s.clip[1];
        if (d->s.clip[2] < r[2]) r[2] = d->s.clip[2];
        if (d->s.clip[3] < r[3]) r[3] = d->s.clip[3];
    }
}
static void g_IntersectClipRect(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    int r[4];
    dc_bounds(d, r);
    int x0 = lx(d, (int)ARG(1)), y0 = ly(d, (int)ARG(2)), x1 = lx(d, (int)ARG(3)), y1 = ly(d, (int)ARG(4));
    d->s.clip[0] = x0 > r[0] ? x0 : r[0];
    d->s.clip[1] = y0 > r[1] ? y0 : r[1];
    d->s.clip[2] = x1 < r[2] ? x1 : r[2];
    d->s.clip[3] = y1 < r[3] ? y1 : r[3];
    d->s.clip_on = 1;
    RET(region_type(d));
}
static void g_ExcludeClipRect(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    if (d->s.nexcl < 8) {
        int* e = d->s.excl[d->s.nexcl++];
        e[0] = lx(d, (int)ARG(1)); e[1] = ly(d, (int)ARG(2)); e[2] = lx(d, (int)ARG(3)); e[3] = ly(d, (int)ARG(4));
    }
    RET(region_type(d));
}
static void g_GetClipBox(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d) { RET(0); return; }
    int r[4];
    dc_bounds(d, r);
    uint32_t out = ARG(1);
    M32(out) = (uint32_t)(r[0] + d->s.org_x - d->s.vp_x); M32(out + 4) = (uint32_t)(r[1] + d->s.org_y - d->s.vp_y);
    M32(out + 8) = (uint32_t)(r[2] + d->s.org_x - d->s.vp_x); M32(out + 12) = (uint32_t)(r[3] + d->s.org_y - d->s.vp_y);
    RET(region_type(d));
}
static void g_RectVisible(CPU* c) {
    Dc* d = dc_get(ARG(0));
    uint32_t rc = ARG(1);
    if (!d) { RET(0); return; }
    int r[4];
    dc_bounds(d, r);
    int x0 = lx(d, (int)M32(rc)), y0 = ly(d, (int)M32(rc + 4)), x1 = lx(d, (int)M32(rc + 8)), y1 = ly(d, (int)M32(rc + 12));
    RET(x0 < r[2] && x1 > r[0] && y0 < r[3] && y1 > r[1]);
}

/* ---- drawing ---- */

static void u_FillRect(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    uint32_t rc = ARG(1);
    if (!dc_target(d, &b)) { RET(0); return; }
    fill_rect_dev(d, &b, lx(d, (int)M32(rc)), ly(d, (int)M32(rc + 4)), lx(d, (int)M32(rc + 8)), ly(d, (int)M32(rc + 12)),
                  ARG(2), 0xf00021);
    RET(1);
}
static void u_FrameRect(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    uint32_t rc = ARG(1), br = ARG(2);
    if (!dc_target(d, &b)) { RET(0); return; }
    int x0 = lx(d, (int)M32(rc)), y0 = ly(d, (int)M32(rc + 4)), x1 = lx(d, (int)M32(rc + 8)), y1 = ly(d, (int)M32(rc + 12));
    if (x1 <= x0 || y1 <= y0) { RET(1); return; }
    fill_rect_dev(d, &b, x0, y0, x1, y0 + 1, br, 0xf00021);
    fill_rect_dev(d, &b, x0, y1 - 1, x1, y1, br, 0xf00021);
    fill_rect_dev(d, &b, x0, y0, x0 + 1, y1, br, 0xf00021);
    fill_rect_dev(d, &b, x1 - 1, y0, x1, y1, br, 0xf00021);
    RET(1);
}
static void g_Rectangle(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    if (!dc_target(d, &b)) { RET(0); return; }
    int x0 = lx(d, (int)ARG(1)), y0 = ly(d, (int)ARG(2)), x1 = lx(d, (int)ARG(3)), y1 = ly(d, (int)ARG(4));
    fill_rect_dev(d, &b, x0 + 1, y0 + 1, x1 - 1, y1 - 1, d->s.brush, 0xf00021);
    line_dev(d, &b, x0, y0, x1 - 1, y0);
    line_dev(d, &b, x1 - 1, y0, x1 - 1, y1 - 1);
    line_dev(d, &b, x1 - 1, y1 - 1, x0, y1 - 1);
    line_dev(d, &b, x0, y1 - 1, x0, y0);
    RET(1);
}
static void g_PatBlt(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    if (!dc_target(d, &b)) { RET(0); return; }
    int x = lx(d, (int)ARG(1)), y = ly(d, (int)ARG(2));
    fill_rect_dev(d, &b, x, y, x + (int)ARG(3), y + (int)ARG(4), d->s.brush, ARG(5));
    RET(1);
}
static void g_LineTo(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    if (!dc_target(d, &b)) { RET(0); return; }
    int x = (int)ARG(1), y = (int)ARG(2);
    line_dev(d, &b, lx(d, d->s.pos_x), ly(d, d->s.pos_y), lx(d, x), ly(d, y));
    d->s.pos_x = x; d->s.pos_y = y;
    RET(1);
}
static void g_SetPixel(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    int x = 0, y = 0;
    if (!dc_target(d, &b) || clipped(d, &b, x = lx(d, (int)ARG(1)), y = ly(d, (int)ARG(2)))) { RET(0xffffffffu); return; }
    px_set(&b, x, y, ARG(3) & 0xffffff);
    RET(px_get(&b, x, y));
}
static void g_GetPixel(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    int x = 0, y = 0;
    if (!dc_target(d, &b) || clipped(d, &b, x = lx(d, (int)ARG(1)), y = ly(d, (int)ARG(2)))) { RET(0xffffffffu); return; }
    RET(px_get(&b, x, y));
}

static int rop_uses_source(uint32_t rop) {
    uint32_t code = rop >> 16 & 0xff;
    return ((code >> 2) & 0x33) != (code & 0x33);
}
static void g_BitBlt(CPU* c) {
    Dc *dd = dc_get(ARG(0)), *sd = dc_get(ARG(5));
    GdiBitmap db, sb;
    uint32_t rop = ARG(8);
    if (!dc_target(dd, &db)) { RET(0); return; }
    int src = rop_uses_source(rop) && dc_target(sd, &sb);
    if (rop_uses_source(rop) && !src) { RET(0); return; }
    blit(dd, &db, lx(dd, (int)ARG(1)), ly(dd, (int)ARG(2)), (int)ARG(3), (int)ARG(4), sd, src ? &sb : 0,
         src ? lx(sd, (int)ARG(6)) : 0, src ? ly(sd, (int)ARG(7)) : 0, (int)ARG(3), (int)ARG(4), rop, 0, 0, 0, 0);
    RET(1);
}
static void g_StretchBlt(CPU* c) {
    Dc *dd = dc_get(ARG(0)), *sd = dc_get(ARG(5));
    GdiBitmap db, sb;
    uint32_t rop = ARG(10);
    if (!dc_target(dd, &db)) { RET(0); return; }
    int src = rop_uses_source(rop) && dc_target(sd, &sb);
    if (rop_uses_source(rop) && !src) { RET(0); return; }
    blit(dd, &db, lx(dd, (int)ARG(1)), ly(dd, (int)ARG(2)), (int)ARG(3), (int)ARG(4), sd, src ? &sb : 0,
         src ? lx(sd, (int)ARG(6)) : 0, src ? ly(sd, (int)ARG(7)) : 0, (int)ARG(8), (int)ARG(9), rop, 0, 0, 0, 0);
    RET(1);
}
/* MaskBlt's rop4 is MAKEROP4(fore, back): the foreground ROP applies where the mask bit is 1, the background one
   (in the top byte) where it is 0. */
static void g_MaskBlt(CPU* c) {
    Dc *dd = dc_get(ARG(0)), *sd = dc_get(ARG(5));
    GdiBitmap db, sb, mb;
    uint32_t rop4 = ARG(11), fore = rop4 & 0xff0000, back = (rop4 >> 8) & 0xff0000;
    if (!dc_target(dd, &db)) { RET(0); return; }
    int has_src = dc_target(sd, &sb), has_mask = ARG(8) && gdi_bitmap(ARG(8), &mb) && mb.bpp == 1;
    blit(dd, &db, lx(dd, (int)ARG(1)), ly(dd, (int)ARG(2)), (int)ARG(3), (int)ARG(4), sd, has_src ? &sb : 0,
         has_src ? lx(sd, (int)ARG(6)) : 0, has_src ? ly(sd, (int)ARG(7)) : 0, (int)ARG(3), (int)ARG(4), fore,
         has_mask ? &mb : 0, (int)ARG(9), (int)ARG(10), back);
    RET(1);
}

/* 3D edges with the system colours, as DrawEdge draws them (outer ring, then inner ring). */
static void edge_side(Dc* d, GdiBitmap* b, int x0, int y0, int x1, int y1, uint32_t color) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (!clipped(d, b, x, y)) px_set(b, x, y, color);
}
static void draw_ring(Dc* d, GdiBitmap* b, int r[4], uint32_t flags, uint32_t tl, uint32_t br) {
    if (flags & 2) edge_side(d, b, r[0], r[1], r[2], r[1] + 1, tl);
    if (flags & 1) edge_side(d, b, r[0], r[1], r[0] + 1, r[3], tl);
    if (flags & 8) edge_side(d, b, r[0], r[3] - 1, r[2], r[3], br);
    if (flags & 4) edge_side(d, b, r[2] - 1, r[1], r[2], r[3], br);
    if (flags & 1) r[0]++;
    if (flags & 2) r[1]++;
    if (flags & 4) r[2]--;
    if (flags & 8) r[3]--;
}
static void draw_edge(Dc* d, GdiBitmap* b, int r[4], uint32_t edge, uint32_t flags) {
    uint32_t outer = edge & 3, inner = edge & 12;
    if (flags & 0x10) {   /* BF_DIAGONAL: one line from corner to corner in the outer colour */
        uint32_t color = outer == 1 ? sys_color(21) : sys_color(16);
        int x0 = r[0], y0 = flags & 2 ? r[1] : r[3] - 1, x1 = r[2] - 1, y1 = flags & 2 ? r[3] - 1 : r[1];
        int n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
        for (int k = 0; k <= n; k++) {
            int x = x0 + (n ? (x1 - x0) * k / n : 0), y = y0 + (n ? (y1 - y0) * k / n : 0);
            if (!clipped(d, b, x, y)) px_set(b, x, y, color);
        }
        return;
    }
    uint32_t mono = flags & 0x8000, flat = flags & 0x4000;
    uint32_t otl = outer == 1 ? sys_color(22) : sys_color(16), obr = outer == 1 ? sys_color(21) : sys_color(20);
    uint32_t itl = inner == 4 ? sys_color(20) : sys_color(21), ibr = inner == 4 ? sys_color(16) : sys_color(22);
    if (mono) otl = obr = itl = ibr = sys_color(6);
    else if (flat) { otl = obr = sys_color(16); itl = ibr = sys_color(15); }
    if (outer) draw_ring(d, b, r, flags, otl, obr);
    if (inner) draw_ring(d, b, r, flags, itl, ibr);
    if (flags & 0x800) edge_side(d, b, r[0], r[1], r[2], r[3], mono ? sys_color(5) : sys_color(15));
}
static void u_DrawEdge(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    uint32_t rc = ARG(1);
    if (!dc_target(d, &b)) { RET(0); return; }
    int r[4] = {lx(d, (int)M32(rc)), ly(d, (int)M32(rc + 4)), lx(d, (int)M32(rc + 8)), ly(d, (int)M32(rc + 12))};
    draw_edge(d, &b, r, ARG(2), ARG(3));
    if (ARG(3) & 0x2000) {   /* BF_ADJUST */
        M32(rc) = (uint32_t)(r[0] + d->s.org_x - d->s.vp_x); M32(rc + 4) = (uint32_t)(r[1] + d->s.org_y - d->s.vp_y);
        M32(rc + 8) = (uint32_t)(r[2] + d->s.org_x - d->s.vp_x); M32(rc + 12) = (uint32_t)(r[3] + d->s.org_y - d->s.vp_y);
    }
    RET(1);
}

/* Glyphs for frame controls: a check mark, arrows and the caption buttons, as 7x7 bitmaps. */
static const uint8_t glyph_check[7] = {0x02, 0x06, 0x8e, 0xdc, 0xf8, 0x70, 0x20};
static const uint8_t glyph_up[7] = {0x00, 0x10, 0x38, 0x7c, 0xfe, 0x00, 0x00};
static const uint8_t glyph_close[7] = {0xc6, 0x6c, 0x38, 0x38, 0x6c, 0xc6, 0x00};
static const uint8_t glyph_min[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x7c};
static const uint8_t glyph_max[7] = {0xfe, 0xfe, 0x82, 0x82, 0x82, 0xfe, 0x00};
static const uint8_t glyph_bullet[7] = {0x00, 0x38, 0x7c, 0x7c, 0x7c, 0x38, 0x00};

static void draw_glyph(Dc* d, GdiBitmap* b, const uint8_t* g, int r[4], int rotate, uint32_t color) {
    int cx = (r[0] + r[2]) / 2 - 3, cy = (r[1] + r[3]) / 2 - 3;
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++) {
            int sx = x, sy = y;
            if (rotate == 1) sy = 6 - y;                  /* down */
            if (rotate == 2) { sx = y; sy = x; }           /* left */
            if (rotate == 3) { sx = y; sy = 6 - x; }       /* right */
            if (g[sy] & (0x80 >> sx) && !clipped(d, b, cx + x, cy + y)) px_set(b, cx + x, cy + y, color);
        }
}
static void u_DrawFrameControl(CPU* c) {
    Dc* d = dc_get(ARG(0));
    GdiBitmap b;
    uint32_t rc = ARG(1), type = ARG(2), state = ARG(3);
    if (!dc_target(d, &b)) { RET(0); return; }
    int r[4] = {lx(d, (int)M32(rc)), ly(d, (int)M32(rc + 4)), lx(d, (int)M32(rc + 8)), ly(d, (int)M32(rc + 12))};
    uint32_t kind = state & 0xff, pushed = state & 0x200, checked = state & 0x400, inactive = state & 0x100;
    uint32_t ink = inactive ? sys_color(17) : sys_color(18);
    int flat = (state & 0x4000) != 0;
    if (type == 4 && (kind & 0x10)) {                       /* push button */
        draw_edge(d, &b, r, pushed || checked ? 0x0a : 0x05, 0x0f | 0x800 | (flat ? 0x4000 : 0));
    } else if (type == 4 && (kind & 4)) {                   /* radio button */
        for (int y = r[1]; y < r[3]; y++)
            for (int x = r[0]; x < r[2]; x++) {
                int cx2 = (r[0] + r[2]) - 1, cy2 = (r[1] + r[3]) - 1, rad = (r[2] - r[0]) / 2;
                int dx2 = 2 * x - cx2, dy2 = 2 * y - cy2, dist = dx2 * dx2 + dy2 * dy2;
                if (dist <= rad * rad * 4 && !clipped(d, &b, x, y))
                    px_set(&b, x, y, dist >= (rad - 1) * (rad - 1) * 4 ? sys_color(16) : sys_color(5));
            }
        if (checked) draw_glyph(d, &b, glyph_bullet, r, 0, ink);
    } else if (type == 4) {                                 /* check box (2-state or 3-state) */
        draw_edge(d, &b, r, 0x0a, 0x0f | (flat ? 0x4000 : 0));
        edge_side(d, &b, r[0], r[1], r[2], r[3], (kind & 8) && checked ? sys_color(15) : sys_color(5));
        if (checked) draw_glyph(d, &b, glyph_check, r, 0, ink);
    } else if (type == 3) {                                 /* scroll bar parts */
        draw_edge(d, &b, r, pushed ? 0x0a : 0x05, 0x0f | 0x800 | (flat ? 0x4000 : 0));
        if (kind <= 3) draw_glyph(d, &b, glyph_up, r, (int)kind, ink);
        else if (kind == 5) draw_glyph(d, &b, glyph_up, r, 1, ink);
    } else if (type == 1) {                                 /* caption buttons */
        draw_edge(d, &b, r, pushed ? 0x0a : 0x05, 0x0f | 0x800);
        const uint8_t* g = kind == 0 ? glyph_close : kind == 1 ? glyph_min : glyph_max;
        draw_glyph(d, &b, g, r, 0, ink);
    } else if (type == 2 || type == 5) {                    /* menu glyphs */
        const uint8_t* g = (kind & 0xff) == 1 ? glyph_check : (kind & 0xff) == 2 ? glyph_bullet : glyph_up;
        draw_glyph(d, &b, g, r, (kind & 0xff) == 0 ? 3 : 0, ink);
    } else { RET(0); return; }
    RET(1);
}

/* ---- text ---- */

static void draw_text(Dc* d, int x, int y, uint32_t text, int n, int wide, uint32_t opts, int* rect) {
    GdiBitmap b;
    if (!dc_target(d, &b)) return;
    Font* f = dc_font(d);
    if (rect && (opts & 2)) {
        for (int yy = rect[1]; yy < rect[3]; yy++)
            for (int xx = rect[0]; xx < rect[2]; xx++)
                if (!clipped(d, &b, xx, yy)) px_set(&b, xx, yy, d->s.bk_color);
    }
    if (n <= 0) return;
    int m[3];
    measure(f, text, n, wide, m);
    int w = m[0] + f->size / 2 + 2, h = m[1] + m[2];
    if ((d->s.align & 6) == 6) x -= m[0] / 2;
    else if (d->s.align & 2) x -= m[0];
    if ((d->s.align & 24) == 24) y -= m[1];
    else if (d->s.align & 8) y -= h;
    if (d->s.bk_mode == 2 && !(opts & 2))
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + m[0]; xx++)
                if (!clipped(d, &b, xx, yy) && !(rect && (opts & 4) && (xx < rect[0] || yy < rect[1] || xx >= rect[2] || yy >= rect[3])))
                    px_set(&b, xx, yy, d->s.bk_color);
    uint8_t* cov = malloc((size_t)w * (size_t)h);
    if (!js_text_render(f->css, GPTR(text), n, wide, w, h, m[1], cov)) { free(cov); return; }
    uint32_t tc = d->s.text_color;
    int cr = tc & 0xff, cg = (tc >> 8) & 0xff, cb = (tc >> 16) & 0xff;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int xx = x + i, yy = y + j, a = cov[j * w + i];
            if (!a || clipped(d, &b, xx, yy)) continue;
            if (rect && (opts & 4) && (xx < rect[0] || yy < rect[1] || xx >= rect[2] || yy >= rect[3])) continue;
            if (b.bpp >= 24) {
                uint8_t* p = row_ptr(&b, yy) + xx * (int)(b.bpp / 8);
                p[0] = (uint8_t)((p[0] * (255 - a) + cb * a) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + cg * a) / 255);
                p[2] = (uint8_t)((p[2] * (255 - a) + cr * a) / 255);
                if (b.bpp == 32 && p[3] < a) p[3] = (uint8_t)a;
            } else if (a >= 128) px_set(&b, xx, yy, tc);
        }
    free(cov);
}

static void g_TextOutA(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (d) draw_text(d, lx(d, (int)ARG(1)), ly(d, (int)ARG(2)), ARG(3), (int)ARG(4), 0, 0, 0);
    RET(d != 0);
}
static void g_TextOutW(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (d) draw_text(d, lx(d, (int)ARG(1)), ly(d, (int)ARG(2)), ARG(3), (int)ARG(4), 1, 0, 0);
    RET(d != 0);
}
static void g_ExtTextOutA(CPU* c) {
    Dc* d = dc_get(ARG(0));
    uint32_t rc = ARG(4);
    if (!d) { RET(0); return; }
    int r[4];
    if (rc) { r[0] = lx(d, (int)M32(rc)); r[1] = ly(d, (int)M32(rc + 4)); r[2] = lx(d, (int)M32(rc + 8)); r[3] = ly(d, (int)M32(rc + 12)); }
    draw_text(d, lx(d, (int)ARG(1)), ly(d, (int)ARG(2)), ARG(5), (int)ARG(6), 0, ARG(3), rc ? r : 0);
    RET(1);
}

/* DrawTextA: lines (DT_SINGLELINE or broken at CR/LF and, with DT_WORDBREAK, at spaces), '&' prefixes,
   alignment, end ellipsis, DT_CALCRECT. Returns the text height. */
static void u_DrawTextA(CPU* c) {
    Dc* d = dc_get(ARG(0));
    uint32_t text = ARG(1), rc = ARG(3), fmt = ARG(4);
    int32_t n = (int32_t)ARG(2);
    if (!d || !text) { RET(0); return; }
    if (n < 0) n = (int32_t)strlen(GSTR(text));
    Font* f = dc_font(d);
    char* s = malloc((size_t)n * 4 + 4);   /* room for DT_EXPANDTABS */
    int len = 0, prefix = !(fmt & 0x800);
    for (int k = 0; k < n; k++) {
        char ch = (char)M8(text + (uint32_t)k);
        if (prefix && ch == '&') { if (k + 1 < n && M8(text + (uint32_t)k + 1) == '&') { s[len++] = '&'; k++; } continue; }
        if (ch == '\t' && (fmt & 0x40)) { for (int t = 0; t < 4; t++) s[len++] = ' '; continue; }
        s[len++] = ch;
    }
    uint32_t buf = guest_alloc((uint32_t)len + 4, 4);
    int left = (int)M32(rc), top = (int)M32(rc + 4), right = (int)M32(rc + 8), bottom = (int)M32(rc + 12);
    int width = right - left, m[3], line_h, max_w = 0, lines = 0;
    measure(f, 0, 0, 0, m);
    line_h = m[1] + m[2];
    /* first pass: break into lines, recording [start, end) for each */
    int starts[256], ends[256];
    for (int k = 0; k < len && lines < 256;) {
        int e = k, last_space = -1;
        while (e < len && s[e] != '\r' && s[e] != '\n') {
            if (!(fmt & 0x20) && (fmt & 0x10)) {
                if (s[e] == ' ') last_space = e;
                memcpy(GPTR(buf), s + k, (size_t)(e - k + 1));
                int mm[3];
                js_text_measure(f->css, GPTR(buf), e - k + 1, 0, mm);
                if (mm[0] > width && e > k) { if (last_space > k) e = last_space; break; }
            }
            e++;
        }
        if (fmt & 0x20) { while (e < len) e++; }
        starts[lines] = k; ends[lines] = e; lines++;
        k = e;
        if (k < len && s[k] == '\r') k++;
        if (k < len && s[k] == '\n') k++;
        else if (k < len && s[k] == ' ') k++;
    }
    if (!len) { starts[0] = ends[0] = 0; lines = 1; }
    for (int l = 0; l < lines; l++) {
        int mm[3] = {0, 0, 0};
        memcpy(GPTR(buf), s + starts[l], (size_t)(ends[l] - starts[l]));
        js_text_measure(f->css, GPTR(buf), ends[l] - starts[l], 0, mm);
        if (mm[0] > max_w) max_w = mm[0];
    }
    int total = lines * line_h;
    if (fmt & 0x400) {   /* DT_CALCRECT */
        M32(rc + 8) = (uint32_t)(left + max_w);
        M32(rc + 12) = (uint32_t)(top + total);
        guest_free(buf);
        free(s);
        RET(total);
        return;
    }
    int y = top;
    if ((fmt & 0x20) && (fmt & 4)) y = top + (bottom - top - line_h) / 2;
    else if ((fmt & 0x20) && (fmt & 8)) y = bottom - line_h;
    int clip[4] = {lx(d, left), ly(d, top), lx(d, right), ly(d, bottom)};
    uint32_t saved_align = d->s.align;
    d->s.align = 0;
    for (int l = 0; l < lines; l++, y += line_h) {
        int cnt = ends[l] - starts[l];
        memcpy(GPTR(buf), s + starts[l], (size_t)cnt);
        int mm[3];
        js_text_measure(f->css, GPTR(buf), cnt, 0, mm);
        if ((fmt & 0x8000) && mm[0] > width) {   /* DT_END_ELLIPSIS */
            while (cnt > 0) {
                memcpy(GPTR(buf + (uint32_t)cnt), "...", 3);
                js_text_measure(f->css, GPTR(buf), cnt + 3, 0, mm);
                if (mm[0] <= width) break;
                cnt--;
            }
            memcpy(GPTR(buf + (uint32_t)cnt), "...", 3);
            cnt += 3;
        }
        int x = left;
        if (fmt & 1) x = left + (width - mm[0]) / 2;
        else if (fmt & 2) x = right - mm[0];
        draw_text(d, lx(d, x), ly(d, y), buf, cnt, 0, fmt & 0x100 ? 0 : 4, fmt & 0x100 ? 0 : clip);
    }
    d->s.align = saved_align;
    guest_free(buf);
    free(s);
    RET(total);
}

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
    uint32_t tmp = guest_alloc(4, 4);
    M8(tmp) = 'x';
    js_text_measure(f->css, GPTR(tmp), 1, 0, x);
    M8(tmp) = 'W';
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

/* ---- font files ---- */

static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The family name (name ID 1) from a TrueType/OpenType file's 'name' table. */
static int font_family(const uint8_t* f, size_t size, char* out, size_t n) {
    if (size < 12) return 0;
    uint32_t tables = be16(f + 4);
    for (uint32_t t = 0; t < tables && 12 + 16 * (t + 1) <= size; t++) {
        const uint8_t* rec = f + 12 + 16 * t;
        if (memcmp(rec, "name", 4)) continue;
        uint32_t off = be32(rec + 8);
        if (off + 6 > size) return 0;
        const uint8_t* name = f + off;
        uint32_t count = be16(name + 2), strings = off + be16(name + 4);
        for (uint32_t k = 0; k < count && off + 6 + 12 * (k + 1) <= size; k++) {
            const uint8_t* r = name + 6 + 12 * k;
            uint32_t platform = be16(r), id = be16(r + 6), len = be16(r + 8), at = strings + be16(r + 10);
            if (id != 1 || at + len > size) continue;
            size_t o = 0;
            if (platform == 3 || platform == 0) for (uint32_t i = 0; i + 1 < len && o + 1 < n; i += 2) out[o++] = (char)f[at + i + 1];
            else for (uint32_t i = 0; i < len && o + 1 < n; i++) out[o++] = (char)f[at + i];
            out[o] = 0;
            return o > 0;
        }
    }
    return 0;
}

static int load_font_file(uint32_t guest_path, char* family, size_t n, uint8_t** data, long* size) {
    char path[600];
    port_host_path(GSTR(guest_path), path, sizeof path);
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *data = malloc((size_t)*size);
    fread(*data, 1, (size_t)*size, fp);
    fclose(fp);
    return font_family(*data, (size_t)*size, family, n);
}
static void g_AddFontResourceA(CPU* c) {
    char family[128];
    uint8_t* data = 0;
    long size = 0;
    if (!ARG(0) || !load_font_file(ARG(0), family, sizeof family, &data, &size)) { free(data); RET(0); return; }
    int ok = js_font_add(family, data, (int)size);
    port_log("AddFontResourceA %s: family \"%s\"%s", GSTR(ARG(0)), family, ok ? "" : " (browser font engine unavailable)");
    free(data);
    RET(1);
}
static void g_RemoveFontResourceA(CPU* c) {
    char family[128];
    uint8_t* data = 0;
    long size = 0;
    int ok = ARG(0) && load_font_file(ARG(0), family, sizeof family, &data, &size) && js_font_remove(family);
    free(data);
    RET(ok);
}

/* ---- window DCs ---- */

static uint32_t window_dc(uint32_t hwnd) {
    uint32_t h = dc_create(DC_WINDOW);
    dc_get(h)->hwnd = hwnd;
    return h;
}
static void u_GetDC(CPU* c) { RET(window_dc(ARG(0))); }
static void u_GetWindowDC(CPU* c) { RET(window_dc(ARG(0))); }
/* DCX_* flags select clipping behaviour we do not need: every window DC draws to the window's whole surface. */
static void u_GetDCEx(CPU* c) { RET(window_dc(ARG(0))); }
static void u_ReleaseDC(CPU* c) {
    Dc* d = dc_get(ARG(1));
    if (d && d->kind == DC_WINDOW) { d->used = 0; RET(1); return; }
    RET(0);
}
static void u_BeginPaint(CPU* c) {
    uint32_t hwnd = ARG(0), ps = ARG(1), w, h;
    uint32_t dc = window_dc(hwnd);
    hle_window_client_size(hwnd, &w, &h);
    memset(GPTR(ps), 0, 64);
    M32(ps) = dc;
    M32(ps + 4) = 0;   /* fErase: the background was erased */
    M32(ps + 8) = 0; M32(ps + 12) = 0; M32(ps + 16) = w; M32(ps + 20) = h;
    RET(dc);
}
static void u_EndPaint(CPU* c) {
    Dc* d = dc_get(M32(ARG(1)));
    if (d && d->kind == DC_WINDOW) d->used = 0;
    RET(1);
}

/* ---- icons and cursors ---- */

static int cursor_visible = 1;
static uint32_t current_cursor;

/* Builds an icon from a DIB as stored in icon/cursor resources and files: the header's height covers the colour
   image and the AND mask below it. */
static uint32_t icon_from_dib(const uint8_t* dib, uint32_t size, int is_icon, int hot_x, int hot_y) {
    if (size >= 8 && !memcmp(dib, "\x89PNG", 4)) return 0;   /* PNG-compressed entries come only in large icons */
    if (size < 40) return 0;
    int32_t w = (int32_t)(dib[4] | dib[5] << 8 | dib[6] << 16 | (uint32_t)dib[7] << 24);
    int32_t h2 = (int32_t)(dib[8] | dib[9] << 8 | dib[10] << 16 | (uint32_t)dib[11] << 24);
    uint32_t bpp = dib[14] | dib[15] << 8, hdr = dib[0] | dib[1] << 8;
    uint32_t used = dib[32] | dib[33] << 8 | dib[34] << 16 | (uint32_t)dib[35] << 24;
    int32_t h = h2 / 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return 0;
    uint32_t ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    const uint8_t* pal = dib + hdr;
    const uint8_t* xor_bits = pal + 4 * ncol;
    uint32_t xor_stride = ((uint32_t)w * bpp + 31) / 32 * 4, and_stride = ((uint32_t)w + 31) / 32 * 4;
    const uint8_t* and_bits = xor_bits + xor_stride * (uint32_t)h;
    if ((size_t)(and_bits - dib) + and_stride * (uint32_t)h > size) return 0;
    Icon* ic;
    uint32_t handle = icon_new(&ic);
    ic->is_icon = is_icon; ic->w = w; ic->h = h; ic->hot_x = hot_x; ic->hot_y = hot_y;
    ic->color = gdi_new_bitmap(w, -h, 32);
    ic->mask = gdi_new_bitmap(w, -h, 1);
    GdiBitmap cb, mb;
    gdi_bitmap(ic->color, &cb);
    gdi_bitmap(ic->mask, &mb);
    for (int y = 0; y < h; y++) {
        const uint8_t* xr = xor_bits + xor_stride * (uint32_t)(h - 1 - y);
        const uint8_t* ar = and_bits + and_stride * (uint32_t)(h - 1 - y);
        uint8_t* out = row_ptr(&cb, y);
        for (int x = 0; x < w; x++) {
            uint32_t bgra;
            if (bpp == 32) { memcpy(&bgra, xr + 4 * x, 4); if (bgra >> 24) ic->has_alpha = 1; }
            else if (bpp == 24) bgra = xr[3 * x] | xr[3 * x + 1] << 8 | (uint32_t)xr[3 * x + 2] << 16;
            else {
                uint32_t idx = bpp == 8 ? xr[x] : bpp == 4 ? (x & 1 ? xr[x / 2] & 15 : xr[x / 2] >> 4) : (xr[x / 8] >> (7 - (x & 7))) & 1;
                memcpy(&bgra, pal + 4 * (idx < ncol ? idx : 0), 4);
                bgra &= 0xffffff;
            }
            memcpy(out + 4 * x, &bgra, 4);
            int transparent = (ar[x / 8] >> (7 - (x & 7))) & 1;
            px_set(&mb, x, y, transparent ? 0xffffff : 0);
        }
    }
    if (!ic->has_alpha)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) row_ptr(&cb, y)[4 * x + 3] = px_get(&mb, x, y) ? 0 : 0xff;
    return handle;
}

/* A resource icon or cursor: the group directory picks the entry closest to 32x32. */
static uint32_t load_resource_icon(uint32_t name, int is_icon) {
    const char* str = name >= 0x10000 ? GSTR(name) : 0;
    uint32_t id = name < 0x10000 ? name : 0, size = 0;
    uint32_t dir = res_find(is_icon ? 14 : 12, 0, id, str, &size);
    if (!dir || size < 6) return 0;
    uint32_t count = M16(dir + 4), best = 0, best_score = 0xffffffffu;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t e = dir + 6 + 14 * k;
        uint32_t w = is_icon ? (M8(e) ? M8(e) : 256) : M16(e), bits = is_icon ? M16(e + 6) : M16(e + 6);
        uint32_t score = (w > 32 ? w - 32 : 32 - w) * 64 + (32 - (bits > 32 ? 32 : bits));
        if (score < best_score) { best_score = score; best = M16(e + 12); }
    }
    uint32_t data = res_find(is_icon ? 3 : 1, 0, best, 0, &size);
    if (!data) return 0;
    if (is_icon) return icon_from_dib((const uint8_t*)GPTR(data), size, 1, 16, 16);
    return icon_from_dib((const uint8_t*)GPTR(data + 4), size - 4, 0, M16(data), M16(data + 2));
}

/* Built-in icons and cursors (IDI_*, IDC_*) as simple shapes; cursors are shown with the browser's own. */
static uint32_t system_icon(uint32_t id, int is_icon) {
    static uint32_t cache[2][32];
    uint32_t slot = id >= 32512 ? (id - 32512) & 31 : 0;
    if (cache[is_icon][slot]) return cache[is_icon][slot];
    Icon* ic;
    uint32_t h = icon_new(&ic);
    ic->is_icon = is_icon; ic->w = ic->h = 32; ic->hot_x = is_icon ? 16 : 0; ic->hot_y = is_icon ? 16 : 0;
    ic->color = gdi_new_bitmap(32, -32, 32);
    ic->mask = gdi_new_bitmap(32, -32, 1);
    ic->has_alpha = 1;
    GdiBitmap cb;
    gdi_bitmap(ic->color, &cb);
    uint32_t color = id == 32513 ? 0x0000ff : id == 32515 ? 0x00ffff : id == 32516 ? 0xff8000 : id == 32514 ? 0xff8000 : 0x808080;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int dx = x - 16, dy = y - 16, inside = is_icon ? dx * dx + dy * dy < 196 : 0;
            uint8_t* p = row_ptr(&cb, y) + 4 * x;
            p[0] = (uint8_t)(color >> 16); p[1] = (uint8_t)(color >> 8); p[2] = (uint8_t)color; p[3] = inside ? 0xff : 0;
        }
    cache[is_icon][slot] = h;
    return h;
}

static void u_LoadIconA(CPU* c) {
    uint32_t hinst = ARG(0), name = ARG(1);
    uint32_t h = hinst ? load_resource_icon(name, 1) : 0;
    RET(h ? h : system_icon(name < 0x10000 ? name : 32512, 1));
}
static void u_LoadCursorA(CPU* c) {
    uint32_t hinst = ARG(0), name = ARG(1);
    uint32_t h = hinst ? load_resource_icon(name, 0) : 0;
    RET(h ? h : system_icon(name < 0x10000 ? name : 32512, 0));
}

/* .cur and .ani files: an ICONDIR (or a RIFF 'ACON' whose first 'icon' chunk is one) with hot spots. */
static uint32_t icon_from_cur(const uint8_t* f, size_t size) {
    if (size >= 12 && !memcmp(f, "RIFF", 4) && !memcmp(f + 8, "ACON", 4)) {
        for (size_t p = 12; p + 8 <= size;) {
            uint32_t len = f[p + 4] | f[p + 5] << 8 | f[p + 6] << 16 | (uint32_t)f[p + 7] << 24;
            if (!memcmp(f + p, "LIST", 4)) { p += 12; continue; }
            if (!memcmp(f + p, "icon", 4)) return icon_from_cur(f + p + 8, len);
            p += 8 + len + (len & 1);
        }
        return 0;
    }
    if (size < 22) return 0;
    uint32_t type = f[2] | f[3] << 8;
    const uint8_t* e = f + 6;
    uint32_t bytes = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
    uint32_t off = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
    if (off + bytes > size) return 0;
    return icon_from_dib(f + off, bytes, type == 1, type == 2 ? (e[4] | e[5] << 8) : 16, type == 2 ? (e[6] | e[7] << 8) : 16);
}
static void u_LoadCursorFromFileA(CPU* c) {
    char path[600];
    port_host_path(ARG(0) ? GSTR(ARG(0)) : "", path, sizeof path);
    FILE* fp = fopen(path, "rb");
    if (!fp) { set_last_error(2); RET(0); return; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* data = malloc((size_t)n);
    fread(data, 1, (size_t)n, fp);
    fclose(fp);
    uint32_t h = icon_from_cur(data, (size_t)n);
    free(data);
    RET(h);
}

/* As on Windows, no cursor shape (SetCursor(NULL)) or a negative display count hides the cursor. */
static void show_cursor(void) {
    Icon* ic = icon_get(current_cursor);
    if (!cursor_visible || !ic) { js_set_cursor(0, 0, 0, 0, 0, 0); return; }
    GdiBitmap cb;
    gdi_bitmap(ic->color, &cb);
    uint8_t* rgba = malloc((size_t)(ic->w * ic->h * 4));
    for (int y = 0; y < ic->h; y++)
        for (int x = 0; x < ic->w; x++) {
            uint8_t* p = row_ptr(&cb, y) + 4 * x, *o = rgba + 4 * (y * ic->w + x);
            o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
        }
    js_set_cursor(rgba, ic->w, ic->h, ic->hot_x, ic->hot_y, 1);
    free(rgba);
}
static void u_SetCursor(CPU* c) {
    uint32_t old = current_cursor;
    if (ARG(0) != current_cursor) { current_cursor = ARG(0); show_cursor(); }
    RET(old);
}
static void u_GetCursor(CPU* c) { RET(current_cursor); }
/* The display counter: the cursor shows while it is >= 0. */
static int cursor_count;
static void u_ShowCursor(CPU* c) {
    cursor_count += ARG(0) ? 1 : -1;
    int visible = cursor_count >= 0;
    if (visible != cursor_visible) { cursor_visible = visible; show_cursor(); }
    RET(cursor_count);
}
static void free_icon(uint32_t h) {
    Icon* ic = icon_get(h);
    if (!ic) return;
    gdi_delete_bitmap(ic->color);
    gdi_delete_bitmap(ic->mask);
    ic->used = 0;
}
static void u_DestroyIcon(CPU* c) { int ok = icon_get(ARG(0)) != 0; free_icon(ARG(0)); RET(ok); }
static void u_DestroyCursor(CPU* c) { int ok = icon_get(ARG(0)) != 0; if (ARG(0) != current_cursor) free_icon(ARG(0)); RET(ok); }

static uint32_t copy_bitmap(uint32_t src) {
    GdiBitmap s, d;
    if (!gdi_bitmap(src, &s)) return 0;
    uint32_t h = gdi_new_bitmap(s.w, s.top_down ? -s.h : s.h, s.bpp);
    gdi_bitmap(h, &d);
    memcpy(GPTR(d.bits), GPTR(s.bits), s.stride * (uint32_t)s.h);
    memcpy(d.colors, s.colors, sizeof(uint32_t) * s.ncolors);
    return h;
}
static void u_CopyIcon(CPU* c) {
    Icon* src = icon_get(ARG(0));
    if (!src) { RET(0); return; }
    Icon* ic;
    uint32_t h = icon_new(&ic);
    *ic = *src;
    ic->color = copy_bitmap(src->color);
    ic->mask = copy_bitmap(src->mask);
    RET(h);
}
static void u_GetIconInfo(CPU* c) {
    Icon* ic = icon_get(ARG(0));
    uint32_t out = ARG(1);
    if (!ic) { RET(0); return; }
    M32(out) = (uint32_t)ic->is_icon;
    M32(out + 4) = (uint32_t)ic->hot_x;
    M32(out + 8) = (uint32_t)ic->hot_y;
    M32(out + 12) = copy_bitmap(ic->mask);
    M32(out + 16) = copy_bitmap(ic->color);
    RET(1);
}
/* CreateIcon from 1bpp AND bits and colour XOR bits (bpp 1 or 32 in practice). */
static void u_CreateIcon(CPU* c) {
    int w = (int)ARG(1), h = (int)ARG(2);
    uint32_t bpp = ARG(4) & 0xff ? ARG(4) & 0xff : 1, and_bits = ARG(5), xor_bits = ARG(6);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) { RET(0); return; }
    Icon* ic;
    uint32_t handle = icon_new(&ic);
    ic->is_icon = 1; ic->w = w; ic->h = h; ic->hot_x = w / 2; ic->hot_y = h / 2;
    ic->color = gdi_new_bitmap(w, -h, 32);
    ic->mask = gdi_new_bitmap(w, -h, 1);
    GdiBitmap cb, mb;
    gdi_bitmap(ic->color, &cb);
    gdi_bitmap(ic->mask, &mb);
    uint32_t and_stride = ((uint32_t)w + 15) / 16 * 2, xor_stride = ((uint32_t)w * bpp + 15) / 16 * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int t = (M8(and_bits + and_stride * (uint32_t)y + (uint32_t)x / 8) >> (7 - (x & 7))) & 1;
            uint32_t col;
            if (bpp >= 24) col = M32(xor_bits + xor_stride * (uint32_t)y + 4 * (uint32_t)x) & 0xffffff;
            else col = (M8(xor_bits + xor_stride * (uint32_t)y + (uint32_t)x / 8) >> (7 - (x & 7))) & 1 ? 0xffffff : 0;
            uint8_t* p = row_ptr(&cb, y) + 4 * x;
            p[0] = (uint8_t)col; p[1] = (uint8_t)(col >> 8); p[2] = (uint8_t)(col >> 16); p[3] = t ? 0 : 0xff;
            px_set(&mb, x, y, t ? 0xffffff : 0);
        }
    RET(handle);
}

/* DrawIconEx: alpha-blended for icons with alpha, otherwise AND mask then XOR image. */
static void draw_icon(Dc* d, GdiBitmap* b, Icon* ic, int x0, int y0, int w, int h, uint32_t flags) {
    GdiBitmap cb, mb;
    gdi_bitmap(ic->color, &cb);
    gdi_bitmap(ic->mask, &mb);
    if (!w) w = ic->w;
    if (!h) h = ic->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int X = x0 + x, Y = y0 + y, sx = x * ic->w / w, sy = y * ic->h / h;
            if (clipped(d, b, X, Y)) continue;
            uint8_t* p = row_ptr(&cb, sy) + 4 * sx;
            uint32_t col = p[2] | p[1] << 8 | (uint32_t)p[0] << 16, dst = px_get(b, X, Y);
            if ((flags & 3) == 1) { px_set(b, X, Y, px_get(&mb, sx, sy) ? 0xffffff : 0); continue; }
            if ((flags & 3) == 2) { px_set(b, X, Y, col); continue; }
            if (ic->has_alpha) {
                uint32_t a = p[3], out = 0;
                for (int k = 0; k < 24; k += 8) out |= (((col >> k & 0xff) * a + (dst >> k & 0xff) * (255 - a)) / 255) << k;
                px_set(b, X, Y, out);
            } else px_set(b, X, Y, ((px_get(&mb, sx, sy) ? dst : 0) ^ col) & 0xffffff);
        }
}
static void u_DrawIconEx(CPU* c) {
    Dc* d = dc_get(ARG(0));
    Icon* ic = icon_get(ARG(3));
    GdiBitmap b;
    if (!ic || !dc_target(d, &b)) { RET(0); return; }
    if (ARG(7)) {   /* flicker-free brush: fill the destination first */
        int x = lx(d, (int)ARG(1)), y = ly(d, (int)ARG(2));
        fill_rect_dev(d, &b, x, y, x + (int)(ARG(4) ? ARG(4) : (uint32_t)ic->w), y + (int)(ARG(5) ? ARG(5) : (uint32_t)ic->h), ARG(7), 0xf00021);
    }
    draw_icon(d, &b, ic, lx(d, (int)ARG(1)), ly(d, (int)ARG(2)), (int)ARG(4), (int)ARG(5), ARG(8) ? ARG(8) : 3);
    RET(1);
}
static void u_DrawIcon(CPU* c) {
    Dc* d = dc_get(ARG(0));
    Icon* ic = icon_get(ARG(3));
    GdiBitmap b;
    if (!ic || !dc_target(d, &b)) { RET(0); return; }
    draw_icon(d, &b, ic, lx(d, (int)ARG(1)), ly(d, (int)ARG(2)), 0, 0, 3);
    RET(1);
}

/* LoadBitmapA: a resource bitmap (packed DIB) as a DIB section. */
static void u_LoadBitmapA(CPU* c) {
    uint32_t name = ARG(1), size = 0;
    uint32_t dib = res_find(2, name >= 0x10000 ? GSTR(name) : 0, name < 0x10000 ? name : 0, 0, &size);
    if (!dib || !ARG(0)) { RET(0); return; }
    int32_t w = (int32_t)M32(dib + 4), h = (int32_t)M32(dib + 8);
    uint32_t bpp = M16(dib + 14), hdr = M32(dib), used = M32(dib + 32);
    uint32_t ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t bmp = gdi_new_bitmap(w, h, bpp);
    GdiBitmap b;
    gdi_bitmap(bmp, &b);
    for (uint32_t i = 0; i < ncol && i < b.ncolors; i++) b.colors[i] = M32(dib + hdr + 4 * i) & 0xffffff;
    memcpy(GPTR(b.bits), GPTR(dib + hdr + 4 * ncol), b.stride * (uint32_t)b.h);
    RET(bmp);
}

/* ---- metafiles ---- */

/* A metafile DC records onto a bitmap the size of its frame (.01 mm at 96 dpi), so playing it back reproduces what
   was drawn; GetEnhMetaFileBits serialises it as a real EMF (header, one STRETCHDIBITS record, EOF). */
static int mm100_to_px(int v) { return (int)((int64_t)v * 96 / 2540); }

static void g_CreateEnhMetaFileA(CPU* c) {
    uint32_t rc = ARG(2), desc = ARG(3);
    int frame[4] = {0, 0, 1024 * 2540 / 96, 768 * 2540 / 96};
    if (rc) for (int k = 0; k < 4; k++) frame[k] = (int)M32(rc + 4 * (uint32_t)k);
    int w = mm100_to_px(frame[2] - frame[0]), h = mm100_to_px(frame[3] - frame[1]);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint32_t dch = dc_create(DC_META);
    Dc* d = dc_get(dch);
    Emf* e;
    d->meta = emf_new(&e);
    e->used = 2;   /* recording */
    memcpy(e->frame, frame, sizeof frame);
    e->bmp = gdi_new_bitmap(w, -h, 32);
    if (desc) {
        uint32_t n = (uint32_t)strlen(GSTR(desc));
        n += (uint32_t)strlen(GSTR(desc + n + 1)) + 2;
        e->desc = guest_alloc(n + 1, 4);
        memcpy(GPTR(e->desc), GPTR(desc), n);
        M8(e->desc + n) = 0;
    }
    if (ARG(1)) {   /* the file name is kept so CloseEnhMetaFile can write it */
        uint32_t n = (uint32_t)strlen(GSTR(ARG(1)));
        d->meta_file = guest_alloc(n + 1, 4);
        memcpy(GPTR(d->meta_file), GPTR(ARG(1)), n + 1);
    }
    GdiBitmap b;
    gdi_bitmap(e->bmp, &b);
    for (int y = 0; y < b.h; y++) memset(row_ptr(&b, y), 0xff, (size_t)b.w * 4);
    d->s.bmp = e->bmp;
    RET(dch);
}

static uint32_t emf_serialise(Emf* e, uint8_t** out) {
    GdiBitmap b;
    gdi_bitmap(e->bmp, &b);
    uint32_t desc_chars = 0;
    if (e->desc) { desc_chars = (uint32_t)strlen(GSTR(e->desc)); desc_chars += (uint32_t)strlen(GSTR(e->desc + desc_chars + 1)) + 2; }
    uint32_t desc_bytes = (desc_chars * 2 + 3) & ~3u, header = 108 + desc_bytes;
    uint32_t image = (uint32_t)b.w * (uint32_t)b.h * 4, sdib = 80 + 40 + image, total = header + sdib + 20;
    uint8_t* p = calloc(1, total);
    #define W32(at, v) do { uint32_t _v = (uint32_t)(v); memcpy(p + (at), &_v, 4); } while (0)
    W32(0, 1); W32(4, header);
    W32(8, 0); W32(12, 0); W32(16, b.w - 1); W32(20, b.h - 1);
    for (int k = 0; k < 4; k++) W32(24 + 4 * k, e->frame[k]);
    W32(40, 0x464D4520); W32(44, 0x10000); W32(48, total); W32(52, 3); W32(56, 0);
    W32(60, desc_chars); W32(64, desc_chars ? 108 : 0); W32(68, 0);
    W32(72, 1920); W32(76, 1080); W32(80, 508); W32(84, 286);
    for (uint32_t k = 0; k < desc_chars; k++) p[108 + 2 * k] = M8(e->desc + k);
    uint32_t r = header;   /* EMR_STRETCHDIBITS */
    W32(r, 81); W32(r + 4, sdib);
    W32(r + 8, 0); W32(r + 12, 0); W32(r + 16, b.w - 1); W32(r + 20, b.h - 1);
    W32(r + 24, 0); W32(r + 28, 0); W32(r + 32, 0); W32(r + 36, 0); W32(r + 40, b.w); W32(r + 44, b.h);
    W32(r + 48, 80); W32(r + 52, 40); W32(r + 56, 120); W32(r + 60, image); W32(r + 64, 0); W32(r + 68, 0xCC0020);
    W32(r + 72, b.w); W32(r + 76, b.h);
    W32(r + 80, 40); W32(r + 84, b.w); W32(r + 88, b.h); W32(r + 92, 1 | 32 << 16); W32(r + 96, 0); W32(r + 100, image);
    for (int y = 0; y < b.h; y++) memcpy(p + r + 120 + (size_t)y * (size_t)b.w * 4, row_ptr(&b, b.h - 1 - y), (size_t)b.w * 4);
    uint32_t eof = header + sdib;
    W32(eof, 14); W32(eof + 4, 20); W32(eof + 8, 0); W32(eof + 12, 16); W32(eof + 16, 20);
    #undef W32
    *out = p;
    return total;
}

static void write_emf_file(Emf* e, uint32_t guest_name) {
    char path[600];
    port_host_path(GSTR(guest_name), path, sizeof path);
    uint8_t* data;
    uint32_t n = emf_serialise(e, &data);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(data, 1, n, f); fclose(f); }
    free(data);
}

static void g_CloseEnhMetaFile(CPU* c) {
    Dc* d = dc_get(ARG(0));
    if (!d || d->kind != DC_META) { RET(0); return; }
    Emf* e = emf_get(d->meta);
    uint32_t h = d->meta;
    if (e) { e->used = 1; if (d->meta_file) { write_emf_file(e, d->meta_file); guest_free(d->meta_file); } }
    d->used = 0;
    RET(h);
}
static void g_DeleteEnhMetaFile(CPU* c) {
    Emf* e = emf_get(ARG(0));
    if (!e) { RET(0); return; }
    gdi_delete_bitmap(e->bmp);
    if (e->desc) guest_free(e->desc);
    e->used = 0;
    RET(1);
}
static void g_CopyEnhMetaFileA(CPU* c) {
    Emf* src = emf_get(ARG(0));
    if (!src) { RET(0); return; }
    Emf* e;
    uint32_t h = emf_new(&e);
    *e = *src;
    e->bmp = copy_bitmap(src->bmp);
    if (src->desc) {
        uint32_t n = (uint32_t)strlen(GSTR(src->desc));
        n += (uint32_t)strlen(GSTR(src->desc + n + 1)) + 2;
        e->desc = guest_alloc(n + 1, 4);
        memcpy(GPTR(e->desc), GPTR(src->desc), n + 1);
    }
    if (ARG(1)) write_emf_file(e, ARG(1));
    RET(h);
}
static void g_GetEnhMetaFileBits(CPU* c) {
    Emf* e = emf_get(ARG(0));
    if (!e) { RET(0); return; }
    uint8_t* data;
    uint32_t n = emf_serialise(e, &data);
    if (ARG(2)) memcpy(GPTR(ARG(2)), data, ARG(1) < n ? ARG(1) : n);
    free(data);
    RET(n);
}
static void g_GetEnhMetaFileHeader(CPU* c) {
    Emf* e = emf_get(ARG(0));
    if (!e) { RET(0); return; }
    uint8_t* data;
    emf_serialise(e, &data);
    uint32_t header = 108;
    if (ARG(2)) memcpy(GPTR(ARG(2)), data, ARG(1) < header ? ARG(1) : header);
    free(data);
    RET(header);
}
static void g_GetEnhMetaFileDescriptionA(CPU* c) {
    Emf* e = emf_get(ARG(0));
    if (!e) { RET(0xffffffffu); return; }
    if (!e->desc) { RET(0); return; }
    uint32_t n = (uint32_t)strlen(GSTR(e->desc));
    n += (uint32_t)strlen(GSTR(e->desc + n + 1)) + 2;
    if (ARG(2)) memcpy(GPTR(ARG(2)), GPTR(e->desc), ARG(1) < n ? ARG(1) : n);
    RET(n);
}
/* Our metafiles hold no palette. */
static void g_GetEnhMetaFilePaletteEntries(CPU* c) { RET(emf_get(ARG(0)) ? 0 : 0xffffffffu); }

static void g_PlayEnhMetaFile(CPU* c) {
    Dc* d = dc_get(ARG(0));
    Emf* e = emf_get(ARG(1));
    uint32_t rc = ARG(2);
    GdiBitmap db, sb;
    if (!e || !dc_target(d, &db) || !gdi_bitmap(e->bmp, &sb)) { RET(0); return; }
    int x0 = lx(d, (int)M32(rc)), y0 = ly(d, (int)M32(rc + 4)), x1 = lx(d, (int)M32(rc + 8)), y1 = ly(d, (int)M32(rc + 12));
    blit(d, &db, x0, y0, x1 - x0, y1 - y0, 0, &sb, 0, 0, sb.w, sb.h, 0xCC0020, 0, 0, 0, 0);
    RET(1);
}

/* Loads the first bitmap record of an EMF (STRETCHDIBITS, SETDIBITSTODEVICE, BITBLT or STRETCHBLT): this host only
   produces such metafiles, and vector records found elsewhere are not rasterised. */
static uint32_t emf_from_bits(const uint8_t* p, uint32_t size) {
    if (size < 108 || p[0] != 1 || memcmp(p + 40, " EMF", 4)) return 0;
    Emf* e;
    uint32_t h = emf_new(&e);
    for (int k = 0; k < 4; k++) memcpy(&e->frame[k], p + 24 + 4 * k, 4);
    for (uint32_t r = 0; r + 8 <= size;) {
        uint32_t type, len;
        memcpy(&type, p + r, 4);
        memcpy(&len, p + r + 4, 4);
        if (!len || r + len > size) break;
        uint32_t bmi_off = 0, bits_off = 0;
        if (type == 81) { memcpy(&bmi_off, p + r + 48, 4); memcpy(&bits_off, p + r + 56, 4); }
        else if (type == 80) { memcpy(&bmi_off, p + r + 48, 4); memcpy(&bits_off, p + r + 56, 4); }
        else if (type == 76) { memcpy(&bmi_off, p + r + 84, 4); memcpy(&bits_off, p + r + 92, 4); }
        else if (type == 77) { memcpy(&bmi_off, p + r + 92, 4); memcpy(&bits_off, p + r + 100, 4); }
        if (bmi_off && bits_off && r + bmi_off + 40 <= size) {
            const uint8_t* bi = p + r + bmi_off;
            int32_t w, hh;
            uint16_t bpp;
            memcpy(&w, bi + 4, 4); memcpy(&hh, bi + 8, 4); memcpy(&bpp, bi + 14, 2);
            e->bmp = gdi_new_bitmap(w, hh, bpp ? bpp : 32);
            GdiBitmap b;
            gdi_bitmap(e->bmp, &b);
            uint32_t ncol = bpp <= 8 ? 1u << bpp : 0;
            for (uint32_t i = 0; i < ncol && i < b.ncolors; i++) memcpy(&b.colors[i], bi + 40 + 4 * i, 4);
            uint32_t n = b.stride * (uint32_t)b.h;
            if (r + bits_off + n <= size) memcpy(GPTR(b.bits), p + r + bits_off, n);
            break;
        }
        if (type == 14) break;
        r += len;
    }
    if (!e->bmp) e->bmp = gdi_new_bitmap(1, 1, 32);
    return h;
}
static void g_SetEnhMetaFileBits(CPU* c) { RET(emf_from_bits((const uint8_t*)GPTR(ARG(1)), ARG(0))); }

/* Windows metafiles: META_STRETCHDIB of the picture, with the standard 18-byte header. */
static void g_GetWinMetaFileBits(CPU* c) {
    Emf* e = emf_get(ARG(0));
    GdiBitmap b;
    if (!e || !gdi_bitmap(e->bmp, &b)) { RET(0); return; }
    uint32_t image = (uint32_t)b.w * (uint32_t)b.h * 4, rec = 6 + 2 * 11 + 40 + image, total = 18 + rec + 6;
    if (ARG(2) && ARG(1) >= total) {
        uint8_t* p = (uint8_t*)GPTR(ARG(2));
        memset(p, 0, total);
        #define W16(at, v) do { uint16_t _v = (uint16_t)(v); memcpy(p + (at), &_v, 2); } while (0)
        #define W32(at, v) do { uint32_t _v = (uint32_t)(v); memcpy(p + (at), &_v, 4); } while (0)
        W16(0, 1); W16(2, 9); W16(4, 0x300); W32(6, total / 2); W16(10, 0); W32(12, rec / 2); W16(16, 0);
        uint32_t r = 18;
        W32(r, rec / 2); W16(r + 4, 0x0F43);
        W32(r + 6, 0xCC0020); W16(r + 10, 0); W16(r + 12, b.h); W16(r + 14, b.w); W16(r + 16, 0); W16(r + 18, 0);
        W16(r + 20, b.h); W16(r + 22, b.w); W16(r + 24, 0); W16(r + 26, 0);
        W32(r + 28, 40); W32(r + 32, b.w); W32(r + 36, b.h); W16(r + 40, 1); W16(r + 42, 32); W32(r + 48, image);
        for (int y = 0; y < b.h; y++) memcpy(p + r + 68 + (size_t)y * (size_t)b.w * 4, row_ptr(&b, b.h - 1 - y), (size_t)b.w * 4);
        W32(18 + rec, 3); W16(18 + rec + 4, 0);
        #undef W16
        #undef W32
    }
    RET(total);
}
static void g_SetWinMetaFileBits(CPU* c) {
    uint32_t size = ARG(0);
    const uint8_t* p = (const uint8_t*)GPTR(ARG(1));
    Emf* e;
    uint32_t h = emf_new(&e);
    for (uint32_t r = 18; r + 6 <= size;) {
        uint32_t words;
        uint16_t fn;
        memcpy(&words, p + r, 4);
        memcpy(&fn, p + r + 4, 2);
        if (!words || r + words * 2 > size) break;
        if (fn == 0x0F43 || fn == 0x0B41) {
            const uint8_t* bi = p + r + (fn == 0x0F43 ? 28 : 26);
            int32_t w, hh;
            uint16_t bpp;
            memcpy(&w, bi + 4, 4); memcpy(&hh, bi + 8, 4); memcpy(&bpp, bi + 14, 2);
            e->bmp = gdi_new_bitmap(w, hh, bpp ? bpp : 32);
            GdiBitmap b;
            gdi_bitmap(e->bmp, &b);
            uint32_t ncol = bpp <= 8 ? 1u << bpp : 0;
            for (uint32_t i = 0; i < ncol && i < b.ncolors; i++) memcpy(&b.colors[i], bi + 40 + 4 * i, 4);
            uint32_t n = b.stride * (uint32_t)b.h;
            if ((size_t)(bi + 40 + 4 * ncol - p) + n <= size) memcpy(GPTR(b.bits), bi + 40 + 4 * ncol, n);
            e->frame[2] = w * 2540 / 96;
            e->frame[3] = (hh < 0 ? -hh : hh) * 2540 / 96;
            break;
        }
        if (fn == 0) break;
        r += words * 2;
    }
    if (!e->bmp) e->bmp = gdi_new_bitmap(1, 1, 32);
    RET(h);
}

/* ---- registration ---- */

#define G(n, a) {"gdi32.dll", #n, a, g_##n}
#define U(n, a) {"user32.dll", #n, a, u_##n}
static const Hle dc_table[] = {
    G(CreateFontIndirectA, 1), G(CreateSolidBrush, 1), G(CreateBrushIndirect, 1), G(CreatePenIndirect, 1),
    G(CreatePalette, 1), G(CreateHalftonePalette, 1), G(CreateCompatibleDC, 1), G(DeleteDC, 1), G(SelectObject, 2),
    G(DeleteObject, 1), G(GetObjectA, 3), G(GetObjectType, 1), G(GetStockObject, 1), G(UnrealizeObject, 1),
    G(SelectPalette, 3), G(RealizePalette, 1), G(GetPaletteEntries, 4), G(GetSystemPaletteEntries, 4),
    G(GetDIBColorTable, 4), G(SetDIBColorTable, 4),
    G(SetTextColor, 2), G(SetBkColor, 2), G(GetBkColor, 1), G(SetBkMode, 2), G(SetTextAlign, 2), G(SetROP2, 2),
    G(SetStretchBltMode, 2), G(SetMapMode, 2), G(SetWindowOrgEx, 4), G(GetWindowOrgEx, 2), G(SetViewportOrgEx, 4),
    G(SetBrushOrgEx, 4), G(GetBrushOrgEx, 2), G(GetDCOrgEx, 2), G(MoveToEx, 4), G(GetCurrentPositionEx, 2),
    G(SaveDC, 1), G(RestoreDC, 2), G(IntersectClipRect, 5), G(ExcludeClipRect, 5), G(GetClipBox, 2), G(RectVisible, 2),
    G(Rectangle, 5), G(PatBlt, 6), G(LineTo, 3), G(SetPixel, 4), G(GetPixel, 3), G(BitBlt, 9), G(StretchBlt, 11),
    G(MaskBlt, 12), G(TextOutA, 5), G(TextOutW, 5), G(ExtTextOutA, 8),
    G(GetTextExtentPoint32A, 4), G(GetTextExtentPoint32W, 4), G(GetTextExtentExPointA, 7), G(GetTextMetricsA, 2),
    G(AddFontResourceA, 1), G(RemoveFontResourceA, 1),
    G(CreateEnhMetaFileA, 4), G(CloseEnhMetaFile, 1), G(DeleteEnhMetaFile, 1), G(CopyEnhMetaFileA, 2),
    G(GetEnhMetaFileBits, 3), G(GetEnhMetaFileHeader, 3), G(GetEnhMetaFileDescriptionA, 3),
    G(GetEnhMetaFilePaletteEntries, 3), G(PlayEnhMetaFile, 3), G(SetEnhMetaFileBits, 2), G(GetWinMetaFileBits, 5),
    G(SetWinMetaFileBits, 4),
    U(GetSysColor, 1), U(GetSysColorBrush, 1), U(FillRect, 3), U(FrameRect, 3), U(DrawEdge, 4), U(DrawFrameControl, 4),
    U(DrawTextA, 5), U(GetDC, 1), U(GetWindowDC, 1), U(GetDCEx, 3), U(ReleaseDC, 2), U(BeginPaint, 2), U(EndPaint, 2),
    U(LoadIconA, 2), U(LoadCursorA, 2), U(LoadCursorFromFileA, 1), U(SetCursor, 1), U(GetCursor, 0), U(ShowCursor, 1),
    U(DestroyIcon, 1), U(DestroyCursor, 1), U(CopyIcon, 1), U(GetIconInfo, 2), U(CreateIcon, 7), U(DrawIconEx, 9),
    U(DrawIcon, 4), U(LoadBitmapA, 2),
};

void hle_dc_register(void) { hle_register(dc_table, sizeof dc_table / sizeof dc_table[0]); }
