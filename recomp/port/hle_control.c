/* Automation from the page: control.js serves requests between window messages, on the thread that runs the game,
   so a client function called from here runs where the client's own input handlers would. */
#include "port.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

static CPU* control_cpu;

/* Delphi's register convention: EAX, EDX and ECX, then the remaining arguments pushed left to right. The caller's
   state is restored whole, because this runs inside whatever API call the client was making. */
EMSCRIPTEN_KEEPALIVE uint32_t control_call(uint32_t fn, uint32_t a, uint32_t d, uint32_t cx, int n, uint32_t s0,
                                           uint32_t s1, uint32_t s2) {
    CPU* c = control_cpu;
    if (!c || n < 0 || n > 3) return 0;
    uint32_t stack[3] = {s0, s1, s2};
    CPU saved;
    memcpy(&saved, c, sizeof saved);
    uint32_t r = guest_call_register(c, fn, a, d, cx, n, stack);
    if (c->pend) port_log("control: %08x left through an exception", fn);
    memcpy(c, &saved, sizeof saved);
    return r;
}

EM_JS(void, js_control_poll, (void), {
    if (globalThis.controlPoll) globalThis.controlPoll();
});

/* A client function called from a request can pump messages and land here again: keep the outer CPU for the rest of it. */
void port_control_poll(CPU* c) {
    CPU* outer = control_cpu;
    control_cpu = c;
    js_control_poll();
    control_cpu = outer;
}

#else
void port_control_poll(CPU* c) { (void)c; }
#endif
