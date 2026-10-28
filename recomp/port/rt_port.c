#include "port.h"
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

char port_guest_path[260];
char port_guest_cmdline[1024];
uint32_t port_guest_cmdline_addr;

static FILE* log_file;
static CPU* main_cpu;
static int port_trace;
int port_trace_budget;

/* While a click trace is running, each guest function seen in the entry ring is logged once. */
static uint32_t fn_seen[4096];
static uint32_t fn_trace_pos;
static int fn_trace_active;

void port_fn_trace_start(CPU* c) {
    memset(fn_seen, 0, sizeof fn_seen);
    fn_trace_pos = c->trace_pos;
    fn_trace_active = 1;
}

static void fn_trace_drain(CPU* c) {
    if (!fn_trace_active) return;
    if (port_trace_budget <= 0) { fn_trace_active = 0; return; }
    if (c->trace_pos - fn_trace_pos > 64) fn_trace_pos = c->trace_pos - 64;
    for (; fn_trace_pos != c->trace_pos; fn_trace_pos++) {
        uint32_t p = fn_trace_pos & 63, e = c->trace[p * 2], h = (e * 2654435761u) >> 20;
        while (fn_seen[h] && fn_seen[h] != e) h = (h + 1) & 4095;
        if (fn_seen[h]) continue;
        fn_seen[h] = e;
        port_log("fn %08x (ret %08x)", e, c->trace[p * 2 + 1]);
    }
}

void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (log_file) {
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        fputc('\n', log_file);
        fflush(log_file);
        va_end(ap);
    }
}

void rt_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    port_log("%s", buf);
}

static void dump_state(CPU* c) {
    if (!c) return;
    port_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
             c->r[0].d, c->r[1].d, c->r[2].d, c->r[3].d, c->r[4].d, c->r[5].d, c->r[6].d, c->r[7].d);
    for (int k = 16; k > 0; k--) {
        uint32_t p = (c->trace_pos - k) & 63;
        if (c->trace[p * 2]) port_log("  trace -%02d: %08x (ret %08x)", k, c->trace[p * 2], c->trace[p * 2 + 1]);
    }
}

void port_fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    port_log("FATAL: %s", buf);
    dump_state(main_cpu);
    exit(3);
}

void rt_fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    port_fatal("%s", buf);
}

const char* rt_host_name(uint32_t addr);
void rt_describe_arg(uint32_t v, char* out, unsigned size) { snprintf(out, size, "%08x", v); }

/* ---- paths: guest Windows paths -> host paths. In a browser worker the game files are mounted under /C:/..., under
   Node (NODERAWFS) the Windows path itself works. ---- */

static int is_web = -1;
int port_is_web(void) {
#ifdef __EMSCRIPTEN__
    if (is_web < 0) is_web = EM_ASM_INT({ return (typeof importScripts === 'function') ? 1 : 0; });
#else
    is_web = 0;
#endif
    return is_web;
}

void port_host_path(const char* guest, char* out, size_t n) {
    const char* prefix = (port_is_web() && guest[0] && guest[1] == ':') ? "/" : "";
    snprintf(out, n, "%s%s", prefix, guest);
    /* Windows paths are case-insensitive; the browser file system is not, so it is mounted in lower case. */
    for (char* p = out; *p; p++) {
        if (*p == '\\') *p = '/';
        else if (port_is_web()) *p = (char)tolower((unsigned char)*p);
    }
}

/* ---- guest memory ---- */

uint32_t guest_alloc(uint32_t size, uint32_t align) {
    void* p = 0;
    if (align < 16) align = 16;
    if (posix_memalign(&p, align, size ? size : 1)) return 0;
    memset(p, 0, size);
    return (uint32_t)(uintptr_t)p;
}

void guest_free(uint32_t p) { free(GPTR(p)); }

uint32_t guest_strdup(const char* s) {
    uint32_t p = guest_alloc((uint32_t)strlen(s) + 1, 16);
    strcpy(GSTR(p), s);
    return p;
}

static uint32_t last_error;
void set_last_error(uint32_t e) { last_error = e; }
uint32_t get_last_error(void) { return last_error; }

/* ---- import trampolines ---- */

typedef struct {
    char dll[32];
    char name[64];
    uint8_t argc;
    uint8_t soft;
    uint8_t logged;
    HleFn fn;
} Tramp;

/* Unimplemented APIs whose stdcall argument count the SDK tells us become soft stubs: log once, return 0. */
static int known_argc(const char* name) {
    int lo = 0, hi = stdcall_argc_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, r = strcmp(stdcall_argc[mid].name, name);
        if (!r) return stdcall_argc[mid].argc;
        if (r < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

#define MAX_TRAMP ((TRAMP_LIMIT - TRAMP_BASE) / 16)
static Tramp* tramps;
static uint32_t tramp_count;
static const Hle* tables[1024];
static int table_sizes[1024], table_count;

void hle_register(const Hle* table, int count) {
    if (table_count >= 1024) port_fatal("too many HLE tables");
    tables[table_count] = table;
    table_sizes[table_count++] = count;
}

static void lower(char* d, const char* s, size_t n) {
    size_t k = 0;
    for (; s[k] && k + 1 < n; k++) d[k] = (char)tolower((unsigned char)s[k]);
    d[k] = 0;
}

static const Hle* hle_find(const char* dll, const char* name) {
    char want[32], have[32];
    lower(want, dll, sizeof want);
    for (int t = 0; t < table_count; t++)
        for (int k = 0; k < table_sizes[t]; k++) {
            lower(have, tables[t][k].dll, sizeof have);
            if (!strcmp(have, want) && !strcmp(tables[t][k].name, name)) return &tables[t][k];
        }
    /* Win32 export names are unique across the system DLLs we implement, so the DLL is only a tie-breaker. */
    for (int t = 0; t < table_count; t++)
        for (int k = 0; k < table_sizes[t]; k++)
            if (!strcmp(tables[t][k].name, name)) return &tables[t][k];
    return 0;
}

uint32_t hle_trampoline(const char* dll, const char* name) {
    char d[32];
    lower(d, dll, sizeof d);
    for (uint32_t k = 0; k < tramp_count; k++)
        if (!strcmp(tramps[k].dll, d) && !strcmp(tramps[k].name, name)) return TRAMP_BASE + k * 16;
    if (tramp_count >= MAX_TRAMP) port_fatal("out of import trampolines");
    Tramp* t = &tramps[tramp_count];
    snprintf(t->dll, sizeof t->dll, "%s", d);
    snprintf(t->name, sizeof t->name, "%s", name);
    const Hle* h = hle_find(d, name);
    t->fn = h ? h->fn : 0;
    t->argc = h ? h->argc : 0;
    if (!h) {
        int a = known_argc(name);
        if (a >= 0) { t->soft = 1; t->argc = (uint8_t)a; }
    }
    return TRAMP_BASE + tramp_count++ * 16;
}

const char* rt_host_name(uint32_t addr) {
    if (addr < TRAMP_BASE || addr >= TRAMP_LIMIT) return 0;
    uint32_t k = (addr - TRAMP_BASE) / 16;
    return k < tramp_count ? tramps[k].name : 0;
}

/* Fake module handles for DLLs the guest loads by name: 0x7f000000 + index * 0x10000. */
static char modules[64][32];
static int module_count;

uint32_t hle_module_handle(const char* dll) {
    char d[32];
    const char* base = strrchr(dll, '\\');
    lower(d, base ? base + 1 : dll, sizeof d);
    if (!strchr(d, '.')) strncat(d, ".dll", sizeof d - strlen(d) - 1);
    for (int k = 0; k < module_count; k++)
        if (!strcmp(modules[k], d)) return 0x7f000000u + (uint32_t)k * 0x10000u;
    strcpy(modules[module_count], d);
    return 0x7f000000u + (uint32_t)module_count++ * 0x10000u;
}

const char* hle_module_name(uint32_t h) {
    if (h < 0x7f000000u) return 0;
    uint32_t k = (h - 0x7f000000u) / 0x10000u;
    return (int)k < module_count ? modules[k] : 0;
}

/* Called by rt.c for every call or jump to a non-guest address. */
void rt_call_host_raw(CPU* c, uint32_t target) {
    if (target >= TRAMP_BASE && target < TRAMP_LIMIT && ((target - TRAMP_BASE) % 16) == 0) {
        Tramp* t = &tramps[(target - TRAMP_BASE) / 16];
        if (!t->fn && t->soft) {
            if (!t->logged) { t->logged = 1; port_log("STUB %s!%s (returns 0) first called from %08x", t->dll, t->name, M32(c->esp)); }
            c->eax = 0;
            c->esp += 4 + 4u * t->argc;
            return;
        }
        if (!t->fn) port_fatal("unimplemented API %s!%s (called from %08x)", t->dll, t->name, M32(c->esp));
        fn_trace_drain(c);
        int traced = port_trace || (port_trace_budget > 0 && !strstr(t->name, "::") && port_trace_budget--);
        if (traced) port_log("api %s!%s from %08x esp=%08x args %08x %08x %08x %08x", t->dll, t->name, M32(c->esp), c->esp,
                             M32(c->esp + 4), M32(c->esp + 8), M32(c->esp + 12), M32(c->esp + 16));
        t->fn(c);
        if (traced) port_log("    -> %08x", c->eax);
        if (!c->pend) c->esp += 4 + (t->argc == CDECL_ARGS ? 0 : 4u * t->argc);
        return;
    }
    /* Code the guest wrote into its heap at run time. Delphi's MakeObjectInstance window thunks are
       'call BlockCode' -> 'pop ecx; jmp StdWndProc'; run those few instructions, then the recompiled StdWndProc. */
    for (int steps = 0; steps < 8; steps++) {
        uint8_t op = M8(target);
        if (op == 0xE8) { PUSH(target + 5); target = target + 5 + M32(target + 1); }
        else if (op == 0x59) { c->ecx = M32(c->esp); c->esp += 4; target += 1; }
        else if (op == 0xE9) { target = target + 5 + M32(target + 1); }
        else break;
        if (target >= rt_code_lo && target < rt_code_hi) {
            rt_call(c, target, M32(c->esp));
            return;
        }
    }
    port_fatal("call to unknown host address %08x (bytes %02x %02x %02x %02x %02x, from %08x)", target, M8(target),
               M8(target + 1), M8(target + 2), M8(target + 3), M8(target + 4), M32(c->esp));
}

/* ---- calling back into the guest ---- */

static uint32_t sentinel_depth;

/* Calls a guest function from host code. If the guest leaves through a non-local jump (a Delphi except block
   further out), c->pend stays set and the caller must return straight away so the jump keeps propagating. */
uint32_t guest_call(CPU* c, uint32_t fn, int n, ...) {
    uint32_t args[16], save_esp = c->esp, saved[4] = {c->ebx, c->esi, c->edi, c->ebp};
    uint32_t sentinel = SENTINEL_BASE + 16 * ++sentinel_depth;
    va_list ap;
    va_start(ap, n);
    for (int k = 0; k < n; k++) args[k] = va_arg(ap, uint32_t);
    va_end(ap);
    c->esp = (c->esp - 16) & ~3u;
    for (int k = n - 1; k >= 0; k--) PUSH(args[k]);
    PUSH(sentinel);
    rt_call(c, fn, sentinel);
    sentinel_depth--;
    if (c->pend) {
        if (c->pend_target == sentinel) c->pend = 0;
        else return 0;
    }
    c->esp = save_esp;
    c->ebx = saved[0]; c->esi = saved[1]; c->edi = saved[2]; c->ebp = saved[3];
    return c->eax;
}

/* ---- PE image ---- */

static uint8_t* file_bytes;
static uint32_t rsrc_rva, rsrc_size;

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static void load_image(const char* guest) {
    char path[600];
    port_host_path(guest, path, sizeof path);
    FILE* f = fopen(path, "rb");
    long size;
    if (!f) port_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file_bytes = malloc((size_t)size);
    fread(file_bytes, 1, (size_t)size, f);
    fclose(f);

    uint32_t pe = rd32(file_bytes + 0x3c);
    uint16_t nsec = rd16(file_bytes + pe + 6), optsize = rd16(file_bytes + pe + 20);
    const uint8_t* opt = file_bytes + pe + 24;
    uint32_t base = rd32(opt + 28), headers = rd32(opt + 60);
    if (base != GUEST_BASE) port_fatal("image base %08x unsupported", base);
    memcpy(GPTR(GUEST_BASE), file_bytes, headers);
    const uint8_t* sec = opt + optsize;
    for (int k = 0; k < nsec; k++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), va = rd32(sec + 12), rawsize = rd32(sec + 16), raw = rd32(sec + 20);
        uint32_t n = rawsize < vsize || !vsize ? rawsize : vsize;
        memset(GPTR(GUEST_BASE + va), 0, vsize > rawsize ? vsize : rawsize);
        if (n) memcpy(GPTR(GUEST_BASE + va), file_bytes + raw, n);
    }
    rsrc_rva = rd32(opt + 96 + 2 * 8);
    rsrc_size = rd32(opt + 96 + 2 * 8 + 4);

    /* Imports -> trampolines. */
    uint32_t imp = rd32(opt + 96 + 1 * 8), unresolved = 0;
    for (uint32_t d = GUEST_BASE + imp; M32(d + 12); d += 20) {
        const char* dll = GSTR(GUEST_BASE + M32(d + 12));
        uint32_t names = GUEST_BASE + (M32(d) ? M32(d) : M32(d + 16)), iat = GUEST_BASE + M32(d + 16);
        for (; M32(names); names += 4, iat += 4) {
            char ord[16];
            const char* name;
            if (M32(names) & 0x80000000u) { snprintf(ord, sizeof ord, "#%u", M32(names) & 0xffff); name = ord; }
            else name = GSTR(GUEST_BASE + M32(names) + 2);
            M32(iat) = hle_trampoline(dll, name);
            if (!tramps[tramp_count - 1].fn && !strcmp(tramps[tramp_count - 1].name, name)) unresolved++;
        }
    }
    port_log("image loaded, %u imports, %u without an implementation yet", tramp_count, unresolved);

    /* TLS: one static block; the guest's single fs:[0x2C] read goes to CPU.tls_slots. */
    uint32_t tls = rd32(opt + 96 + 9 * 8);
    if (tls) {
        uint32_t t = GUEST_BASE + tls, lo = M32(t), hi = M32(t + 4);
        uint32_t block = guest_alloc(hi - lo + 0x100, 16);
        memcpy(GPTR(block), GPTR(lo), hi - lo);
        M32(M32(t + 8)) = 0;
        main_cpu->tls_slots[0] = block;
    }
}

/* ---- resources (.rsrc of the guest image) ---- */

static int res_name_eq(uint32_t entry_name, const char* want) {
    uint32_t p = GUEST_BASE + rsrc_rva + (entry_name & 0x7fffffffu);
    uint16_t n = M16(p);
    if (strlen(want) != n) return 0;
    for (uint16_t k = 0; k < n; k++)
        if (toupper((unsigned char)want[k]) != toupper(M16(p + 2 + 2 * k) & 0xff)) return 0;
    return 1;
}

static uint32_t res_child(uint32_t dir, uint32_t id, const char* name) {
    uint32_t root = GUEST_BASE + rsrc_rva, d = root + dir;
    uint16_t named = M16(d + 12), ids = M16(d + 14);
    uint32_t e = d + 16;
    for (uint32_t k = 0; k < (uint32_t)named + ids; k++, e += 8) {
        uint32_t nm = M32(e), off = M32(e + 4);
        int match = name ? ((nm & 0x80000000u) && res_name_eq(nm, name)) : (!(nm & 0x80000000u) && nm == id);
        if (match || (!name && id == 0xFFFFFFFFu)) return off;
    }
    return 0xFFFFFFFFu;
}

uint32_t res_find(uint32_t type, const char* type_name, uint32_t id, const char* name, uint32_t* size) {
    if (!rsrc_rva) return 0;
    uint32_t t = res_child(0, type, type_name);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    uint32_t n = res_child(t & 0x7fffffffu, id, name);
    if (n == 0xFFFFFFFFu || !(n & 0x80000000u)) return 0;
    uint32_t l = res_child(n & 0x7fffffffu, 0xFFFFFFFFu, 0);
    if (l == 0xFFFFFFFFu || (l & 0x80000000u)) return 0;
    uint32_t leaf = GUEST_BASE + rsrc_rva + l;
    if (size) *size = M32(leaf + 4);
    return GUEST_BASE + M32(leaf);
}

/* ---- startup ---- */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nostale.js <path to client exe> [client args...]\n");
        return 2;
    }
    snprintf(port_guest_path, sizeof port_guest_path, "%s", argv[1]);
    for (char* p = port_guest_path; *p; p++)
        if (*p == '/') *p = '\\';
    snprintf(port_guest_cmdline, sizeof port_guest_cmdline, "\"%s\"", port_guest_path);
    for (int k = 2; k < argc; k++) {
        strncat(port_guest_cmdline, " ", sizeof port_guest_cmdline - strlen(port_guest_cmdline) - 1);
        strncat(port_guest_cmdline, argv[k], sizeof port_guest_cmdline - strlen(port_guest_cmdline) - 1);
    }

    {
        char dir[600], *s;
        port_host_path(argv[1], dir, sizeof dir);
        s = strrchr(dir, '/');
        if (!s) s = strrchr(dir, '\\');
        if (s) { *s = 0; if (chdir(dir)) port_log("cannot chdir to %s", dir); }
    }
    port_trace = getenv("PORT_TRACE") != 0;
    tramps = calloc(MAX_TRAMP, sizeof(Tramp));
    hle_text_register();
    hle_gdi_register();
    hle_kernel32_register();
    hle_user32_register();
    hle_audio_register();
    hle_misc_register();
    hle_d3d9_register();

    main_cpu = calloc(1, sizeof(CPU));
    main_cpu->fcw = 0x37f;
    main_cpu->fs_base = GUEST_TEB;
    M32(GUEST_TEB + 0x00) = 0xFFFFFFFFu;
    M32(GUEST_TEB + 0x04) = GUEST_STACK_TOP;
    M32(GUEST_TEB + 0x08) = GUEST_STACK_TOP - GUEST_STACK_SIZE;
    M32(GUEST_TEB + 0x18) = GUEST_TEB;
    M32(GUEST_TEB + 0x20) = 1234;
    M32(GUEST_TEB + 0x24) = 1;
    M32(GUEST_TEB + 0x30) = GUEST_PEB;
    M32(GUEST_PEB + 0x08) = GUEST_BASE;

    load_image(argv[1]);
    port_guest_cmdline_addr = guest_strdup(port_guest_cmdline);

    uint32_t pe = M32(GUEST_BASE + 0x3c);
    uint32_t entry = GUEST_BASE + M32(GUEST_BASE + pe + 24 + 16);
    CPU* c = main_cpu;
    c->esp = GUEST_STACK_TOP - 64;
    port_log("entering guest at %08x", entry);
    PUSH(SENTINEL_BASE);
    rt_call(c, entry, SENTINEL_BASE);
    port_fatal("guest entry point returned (pend=%u target=%08x)", main_cpu->pend, main_cpu->pend_target);
    return 0;
}
