#include "port.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RET(v) (c->eax = (uint32_t)(v))
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* ---- handles: files, find handles, events, threads share one table ---- */

enum { H_FREE, H_FILE, H_FIND, H_EVENT, H_THREAD, H_MAPPING };
typedef struct {
    int kind;
    int fd;
    int signaled, manual;                 /* events */
    int suspended, exited;                /* threads */
    uint32_t exit_code;
    DIR* dir;
    char pattern[260];
    char dirpath[260];
} Handle;
static Handle handles[512];
#define HANDLE_BASE 0x1000u

static uint32_t handle_new(int kind) {
    for (int k = 1; k < 512; k++)
        if (handles[k].kind == H_FREE) {
            memset(&handles[k], 0, sizeof handles[k]);
            handles[k].kind = kind;
            return HANDLE_BASE + (uint32_t)k * 4;
        }
    port_fatal("out of handles");
    return 0;
}

static Handle* handle_get(uint32_t h) {
    uint32_t k = (h - HANDLE_BASE) / 4;
    if (h < HANDLE_BASE || k >= 512 || handles[k].kind == H_FREE) return 0;
    return &handles[k];
}
int port_file_fd(uint32_t handle) {
    Handle* h = handle_get(handle);
    return h && h->kind == H_FILE ? h->fd : -1;
}

/* Guest paths are Windows paths; the host sees them with forward slashes. */
static void host_path(const char* g, char* out, size_t n) { port_host_path(g, out, n); }

/* ---- process, module, version ---- */

static void k_GetModuleHandleA(CPU* c) {
    uint32_t name = ARG(0);
    if (!name) { RET(GUEST_BASE); return; }
    if (strstr(GSTR(name), "libcef")) { RET(0); return; }
    RET(hle_module_handle(GSTR(name)));
}

static void k_GetModuleFileNameA(CPU* c) {
    uint32_t mod = ARG(0), buf = ARG(1), size = ARG(2);
    const char* name = (!mod || mod == GUEST_BASE) ? port_guest_path : hle_module_name(mod);
    char tmp[300];
    if (!name) { RET(0); return; }
    if (mod && mod != GUEST_BASE) snprintf(tmp, sizeof tmp, "C:\\Windows\\System32\\%s", name), name = tmp;
    uint32_t n = (uint32_t)strlen(name);
    if (!size) { RET(0); return; }
    if (n >= size) n = size - 1;
    memcpy(GPTR(buf), name, n);
    M8(buf + n) = 0;
    RET(n);
}

static void k_GetCommandLineA(CPU* c) { RET(port_guest_cmdline_addr); }
static void k_GetStartupInfoA(CPU* c) { memset(GPTR(ARG(0)), 0, 68); M32(ARG(0)) = 68; }
static void k_GetVersion(CPU* c) { RET(0x1DB10106u); }
static void k_GetVersionExA(CPU* c) {
    uint32_t p = ARG(0);
    M32(p + 4) = 6; M32(p + 8) = 1; M32(p + 12) = 7601; M32(p + 16) = 2;
    strcpy(GSTR(p + 20), "Service Pack 1");
    RET(1);
}
static void k_GetCurrentThreadId(CPU* c) { RET(1); }
static void k_GetCurrentProcessId(CPU* c) { RET(1234); }
static void k_GetCurrentProcess(CPU* c) { RET(0xFFFFFFFFu); }
static void k_GetCurrentThread(CPU* c) { RET(0xFFFFFFFEu); }
static void k_ExitProcess(CPU* c) { port_log("guest ExitProcess(%u)", ARG(0)); exit((int)ARG(0)); }
static void k_GetLastError(CPU* c) { RET(get_last_error()); }
static void k_SetLastError(CPU* c) { set_last_error(ARG(0)); }
static void k_SetErrorMode(CPU* c) { RET(0); }
static void k_GetKeyboardType(CPU* c) { RET(ARG(0) == 0 ? 4 : ARG(0) == 1 ? 0 : 12); }
static void k_GetACP(CPU* c) { RET(1252); }
static void k_GetOEMCP(CPU* c) { RET(437); }
static void k_GetCPInfo(CPU* c) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 20);
    M32(p) = 1;
    M8(p + 4) = '?';
    RET(1);
}

static void k_GetEnvironmentVariableA(CPU* c) {
    const char* v = getenv(GSTR(ARG(0)));
    uint32_t buf = ARG(1), size = ARG(2), n;
    if (!v) { set_last_error(203); RET(0); return; }
    n = (uint32_t)strlen(v);
    if (n + 1 > size) { RET(n + 1); return; }
    strcpy(GSTR(buf), v);
    RET(n);
}

static void k_GetTempPathA(CPU* c) {
    const char* t = "C:\\Temp\\";
    if (ARG(0) > strlen(t)) strcpy(GSTR(ARG(1)), t);
    RET(strlen(t));
}

/* ---- strings ---- */

static void k_lstrlenA(CPU* c) { RET(ARG(0) ? strlen(GSTR(ARG(0))) : 0); }
static void k_lstrcpyA(CPU* c) { strcpy(GSTR(ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
static void k_lstrcpynA(CPU* c) {
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), k = 0;
    if (!n) { RET(d); return; }
    for (; k + 1 < n && M8(s + k); k++) M8(d + k) = M8(s + k);
    M8(d + k) = 0;
    RET(d);
}
static void k_lstrcatA(CPU* c) { strcat(GSTR(ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
static void k_lstrcmpA(CPU* c) { RET((uint32_t)strcmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void k_lstrcmpiA(CPU* c) { RET((uint32_t)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }

static void k_MultiByteToWideChar(CPU* c) {
    uint32_t src = ARG(2), srclen = ARG(3), dst = ARG(4), dstlen = ARG(5), n, k;
    n = (int32_t)srclen < 0 ? (uint32_t)strlen(GSTR(src)) + 1 : srclen;
    if (!dstlen) { RET(n); return; }
    if (n > dstlen) n = dstlen;
    for (k = 0; k < n; k++) M16(dst + 2 * k) = M8(src + k);
    RET(n);
}

static void k_WideCharToMultiByte(CPU* c) {
    uint32_t src = ARG(2), srclen = ARG(3), dst = ARG(4), dstlen = ARG(5), n = 0, k;
    if ((int32_t)srclen < 0) { while (M16(src + 2 * n)) n++; n++; }
    else n = srclen;
    if (!dstlen) { RET(n); return; }
    if (n > dstlen) n = dstlen;
    for (k = 0; k < n; k++) { uint16_t w = M16(src + 2 * k); M8(dst + k) = (uint8_t)(w < 256 ? w : '?'); }
    RET(n);
}

/* ---- locale: enough en-US for Delphi's SysUtils initialisation ---- */

static const char* locale_value(uint32_t type) {
    static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December"};
    static const char* smonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char* sdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    type &= 0xffff;
    if (type >= 0x38 && type <= 0x43) return months[type - 0x38];
    if (type >= 0x44 && type <= 0x4f) return smonths[type - 0x44];
    if (type >= 0x2a && type <= 0x30) return days[type - 0x2a];
    if (type >= 0x31 && type <= 0x37) return sdays[type - 0x31];
    switch (type) {
    case 0x14: return "$";          /* LOCALE_SCURRENCY */
    case 0x1b: return "0";          /* LOCALE_ICURRENCY */
    case 0x1c: return "0";          /* LOCALE_INEGCURR */
    case 0x19: return "2";          /* LOCALE_ICURRDIGITS */
    case 0x0e: return ".";          /* LOCALE_SDECIMAL */
    case 0x0f: return ",";          /* LOCALE_STHOUSAND */
    case 0x1d: return "/";          /* LOCALE_SDATE */
    case 0x1e: return ":";          /* LOCALE_STIME */
    case 0x1f: return "M/d/yyyy";   /* LOCALE_SSHORTDATE */
    case 0x20: return "dddd, MMMM d, yyyy";
    case 0x21: return "0";          /* LOCALE_IDATE */
    case 0x23: return "0";          /* LOCALE_ITIME */
    case 0x25: return "0";          /* LOCALE_ITLZERO */
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x1003: return "h:mm:ss tt";
    case 0x1009: return "1";        /* LOCALE_ICALENDARTYPE */
    case 0x100c: return "0";
    case 0x0b: return "437";        /* LOCALE_IDEFAULTCODEPAGE */
    case 0x1004: return "1252";     /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x03: return "ENU";
    case 0x07: return "USA";
    case 0x59: return "en";
    case 0x5a: return "US";
    case 0x0c: return ",";          /* LOCALE_SLIST */
    case 0x0d: return "1";          /* LOCALE_IMEASURE */
    case 0x10: return "3;0";
    case 0x11: return "2";          /* LOCALE_IDIGITS */
    case 0x12: return "1";          /* LOCALE_ILZERO */
    case 0x13: return "0123456789";
    case 0x16: return ".";
    case 0x17: return ",";
    case 0x18: return "3;0";
    default: return "";
    }
}

static void k_GetLocaleInfoA(CPU* c) {
    const char* v = locale_value(ARG(1));
    uint32_t buf = ARG(2), size = ARG(3), n = (uint32_t)strlen(v) + 1;
    if (!size) { RET(n); return; }
    if (n > size) { set_last_error(122); RET(0); return; }
    memcpy(GPTR(buf), v, n);
    RET(n);
}
static void k_GetThreadLocale(CPU* c) { RET(0x409); }
static void k_GetUserDefaultLCID(CPU* c) { RET(0x409); }
static void k_GetSystemDefaultLCID(CPU* c) { RET(0x409); }
static void k_GetUserDefaultLangID(CPU* c) { RET(0x409); }
static void k_GetSystemDefaultLangID(CPU* c) { RET(0x409); }

/* ---- memory ---- */

static void k_VirtualAlloc(CPU* c) {
    uint32_t addr = ARG(0), size = ARG(1);
    if (addr) { RET(addr); return; }
    RET(guest_alloc((size + 0xfff) & ~0xfffu, 0x10000));
}
static void k_VirtualFree(CPU* c) {
    if (ARG(2) & 0x8000) guest_free(ARG(0));
    RET(1);
}
static void k_VirtualProtect(CPU* c) { if (ARG(3)) M32(ARG(3)) = 4; RET(1); }
static void k_VirtualQuery(CPU* c) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 28);
    M32(p) = ARG(0) & ~0xfffu;
    M32(p + 4) = M32(p);
    M32(p + 12) = 0x1000;
    M32(p + 16) = 0x1000;
    M32(p + 20) = 4;
    M32(p + 24) = 0x20000;
    RET(28);
}

/* Global/Local/Heap memory: a 16-byte header holding the size precedes each block; handle == pointer. */
static uint32_t block_alloc(uint32_t size, int zero) {
    uint32_t p = guest_alloc(size + 16, 16);
    if (!p) return 0;
    M32(p) = size;
    (void)zero;
    return p + 16;
}
static uint32_t block_size(uint32_t p) { return p ? M32(p - 16) : 0; }
static void block_free(uint32_t p) { if (p) guest_free(p - 16); }
static uint32_t block_realloc(uint32_t p, uint32_t size) {
    uint32_t q = block_alloc(size, 1), old = block_size(p);
    if (q && p) memcpy(GPTR(q), GPTR(p), old < size ? old : size);
    block_free(p);
    return q;
}

uint32_t port_global_alloc(uint32_t size) { return block_alloc(size, 1); }
uint32_t port_global_size(uint32_t h) { return block_size(h); }
void port_global_free(uint32_t h) { block_free(h); }

static void k_GlobalAlloc(CPU* c) { RET(block_alloc(ARG(1), 1)); }
static void k_GlobalReAlloc(CPU* c) { RET(block_realloc(ARG(0), ARG(1))); }
static void k_GlobalFree(CPU* c) { block_free(ARG(0)); RET(0); }
static void k_GlobalLock(CPU* c) { RET(ARG(0)); }
static void k_GlobalUnlock(CPU* c) { RET(0); }
static void k_GlobalHandle(CPU* c) { RET(ARG(0)); }
static void k_GlobalSize(CPU* c) { RET(block_size(ARG(0))); }
static void k_LocalAlloc(CPU* c) { RET(block_alloc(ARG(1), 1)); }
static void k_LocalFree(CPU* c) { block_free(ARG(0)); RET(0); }
static void k_LocalReAlloc(CPU* c) { RET(block_realloc(ARG(0), ARG(1))); }
static void k_GetProcessHeap(CPU* c) { RET(0x00a00000u); }
static void k_HeapCreate(CPU* c) { RET(0x00a10000u); }
static void k_HeapDestroy(CPU* c) { RET(1); }
static void k_HeapAlloc(CPU* c) { RET(block_alloc(ARG(2), 1)); }
static void k_HeapReAlloc(CPU* c) { RET(block_realloc(ARG(2), ARG(3))); }
static void k_HeapFree(CPU* c) { block_free(ARG(2)); RET(1); }
static void k_HeapSize(CPU* c) { RET(block_size(ARG(2))); }

/* ---- TLS ---- */

static uint32_t tls_values[64];
static uint8_t tls_used[64];
static void k_TlsAlloc(CPU* c) {
    for (uint32_t k = 0; k < 64; k++)
        if (!tls_used[k]) { tls_used[k] = 1; tls_values[k] = 0; RET(k); return; }
    RET(0xFFFFFFFFu);
}
static void k_TlsFree(CPU* c) { if (ARG(0) < 64) tls_used[ARG(0)] = 0; RET(1); }
static void k_TlsGetValue(CPU* c) { set_last_error(0); RET(ARG(0) < 64 ? tls_values[ARG(0)] : 0); }
static void k_TlsSetValue(CPU* c) { if (ARG(0) < 64) tls_values[ARG(0)] = ARG(1); RET(1); }

/* ---- synchronisation ---- */

/* One guest thread runs; the only other thread the client starts is emulated by port_threads_pump. A critical
   section therefore only ever has this thread as owner, but its fields are kept as Windows keeps them. */
#define GUEST_THREAD_ID 1u
static void k_InitializeCriticalSection(CPU* c) { memset(GPTR(ARG(0)), 0, 24); M32(ARG(0) + 4) = 0xFFFFFFFFu; }
static void k_EnterCriticalSection(CPU* c) {
    uint32_t cs = ARG(0);
    M32(cs + 4) += 1;
    M32(cs + 8) += 1;
    M32(cs + 12) = GUEST_THREAD_ID;
}
static void k_LeaveCriticalSection(CPU* c) {
    uint32_t cs = ARG(0);
    M32(cs + 4) -= 1;
    if (M32(cs + 8) && !--M32(cs + 8)) M32(cs + 12) = 0;
}
static void k_DeleteCriticalSection(CPU* c) { memset(GPTR(ARG(0)), 0, 24); }

static void k_CreateEventA(CPU* c) {
    uint32_t h = handle_new(H_EVENT);
    Handle* e = handle_get(h);
    e->manual = ARG(1) != 0;
    e->signaled = ARG(2) != 0;
    RET(h);
}
static void event_set(CPU* c, int on) {
    Handle* e = handle_get(ARG(0));
    if (!e || e->kind != H_EVENT) { set_last_error(6); RET(0); return; }
    e->signaled = on;
    RET(1);
}
static void k_SetEvent(CPU* c) { event_set(c, 1); }
static void k_ResetEvent(CPU* c) { event_set(c, 0); }

/* A timed Atomics.wait in the worker rounds up to the 15.6 ms Windows timer tick, while the client asks for 1 ms
   timers (timeBeginPeriod). A precise sleep blocks until the last tick before the deadline and spins the rest; a
   coarse one, for waits with no deadline, only blocks. */
#ifdef __EMSCRIPTEN__
EM_JS(void, js_sleep, (double ms, int precise), {
    const g = globalThis, cell = g.__sleepCell || (g.__sleepCell = new Int32Array(new SharedArrayBuffer(4)));
    const end = performance.now() + ms;
    if (!precise) { Atomics.wait(cell, 0, 0, ms); return; }
    if (ms > 20) Atomics.wait(cell, 0, 0, ms - 17);
    while (performance.now() < end) {}
});
#else
static void js_sleep(double ms, int precise) { (void)precise; usleep((useconds_t)(ms * 1000)); }
#endif
void port_sleep_ms(double ms) { if (ms > 0) js_sleep(ms, 1); }
void port_idle_ms(double ms) { if (ms > 0) js_sleep(ms, 0); }

/* Whether a handle is signalled, taking the signal (when take is set) from an auto-reset event; -1 for a handle that
   cannot be waited on. A thread counts as finished once something waits for it, since the emulated thread stops as soon as
   its owner asks it to. */
int port_handle_take(uint32_t handle, int take) {
    Handle* h = handle_get(handle);
    if (!h) return -1;
    switch (h->kind) {
    case H_EVENT:
        if (!h->signaled) return 0;
        if (take && !h->manual) h->signaled = 0;
        return 1;
    case H_THREAD:
        if (take) h->exited = 1;
        return 1;
    case H_FILE: return 1;
    default: return -1;
    }
}

/* Waits up to ms, letting the emulated thread run meanwhile: WAIT_OBJECT_0, WAIT_TIMEOUT or WAIT_FAILED. */
static uint32_t wait_handle(CPU* c, uint32_t handle, uint32_t ms) {
    double start = port_now_ms(), deadline = ms == 0xFFFFFFFFu ? -1 : start + ms;
    int warned = 0;
    for (;;) {
        int got = port_handle_take(handle, 1);
        if (got < 0) { set_last_error(6); return 0xFFFFFFFFu; }
        if (got) return 0;
        double now = port_now_ms();
        if (deadline >= 0 && now >= deadline) return 0x102;
        if (!warned && now - start > 5000) {
            warned = 1;
            port_log("WaitForSingleObject(%08x) still waiting after 5 s; only the emulated thread could signal it", handle);
        }
        port_threads_pump(c);
        if (c->pend) return 0xFFFFFFFFu;
        if (deadline < 0) port_idle_ms(16);
        else port_sleep_ms(deadline - now < 16 ? deadline - now : 16);
    }
}
static void k_WaitForSingleObject(CPU* c) { RET(wait_handle(c, ARG(0), ARG(1))); }
static void k_InterlockedIncrement(CPU* c) { RET(++M32(ARG(0))); }
static void k_InterlockedDecrement(CPU* c) { RET(--M32(ARG(0))); }
static void k_InterlockedExchange(CPU* c) { uint32_t o = M32(ARG(0)); M32(ARG(0)) = ARG(1); RET(o); }
static void k_InterlockedCompareExchange(CPU* c) {
    uint32_t o = M32(ARG(0));
    if (o == ARG(2)) M32(ARG(0)) = ARG(1);
    RET(o);
}
static void k_InterlockedExchangeAdd(CPU* c) { uint32_t o = M32(ARG(0)); M32(ARG(0)) = o + ARG(1); RET(o); }
static void sleep_ms(CPU* c, uint32_t ms) {
    double until = port_now_ms() + ms;
    for (double now = port_now_ms(); now < until && !c->pend; now = port_now_ms()) {
        port_threads_pump(c);
        port_sleep_ms(until - now < 100 ? until - now : 100);
    }
}
static void k_Sleep(CPU* c) { sleep_ms(c, ARG(0)); }
/* No asynchronous procedure call is ever queued, so an alertable sleep only ends on its timeout. */
static void k_SleepEx(CPU* c) { sleep_ms(c, ARG(0)); RET(0); }
/* The client's only thread is a TCkThread: every 10 minutes it raises an event (method pointer at +0x40/+0x44,
   arguments at +0x54/+0x58), through Synchronize, i.e. on the main thread. Threads cannot run here, so the message
   pump raises that event on the same schedule. */
static uint64_t now_ms(void);
#define CK_INTERVAL_MS 600000.0
static uint32_t ck_thread, ck_handle;
static double ck_next;

void port_threads_pump(CPU* c) {
    Handle* t = handle_get(ck_handle);
    if (!ck_thread || !t || t->kind != H_THREAD || t->suspended || t->exited || now_ms() < ck_next) return;
    ck_next = now_ms() + CK_INTERVAL_MS;
    if (!M16(ck_thread + 0x42)) return;
    uint32_t saved[3] = {c->eax, c->edx, c->ecx};
    c->eax = M32(ck_thread + 0x44);
    c->edx = M32(ck_thread + 0x54);
    c->ecx = M32(ck_thread + 0x58);
    guest_call(c, M32(ck_thread + 0x40), 0);
    if (!c->pend) { c->eax = saved[0]; c->edx = saved[1]; c->ecx = saved[2]; }
}

static void k_CreateThread(CPU* c) {
    /* Delphi threads start in ThreadWrapper with a {Func, Parameter} record; Parameter is the TThread object, whose
       class name is the ShortString at VMT - 44. */
    uint32_t rec = ARG(3), obj = rec ? M32(rec + 4) : 0, vmt = obj ? M32(obj) : 0, name = vmt ? M32(vmt - 44) : 0;
    char cls[64] = "?";
    if (name >= 0x400000u && name < 0x10000000u) snprintf(cls, sizeof cls, "%.*s", M8(name), GSTR(name + 1));
    uint32_t h = handle_new(H_THREAD);
    Handle* t = handle_get(h);
    t->suspended = (ARG(4) & 4) != 0;   /* CREATE_SUSPENDED */
    t->exit_code = 259;                 /* STILL_ACTIVE */
    if (!strcmp(cls, "TCkThread")) {
        ck_thread = obj;
        ck_handle = h;
        ck_next = now_ms() + CK_INTERVAL_MS;
        port_log("CreateThread: %s emulated as a main-thread timer", cls);
    } else
        port_log("CreateThread(start=%08x, param=%08x) func=%08x class=%s not run", ARG(2), rec, rec ? M32(rec) : 0, cls);
    if (ARG(5)) M32(ARG(5)) = GUEST_THREAD_ID + 1 + (uint32_t)(t - handles);
    RET(h);
}
static void k_ResumeThread(CPU* c) {
    Handle* t = handle_get(ARG(0));
    if (!t || t->kind != H_THREAD) { set_last_error(6); RET(0xFFFFFFFFu); return; }
    RET(t->suspended);
    if (t->suspended) t->suspended--;
}
static void k_SetThreadPriority(CPU* c) {
    Handle* t = handle_get(ARG(0));
    int ok = (t && t->kind == H_THREAD) || ARG(0) == 0xFFFFFFFEu;
    if (!ok) set_last_error(6);
    RET(ok);
}
static void k_GetExitCodeThread(CPU* c) {
    Handle* t = handle_get(ARG(0));
    if (!t || t->kind != H_THREAD) { set_last_error(6); RET(0); return; }
    if (ARG(1)) M32(ARG(1)) = t->exited ? 0 : t->exit_code;
    RET(1);
}

/* ---- time ---- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void k_GetTickCount(CPU* c) { RET((uint32_t)now_ms()); }
static void k_timeGetTime(CPU* c) { RET((uint32_t)now_ms()); }
static void k_QueryPerformanceFrequency(CPU* c) { M32(ARG(0)) = 1000000; M32(ARG(0) + 4) = 0; RET(1); }
static void k_QueryPerformanceCounter(CPU* c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    M32(ARG(0)) = (uint32_t)us;
    M32(ARG(0) + 4) = (uint32_t)(us >> 32);
    RET(1);
}
static void fill_systemtime(uint32_t p, int local) {
    time_t t = time(0);
    struct tm tm;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    M16(p) = (uint16_t)(tm.tm_year + 1900); M16(p + 2) = (uint16_t)(tm.tm_mon + 1); M16(p + 4) = (uint16_t)tm.tm_wday;
    M16(p + 6) = (uint16_t)tm.tm_mday; M16(p + 8) = (uint16_t)tm.tm_hour; M16(p + 10) = (uint16_t)tm.tm_min;
    M16(p + 12) = (uint16_t)tm.tm_sec; M16(p + 14) = 0;
}
static void k_GetLocalTime(CPU* c) { fill_systemtime(ARG(0), 1); }
static void k_GetSystemTime(CPU* c) { fill_systemtime(ARG(0), 0); }
static void k_FileTimeToLocalFileTime(CPU* c) { M32(ARG(1)) = M32(ARG(0)); M32(ARG(1) + 4) = M32(ARG(0) + 4); RET(1); }
static void k_FileTimeToDosDateTime(CPU* c) { M16(ARG(1)) = 0x5021; M16(ARG(2)) = 0; RET(1); }

/* ---- files ---- */

static void fill_find_data(uint32_t p, const char* dir, const char* name) {
    char full[600];
    struct stat st;
    memset(GPTR(p), 0, 320);
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (!stat(full, &st)) {
        M32(p) = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
        M32(p + 32) = (uint32_t)((uint64_t)st.st_size >> 32);
        M32(p + 28) = (uint32_t)st.st_size;
    }
    snprintf(GSTR(p + 44), 260, "%s", name);
}

static int wild_match(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return wild_match(p + 1, s) || (*s && wild_match(p, s + 1));
    if (*p == '?') return *s && wild_match(p + 1, s + 1);
    return tolower((unsigned char)*p) == tolower((unsigned char)*s) && wild_match(p + 1, s + 1);
}

static int find_next(Handle* h, uint32_t data) {
    struct dirent* e;
    while ((e = readdir(h->dir)))
        if (wild_match(h->pattern, e->d_name)) { fill_find_data(data, h->dirpath, e->d_name); return 1; }
    return 0;
}

static void k_FindFirstFileA(CPU* c) {
    char path[600], *slash;
    host_path(GSTR(ARG(0)), path, sizeof path);
    slash = strrchr(path, '/');
    uint32_t h = handle_new(H_FIND);
    Handle* hd = handle_get(h);
    if (slash) { *slash = 0; snprintf(hd->dirpath, sizeof hd->dirpath, "%s", path); snprintf(hd->pattern, sizeof hd->pattern, "%s", slash + 1); }
    else { strcpy(hd->dirpath, "."); snprintf(hd->pattern, sizeof hd->pattern, "%s", path); }
    hd->dir = opendir(hd->dirpath);
    if (!hd->dir || !find_next(hd, ARG(1))) {
        if (hd->dir) closedir(hd->dir);
        hd->kind = H_FREE;
        set_last_error(2);
        RET(0xFFFFFFFFu);
        return;
    }
    RET(h);
}
static void k_FindNextFileA(CPU* c) {
    Handle* h = handle_get(ARG(0));
    if (!h || h->kind != H_FIND || !find_next(h, ARG(1))) { set_last_error(18); RET(0); return; }
    RET(1);
}
static void k_FindClose(CPU* c) {
    Handle* h = handle_get(ARG(0));
    if (h && h->dir) closedir(h->dir);
    if (h) h->kind = H_FREE;
    RET(1);
}

static void k_GetFileAttributesA(CPU* c) {
    char path[600];
    struct stat st;
    host_path(GSTR(ARG(0)), path, sizeof path);
    if (stat(path, &st)) { set_last_error(2); RET(0xFFFFFFFFu); return; }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}

static void k_CreateFileA(CPU* c) {
    char path[600];
    uint32_t access = ARG(1), disp = ARG(4);
    int flags = O_BINARY;
    host_path(GSTR(ARG(0)), path, sizeof path);
    if ((access & 0x40000000u) && (access & 0x80000000u)) flags |= O_RDWR;
    else if (access & 0x40000000u) flags |= O_WRONLY;
    else flags |= O_RDONLY;
    if (disp == 1) flags |= O_CREAT | O_EXCL;
    else if (disp == 2) flags |= O_CREAT | O_TRUNC;
    else if (disp == 4) flags |= O_CREAT;
    else if (disp == 5) flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0) { set_last_error(errno == ENOENT ? 2 : 5); RET(0xFFFFFFFFu); return; }
    uint32_t h = handle_new(H_FILE);
    handle_get(h)->fd = fd;
    set_last_error(0);
    RET(h);
}
static void k_ReadFile(CPU* c) {
    Handle* h = handle_get(ARG(0));
    if (!h || h->kind != H_FILE) { RET(0); return; }
    ssize_t n = read(h->fd, GPTR(ARG(1)), ARG(2));
    if (ARG(3)) M32(ARG(3)) = n > 0 ? (uint32_t)n : 0;
    RET(n >= 0);
}
static void k_WriteFile(CPU* c) {
    Handle* h = handle_get(ARG(0));
    if (!h || h->kind != H_FILE) { RET(0); return; }
    ssize_t n = write(h->fd, GPTR(ARG(1)), ARG(2));
    if (ARG(3)) M32(ARG(3)) = n > 0 ? (uint32_t)n : 0;
    RET(n >= 0);
}
static void k_SetFilePointer(CPU* c) {
    Handle* h = handle_get(ARG(0));
    if (!h || h->kind != H_FILE) { RET(0xFFFFFFFFu); return; }
    off_t r = lseek(h->fd, (int32_t)ARG(1), ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    RET((uint32_t)r);
}
static void k_GetFileSize(CPU* c) {
    Handle* h = handle_get(ARG(0));
    struct stat st;
    if (!h || h->kind != H_FILE || fstat(h->fd, &st)) { RET(0xFFFFFFFFu); return; }
    if (ARG(1)) M32(ARG(1)) = 0;
    RET((uint32_t)st.st_size);
}
static void k_GetFileType(CPU* c) { RET(handle_get(ARG(0)) ? 1 : 2); }
static void k_CloseHandle(CPU* c) {
    Handle* h = handle_get(ARG(0));
    if (h && h->kind == H_FILE) close(h->fd);
    if (h) h->kind = H_FREE;
    RET(1);
}
static void k_GetStdHandle(CPU* c) { RET(0); }
static void k_DeleteFileA(CPU* c) { char p[600]; host_path(GSTR(ARG(0)), p, sizeof p); RET(unlink(p) == 0); }
static void k_CreateDirectoryA(CPU* c) { char p[600]; host_path(GSTR(ARG(0)), p, sizeof p); RET(mkdir(p, 0755) == 0); }
static void k_GetCurrentDirectoryA(CPU* c) {
    char d[260];
    snprintf(d, sizeof d, "%s", port_guest_path);
    char* s = strrchr(d, '\\');
    if (s) *s = 0;
    if (ARG(0) > strlen(d)) strcpy(GSTR(ARG(1)), d);
    RET(strlen(d));
}
static void k_SetCurrentDirectoryA(CPU* c) { RET(1); }
static void k_GetFullPathNameA(CPU* c) {
    const char* s = GSTR(ARG(0));
    if (ARG(1) > strlen(s)) strcpy(GSTR(ARG(2)), s);
    if (ARG(3)) { char* b = strrchr(GSTR(ARG(2)), '\\'); M32(ARG(3)) = b ? (uint32_t)(uintptr_t)(b + 1) : ARG(2); }
    RET(strlen(s));
}
static void k_GetDiskFreeSpaceA(CPU* c) {
    if (ARG(1)) M32(ARG(1)) = 8;
    if (ARG(2)) M32(ARG(2)) = 512;
    if (ARG(3)) M32(ARG(3)) = 0x100000;
    if (ARG(4)) M32(ARG(4)) = 0x200000;
    RET(1);
}

/* ---- modules ---- */

static void k_LoadLibraryA(CPU* c) {
    const char* n = GSTR(ARG(0));
    if (strstr(n, "libcef") || strstr(n, "LIBCEF")) { set_last_error(126); RET(0); return; }
    RET(hle_module_handle(n));
}
static void k_LoadLibraryExW(CPU* c) {
    char n[260];
    uint32_t p = ARG(0), k = 0;
    for (; M16(p + 2 * k) && k + 1 < sizeof n; k++) n[k] = (char)M16(p + 2 * k);
    n[k] = 0;
    if (strstr(n, "libcef")) { set_last_error(126); RET(0); return; }
    RET(hle_module_handle(n));
}
static void k_FreeLibrary(CPU* c) { RET(1); }
static void k_GetProcAddress(CPU* c) {
    const char* dll = hle_module_name(ARG(0));
    char ord[16];
    const char* name = ARG(1) < 0x10000 ? (snprintf(ord, sizeof ord, "#%u", ARG(1)), ord) : GSTR(ARG(1));
    if (ARG(0) == GUEST_BASE) { RET(0); return; }
    if (!dll) { RET(0); return; }
    if (!hle_implemented(dll, name)) { set_last_error(127); RET(0); return; }   /* ERROR_PROC_NOT_FOUND */
    RET(hle_trampoline(dll, name));
}
static void k_DisableThreadLibraryCalls(CPU* c) { RET(1); }

/* ---- resources ---- */

static uint32_t res_key(uint32_t v, const char** name) {
    *name = v >= 0x10000 ? GSTR(v) : 0;
    if (*name && **name == '#') { uint32_t id = (uint32_t)atoi(*name + 1); *name = 0; return id; }
    return v;
}
static void k_FindResourceA(CPU* c) {
    const char *tn, *nn;
    uint32_t t = res_key(ARG(2), &tn), n = res_key(ARG(1), &nn), size = 0;
    uint32_t p = res_find(t, tn, n, nn, &size);
    if (!p) { set_last_error(1813); RET(0); return; }
    uint32_t h = guest_alloc(8, 16);
    M32(h) = p;
    M32(h + 4) = size;
    RET(h);
}
static void k_LoadResource(CPU* c) { RET(ARG(1)); }
static void k_LockResource(CPU* c) { RET(ARG(0) ? M32(ARG(0)) : 0); }
static void k_SizeofResource(CPU* c) { RET(ARG(1) ? M32(ARG(1) + 4) : 0); }
static void k_FreeResource(CPU* c) { RET(0); }

/* LoadStringA reads the RT_STRING table: block id/16+1, entry id%16, length-prefixed UTF-16. */
static void u_LoadStringA(CPU* c) {
    uint32_t id = ARG(1), buf = ARG(2), max = ARG(3), size = 0, k;
    uint32_t p = res_find(6, 0, id / 16 + 1, 0, &size);
    if (!p || !max) { RET(0); return; }
    for (k = 0; k < id % 16; k++) p += 2 + 2u * M16(p);
    uint32_t len = M16(p), n = len < max - 1 ? len : max - 1;
    for (k = 0; k < n; k++) M8(buf + k) = (uint8_t)M16(p + 2 + 2 * k);
    M8(buf + n) = 0;
    RET(n);
}

/* Unmapped on Windows: the low 64 KB, and here anything past the end of linear memory. The client calls this while
   walking its own call stack in an integrity check, and that walk can wander into garbage. */
static void k_IsBadReadPtr(CPU* c) {
    uint32_t p = ARG(0), n = ARG(1);
#ifdef __EMSCRIPTEN__
    uint64_t end = (uint64_t)emscripten_get_heap_size();
#else
    uint64_t end = 0x100000000ull;
#endif
    RET(p < 0x10000 || (uint64_t)p + n > end ? 1 : 0);
}

/* ---- SEH: walk the guest's fs:[0] chain the way the OS dispatcher does ---- */

#define EXCEPTION_RECORD_SIZE 80
#define CONTEXT_SIZE 0x2cc

static void fill_context(CPU* c, uint32_t ctx, uint32_t eip) {
    memset(GPTR(ctx), 0, CONTEXT_SIZE);
    M32(ctx) = 0x1003f;
    M32(ctx + 0x9c) = c->edi; M32(ctx + 0xa0) = c->esi; M32(ctx + 0xa4) = c->ebx; M32(ctx + 0xa8) = c->edx;
    M32(ctx + 0xac) = c->ecx; M32(ctx + 0xb0) = c->eax; M32(ctx + 0xb4) = c->ebp; M32(ctx + 0xb8) = eip;
    M32(ctx + 0xc4) = c->esp;
}

static void dispatch_exception(CPU* c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args, uint32_t addr) {
    uint32_t save = c->esp;
    c->esp = (c->esp - 0x400) & ~15u;
    uint32_t rec = c->esp + 0x10, ctx = rec + EXCEPTION_RECORD_SIZE + 16;
    c->esp = (c->esp - 0x100) & ~15u;
    memset(GPTR(rec), 0, EXCEPTION_RECORD_SIZE);
    M32(rec) = code; M32(rec + 4) = flags; M32(rec + 12) = addr; M32(rec + 16) = nargs > 15 ? 15 : nargs;
    for (uint32_t k = 0; k < nargs && k < 15; k++) M32(rec + 20 + 4 * k) = M32(args + 4 * k);
    fill_context(c, ctx, addr);
    for (uint32_t frame = M32(GUEST_TEB); frame != 0xFFFFFFFFu && frame; frame = M32(frame)) {
        uint32_t disp = guest_call(c, M32(frame + 4), 4, rec, frame, ctx, 0);
        if (c->pend) return;
        if (disp == 0) { c->esp = save; return; }
    }
    port_fatal("unhandled guest exception %08x at %08x", code, addr);
}

static void k_RaiseException(CPU* c) {
    port_log("RaiseException %08x from %08x", ARG(0), M32(c->esp));
    /* Delphi exceptions (0EEDFADE): args = {raise address, exception object}; object -> VMT, VMT-44 -> class name
       (short string), object+4 -> message (AnsiString). */
    if (ARG(0) == 0x0EEDFADEu && ARG(2) >= 2) {
        uint32_t obj = M32(ARG(3) + 4), vmt = obj ? M32(obj) : 0, name = vmt ? M32(vmt - 44) : 0, msg = obj ? M32(obj + 4) : 0;
        port_log("  Delphi %.*s: %s (raised at %08x)", name ? M8(name) : 0, name ? GSTR(name + 1) : "", msg ? GSTR(msg) : "",
                 M32(ARG(3)));
    }
    dispatch_exception(c, ARG(0), ARG(1), ARG(2), ARG(3), M32(c->esp));
}

static void k_RtlUnwind(CPU* c) {
    uint32_t target = ARG(0), rec = ARG(2), save = c->esp, ctx;
    if (!rec) {
        c->esp = (c->esp - 0x200) & ~15u;
        rec = c->esp;
        memset(GPTR(rec), 0, EXCEPTION_RECORD_SIZE);
        M32(rec) = 0xC0000027u;
    }
    M32(rec + 4) |= 2;
    c->esp = (c->esp - 0x400) & ~15u;
    ctx = c->esp + 0x10;
    c->esp = (c->esp - 0x10) & ~15u;
    fill_context(c, ctx, M32(save));
    for (uint32_t frame = M32(GUEST_TEB); frame != 0xFFFFFFFFu && frame && frame != target; frame = M32(GUEST_TEB)) {
        guest_call(c, M32(frame + 4), 4, rec, frame, ctx, 0);
        if (c->pend) return;
        M32(GUEST_TEB) = M32(frame);
    }
    c->esp = save;
    c->eax = ARG(3);
}

static void k_UnhandledExceptionFilter(CPU* c) { RET(0); }
static void k_SetUnhandledExceptionFilter(CPU* c) { RET(0); }

static void k_IsDebuggerPresent(CPU* c) { RET(0); }
static void k_OutputDebugStringA(CPU* c) { port_log("guest debug: %s", GSTR(ARG(0))); }
static void k_GetSystemInfo(CPU* c) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 36);
    M32(p + 4) = 0x1000; M32(p + 8) = 0x10000; M32(p + 12) = 0x7ffeffff; M32(p + 16) = 1; M32(p + 20) = 1;
    M32(p + 24) = 586; M32(p + 28) = 0x10000;
}
static void k_GlobalMemoryStatus(CPU* c) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 32);
    M32(p) = 32; M32(p + 8) = 0x7fffffff; M32(p + 12) = 0x7fffffff; M32(p + 24) = 0x7fffffff; M32(p + 28) = 0x7fffffff;
}

static const Hle kernel32[] = {
    {"kernel32.dll", "GetModuleHandleA", 1, k_GetModuleHandleA},
    {"kernel32.dll", "GetModuleFileNameA", 3, k_GetModuleFileNameA},
    {"kernel32.dll", "GetCommandLineA", 0, k_GetCommandLineA},
    {"kernel32.dll", "GetStartupInfoA", 1, k_GetStartupInfoA},
    {"kernel32.dll", "GetVersion", 0, k_GetVersion},
    {"kernel32.dll", "GetVersionExA", 1, k_GetVersionExA},
    {"kernel32.dll", "GetCurrentThreadId", 0, k_GetCurrentThreadId},
    {"kernel32.dll", "GetCurrentProcessId", 0, k_GetCurrentProcessId},
    {"kernel32.dll", "GetCurrentProcess", 0, k_GetCurrentProcess},
    {"kernel32.dll", "GetCurrentThread", 0, k_GetCurrentThread},
    {"kernel32.dll", "ExitProcess", 1, k_ExitProcess},
    {"kernel32.dll", "GetLastError", 0, k_GetLastError},
    {"kernel32.dll", "SetLastError", 1, k_SetLastError},
    {"kernel32.dll", "SetErrorMode", 1, k_SetErrorMode},
    {"kernel32.dll", "GetACP", 0, k_GetACP},
    {"kernel32.dll", "GetOEMCP", 0, k_GetOEMCP},
    {"user32.dll", "GetKeyboardType", 1, k_GetKeyboardType},
    {"kernel32.dll", "GetCPInfo", 2, k_GetCPInfo},
    {"kernel32.dll", "GetEnvironmentVariableA", 3, k_GetEnvironmentVariableA},
    {"kernel32.dll", "GetTempPathA", 2, k_GetTempPathA},
    {"kernel32.dll", "lstrlenA", 1, k_lstrlenA},
    {"kernel32.dll", "lstrcpyA", 2, k_lstrcpyA},
    {"kernel32.dll", "lstrcpynA", 3, k_lstrcpynA},
    {"kernel32.dll", "lstrcatA", 2, k_lstrcatA},
    {"kernel32.dll", "lstrcmpA", 2, k_lstrcmpA},
    {"kernel32.dll", "lstrcmpiA", 2, k_lstrcmpiA},
    {"kernel32.dll", "MultiByteToWideChar", 6, k_MultiByteToWideChar},
    {"kernel32.dll", "WideCharToMultiByte", 8, k_WideCharToMultiByte},
    {"kernel32.dll", "GetLocaleInfoA", 4, k_GetLocaleInfoA},
    {"kernel32.dll", "GetThreadLocale", 0, k_GetThreadLocale},
    {"kernel32.dll", "GetUserDefaultLCID", 0, k_GetUserDefaultLCID},
    {"kernel32.dll", "GetSystemDefaultLCID", 0, k_GetSystemDefaultLCID},
    {"kernel32.dll", "GetUserDefaultLangID", 0, k_GetUserDefaultLangID},
    {"kernel32.dll", "GetSystemDefaultLangID", 0, k_GetSystemDefaultLangID},
    {"kernel32.dll", "VirtualAlloc", 4, k_VirtualAlloc},
    {"kernel32.dll", "VirtualFree", 3, k_VirtualFree},
    {"kernel32.dll", "VirtualProtect", 4, k_VirtualProtect},
    {"kernel32.dll", "VirtualQuery", 3, k_VirtualQuery},
    {"kernel32.dll", "GlobalAlloc", 2, k_GlobalAlloc},
    {"kernel32.dll", "GlobalReAlloc", 3, k_GlobalReAlloc},
    {"kernel32.dll", "GlobalFree", 1, k_GlobalFree},
    {"kernel32.dll", "GlobalLock", 1, k_GlobalLock},
    {"kernel32.dll", "GlobalUnlock", 1, k_GlobalUnlock},
    {"kernel32.dll", "GlobalHandle", 1, k_GlobalHandle},
    {"kernel32.dll", "GlobalSize", 1, k_GlobalSize},
    {"kernel32.dll", "LocalAlloc", 2, k_LocalAlloc},
    {"kernel32.dll", "LocalFree", 1, k_LocalFree},
    {"kernel32.dll", "LocalReAlloc", 3, k_LocalReAlloc},
    {"kernel32.dll", "GetProcessHeap", 0, k_GetProcessHeap},
    {"kernel32.dll", "HeapCreate", 3, k_HeapCreate},
    {"kernel32.dll", "HeapDestroy", 1, k_HeapDestroy},
    {"kernel32.dll", "HeapAlloc", 3, k_HeapAlloc},
    {"kernel32.dll", "HeapReAlloc", 4, k_HeapReAlloc},
    {"kernel32.dll", "HeapFree", 3, k_HeapFree},
    {"kernel32.dll", "HeapSize", 3, k_HeapSize},
    {"kernel32.dll", "TlsAlloc", 0, k_TlsAlloc},
    {"kernel32.dll", "TlsFree", 1, k_TlsFree},
    {"kernel32.dll", "TlsGetValue", 1, k_TlsGetValue},
    {"kernel32.dll", "TlsSetValue", 2, k_TlsSetValue},
    {"kernel32.dll", "InitializeCriticalSection", 1, k_InitializeCriticalSection},
    {"kernel32.dll", "EnterCriticalSection", 1, k_EnterCriticalSection},
    {"kernel32.dll", "LeaveCriticalSection", 1, k_LeaveCriticalSection},
    {"kernel32.dll", "DeleteCriticalSection", 1, k_DeleteCriticalSection},
    {"kernel32.dll", "CreateEventA", 4, k_CreateEventA},
    {"kernel32.dll", "SetEvent", 1, k_SetEvent},
    {"kernel32.dll", "ResetEvent", 1, k_ResetEvent},
    {"kernel32.dll", "SleepEx", 2, k_SleepEx},
    {"kernel32.dll", "ResumeThread", 1, k_ResumeThread},
    {"kernel32.dll", "SetThreadPriority", 2, k_SetThreadPriority},
    {"kernel32.dll", "GetExitCodeThread", 2, k_GetExitCodeThread},
    {"kernel32.dll", "WaitForSingleObject", 2, k_WaitForSingleObject},
    {"kernel32.dll", "InterlockedIncrement", 1, k_InterlockedIncrement},
    {"kernel32.dll", "InterlockedDecrement", 1, k_InterlockedDecrement},
    {"kernel32.dll", "InterlockedExchange", 2, k_InterlockedExchange},
    {"kernel32.dll", "InterlockedCompareExchange", 3, k_InterlockedCompareExchange},
    {"kernel32.dll", "InterlockedExchangeAdd", 2, k_InterlockedExchangeAdd},
    {"kernel32.dll", "Sleep", 1, k_Sleep},
    {"kernel32.dll", "CreateThread", 6, k_CreateThread},
    {"kernel32.dll", "GetTickCount", 0, k_GetTickCount},
    {"kernel32.dll", "IsBadReadPtr", 2, k_IsBadReadPtr},
    {"winmm.dll", "timeGetTime", 0, k_timeGetTime},
    {"kernel32.dll", "QueryPerformanceFrequency", 1, k_QueryPerformanceFrequency},
    {"kernel32.dll", "QueryPerformanceCounter", 1, k_QueryPerformanceCounter},
    {"kernel32.dll", "GetLocalTime", 1, k_GetLocalTime},
    {"kernel32.dll", "GetSystemTime", 1, k_GetSystemTime},
    {"kernel32.dll", "FileTimeToLocalFileTime", 2, k_FileTimeToLocalFileTime},
    {"kernel32.dll", "FileTimeToDosDateTime", 3, k_FileTimeToDosDateTime},
    {"kernel32.dll", "FindFirstFileA", 2, k_FindFirstFileA},
    {"kernel32.dll", "FindNextFileA", 2, k_FindNextFileA},
    {"kernel32.dll", "FindClose", 1, k_FindClose},
    {"kernel32.dll", "GetFileAttributesA", 1, k_GetFileAttributesA},
    {"kernel32.dll", "CreateFileA", 7, k_CreateFileA},
    {"kernel32.dll", "ReadFile", 5, k_ReadFile},
    {"kernel32.dll", "WriteFile", 5, k_WriteFile},
    {"kernel32.dll", "SetFilePointer", 4, k_SetFilePointer},
    {"kernel32.dll", "GetFileSize", 2, k_GetFileSize},
    {"kernel32.dll", "GetFileType", 1, k_GetFileType},
    {"kernel32.dll", "CloseHandle", 1, k_CloseHandle},
    {"kernel32.dll", "GetStdHandle", 1, k_GetStdHandle},
    {"kernel32.dll", "DeleteFileA", 1, k_DeleteFileA},
    {"kernel32.dll", "CreateDirectoryA", 2, k_CreateDirectoryA},
    {"kernel32.dll", "GetCurrentDirectoryA", 2, k_GetCurrentDirectoryA},
    {"kernel32.dll", "SetCurrentDirectoryA", 1, k_SetCurrentDirectoryA},
    {"kernel32.dll", "GetFullPathNameA", 4, k_GetFullPathNameA},
    {"kernel32.dll", "GetDiskFreeSpaceA", 5, k_GetDiskFreeSpaceA},
    {"kernel32.dll", "LoadLibraryA", 1, k_LoadLibraryA},
    {"kernel32.dll", "LoadLibraryExA", 3, k_LoadLibraryA},
    {"kernel32.dll", "LoadLibraryExW", 3, k_LoadLibraryExW},
    {"kernel32.dll", "FreeLibrary", 1, k_FreeLibrary},
    {"kernel32.dll", "GetProcAddress", 2, k_GetProcAddress},
    {"kernel32.dll", "DisableThreadLibraryCalls", 1, k_DisableThreadLibraryCalls},
    {"kernel32.dll", "FindResourceA", 3, k_FindResourceA},
    {"kernel32.dll", "LoadResource", 2, k_LoadResource},
    {"kernel32.dll", "LockResource", 1, k_LockResource},
    {"kernel32.dll", "SizeofResource", 2, k_SizeofResource},
    {"kernel32.dll", "FreeResource", 1, k_FreeResource},
    {"user32.dll", "LoadStringA", 4, u_LoadStringA},
    {"kernel32.dll", "RaiseException", 4, k_RaiseException},
    {"kernel32.dll", "RtlUnwind", 4, k_RtlUnwind},
    {"kernel32.dll", "UnhandledExceptionFilter", 1, k_UnhandledExceptionFilter},
    {"kernel32.dll", "SetUnhandledExceptionFilter", 1, k_SetUnhandledExceptionFilter},
    {"kernel32.dll", "IsDebuggerPresent", 0, k_IsDebuggerPresent},
    {"kernel32.dll", "OutputDebugStringA", 1, k_OutputDebugStringA},
    {"kernel32.dll", "GetSystemInfo", 1, k_GetSystemInfo},
    {"kernel32.dll", "GlobalMemoryStatus", 1, k_GlobalMemoryStatus},
};

void hle_kernel32_register(void) { hle_register(kernel32, sizeof kernel32 / sizeof kernel32[0]); }
