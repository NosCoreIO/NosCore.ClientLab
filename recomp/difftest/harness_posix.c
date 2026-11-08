/* POSIX build of harness.c: runs lifted single-instruction tests on states supplied by difftest.py and writes the
   resulting states back. Guest addresses are 32-bit, so the sandbox is mapped at its fixed low address and the
   harness can be a native 64-bit build. Faults (#DE, bad accesses, rt_fatal) jump back and report status 1. */
#include "rt.h"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SANDBOX 0x10000000u
#define SANDBOX_SIZE 0x1000u

typedef void (*TestFn)(CPU*);
extern const TestFn difftests[];
extern const uint32_t difftest_count;

const RtLabel rt_labels[1] = {{0, 0, 0}};
const uint32_t rt_label_count = 0;
const uint32_t rt_code_lo = 0, rt_code_hi = 0;

static sigjmp_buf fault_jb;
void rt_fatal(const char* fmt, ...) { (void)fmt; siglongjmp(fault_jb, 1); }
void rt_log(const char* fmt, ...) { (void)fmt; }
void rt_call_host_raw(CPU* c, uint32_t t) { (void)c; (void)t; siglongjmp(fault_jb, 1); }
const char* rt_host_name(uint32_t a) { (void)a; return 0; }
void rt_describe_arg(uint32_t v, char* out, unsigned size) { (void)v; (void)size; out[0] = 0; }

static void on_fault(int sig) { (void)sig; siglongjmp(fault_jb, 1); }

#pragma pack(push, 1)
typedef struct {
    uint32_t test, regs[8], flags, seed;
    double st[8];
    uint32_t ftop, fcw, fpu_used;
} In;
typedef struct {
    uint32_t status, regs[8], flags;
    uint8_t mem[SANDBOX_SIZE];
} Out;
#pragma pack(pop)

static uint32_t xorshift(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

int main(int argc, char** argv) {
    FILE *fi, *fo;
    static In in;
    static Out out;
    static CPU* volatile c;
    uint8_t* sb;
    struct sigaction sa;
    if (argc < 3) { fprintf(stderr, "usage: harness in out\n"); return 2; }
    c = (CPU*)calloc(1, sizeof(CPU));
    sb = (uint8_t*)mmap((void*)(uintptr_t)SANDBOX, SANDBOX_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (sb != (uint8_t*)(uintptr_t)SANDBOX) { fprintf(stderr, "cannot map the sandbox\n"); return 2; }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    if (!(fi = fopen(argv[1], "rb")) || !(fo = fopen(argv[2], "wb"))) return 2;
    while (fread(&in, sizeof in, 1, fi) == 1) {
        uint32_t s = in.seed ? in.seed : 1;
        for (uint32_t k = 0; k < SANDBOX_SIZE / 4; k++) ((uint32_t*)sb)[k] = xorshift(&s);
        if (in.fpu_used) memcpy(sb + 0xF00, in.st, 32);
        memset(c, 0, sizeof *c);
        for (int k = 0; k < 8; k++) c->r[k].d = in.regs[k];
        rt_set_eflags(c, in.flags);
        for (int k = 0; k < 8; k++) c->st[k] = in.st[k];
        c->ftop = in.ftop;
        c->fcw = (uint16_t)in.fcw;
        out.status = 0;
        if (in.test >= difftest_count) out.status = 2;
        else if (sigsetjmp(fault_jb, 1)) out.status = 1;
        else difftests[in.test](c);
        for (int k = 0; k < 8; k++) out.regs[k] = c->r[k].d;
        out.flags = rt_get_eflags(c);
        memcpy(out.mem, sb, SANDBOX_SIZE);
        fwrite(&out, sizeof out, 1, fo);
    }
    fclose(fi);
    fclose(fo);
    return 0;
}
