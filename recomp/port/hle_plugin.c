/* Packet logger plugins (desktop/ClientLab.Plugins): the launcher's requests reach the game thread here, between
   window messages, so a client function a plugin calls runs where the client's own input handlers would. worker.js
   reads and writes guest memory itself; this file calls client functions and hands packets to the client's own
   world send/receive functions, with the object each one was last called with. */
#include "port.h"
#include <string.h>

/* The world send (0) and receive (1) capture sites are the entries of those functions (PacketHooks.cs); EAX is the
   object they work on. Recorded on every call so a plugin can call them the same way. */
static uint32_t world_fn[2], world_this[2];

void port_capture_world_packet(CPU* c, int kind, uint32_t fn) {
    if (kind == 0 || kind == 1) {
        world_fn[kind] = fn;
        world_this[kind] = c->eax;
    }
    port_capture_packet(c->edx, kind);
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

static CPU* plugin_cpu;

/* The caller's state is restored whole, because this runs inside whatever API call the client was making. */
EMSCRIPTEN_KEEPALIVE uint32_t plugin_call(uint32_t fn, uint32_t a, uint32_t d, uint32_t cx, int n, uint32_t s0,
                                          uint32_t s1, uint32_t s2) {
    CPU* c = plugin_cpu;
    if (!c || !fn || n < 0 || n > 3) return 0;
    uint32_t stack[3] = {s0, s1, s2};
    CPU saved;
    memcpy(&saved, c, sizeof saved);
    uint32_t r = guest_call_register(c, fn, a, d, cx, n, stack);
    if (c->pend) port_log("plugin: %08x left through an exception", fn);
    memcpy(c, &saved, sizeof saved);
    return r;
}

/* A Delphi AnsiString the client never frees: code page, element size, reference count -1 (a literal, which the
   client copies before changing), length, then the text and a terminating zero. Returns the text's address for the
   caller to fill; it is deliberately leaked, since the client may still hold it after the call. */
EMSCRIPTEN_KEEPALIVE uint32_t plugin_string(uint32_t length) {
    if (length > 65536) return 0;
    uint32_t raw = guest_alloc(12 + length + 1, 16);
    if (!raw) return 0;
    M16(raw) = 1252;
    M16(raw + 2) = 1;
    M32(raw + 4) = 0xffffffffu;
    M32(raw + 8) = length;
    return raw + 12;
}

/* 1 when the packet was handed to the client, 0 when the client has not used that function yet. */
EMSCRIPTEN_KEEPALIVE int plugin_packet(int kind, uint32_t text) {
    if ((kind != 0 && kind != 1) || !world_fn[kind] || !text || !plugin_cpu) return 0;
    plugin_call(world_fn[kind], world_this[kind], text, 0, 0, 0, 0, 0);
    return 1;
}

EM_JS(void, js_plugin_poll, (void), {
    if (globalThis.pluginPoll) globalThis.pluginPoll();
});

/* A client function called from a request can pump messages and land here again: keep the outer CPU for the rest of it. */
void port_plugin_poll(CPU* c) {
    CPU* outer = plugin_cpu;
    plugin_cpu = c;
    js_plugin_poll();
    plugin_cpu = outer;
}

#else
void port_plugin_poll(CPU* c) { (void)c; }
#endif
