/* Browser glue: inside a Web Worker, Present posts the backbuffer (as RGBA) to the page at most ~30 times a second. */
#include "port.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <stdlib.h>

static int in_worker = -1;
static double last_post;
static uint8_t* rgba;
static uint32_t rgba_size;

void web_present(uint32_t surface) {
    if (in_worker < 0) in_worker = EM_ASM_INT({ return (typeof importScripts === 'function') ? 1 : 0; });
    if (!in_worker) return;
    double now = emscripten_get_now();
    if (now - last_post < 33) return;
    last_post = now;
    uint32_t w = M32(surface + 12), h = M32(surface + 16), pitch = M32(surface + 32), bits = M32(surface + 28);
    if (w * h * 4 > rgba_size) { free(rgba); rgba_size = w * h * 4; rgba = malloc(rgba_size); }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = (const uint8_t*)(uintptr_t)(bits + y * pitch);
        uint8_t* dst = rgba + y * w * 4;
        for (uint32_t x = 0; x < w; x++, src += 4, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255; }
    }
    EM_ASM({
        const c = self.pageCanvas;
        const rgba = new Uint8ClampedArray(HEAPU8.buffer, $2, $0 * $1 * 4);
        if (!c) { postMessage({type: 'frame', w: $0, h: $1, data: rgba.slice()}); return; }
        if (c.width !== $0 || c.height !== $1) { c.width = $0; c.height = $1; }
        c.getContext('2d').putImageData(new ImageData(rgba.slice(), $0, $1), 0, 0);
    }, w, h, rgba);
}

/* With the page's canvas handed to the worker, what was drawn only reaches the screen when the worker yields, so
   Present waits here for the next display frame; the page is told how many frames it showed each second. */
EM_ASYNC_JS(void, js_frame_wait, (void), {
    if (!self.pageCanvas) return;
    await new Promise((r) => requestAnimationFrame(r));
    const now = performance.now(), F = self.__frames || (self.__frames = { n: 0, since: now });
    F.n++;
    if (now - F.since >= 1000) {
        postMessage({ type: 'shown', frames: F.n, w: self.pageCanvas.width, h: self.pageCanvas.height });
        F.n = 0; F.since = now;
    }
});
void web_frame_wait(void) { js_frame_wait(); }
#else
void web_present(uint32_t surface) { (void)surface; }
void web_frame_wait(void) {}
#endif
