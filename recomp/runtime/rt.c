#include "rt.h"
#include <string.h>

void rt_fatal(const char* fmt, ...);
void rt_call_host(CPU* c, uint32_t target);

static int is_guest(uint32_t a) { return a >= rt_code_lo && a < rt_code_hi; }

RtFn rt_lookup(uint32_t addr, uint32_t* is_entry) {
    uint32_t lo = 0, hi = rt_label_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rt_labels[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < rt_label_count && rt_labels[lo].addr == addr) {
        if (is_entry) *is_entry = rt_labels[lo].is_entry;
        return rt_labels[lo].fn;
    }
    return 0;
}

void rt_call(CPU* c, uint32_t t, uint32_t ret) {
    if (is_guest(t)) {
        RtFn f = rt_lookup(t, 0);
        if (!f) rt_fatal("no recompiled code for guest address %08x (return %08x)", t, ret);
        f(c, t, ret);
    } else {
        rt_call_host(c, t);
    }
}

void rt_jump(CPU* c, uint32_t t, uint32_t ret) { rt_call(c, t, ret); }

int rt_fn_has_label(RtFn fn, uint32_t addr) {
    uint32_t lo = 0, hi = rt_label_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rt_labels[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < rt_label_count && rt_labels[lo].addr == addr; lo++)
        if (rt_labels[lo].fn == fn) return 1;
    return 0;
}

/* kind 1: indirect jmp; kind 2: ret to an address that is not this activation's return address. */
void rt_jump_ind(CPU* c, uint32_t t, uint32_t ret, int kind) {
    uint32_t entry = 0;
    RtFn f;
    if (!is_guest(t)) {
        if (kind == 1) { rt_call_host(c, t); return; }
        c->pend = 1; c->pend_target = t; return;
    }
    f = rt_lookup(t, &entry);
    if (!f) rt_fatal("no recompiled code for indirect jump target %08x", t);
    if (entry) { f(c, t, ret); return; }
    c->pend = 1;
    c->pend_target = t;
}

void rt_act_push(CPU* c, RtAct* a) { a->prev = c->acts; c->acts = a; }

void rt_act_pop(CPU* c, RtAct* a) {
    RtAct** p = &c->acts;
    while (*p && *p != a) p = &(*p)->prev;
    if (*p) *p = a->prev;
}

void rt_unimpl(CPU* c, uint32_t addr, const char* what) { rt_fatal("unimplemented instruction at %08x: %s", addr, what); }
void rt_divzero(CPU* c, uint32_t addr) {
    volatile int zero = 0;
    (void)addr;
    c->r[0].d = 1 / zero;
}
void rt_int3(CPU* c, uint32_t addr) { rt_fatal("int3 at %08x", addr); }

void rt_cpuid(CPU* c) {
    int r[4];
    __cpuidex(r, (int)c->eax, (int)c->ecx);
    c->eax = (uint32_t)r[0]; c->ebx = (uint32_t)r[1]; c->ecx = (uint32_t)r[2]; c->edx = (uint32_t)r[3];
}

uint32_t rt_get_eflags(CPU* c) {
    return 0x202u | (c->cf & 1) | ((c->pf & 1) << 2) | ((c->zf & 1) << 6) | ((c->sf & 1) << 7) | ((c->df & 1) << 10) | ((c->of & 1) << 11);
}

void rt_set_eflags(CPU* c, uint32_t v) {
    c->cf = v & 1; c->pf = (v >> 2) & 1; c->zf = (v >> 6) & 1; c->sf = (v >> 7) & 1; c->df = (v >> 10) & 1; c->of = (v >> 11) & 1;
}

uint32_t rt_shld(CPU* c, uint32_t d, uint32_t s, uint32_t n) {
    uint32_t r = (d << n) | (s >> (32 - n));
    c->cf = (d >> (32 - n)) & 1; c->zf = r == 0; c->sf = r >> 31;
    return r;
}

uint32_t rt_shrd(CPU* c, uint32_t d, uint32_t s, uint32_t n) {
    uint32_t r = (d >> n) | (s << (32 - n));
    c->cf = (d >> (n - 1)) & 1; c->zf = r == 0; c->sf = r >> 31;
    return r;
}

/* ---- x87 ---- */

double rt_f80_load(uint32_t addr) {
    uint64_t mant = M64(addr);
    uint16_t se = M16(addr + 8);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void rt_f80_store(uint32_t addr, double v) {
    uint64_t mant = 0;
    uint16_t se = 0;
    if (v != v) { mant = 0xC000000000000000ull; se = 0x7fff; }
    else if (isinf(v)) { mant = 0x8000000000000000ull; se = (uint16_t)(0x7fff | (v < 0 ? 0x8000 : 0)); }
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);
        mant = (uint64_t)ldexp(m, 64);
        se = (uint16_t)((e - 1 + 16383) | (v < 0 ? 0x8000 : 0));
    } else if (signbit(v)) se = 0x8000;
    M64(addr) = mant;
    M16(addr + 8) = se;
}

static double fround(CPU* c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

double rt_frndint(CPU* c, double v) { return fround(c, v); }

int16_t rt_fist16(CPU* c, double v) {
    double r = fround(c, v);
    return (r >= -32768.0 && r <= 32767.0) ? (int16_t)r : (int16_t)0x8000;
}

int32_t rt_fist32(CPU* c, double v) {
    double r = fround(c, v);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (int32_t)r : (int32_t)0x80000000;
}

int64_t rt_fist64(CPU* c, double v) {
    double r = fround(c, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : (int64_t)0x8000000000000000ull;
}

void rt_fbstp(CPU* c, uint32_t addr, double v) {
    int64_t n = rt_fist64(c, v);
    int neg = n < 0, k;
    uint64_t u = (uint64_t)(neg ? -n : n);
    for (k = 0; k < 9; k++) {
        uint8_t lo = (uint8_t)(u % 10); u /= 10;
        uint8_t hi = (uint8_t)(u % 10); u /= 10;
        M8(addr + k) = (uint8_t)(lo | (hi << 4));
    }
    M8(addr + 9) = neg ? 0x80 : 0;
}

void rt_fcom(CPU* c, double a, double b) {
    c->fsw &= ~0x4500u;
    if (a != a || b != b) c->fsw |= 0x4500u;
    else if (a < b) c->fsw |= 0x0100u;
    else if (a == b) c->fsw |= 0x4000u;
}

uint16_t rt_fsw(CPU* c) { return (uint16_t)((c->fsw & ~0x3800u) | ((c->ftop & 7) << 11)); }

void rt_fninit(CPU* c) { c->fcw = 0x37f; c->fsw = 0; c->ftop = 0; }

/* ---- string instructions ---- */

#define STEP(sz) (c->df ? -(int32_t)(sz) : (int32_t)(sz))

static void cmp_flags(CPU* c, uint32_t a, uint32_t b, uint32_t bits) {
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1), msb = 1u << (bits - 1);
    uint32_t r = (a - b) & mask;
    c->cf = (a & mask) < (b & mask); c->zf = r == 0; c->sf = (r & msb) != 0;
    c->of = (((a ^ b) & (a ^ r)) & msb) != 0; c->pf = PARITY((uint8_t)r);
}

void rt_str_movsb(CPU* c) { M8(c->edi) = M8(c->esi); c->esi += STEP(1); c->edi += STEP(1); }
void rt_str_movsw(CPU* c) { M16(c->edi) = M16(c->esi); c->esi += STEP(2); c->edi += STEP(2); }
void rt_str_movsd(CPU* c) { M32(c->edi) = M32(c->esi); c->esi += STEP(4); c->edi += STEP(4); }

void rt_str_rep_movsb(CPU* c) {
    if (!c->df && c->ecx) { memmove((void*)(uintptr_t)c->edi, (void*)(uintptr_t)c->esi, c->ecx); c->esi += c->ecx; c->edi += c->ecx; c->ecx = 0; return; }
    while (c->ecx) { rt_str_movsb(c); c->ecx--; }
}
void rt_str_rep_movsw(CPU* c) { while (c->ecx) { rt_str_movsw(c); c->ecx--; } }
void rt_str_rep_movsd(CPU* c) {
    if (!c->df && c->ecx) { memmove((void*)(uintptr_t)c->edi, (void*)(uintptr_t)c->esi, c->ecx * 4); c->esi += c->ecx * 4; c->edi += c->ecx * 4; c->ecx = 0; return; }
    while (c->ecx) { rt_str_movsd(c); c->ecx--; }
}

void rt_str_stosb(CPU* c) { M8(c->edi) = R8L(0); c->edi += STEP(1); }
void rt_str_stosw(CPU* c) { M16(c->edi) = R16(0); c->edi += STEP(2); }
void rt_str_stosd(CPU* c) { M32(c->edi) = c->eax; c->edi += STEP(4); }
void rt_str_rep_stosb(CPU* c) {
    if (!c->df && c->ecx) { memset((void*)(uintptr_t)c->edi, R8L(0), c->ecx); c->edi += c->ecx; c->ecx = 0; return; }
    while (c->ecx) { rt_str_stosb(c); c->ecx--; }
}
void rt_str_rep_stosw(CPU* c) { while (c->ecx) { rt_str_stosw(c); c->ecx--; } }
void rt_str_rep_stosd(CPU* c) { while (c->ecx) { rt_str_stosd(c); c->ecx--; } }

void rt_str_lodsb(CPU* c) { R8L(0) = M8(c->esi); c->esi += STEP(1); }
void rt_str_lodsw(CPU* c) { R16(0) = M16(c->esi); c->esi += STEP(2); }
void rt_str_lodsd(CPU* c) { c->eax = M32(c->esi); c->esi += STEP(4); }

void rt_str_scasb(CPU* c) { cmp_flags(c, R8L(0), M8(c->edi), 8); c->edi += STEP(1); }
void rt_str_cmpsb(CPU* c) { cmp_flags(c, M8(c->esi), M8(c->edi), 8); c->esi += STEP(1); c->edi += STEP(1); }

void rt_str_repne_scasb(CPU* c) {
    while (c->ecx) { c->ecx--; rt_str_scasb(c); if (c->zf) break; }
}
void rt_str_repne_scasw(CPU* c) {
    while (c->ecx) { c->ecx--; cmp_flags(c, R16(0), M16(c->edi), 16); c->edi += STEP(2); if (c->zf) break; }
}
void rt_str_repe_scasb(CPU* c) {
    while (c->ecx) { c->ecx--; rt_str_scasb(c); if (!c->zf) break; }
}
void rt_str_repe_cmpsb(CPU* c) {
    while (c->ecx) { c->ecx--; rt_str_cmpsb(c); if (!c->zf) break; }
}
void rt_str_repe_cmpsw(CPU* c) {
    while (c->ecx) { c->ecx--; cmp_flags(c, M16(c->esi), M16(c->edi), 16); c->esi += STEP(2); c->edi += STEP(2); if (!c->zf) break; }
}
void rt_str_repe_cmpsd(CPU* c) {
    while (c->ecx) { c->ecx--; cmp_flags(c, M32(c->esi), M32(c->edi), 32); c->esi += STEP(4); c->edi += STEP(4); if (!c->zf) break; }
}
