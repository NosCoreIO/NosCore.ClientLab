/* text_metrics (hle_dc.c) against js_text_measure itself, with a canvas whose widths depend on the font and every
   character: the same answers for ANSI and wide text across more strings than the cache holds, repeats answered
   without calling into JavaScript, nothing kept when JavaScript says not to, and nothing after a font change.
   Build with emcc (-sERROR_ON_UNDEFINED_SYMBOLS=0 "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$ENV,$UTF8ToString") and run
   under Node. */
#include "../../recomp/port/hle_dc.c"
#include <stdio.h>

EM_JS(void, fake_canvas, (void), {
    globalThis.self = globalThis;
    globalThis.measureCalls = 0;
    globalThis.OffscreenCanvas = class {
        getContext() {
            return { font: '', measureText(s) {
                globalThis.measureCalls++;
                let h = 2166136261;
                for (const ch of this.font + '|' + s) h = Math.imul(h ^ ch.codePointAt(0), 16777619) >>> 0;
                return { width: h % 5000, fontBoundingBoxAscent: 10 + h % 7, fontBoundingBoxDescent: 3 + h % 5 };
            } };
        }
    };
});
EM_JS(int, crossings, (void), { const c = self.__textMetricsCache; return c ? c.hits + c.misses : 0; });
EM_JS(int, measure_calls, (void), { return globalThis.measureCalls; });
EM_JS(void, js_cache, (int on), { self.__textMetricsCache.enabled = !!on; self.__textMetricsCache.entries.clear(); });

static uint32_t seed = 88172645u;
static uint32_t next(void) { seed ^= seed << 13; seed ^= seed >> 17; return seed ^= seed << 5; }
static const char* css_fonts[] = {"12px Arial", "bold 14px Arial", "italic 16px serif", "17px 'Tahoma'"};

static int check(const char* css, const void* text, int n, int wide, const char* what) {
    int a[3], b[3];
    js_text_measure(css, text, n, wide, a);
    text_metrics(css, text, n, wide, b);
    if (memcmp(a, b, sizeof a)) { printf("FAIL: %s: %d %d %d vs %d %d %d\n", what, a[0], a[1], a[2], b[0], b[1], b[2]); return 0; }
    return 1;
}

int main(void) {
    fake_canvas();
    static uint8_t ansi[64];
    static uint16_t wide[64];
    for (int i = 0; i < 12000; i++) {
        int n = (int)(next() % 24);
        const char* css = css_fonts[next() % 4];
        for (int k = 0; k < n; k++) { ansi[k] = (uint8_t)(32 + next() % 224); wide[k] = (uint16_t)(32 + next() % 0x4e00); }
        if (!check(css, ansi, n, 0, "ANSI text") || !check(css, wide, n, 1, "wide text")) return 1;
    }
    int out[3], before;
    const char* label = "NosVille";
    text_metrics(css_fonts[0], label, 8, 0, out);
    before = crossings();
    for (int i = 0; i < 100; i++) text_metrics(css_fonts[0], label, 8, 0, out);
    if (crossings() != before) { printf("FAIL: a repeated label still called into JavaScript\n"); return 1; }
    text_metrics(css_fonts[1], label, 8, 0, out);
    if (crossings() != before + 1) { printf("FAIL: another font reused the label's metrics\n"); return 1; }
    text_cache_clear();
    before = crossings();
    text_metrics(css_fonts[0], label, 8, 0, out);
    if (crossings() != before + 1) { printf("FAIL: metrics survived a font change\n"); return 1; }
    js_cache(0);
    text_cache_clear();
    before = measure_calls();
    for (int i = 0; i < 3; i++) text_metrics(css_fonts[2], label, 8, 0, out);
    if (measure_calls() != before + 3) { printf("FAIL: an answer JavaScript said not to keep was kept\n"); return 1; }
    printf("PASS: cached text metrics match JavaScript's over 24000 ANSI and wide strings; repeats skip JavaScript; "
           "uncacheable answers and font changes are respected.\n");
    return 0;
}
