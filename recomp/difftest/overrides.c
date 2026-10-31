/* Differential test of runtime/rt_game.c: each hand-written function runs against its lifted body (f_<addr>_lifted)
   on random inputs, and the registers callers keep, the stack pointer and the guest memory must match. Linux, from the
   repository root after a lift:

     G=$NOSRECOMP_WORK/recomp/gen R=recomp/runtime
     gcc -O1 -w -I$R recomp/difftest/overrides.c $R/rt.c $R/rt_game.c $G/fn_00400000.c $G/fn_00418000.c \
         $G/fn_00458000.c $G/fn_00470000.c $G/fn_00480000.c $G/fn_00488000.c $G/fn_00490000.c \
         -o /tmp/overrides -Wl,--unresolved-symbols=ignore-all -lm && /tmp/overrides

   Guest addresses are host addresses, so the heap, stack and client globals are mapped at fixed addresses. Paths
   that raise a client exception are not exercised: the lifted raise needs the whole program. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void f_0045d2e0(CPU*, uint32_t, uint32_t);
void f_0045d2e0_lifted(CPU*, uint32_t, uint32_t);
void f_00489ad0(CPU*, uint32_t, uint32_t);
void f_00489ad0_lifted(CPU*, uint32_t, uint32_t);
void f_00490264(CPU*, uint32_t, uint32_t);
void f_00490264_lifted(CPU*, uint32_t, uint32_t);

#define HEAP 0x10000000u
#define HEAP_SIZE 0x100000u
#define STACK_TOP 0x110f0000u
#define FDIV_CHECKED 0x0079604fu

typedef void (*Fn)(CPU*, uint32_t, uint32_t);
typedef void (*Setup)(CPU*);

static uint8_t heap_start[HEAP_SIZE], heap_lifted[HEAP_SIZE];
static uint32_t bump;

static uint32_t alloc(uint32_t n) { uint32_t a = bump; bump += (n + 15) & ~15u; return a; }
static float rnd(float scale) { return (float)((rand() / (double)RAND_MAX - 0.5) * scale); }
static uint32_t make_list(int n) {
    uint32_t l = alloc(0x10), items = alloc(4 * (n ? n : 1));
    M32(l + 4) = items; M32(l + 8) = (uint32_t)n;
    return l;
}

/* Runs one case through both versions from the same memory and compares the first span bytes of the heap; eax is
   compared only for the functions that return in al. */
static uint32_t span;

static int same(Setup setup, Fn mine, Fn lifted, uint32_t entry, uint32_t ret, int returns) {
    static CPU a, b;
    uint8_t fdiv = M8(FDIV_CHECKED);
    memcpy(heap_start, (void*)(uintptr_t)HEAP, span);
    for (int pass = 0; pass < 2; pass++) {
        CPU* c = pass ? &b : &a;
        memset(c, 0, sizeof *c);
        memcpy((void*)(uintptr_t)HEAP, heap_start, span);
        M8(FDIV_CHECKED) = fdiv;
        c->esp = STACK_TOP; c->ebx = 3; c->esi = 4; c->edi = 5; c->ebp = 6;
        setup(c);
        (pass ? mine : lifted)(c, entry, ret);
        c->ecx = M8(FDIV_CHECKED);
        if (!pass) memcpy(heap_lifted, (void*)(uintptr_t)HEAP, span);
    }
    return (!returns || (a.eax & 0xff) == (b.eax & 0xff)) && a.esp == b.esp && a.ebx == b.ebx && a.esi == b.esi && a.edi == b.edi &&
           a.ebp == b.ebp && a.ecx == b.ecx && a.ftop == b.ftop && !a.pend && !b.pend &&
           !memcmp(heap_lifted, (void*)(uintptr_t)HEAP, span);
}

/* ---- ray against triangle ---- */

static void ray_setup(CPU* c) {
    c->eax = HEAP; c->edx = HEAP + 12; c->ecx = HEAP + 24;
    PUSH(HEAP + 48); PUSH(HEAP + 36); PUSH(HEAP + 0x100); PUSH(0x00489bfeu);
}

static long test_ray(int cases) {
    long bad = 0;
    span = 0x200;
    for (int n = 0; n < cases; n++) {
        memset((void*)(uintptr_t)HEAP, 0, span);
        for (int k = 0; k < 15; k++) MF32(HEAP + 4 * k) = rand() % 10 ? rnd(rand() % 2 ? 4 : 400) : 0;
        if (n % 3 == 0) { MF32(HEAP + 12) = 0; MF32(HEAP + 16) = -1; MF32(HEAP + 20) = 0; }
        MF32(HEAP + 0x100) = -12345.f;
        M8(FDIV_CHECKED) = (n & 7) ? 1 : 0;
        bad += !same(ray_setup, f_0045d2e0, f_0045d2e0_lifted, 0x0045d2e0u, 0x00489bfeu, 1);
    }
    return bad;
}

/* ---- height ray over a model's node tree ---- */

static uint32_t height_root, height_from, height_to, height_frame;
static int index_lists;

static uint32_t make_node(int depth) {
    uint32_t node = alloc(0x40), move = alloc(12), turn = alloc(8);
    for (int k = 0; k < 3; k++) MF32(move + 4 * k) = rnd(depth ? 4 : 0.5f);
    for (int k = 0; k < 4; k++) M16(turn + 2 * k) = (uint16_t)((rand() % 65536 - 32768) / (rand() % 4 + 1));
    if (rand() % 3 == 0) { M16(turn) = 0; M16(turn + 2) = 0; M16(turn + 4) = 0; M16(turn + 6) = 32767; }
    int groups = rand() % 4;
    uint32_t g = alloc(2 + 7 * groups);
    M16(g) = (uint16_t)groups;
    for (int i = 0; i < groups; i++) M16(g + 2 + 7 * i + 5) = (uint16_t)(rand() % index_lists);
    int kids = depth < 2 ? rand() % 3 : 0;
    uint32_t children = make_list(kids);
    for (int i = 0; i < kids; i++) M32(M32(children + 4) + 4 * i) = make_node(depth + 1);
    M32(node + 4) = move; M32(node + 8) = turn; M32(node + 0x1c) = g; M32(node + 0x20) = children;
    return node;
}

static void height_setup(CPU* c) {
    c->eax = height_root; c->edx = height_from; c->ecx = height_to;
    PUSH(height_frame); PUSH(0x00489ce3u);
}

static long test_height(int cases) {
    long bad = 0;
    span = 0x20000;
    MF32(0x007864d8u) = 3.0518509447574615e-05f;
    for (int n = 0; n < cases; n++) {
        memset((void*)(uintptr_t)HEAP, 0, span);
        bump = HEAP + 0x1000;
        uint32_t model = alloc(0x40);
        int verts = 3 + rand() % 40;
        uint32_t vb = alloc(2 + 12 * verts);
        for (int k = 0; k < verts; k++)
            for (int j = 0; j < 3; j++) MF32(vb + 2 + 12 * k + 4 * j) = rnd(j == 1 ? 1 : 6);
        index_lists = 1 + rand() % 4;
        uint32_t lists = make_list(index_lists);
        for (int i = 0; i < index_lists; i++) {
            int tris = rand() % 30;
            uint32_t il = alloc(2 + 6 * tris + 6);
            M16(il) = (uint16_t)(3 * tris - (rand() % 5 == 0));
            for (int k = 0; k < 3 * tris; k++) M16(il + 2 + 2 * k) = (uint16_t)(rand() % verts);
            M32(M32(lists + 4) + 4 * i) = il;
        }
        M32(model + 0x10) = vb; M32(model + 0x1c) = lists;
        height_root = make_node(0);
        height_frame = alloc(0x20) + 0x10;
        uint32_t nearest = alloc(4);
        height_from = alloc(12); height_to = alloc(12);
        M32(height_frame - 4) = model; M32(height_frame + 8) = nearest; MF32(nearest) = 1e30f;
        MF32(height_from) = rnd(4); MF32(height_from + 4) = 5 + rnd(2); MF32(height_from + 8) = rnd(4);
        MF32(height_to) = MF32(height_from) + rnd(n % 2 ? 4 : 0.4f);
        MF32(height_to + 4) = MF32(height_from + 4) - 1 + (n % 2 ? rnd(4) : 0);
        MF32(height_to + 8) = MF32(height_from + 8) + rnd(0.4f);
        M8(FDIV_CHECKED) = (n & 7) ? 1 : 0;
        bad += !same(height_setup, f_00489ad0, f_00489ad0_lifted, 0x00489ad0u, 0x00489ce3u, 0);
    }
    return bad;
}

/* ---- grid path search ---- */

static const uint8_t steps[24] = { 0, 1, 0, 255, 255, 1, 1, 255, 255, 0, 1, 0, 255, 255, 1, 1, 2, 3, 0, 1, 6, 7, 4, 5 };
static uint32_t path_start, path_goal, path_grid;
static uint8_t path_mask;

static uint32_t make_array(uint32_t data, uint32_t capacity, uint32_t count) {
    uint32_t o = alloc(0x40);
    M32(o + 4) = count; M32(o + 8) = capacity; M32(o + 0x20) = data;
    return o;
}

static void path_setup(CPU* c) {
    c->eax = path_start; c->edx = path_goal; c->ecx = path_grid;
    PUSH(path_mask); PUSH(HEAP + 0x500); PUSH(0x004931e7u);
}

static long test_path(int cases) {
    long bad = 0;
    span = 0x100000;
    memcpy((void*)0x007873b0u, steps, sizeof steps);
    for (int n = 0; n < cases; n++) {
        memset((void*)(uintptr_t)HEAP, 0, span);
        bump = HEAP + 0x600;
        int w = 1 + rand() % 60, h = 1 + rand() % 60, blocked = rand() % 60;
        uint32_t grid = alloc(4 + (uint32_t)(w * h));
        M16(grid) = (uint16_t)w; M16(grid + 2) = (uint16_t)h;
        for (int k = 0; k < w * h; k++) M8(grid + 4 + k) = rand() % 100 < blocked ? (uint8_t)(1 << rand() % 3) : 0;
        M32(0x0091a2dcu) = make_array(HEAP + 0x10000, 100000, rand() & 1 ? 5000 : 0);
        M32(0x0091a2e0u) = make_array(HEAP + 0x80000, 100000, rand() & 1 ? 5000 : 0);
        M32(0x0091a2e4u) = make_array(HEAP + 0xf0000, 100000, 0);
        M32(HEAP + 0x500) = make_array(HEAP + 0xf8000, 4000, 7);
        int sx = rand() % (w + 2) - 1, sy = rand() % (h + 2) - 1, gx = rand() % (w + 2) - 1, gy = rand() % (h + 2) - 1;
        if (n % 50 == 0) { gx = sx; gy = sy; }
        path_start = (uint16_t)sx | (uint32_t)(uint16_t)sy << 16;
        path_goal = (uint16_t)gx | (uint32_t)(uint16_t)gy << 16;
        path_grid = n % 97 ? grid : 0;
        path_mask = (uint8_t)(rand() % 8);
        bad += !same(path_setup, f_00490264, f_00490264_lifted, 0x00490264u, 0x004931e7u, 1);
    }
    return bad;
}

int main(int argc, char** argv) {
    if (mmap((void*)(uintptr_t)HEAP, 0x1100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
        mmap((void*)0x00780000, 0x1a0000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    srand(argc > 1 ? (unsigned)atoi(argv[1]) : 1);
    long ray = test_ray(200000), height = test_height(20000), path = test_path(20000);
    printf("mismatches: ray/triangle %ld of 200000, height %ld of 20000, path %ld of 20000\n", ray, height, path);
    return ray || height || path;
}
