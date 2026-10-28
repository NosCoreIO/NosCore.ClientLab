/* The long tail of startup imports: OLE strings, version info, menus, file mappings, threads, plus no-op Miles,
   Steam and Winsock stand-ins until audio and the WebSocket bridge exist. */
#include "port.h"
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define RET(v) (c->eax = (uint32_t)(v))

static void r_zero(CPU* c) { RET(0); }
static void r_one(CPU* c) { RET(1); }

/* ---- OLE automation strings (BSTR: length prefix in bytes, then UTF-16, then NUL) ---- */

static uint32_t bstr_alloc(uint32_t src, uint32_t chars) {
    uint32_t p = guest_alloc(4 + 2 * chars + 2, 16);
    M32(p) = 2 * chars;
    if (src) memcpy(GPTR(p + 4), GPTR(src), 2 * chars);
    M16(p + 4 + 2 * chars) = 0;
    return p + 4;
}
static void o_SysAllocStringLen(CPU* c) { RET(bstr_alloc(ARG(0), ARG(1))); }
static void o_SysFreeString(CPU* c) { if (ARG(0)) guest_free(ARG(0) - 4); }
static void o_SysReAllocStringLen(CPU* c) {
    uint32_t old = M32(ARG(0)), n = bstr_alloc(ARG(1), ARG(2));
    if (old) guest_free(old - 4);
    M32(ARG(0)) = n;
    RET(1);
}
static void o_SysStringLen(CPU* c) { RET(ARG(0) ? M32(ARG(0) - 4) / 2 : 0); }
static void o_VariantInit(CPU* c) { memset(GPTR(ARG(0)), 0, 16); }
static void o_VariantClear(CPU* c) { memset(GPTR(ARG(0)), 0, 16); RET(0); }
static void o_VariantCopy(CPU* c) { memcpy(GPTR(ARG(0)), GPTR(ARG(1)), 16); RET(0); }
static void o_CoInitialize(CPU* c) { RET(0); }

/* ---- strings and formatting ---- */

static void k_CompareStringA(CPU* c) {
    const char *a = GSTR(ARG(2)), *b = GSTR(ARG(4));
    int la = (int32_t)ARG(3) < 0 ? (int)strlen(a) : (int)ARG(3), lb = (int32_t)ARG(5) < 0 ? (int)strlen(b) : (int)ARG(5);
    int icase = ARG(1) & 1, n = la < lb ? la : lb, r = 0;
    for (int k = 0; k < n && !r; k++) {
        int x = (unsigned char)a[k], y = (unsigned char)b[k];
        if (icase) { x = tolower(x); y = tolower(y); }
        r = x - y;
    }
    if (!r) r = la - lb;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
static void k_FormatMessageA(CPU* c) {
    char msg[64];
    snprintf(msg, sizeof msg, "Error %u", ARG(2));
    if (ARG(0) & 0x100) { M32(ARG(4)) = guest_strdup(msg); RET(strlen(msg)); return; }
    if (ARG(5) > strlen(msg)) strcpy(GSTR(ARG(4)), msg);
    RET(strlen(msg));
}
static void k_SetThreadLocale(CPU* c) { RET(1); }

/* ---- version info: read RT_VERSION straight out of the PE file on disk ---- */

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Returns a malloc'd copy of the file's first RT_VERSION resource, or 0. */
static uint8_t* read_version_resource(const char* guest_path, uint32_t* size) {
    char path[600];
    port_host_path(guest_path, path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    fread(b, 1, (size_t)n, f);
    fclose(f);
    uint8_t* out = 0;
    uint32_t pe = rd32(b + 0x3c);
    uint16_t nsec = rd16(b + pe + 6), opt = rd16(b + pe + 20), magic = rd16(b + pe + 24);
    uint32_t dirs = pe + 24 + (magic == 0x20b ? 112 : 96);
    uint32_t rva = rd32(b + dirs + 16), sec = pe + 24 + opt, raw = 0, va = 0;
    for (int k = 0; k < nsec; k++, sec += 40)
        if (rva >= rd32(b + sec + 12) && rva < rd32(b + sec + 12) + rd32(b + sec + 16)) { va = rd32(b + sec + 12); raw = rd32(b + sec + 20); }
    if (rva && raw) {
        uint8_t* root = b + raw + (rva - va);
        uint32_t off = 0;
        for (int level = 0; level < 3; level++) {
            uint8_t* d = root + off;
            uint16_t count = rd16(d + 12) + rd16(d + 14);
            uint8_t* e = d + 16;
            int found = 0;
            for (int k = 0; k < count; k++, e += 8)
                if (level != 0 || rd32(e) == 16) { off = rd32(e + 4) & 0x7fffffffu; found = 1; break; }
            if (!found) { off = 0xFFFFFFFFu; break; }
        }
        if (off != 0xFFFFFFFFu) {
            uint8_t* leaf = root + off;
            uint32_t data = rd32(leaf), len = rd32(leaf + 4);
            out = malloc(len);
            memcpy(out, b + raw + (data - va), len);
            *size = len;
        }
    }
    free(b);
    return out;
}

static char* wide_to_ascii(uint32_t p, char* out, size_t n) {
    size_t k = 0;
    for (; M16(p + 2 * k) && k + 1 < n; k++) out[k] = (char)M16(p + 2 * k);
    out[k] = 0;
    return out;
}

static void v_GetFileVersionInfoSizeA(CPU* c) {
    uint32_t size = 0;
    uint8_t* r = read_version_resource(GSTR(ARG(0)), &size);
    free(r);
    if (!r) set_last_error(1813);
    RET(r ? size : 0);
}
static void v_GetFileVersionInfoSizeW(CPU* c) {
    char path[600];
    uint32_t size = 0;
    uint8_t* r = read_version_resource(wide_to_ascii(ARG(0), path, sizeof path), &size);
    free(r);
    if (!r) set_last_error(1813);
    RET(r ? size : 0);
}
static void version_into(const char* path, uint32_t len, uint32_t buf, CPU* c) {
    uint32_t size = 0;
    uint8_t* r = read_version_resource(path, &size);
    if (!r) { RET(0); return; }
    memcpy(GPTR(buf), r, size < len ? size : len);
    free(r);
    RET(1);
}
static void v_GetFileVersionInfoA(CPU* c) { version_into(GSTR(ARG(0)), ARG(2), ARG(3), c); }
static void v_GetFileVersionInfoW(CPU* c) { char p[600]; version_into(wide_to_ascii(ARG(0), p, sizeof p), ARG(2), ARG(3), c); }
/* Only the root query ("\\") is answered: it returns the VS_FIXEDFILEINFO inside the block. */
static void v_VerQueryValueA(CPU* c) {
    uint32_t block = ARG(0);
    if (strcmp(GSTR(ARG(1)), "\\")) { RET(0); return; }
    for (uint32_t k = 0; k < 256; k += 4)
        if (M32(block + k) == 0xFEEF04BDu) { M32(ARG(2)) = block + k; M32(ARG(3)) = 52; RET(1); return; }
    RET(0);
}

/* ---- file mappings: read the whole file into guest memory ---- */

static void k_CreateFileMappingA(CPU* c) { RET(ARG(0) != 0xFFFFFFFFu ? ARG(0) | 0x80000000u : 0x80000000u); }
static void k_MapViewOfFile(CPU* c) {
    uint32_t file = ARG(0) & 0x7fffffffu, size = ARG(4);
    if (!file) { RET(guest_alloc(size ? size : 0x10000, 0x1000)); return; }
    int fd = port_file_fd(file);
    struct stat st;
    uint32_t off = ARG(3);
    if (fd < 0 || fstat(fd, &st) || off > (uint32_t)st.st_size) { RET(0); return; }
    if (!size) size = (uint32_t)st.st_size - off;
    uint32_t p = guest_alloc(size ? size : 1, 0x1000);
    uint32_t got = 0;
    while (got < size) {
        ssize_t n = pread(fd, GPTR(p + got), size - got, off + got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    RET(p);
}
static void k_UnmapViewOfFile(CPU* c) { RET(1); }

/* ---- threads: not run yet; the startup code only resumes helper threads ---- */

static void k_ResumeThread(CPU* c) { port_log("ResumeThread(%08x) ignored", ARG(0)); RET(1); }
static void k_SleepEx(CPU* c) { RET(0); }
static void k_SetThreadPriority(CPU* c) { RET(1); }
static void k_GetExitCodeThread(CPU* c) { if (ARG(1)) M32(ARG(1)) = 259; RET(1); }

/* ---- registry ---- */

static void a_RegCreateKeyExA(CPU* c) {
    uint32_t k = port_reg_known_key(ARG(1));
    if (ARG(7)) M32(ARG(7)) = k ? k : 0x00400100u;
    if (ARG(8)) M32(ARG(8)) = k ? 2 : 1;
    RET(0);
}
static void a_RegSetValueExA(CPU* c) { RET(0); }

/* ---- user32/gdi32 odds and ends ---- */

static void u_InflateRect(CPU* c) {
    uint32_t r = ARG(0);
    M32(r) -= ARG(1); M32(r + 4) -= ARG(2); M32(r + 8) += ARG(1); M32(r + 12) += ARG(2);
    RET(1);
}
static void u_OffsetRect(CPU* c) {
    uint32_t r = ARG(0);
    M32(r) += ARG(1); M32(r + 4) += ARG(2); M32(r + 8) += ARG(1); M32(r + 12) += ARG(2);
    RET(1);
}
static void u_SetRect(CPU* c) { uint32_t r = ARG(0); M32(r) = ARG(1); M32(r + 4) = ARG(2); M32(r + 8) = ARG(3); M32(r + 12) = ARG(4); RET(1); }
static void u_CopyRect(CPU* c) { memcpy(GPTR(ARG(0)), GPTR(ARG(1)), 16); RET(1); }
static void u_PtInRect(CPU* c) {
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    uint32_t r = ARG(0);
    RET(x >= (int32_t)M32(r) && x < (int32_t)M32(r + 8) && y >= (int32_t)M32(r + 4) && y < (int32_t)M32(r + 12));
}
static void u_EnumDisplaySettingsA(CPU* c) {
    uint32_t dm = ARG(2);
    if (ARG(1) > 0 && ARG(1) != 0xFFFFFFFFu && ARG(1) != 0xFFFFFFFEu) { RET(0); return; }
    memset(GPTR(dm + 36), 0, 120);
    M16(dm + 36) = 156;
    M32(dm + 104) = 32; M32(dm + 108) = 1920; M32(dm + 112) = 1080; M32(dm + 120) = 60;
    RET(1);
}
static void u_ChangeDisplaySettingsA(CPU* c) { RET(0); }

/* One 1920x1080 primary monitor. */
#define MONITOR 0x00650001u
static void u_EnumDisplayMonitors(CPU* c) {
    uint32_t rect = guest_alloc(16, 16);
    M32(rect) = 0; M32(rect + 4) = 0; M32(rect + 8) = 1920; M32(rect + 12) = 1080;
    guest_call(c, ARG(2), 4, MONITOR, 0, rect, ARG(3));
    guest_free(rect);
    RET(1);
}
static void u_MonitorFrom(CPU* c) { RET(MONITOR); }
static void u_GetMonitorInfoA(CPU* c) {
    uint32_t p = ARG(1), size = M32(p);
    M32(p + 4) = 0; M32(p + 8) = 0; M32(p + 12) = 1920; M32(p + 16) = 1080;
    M32(p + 20) = 0; M32(p + 24) = 0; M32(p + 28) = 1920; M32(p + 32) = 1040;
    M32(p + 36) = 1;
    if (size >= 72) strcpy(GSTR(p + 40), "\\\\.\\DISPLAY1");
    RET(1);
}
static void u_LoadCursorFromFileA(CPU* c) { RET(0x00060100u); }
static void u_DestroyIcon(CPU* c) { RET(1); }
static void u_GetWindowDC(CPU* c) { RET(0x00300000u + (ARG(0) & 0xffff)); }
static void g_GetCurrentPositionEx(CPU* c) { if (ARG(1)) { M32(ARG(1)) = 0; M32(ARG(1) + 4) = 0; } RET(1); }
static void g_GetBitmapBits(CPU* c) { if (ARG(2)) memset(GPTR(ARG(2)), 0, ARG(1)); RET(ARG(1)); }
static uint32_t gdi_misc_next = 0x00280000u;
static void g_NewHandle(CPU* c) { RET(gdi_misc_next += 4); }
static void i_ImmAssociateContext(CPU* c) { RET(0); }
static void i_ImmNotifyIME(CPU* c) { RET(1); }
static void w_timeGetDevCaps(CPU* c) { if (ARG(0)) { M32(ARG(0)) = 1; M32(ARG(0) + 4) = 1000000; } RET(0); }

/* ---- Gameforge wrapper (noscore_gf.dll): same answers as NosCore.DeveloperTools.GfStub ----
   The session ticket is the _NC_AUTH_CODE GUID as ASCII text; the client sends it verbatim in NoS0577. */

static uint32_t persona_addr, language_addr, branch_addr;
static void s_one(CPU* c) { RET(1); }
static void s_GetAuthSessionTicket(CPU* c) {
    const char* code = getenv("_NC_AUTH_CODE");
    uint32_t buf = ARG(0), size = ARG(1), psize = ARG(2);
    if (!code || !*code || !buf) {
        if (psize) M32(psize) = 0;
        RET(0);
        return;
    }
    uint32_t n = (uint32_t)strlen(code);
    if (n > size) n = size;
    memcpy(GPTR(buf), code, n);
    if (psize) M32(psize) = n;
    RET(1);
}
static void s_GetPersonaName(CPU* c) { if (!persona_addr) persona_addr = guest_strdup(""); RET(persona_addr); }
static void s_GetSteamLanguage(CPU* c) { if (!language_addr) language_addr = guest_strdup("english"); RET(language_addr); }
static void s_GetConnectedBetaBranch(CPU* c) { if (!branch_addr) branch_addr = guest_strdup("live"); RET(branch_addr); }

/* ---- Winsock: no network yet; connections fail cleanly ---- */

static void ws_WSAStartup(CPU* c) {
    uint32_t d = ARG(1);
    memset(GPTR(d), 0, 400);
    M16(d) = 0x0101;
    M16(d + 2) = 0x0202;
    RET(0);
}
static void ws_inet_addr(CPU* c) {
    unsigned a, b, d, e;
    if (sscanf(GSTR(ARG(0)), "%u.%u.%u.%u", &a, &b, &d, &e) != 4) { RET(0xFFFFFFFFu); return; }
    RET(a | b << 8 | d << 16 | e << 24);
}
static void ws_htons(CPU* c) { RET(((ARG(0) & 0xff) << 8) | ((ARG(0) >> 8) & 0xff)); }
/* In the browser, sockets are relayed by the dev server (recomp/web/serve.py, the /net endpoints) through synchronous XHR: the
   worker never yields, so it cannot use a WebSocket. Under Node there is no network and connections are refused. */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, js_net_connect, (const char* host, int port), {
    var x = new XMLHttpRequest();
    x.open('POST', '/net/connect?host=' + UTF8ToString(host) + '&port=' + port, false);
    try { x.send(); } catch (e) { return -1; }
    return x.status === 200 ? parseInt(x.responseText, 10) : -1;
});
EM_JS(int, js_net_send, (int id, const uint8_t* p, int n), {
    var x = new XMLHttpRequest();
    x.open('POST', '/net/send?id=' + id, false);
    x.send(HEAPU8.slice(p, p + n));
    return x.status === 200 ? n : -1;
});
EM_JS(int, js_net_recv, (int id, uint8_t* p, int cap), {
    var x = new XMLHttpRequest();
    x.open('GET', '/net/recv?id=' + id, false);
    x.responseType = 'arraybuffer';
    x.send();
    if (x.status === 410) return -2;
    if (x.status !== 200) return -1;
    var d = new Uint8Array(x.response);
    var n = Math.min(d.length, cap);
    HEAPU8.set(d.subarray(0, n), p);
    return n;
});
EM_JS(void, js_net_close, (int id), {
    var x = new XMLHttpRequest();
    x.open('POST', '/net/close?id=' + id, false);
    x.send();
});
#endif

#define MAX_SOCK 64
#define SOCK_BASE 0x100u
typedef struct {
    int used, remote, connected, closed;
    uint8_t* rx;
    uint32_t rx_len;
    double last_poll;
} Sock;
static Sock socks[MAX_SOCK];
#define RX_CAP (1u << 20)

static Sock* sock_get(uint32_t s) { return s >= SOCK_BASE && s < SOCK_BASE + MAX_SOCK && socks[s - SOCK_BASE].used ? &socks[s - SOCK_BASE] : 0; }

static void sock_poll(Sock* k) {
#ifdef __EMSCRIPTEN__
    if (!k->connected || k->closed || k->rx_len >= RX_CAP / 2) return;
    double now = emscripten_get_now();
    if (now - k->last_poll < 8) return;
    k->last_poll = now;
    int n = js_net_recv(k->remote, k->rx + k->rx_len, (int)(RX_CAP - k->rx_len));
    if (n == -2) k->closed = 1;
    else if (n > 0) k->rx_len += (uint32_t)n;
#else
    (void)k;
#endif
}

static void ws_socket(CPU* c) {
    for (int k = 0; k < MAX_SOCK; k++)
        if (!socks[k].used) {
            memset(&socks[k], 0, sizeof socks[k]);
            socks[k].used = 1;
            socks[k].rx = malloc(RX_CAP);
            RET(SOCK_BASE + (uint32_t)k);
            return;
        }
    RET(0xFFFFFFFFu);
}
static void ws_connect(CPU* c) {
    Sock* k = sock_get(ARG(0));
    uint32_t sa = ARG(1), port = (M8(sa + 2) << 8) | M8(sa + 3);
    char host[32];
    snprintf(host, sizeof host, "%u.%u.%u.%u", M8(sa + 4), M8(sa + 5), M8(sa + 6), M8(sa + 7));
#ifdef __EMSCRIPTEN__
    if (k && port_is_web()) {
        k->remote = js_net_connect(host, (int)port);
        port_log("connect %s:%u -> %s", host, port, k->remote > 0 ? "ok (relayed)" : "refused");
        if (k->remote > 0) { k->connected = 1; RET(0); return; }
    } else
#endif
        port_log("connect to %s:%u refused (network is only relayed in the browser)", host, port);
    (void)k;
    set_last_error(10061);
    RET(0xFFFFFFFFu);
}
static void fdset_filter(uint32_t set, int want_read) {
    if (!set) return;
    uint32_t n = M32(set), out = 0;
    for (uint32_t k = 0; k < n && k < 64; k++) {
        uint32_t s = M32(set + 4 + 4 * k);
        Sock* so = sock_get(s);
        int ready = 0;
        if (so && want_read) { if (!so->rx_len) sock_poll(so); ready = so->rx_len > 0 || so->closed; }
        else if (so) ready = so->connected;
        if (ready) M32(set + 4 + 4 * out++) = s;
    }
    M32(set) = out;
}
static void ws_select(CPU* c) {
    fdset_filter(ARG(1), 1);
    fdset_filter(ARG(2), 0);
    if (ARG(3)) M32(ARG(3)) = 0;
    RET((ARG(1) ? M32(ARG(1)) : 0) + (ARG(2) ? M32(ARG(2)) : 0));
}
static void ws_fdisset(CPU* c) {
    uint32_t s = ARG(0), set = ARG(1), n = M32(set);
    for (uint32_t k = 0; k < n && k < 64; k++)
        if (M32(set + 4 + 4 * k) == s) { RET(1); return; }
    RET(0);
}
static void ws_recv(CPU* c) {
    Sock* k = sock_get(ARG(0));
    uint32_t buf = ARG(1), len = ARG(2);
    if (!k) { set_last_error(10038); RET(0xFFFFFFFFu); return; }
    if (!k->rx_len) sock_poll(k);
    if (k->rx_len) {
        uint32_t n = k->rx_len < len ? k->rx_len : len;
        memcpy(GPTR(buf), k->rx, n);
        memmove(k->rx, k->rx + n, k->rx_len - n);
        k->rx_len -= n;
        static int logged;
        if (logged++ < 20) {
            char hex[3 * 64 + 1] = {0};
            for (uint32_t i = 0; i < n && i < 64; i++) snprintf(hex + 3 * i, 4, "%02x ", M8(buf + i));
            port_log("recv %08x: %u bytes %s", ARG(0), n, hex);
        }
        RET(n);
        return;
    }
    if (k->closed) { RET(0); return; }
    set_last_error(10035);
    RET(0xFFFFFFFFu);
}
static void ws_send(CPU* c) {
    Sock* k = sock_get(ARG(0));
#ifdef __EMSCRIPTEN__
    if (k && k->connected) {
        int n = js_net_send(k->remote, (const uint8_t*)GPTR(ARG(1)), (int)ARG(2));
        static int logged;
        if (logged++ >= 20) port_log("send %08x: %u bytes", ARG(0), ARG(2));
        else {
            char hex[3 * 160 + 1] = {0};
            for (uint32_t i = 0; i < ARG(2) && i < 160; i++) snprintf(hex + 3 * i, 4, "%02x ", M8(ARG(1) + i));
            port_log("send %08x: %u bytes -> %d %s", ARG(0), ARG(2), n, hex);
        }
        if (n >= 0) { RET(n); return; }
    }
#endif
    (void)k;
    set_last_error(10057);
    RET(0xFFFFFFFFu);
}
static void ws_closesocket(CPU* c) {
    Sock* k = sock_get(ARG(0));
    if (k) {
#ifdef __EMSCRIPTEN__
        if (k->connected) js_net_close(k->remote);
#endif
        port_log("closesocket %08x", ARG(0));
        free(k->rx);
        k->used = 0;
    }
    RET(0);
}
static void ws_WSAGetLastError(CPU* c) { RET(get_last_error()); }

static void n_RtlGetVersion(CPU* c) {
    uint32_t p = ARG(0);
    M32(p + 4) = 6; M32(p + 8) = 1; M32(p + 12) = 7601; M32(p + 16) = 2;
    memset(GPTR(p + 20), 0, 256);
    RET(0);
}

#define K(n, a, f) {"kernel32.dll", #n, a, f}
/* The Steam stubs (steam_wrapper.dll / noscore_gf.dll) are cdecl: they end in a plain ret. */
#define S(n) {"steam_wrapper.dll", #n, CDECL_ARGS, r_zero}

static const Hle misc[] = {
    {"oleaut32.dll", "SysAllocStringLen", 2, o_SysAllocStringLen},
    {"oleaut32.dll", "SysFreeString", 1, o_SysFreeString},
    {"oleaut32.dll", "SysReAllocStringLen", 3, o_SysReAllocStringLen},
    {"oleaut32.dll", "SysStringLen", 1, o_SysStringLen},
    {"oleaut32.dll", "VariantInit", 1, o_VariantInit},
    {"oleaut32.dll", "VariantClear", 1, o_VariantClear},
    {"oleaut32.dll", "VariantCopy", 2, o_VariantCopy},
    {"ole32.dll", "CoInitialize", 1, o_CoInitialize},
    {"ole32.dll", "CoUninitialize", 0, r_zero},
    {"ntdll.dll", "RtlGetVersion", 1, n_RtlGetVersion},
    K(CompareStringA, 6, k_CompareStringA),
    K(FormatMessageA, 7, k_FormatMessageA),
    K(SetThreadLocale, 1, k_SetThreadLocale),
    {"version.dll", "GetFileVersionInfoSizeW", 2, v_GetFileVersionInfoSizeW},
    {"version.dll", "GetFileVersionInfoSizeA", 2, v_GetFileVersionInfoSizeA},
    {"version.dll", "GetFileVersionInfoW", 4, v_GetFileVersionInfoW},
    {"version.dll", "GetFileVersionInfoA", 4, v_GetFileVersionInfoA},
    {"version.dll", "VerQueryValueA", 4, v_VerQueryValueA},
    K(CreateFileMappingA, 6, k_CreateFileMappingA),
    K(MapViewOfFile, 5, k_MapViewOfFile),
    K(UnmapViewOfFile, 1, k_UnmapViewOfFile),
    K(ResumeThread, 1, k_ResumeThread),
    K(SleepEx, 2, k_SleepEx),
    K(SetThreadPriority, 2, k_SetThreadPriority),
    K(GetExitCodeThread, 2, k_GetExitCodeThread),
    {"advapi32.dll", "RegCreateKeyExA", 9, a_RegCreateKeyExA},
    {"advapi32.dll", "RegSetValueExA", 6, a_RegSetValueExA},
    {"user32.dll", "SetMenu", 2, r_one},
    {"user32.dll", "InflateRect", 3, u_InflateRect},
    {"user32.dll", "OffsetRect", 3, u_OffsetRect},
    {"user32.dll", "SetRect", 5, u_SetRect},
    {"user32.dll", "CopyRect", 2, u_CopyRect},
    {"user32.dll", "PtInRect", 3, u_PtInRect},
    {"user32.dll", "RedrawWindow", 4, r_one},
    {"user32.dll", "FillRect", 3, r_one},
    {"user32.dll", "EnumDisplaySettingsA", 3, u_EnumDisplaySettingsA},
    {"user32.dll", "ChangeDisplaySettingsA", 2, u_ChangeDisplaySettingsA},
    {"user32.dll", "EnumDisplayMonitors", 4, u_EnumDisplayMonitors},
    {"user32.dll", "MonitorFromWindow", 2, u_MonitorFrom},
    {"user32.dll", "MonitorFromPoint", 3, u_MonitorFrom},
    {"user32.dll", "MonitorFromRect", 2, u_MonitorFrom},
    {"user32.dll", "GetMonitorInfoA", 2, u_GetMonitorInfoA},
    {"user32.dll", "LoadCursorFromFileA", 1, u_LoadCursorFromFileA},
    {"user32.dll", "DestroyIcon", 1, u_DestroyIcon},
    {"user32.dll", "GetWindowDC", 1, u_GetWindowDC},
    {"gdi32.dll", "GetCurrentPositionEx", 2, g_GetCurrentPositionEx},
    {"gdi32.dll", "GetBitmapBits", 3, g_GetBitmapBits},
    {"gdi32.dll", "CreatePenIndirect", 1, g_NewHandle},
    {"gdi32.dll", "CreateHalftonePalette", 1, g_NewHandle},
    {"imm32.dll", "ImmAssociateContext", 2, i_ImmAssociateContext},
    {"imm32.dll", "ImmNotifyIME", 4, i_ImmNotifyIME},
    {"winmm.dll", "timeGetDevCaps", 2, w_timeGetDevCaps},
    {"winmm.dll", "timeBeginPeriod", 1, r_zero},
    {"winmm.dll", "timeEndPeriod", 1, r_zero},
    {"ewsf.ews", "ShowNostaleSplash", 0, r_zero},
    {"ewsf.ews", "FreeNostaleSplash", 0, r_zero},
    {"noscore_gf.dll", "Steam_Init", CDECL_ARGS, s_one},
    {"noscore_gf.dll", "Steam_IsLoggedIn", CDECL_ARGS, s_one},
    {"noscore_gf.dll", "Steam_IsOverlayEnabled", CDECL_ARGS, r_zero},
    {"noscore_gf.dll", "Steam_GetAuthSessionTicket", CDECL_ARGS, s_GetAuthSessionTicket},
    {"noscore_gf.dll", "Steam_GetPersonaName", CDECL_ARGS, s_GetPersonaName},
    {"noscore_gf.dll", "Steam_GetSteamLanguage", CDECL_ARGS, s_GetSteamLanguage},
    {"noscore_gf.dll", "Steam_GetConnectedBetaBranch", CDECL_ARGS, s_GetConnectedBetaBranch},
    S(Steam_OnFrameTick), S(Steam_Shutdown), S(Steam_CancelAuthSessionTicket), S(Steam_OpenOverlayToWebPage),
    {"noscore_gf.dll", "Steam_SetOverlayActivateCallback", CDECL_ARGS, r_zero},
    {"wsock32.dll", "WSAStartup", 2, ws_WSAStartup},
    {"wsock32.dll", "WSACleanup", 0, r_zero},
    {"wsock32.dll", "WSAGetLastError", 0, ws_WSAGetLastError},
    {"wsock32.dll", "inet_addr", 1, ws_inet_addr},
    {"wsock32.dll", "htons", 1, ws_htons},
    {"wsock32.dll", "ntohs", 1, ws_htons},
    {"wsock32.dll", "socket", 3, ws_socket},
    {"wsock32.dll", "setsockopt", 5, r_zero},
    {"wsock32.dll", "ioctlsocket", 3, r_zero},
    {"wsock32.dll", "connect", 3, ws_connect},
    {"wsock32.dll", "select", 5, ws_select},
    {"wsock32.dll", "__WSAFDIsSet", 2, ws_fdisset},
    {"wsock32.dll", "send", 4, ws_send},
    {"wsock32.dll", "recv", 4, ws_recv},
    {"wsock32.dll", "closesocket", 1, ws_closesocket},
};

void hle_misc_register(void) { hle_register(misc, sizeof misc / sizeof misc[0]); }
