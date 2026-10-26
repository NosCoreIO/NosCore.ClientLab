#pragma once
#include <stdint.h>
#include <math.h>
#include <intrin.h>

typedef union {
    uint32_t d;
    uint16_t w;
    struct { uint8_t l, h; } b;
} Reg;

typedef struct CPU CPU;
typedef void (*RtFn)(CPU*, uint32_t, uint32_t);

typedef struct RtAct {
    uint32_t jb[6];
    RtFn fn;
    uint32_t guest_esp;
    uint32_t host_esp;
    struct RtAct* prev;
} RtAct;

struct CPU {
    Reg r[8];
    uint32_t host_esp;
    uint32_t host_stack_top;
    uint32_t pend, pend_target;
    uint32_t cf, zf, sf, of, pf, df;
    double st[8];
    uint32_t ftop;
    uint16_t fcw, fsw;
    uint64_t mm[8];
    uint32_t lj_target;
    RtAct* acts;
    uint32_t tls_slots[4];
    uint32_t trace_pos;
    uint32_t trace[128];
};
#define RT_TRACE(c, e, r) do { uint32_t _p = (c)->trace_pos++ & 63; (c)->trace[_p * 2] = (e); (c)->trace[_p * 2 + 1] = (r); } while (0)
#define CPU_OFF_HOST_ESP 32

#define eax r[0].d
#define ecx r[1].d
#define edx r[2].d
#define ebx r[3].d
#define esp r[4].d
#define ebp r[5].d
#define esi r[6].d
#define edi r[7].d

#define R32(i) (c->r[i].d)
#define R16(i) (c->r[i].w)
#define R8L(i) (c->r[i].b.l)
#define R8H(i) (c->r[i].b.h)

#define CF (c->cf)
#define ZF (c->zf)
#define SF (c->sf)
#define OF (c->of)
#define PF (c->pf)

#define M8(a) (*(uint8_t*)(uintptr_t)(uint32_t)(a))
#define M16(a) (*(uint16_t*)(uintptr_t)(uint32_t)(a))
#define M32(a) (*(uint32_t*)(uintptr_t)(uint32_t)(a))
#define M64(a) (*(uint64_t*)(uintptr_t)(uint32_t)(a))
#define MF32(a) (*(float*)(uintptr_t)(uint32_t)(a))
#define MF64(a) (*(double*)(uintptr_t)(uint32_t)(a))

#define FS8(a) __readfsbyte(a)
#define FS16(a) __readfsword(a)
#define FS32(a) __readfsdword(a)
#define FS32_W(a, v) __writefsdword((a), (v))
#define FS16_W(a, v) __writefsword((a), (v))
#define FS8_W(a, v) __writefsbyte((a), (v))

#define PUSH(v) do { uint32_t _v = (v); c->esp -= 4; M32(c->esp) = _v; } while (0)

#define ST(i) (c->st[(c->ftop + (i)) & 7])
#define FPUSH(v) do { double _v = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = _v; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)

static __forceinline uint32_t PARITY(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (~v) & 1;
}

typedef struct { uint32_t addr; RtFn fn; uint32_t is_entry; } RtLabel;
extern const RtLabel rt_labels[];
extern const uint32_t rt_label_count;
extern const uint32_t rt_code_lo, rt_code_hi;

int rt_setjmp(uint32_t* jb);
void rt_act_push(CPU* c, RtAct* a);
void rt_act_pop(CPU* c, RtAct* a);

RtFn rt_lookup(uint32_t addr, uint32_t* is_entry);
void rt_call(CPU* c, uint32_t target, uint32_t ret);
void rt_jump(CPU* c, uint32_t target, uint32_t ret);
#define rt_tls_array(c) ((uint32_t)(uintptr_t)(c)->tls_slots)
void rt_jump_ind(CPU* c, uint32_t target, uint32_t ret, int kind);
int rt_fn_has_label(RtFn fn, uint32_t addr);
void rt_unimpl(CPU* c, uint32_t addr, const char* what);
void rt_divzero(CPU* c, uint32_t addr);
void rt_int3(CPU* c, uint32_t addr);
void rt_cpuid(CPU* c);
uint32_t rt_get_eflags(CPU* c);
void rt_set_eflags(CPU* c, uint32_t v);
uint32_t rt_shld(CPU* c, uint32_t d, uint32_t s, uint32_t n);
uint32_t rt_shrd(CPU* c, uint32_t d, uint32_t s, uint32_t n);

double rt_f80_load(uint32_t addr);
void rt_f80_store(uint32_t addr, double v);
int16_t rt_fist16(CPU* c, double v);
int32_t rt_fist32(CPU* c, double v);
int64_t rt_fist64(CPU* c, double v);
double rt_frndint(CPU* c, double v);
void rt_fbstp(CPU* c, uint32_t addr, double v);
void rt_fcom(CPU* c, double a, double b);
uint16_t rt_fsw(CPU* c);
void rt_fninit(CPU* c);

void rt_str_movsb(CPU* c);
void rt_str_movsw(CPU* c);
void rt_str_movsd(CPU* c);
void rt_str_rep_movsb(CPU* c);
void rt_str_rep_movsw(CPU* c);
void rt_str_rep_movsd(CPU* c);
void rt_str_stosb(CPU* c);
void rt_str_stosw(CPU* c);
void rt_str_stosd(CPU* c);
void rt_str_rep_stosb(CPU* c);
void rt_str_rep_stosw(CPU* c);
void rt_str_rep_stosd(CPU* c);
void rt_str_lodsb(CPU* c);
void rt_str_lodsw(CPU* c);
void rt_str_lodsd(CPU* c);
void rt_str_scasb(CPU* c);
void rt_str_cmpsb(CPU* c);
void rt_str_repne_scasb(CPU* c);
void rt_str_repne_scasw(CPU* c);
void rt_str_repe_scasb(CPU* c);
void rt_str_repe_cmpsb(CPU* c);
void rt_str_repe_cmpsw(CPU* c);
void rt_str_repe_cmpsd(CPU* c);
