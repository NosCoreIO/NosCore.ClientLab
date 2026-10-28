/* Runs lifted single-instruction tests on states supplied by difftest.py and writes the resulting states back. */
#include "rt.h"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SANDBOX 0x10000000u
#define SANDBOX_SIZE 0x1000u

typedef void (*TestFn)(CPU*);
extern const TestFn difftests[];
extern const uint32_t difftest_count;

const RtLabel rt_labels[1] = {{0, 0, 0}};
const uint32_t rt_label_count = 0;
const uint32_t rt_code_lo = 0, rt_code_hi = 0;

void rt_fatal(const char* fmt, ...) { RaiseException(0xE0000001, 0, 0, 0); }
void rt_log(const char* fmt, ...) {}
void rt_call_host_raw(CPU* c, uint32_t t) { RaiseException(0xE0000002, 0, 0, 0); }
const char* rt_host_name(uint32_t a) { return 0; }
void rt_describe_arg(uint32_t v, char* out, unsigned size) { out[0] = 0; }

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
    In in;
    Out out;
    CPU* c = (CPU*)calloc(1, sizeof(CPU));
    uint8_t* sb = (uint8_t*)VirtualAlloc((void*)(uintptr_t)SANDBOX, SANDBOX_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (argc < 3 || !sb) { fprintf(stderr, "usage: harness in out (sandbox %p)\n", sb); return 2; }
    if (fopen_s(&fi, argv[1], "rb") || fopen_s(&fo, argv[2], "wb")) return 2;
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
        if (in.test < difftest_count) {
            __try {
                difftests[in.test](c);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out.status = 1;
            }
        } else {
            out.status = 2;
        }
        for (int k = 0; k < 8; k++) out.regs[k] = c->r[k].d;
        out.flags = rt_get_eflags(c);
        memcpy(out.mem, sb, SANDBOX_SIZE);
        fwrite(&out, sizeof out, 1, fo);
    }
    fclose(fi);
    fclose(fo);
    return 0;
}
