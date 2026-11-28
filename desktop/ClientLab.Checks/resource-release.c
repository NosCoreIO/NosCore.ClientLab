/* Released textures and buffers are freed once nothing can reach them (hle_d3d9.c reap): never while bound, kept by
   a state block, held as a level surface, locked or still referenced, and exactly once. Build with emcc together with
   recomp/port/render.c (-sERROR_ON_UNDEFINED_SYMBOLS=0) and run under Node. */
#include "../../recomp/port/hle_d3d9.c"
#include <stdio.h>

static int live;
uint32_t guest_alloc(uint32_t size, uint32_t align) {
    void* p = 0;
    if (posix_memalign(&p, align < 16 ? 16 : align, size ? size : 1)) return 0;
    memset(p, 0, size);
    live++;
    return (uint32_t)(uintptr_t)p;
}
void guest_free(uint32_t p) { if (p) { live--; free(GPTR(p)); } }

static CPU cpu;
static uint32_t stack[16];
static uint32_t release(uint32_t o) {
    cpu.esp = (uint32_t)(uintptr_t)stack;
    stack[1] = o;
    res_release(&cpu);
    return cpu.eax;
}

static int failed;
static void expect(int ok, const char* what) { if (!ok) { printf("FAIL: %s (live allocations %d)\n", what, live); failed = 1; } }

int main(void) {
    vt_texture = 0x1000; vt_vb = 0x2000; vt_ib = 0x3000;
    int before = live;

    uint32_t bound = make_texture(64, 64, 0, 21);
    int texture_allocations = live - before;
    r_set_texture(0, bound);
    expect(release(bound) == 0, "the last Release did not return 0");
    reap(&cpu);
    expect(live - before == texture_allocations, "a texture still bound was freed");
    r_set_texture(0, 0);
    reap(&cpu);
    expect(live == before, "an unbound, released texture was not freed with its levels");

    uint32_t held = make_texture(32, 32, 3, 21), level = M32(M32(held + 40) + 4);
    M32(level + 4)++;   /* GetSurfaceLevel */
    release(held);
    reap(&cpu);
    expect(live > before, "a texture whose level surface is still held was freed");
    release(level);
    reap(&cpu);
    expect(live == before, "the texture was not freed once its level surface was released");

    uint32_t recorded = make_texture(16, 16, 1, 21);
    r_set_texture(1, recorded);
    blocks[7].state = malloc(r_state_size());
    r_state_save(blocks[7].state);
    r_set_texture(1, 0);
    release(recorded);
    reap(&cpu);
    expect(live > before, "a texture a state block can restore was freed");
    free(blocks[7].state);
    blocks[7].state = 0;
    reap(&cpu);
    expect(live == before, "the texture was not freed once the state block was gone");

    uint32_t vb = make_buffer(vt_vb, O_VB, 4096, 0), ib = make_buffer(vt_ib, O_IB, 512, 101);
    r_set_stream(vb, 0, 24);
    r_set_indices(ib);
    M32(vb + 64) = 1;   /* locked */
    release(vb); release(ib);
    reap(&cpu);
    expect(live - before == 4, "a bound buffer was freed");
    r_set_stream(0, 0, 0); r_set_indices(0);
    reap(&cpu);
    expect(live - before == 2, "the unbound index buffer was not freed, or the locked vertex buffer was");
    M32(vb + 64) = 0;
    reap(&cpu);
    expect(live == before, "the unlocked vertex buffer was not freed");

    uint32_t kept = make_texture(8, 8, 1, 21);
    M32(kept + 4) = 2;   /* AddRef */
    release(kept);
    reap(&cpu);
    expect(live > before, "a texture with a reference left was freed");
    release(kept);
    expect(release(kept) == 0 && nretired == 1, "releasing past zero queued the texture twice");
    reap(&cpu);
    expect(live == before && nretired == 0, "the released texture was not freed exactly once");

    uint32_t vertex_only = make_texture(16, 16, 1, 21);
    r_set_texture(2, vertex_only);
    blocks[9].state = malloc(r_state_size()); blocks[9].mask = malloc(r_state_size());
    r_state_save(blocks[9].state);
    r_state_mask(3, blocks[9].mask);   /* VERTEXSTATE: cannot restore textures */
    r_set_texture(2, 0);
    release(vertex_only);
    reap(&cpu);
    expect(live == before, "a state block that cannot restore textures kept one alive");
    free(blocks[9].state); free(blocks[9].mask); blocks[9].state = blocks[9].mask = 0;

    uint32_t again = make_texture(8, 8, 1, 21);
    r_set_texture(0, again);
    release(again);                 /* zero while bound: queued */
    M32(again + 4)++;               /* GetTexture */
    release(again);                 /* zero again: must not be queued twice */
    expect(nretired == 1, "a texture released to zero twice was queued twice");
    r_set_texture(0, 0);
    reap(&cpu);
    expect(live == before && nretired == 0, "a texture released to zero twice was not freed exactly once");

    uint32_t tagged = make_texture(8, 8, 2, 21), tagged_level = M32(M32(tagged + 40) + 4);
    PrivateData* d = calloc(1, sizeof *d);
    d->obj = tagged_level; d->data = malloc(1); d->next = private_data; private_data = d;
    release(tagged);
    reap(&cpu);
    expect(live == before && !private_data, "a level surface's private data outlived its texture");

    uint32_t surface = make_surface(8, 8, 21, 0);
    int surface_allocations = live - before;
    release(surface);
    reap(&cpu);
    expect(live - before == surface_allocations, "a device surface was freed on its own");

    if (failed) return 1;
    printf("PASS: released textures and buffers are freed once unbound, not kept by a state block, a held level, a lock "
           "or a reference, and exactly once, even when released to zero twice; state blocks keep only what they can restore; level surfaces' private data goes with them; device surfaces are left alone.\n");
    return 0;
}
