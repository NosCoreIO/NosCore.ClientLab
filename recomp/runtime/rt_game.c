/* Hand-written versions of hot client functions. Each one replaces the lifted code at the same entry (lift.py lists them
   in OVERRIDES and names the lifted body f_<addr>_lifted) and must leave the same guest state the callers read. The
   lifted x87 code computes in double and rounds on each store to a float, so the arithmetic here does the same, in the
   same order. */
#include "rt.h"
#include <string.h>

void f_0045d2e0_lifted(CPU*, uint32_t, uint32_t);

/* Calls a lifted function from here, with ret_at standing in for the return address the original call pushes. */
static void guest_call(CPU* RT_RESTRICT c, void (*fn)(CPU*, uint32_t, uint32_t), uint32_t entry, uint32_t ret_at,
                       uint32_t a, uint32_t d, uint32_t cx) {
    c->eax = a; c->edx = d; c->ecx = cx;
    PUSH(ret_at);
    fn(c, entry, ret_at);
}

typedef struct { float x, y, z; } Vec3;

static Vec3 vload(uint32_t a) { Vec3 v = { MF32(a), MF32(a + 4), MF32(a + 8) }; return v; }
static Vec3 vsub(Vec3 a, Vec3 b) { Vec3 r = { (float)((double)a.x - b.x), (float)((double)a.y - b.y), (float)((double)a.z - b.z) }; return r; }
static Vec3 vcross(Vec3 a, Vec3 b) {
    Vec3 r = { (float)((double)a.y * b.z - (double)a.z * b.y), (float)((double)a.z * b.x - (double)a.x * b.z),
               (float)((double)a.x * b.y - (double)a.y * b.x) };
    return r;
}
static double vdot(Vec3 a, Vec3 b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }

/* Ray against triangle: a = the vertex passed second, b = the one passed third. Returns 1 and the distance along the
   ray on a front-facing hit. The client runs it on every triangle of every object under each entity to find the
   height it stands at. */
static int ray_triangle(Vec3 orig, Vec3 dir, Vec3 v0, Vec3 a, Vec3 b, float* t) {
    Vec3 e1 = vsub(b, v0), e2 = vsub(a, v0), p = vcross(dir, e2);
    float det = (float)vdot(e1, p);
    /* x87 compares: "ja" is taken only when ordered, "jb" also when unordered. */
    if (0.0001 > det) return 0;
    Vec3 s = vsub(orig, v0);
    float u = (float)vdot(s, p);
    if (u < 0.0 || u != u || u > det) return 0;
    Vec3 q = vcross(s, e1);
    float v = (float)vdot(dir, q);
    if (v < 0.0 || v != v || (double)u + v > det) return 0;
    /* The division helper first runs a one-off check for the Pentium FDIV bug, which records 1. */
    if (M8(0x0079604fu) == 0) M8(0x0079604fu) = 1;
    *t = (float)(vdot(e2, q) / det);
    return 1;
}

/* eax = ray origin, edx = direction, ecx = first vertex, stack: distance out, second and third vertex; ret 0xc. */
void f_0045d2e0(CPU* RT_RESTRICT c, uint32_t entry, uint32_t ret) {
    if (entry != 0x0045d2e0u) { f_0045d2e0_lifted(c, entry, ret); return; }
    uint32_t sp = c->esp;
    float t;
    int hit = ray_triangle(vload(c->eax), vload(c->edx), vload(c->ecx), vload(M32(sp + 8)), vload(M32(sp + 12)), &t);
    if (hit) MF32(M32(sp + 4)) = t;
    c->eax = (uint32_t)hit;
    c->esp = sp + 16;
    if (M32(sp) != ret) rt_jump_ind(c, M32(sp), ret, 2);
}

void f_00490264_lifted(CPU*, uint32_t, uint32_t);
void f_00477c68(CPU*, uint32_t, uint32_t);
void f_00477f30(CPU*, uint32_t, uint32_t);

static int grid_inside(int16_t x, int16_t y, int16_t w, int16_t h) { return x >= 0 && x < w && y >= 0 && y < h; }

/* Grid path search (eax = start, edx = goal, each x | y << 16; ecx = walkability grid; stack: path list, blocking
   mask; ret 8). A breadth-first flood from the start over the 8 neighbours, alternating between two global queues and
   recording in a global byte grid the direction each cell was reached by; then walks back from the goal and appends
   the directions to the path list. Returns 1 in al when the goal was reached. The client runs it for every entity
   that moves. The queue bounds are 16-bit, as in the original. */
void f_00490264(CPU* RT_RESTRICT c, uint32_t entry, uint32_t ret) {
    if (entry != 0x00490264u) { f_00490264_lifted(c, entry, ret); return; }
    enum { QUEUES = 0x0091a2dcu, VISITED = 0x0091a2e4u, STEP_X = 0x007873b0u, STEP_Y = 0x007873b8u, BACK = 0x007873c0u };
    uint32_t sp = c->esp, start = c->eax, goal = c->edx, grid = c->ecx, list = M32(sp + 4);
    uint8_t mask = M8(sp + 8);
    uint32_t res = 0;
    if (start == goal) {
        guest_call(c, f_00477c68, 0x00477c68u, 0x0049028eu, M32(list), 0, c->ecx);
        res = 1;
        goto done;
    }
    if (!grid) goto done;
    int16_t w = (int16_t)M16(grid), h = (int16_t)M16(grid + 2);
    if (!grid_inside((int16_t)start, (int16_t)(start >> 16), w, h) || !grid_inside((int16_t)goal, (int16_t)(goal >> 16), w, h))
        goto done;
    guest_call(c, f_00477c68, 0x00477c68u, 0x004902f6u, M32(list), 0, c->ecx);
    int32_t n = (int32_t)w * h;
    for (int k = 0; k < 2; k++)
        if ((int32_t)M32(M32(QUEUES + 4 * k) + 4) < n) guest_call(c, f_00477c68, 0x00477c68u, 0x00490322u, M32(QUEUES + 4 * k), n, c->ecx);
    if ((int32_t)M32(M32(VISITED) + 4) < n) guest_call(c, f_00477c68, 0x00477c68u, 0x00490344u, M32(VISITED), n, c->ecx);
    if (n >= 0) memset((void*)(uintptr_t)M32(M32(VISITED) + 0x20), 0xff, (size_t)n);
    int16_t first[2] = { 0, 0 }, end[2] = { 1, 0 };
    M32(M32(M32(QUEUES) + 0x20)) = start;
    int cur = 0;
    uint32_t at = 0;
    do {
        int added = 0;
        for (int32_t i = first[cur], left = end[cur] - first[cur]; left > 0; i++, left--) {
            uint32_t p = M32(M32(M32(QUEUES + 4 * cur) + 0x20) + 4 * (uint32_t)i);
            for (int dir = 0; dir < 8; dir++) {
                int16_t x = (int16_t)((uint16_t)p + (int8_t)M8(STEP_X + dir));
                int16_t y = (int16_t)((uint16_t)(p >> 16) + (int8_t)M8(STEP_Y + dir));
                at = (uint16_t)x | (uint32_t)(uint16_t)y << 16;
                if (!grid_inside(x, y, w, h)) continue;
                int32_t cell = (int32_t)y * w + x;
                if (M8(grid + 4 + cell) & mask) continue;
                uint32_t seen = M32(M32(VISITED) + 0x20) + cell;
                if (M8(seen) != 0xff) continue;
                M8(seen) = (uint8_t)dir;
                int32_t tail = end[cur ^ 1];
                M32(M32(M32(QUEUES + 4 * (cur ^ 1)) + 0x20) + 4 * (uint32_t)tail) = at;
                if (++tail >= n) goto done;
                end[cur ^ 1] = (int16_t)tail;
                added = 1;
                if (at == goal) break;
            }
            if (at == goal) break;
        }
        if (!added) goto done;
        first[cur] = end[cur];
        cur ^= 1;
    } while (at != goal);
    for (int32_t steps = 1;; ) {
        int16_t x = (int16_t)at, y = (int16_t)(at >> 16);
        if (!grid_inside(x, y, w, h)) goto done;
        uint8_t dir = M8(M32(M32(VISITED) + 0x20) + (int32_t)y * w + x);
        if (dir > 7) goto done;
        guest_call(c, f_00477f30, 0x00477f30u, 0x0049058eu, M32(list), dir, c->ecx);
        uint8_t back = M8(BACK + dir);
        x = (int16_t)(x + (int8_t)M8(STEP_X + back));
        y = (int16_t)(y + (int8_t)M8(STEP_Y + back));
        at = (uint16_t)x | (uint32_t)(uint16_t)y << 16;
        if (++steps > n) goto done;
        if (at == start) { res = 1; break; }
    }
done:
    c->eax = res;
    c->esp = sp + 12;
    if (M32(sp) != ret) rt_jump_ind(c, M32(sp), ret, 2);
}

void f_00489ad0_lifted(CPU*, uint32_t, uint32_t);
void f_0045d2ac(CPU*, uint32_t, uint32_t);
void f_0045cb44(CPU*, uint32_t, uint32_t);
void f_0045cc94(CPU*, uint32_t, uint32_t);
void f_0045b988(CPU*, uint32_t, uint32_t);
void f_0045ba60(CPU*, uint32_t, uint32_t);

/* Lists are [+4] item array, [+8] count; the lifted getter raises on a bad index. */
static int list_has(uint32_t list, int32_t i) { return i >= 0 && i < (int32_t)M32(list + 8); }
static uint32_t list_at(uint32_t list, int32_t i) { return M32(M32(list + 4) + 4 * (uint32_t)i); }

/* One node of a model for the height ray: the ray's two points are moved into the node's space, every triangle of
   the node's index groups is tested, and the children follow with the moved points. The model (vertices, index
   lists), the hit flag and the nearest distance live in the caller's frame, which is passed down. Returns 0 without
   touching anything when a group index is out of range, so the caller can run the lifted body and its exception. */
static int height_node(CPU* RT_RESTRICT c, uint32_t node, uint32_t from, uint32_t to, uint32_t frame) {
    uint32_t model = M32(frame - 4), lists = M32(model + 0x1c), groups = M32(node + 0x1c);
    uint32_t count = M16(groups);
    for (uint32_t g = 0; g < count; g++)
        if (!list_has(lists, (int16_t)M16(groups + 2 + 7 * g + 5))) return 0;
    uint32_t saved = c->esp, l = (saved - 0xa0) & ~15u;
    uint32_t rot = l, mat = l + 0x10, from_copy = l + 0x50, to_copy = l + 0x5c, from_t = l + 0x68, to_t = l + 0x74,
             diff = l + 0x80, dir_n = l + 0x8c;
    c->esp = l;
    for (int k = 0; k < 3; k++) { M32(from_copy + 4 * k) = M32(from + 4 * k); M32(to_copy + 4 * k) = M32(to + 4 * k); }
    guest_call(c, f_0045d2ac, 0x0045d2acu, 0x00489b00u, M32(node + 8), rot, c->ecx);
    guest_call(c, f_0045cb44, 0x0045cb44u, 0x00489b17u, rot, M32(node + 4), mat);
    guest_call(c, f_0045cc94, 0x0045cc94u, 0x00489b28u, from_copy, mat, from_t);
    guest_call(c, f_0045cc94, 0x0045cc94u, 0x00489b39u, to_copy, mat, to_t);
    guest_call(c, f_0045b988, 0x0045b988u, 0x00489b4au, to_t, from_t, diff);
    guest_call(c, f_0045ba60, 0x0045ba60u, 0x00489b58u, diff, dir_n, c->ecx);
    Vec3 orig = vload(from_t), dir = vload(dir_n);
    uint32_t verts = M32(model + 0x10) + 2, out = M32(frame + 8);
    int vertical = dir.x == 0 && dir.z == 0;
    for (uint32_t g = 0; g < count; g++) {
        uint32_t idx = list_at(lists, (int16_t)M16(groups + 2 + 7 * g + 5));
        for (int32_t k = 0; k < (int32_t)M16(idx); k += 3) {
            uint32_t tri = idx + 2 + 2 * (uint32_t)k;
            float t;
            /* The ray is straight up in practice, and then ray_triangle's u is the 2D cross product of the origin and
               the second vertex around the first: the same value bit for bit (but the sign of a zero) whenever u is
               not NaN, so rejecting here on u < 0 or NaN rejects exactly what it would. */
            if (vertical) {
                uint32_t p0 = verts + 12 * M16(tri), pa = verts + 12 * M16(tri + 4);
                float sx = (float)((double)orig.x - MF32(p0)), sz = (float)((double)orig.z - MF32(p0 + 8));
                float px = (float)((double)dir.y * (float)((double)MF32(pa + 8) - MF32(p0 + 8)));
                float pz = (float)-((double)dir.y * (float)((double)MF32(pa) - MF32(p0)));
                if (!((float)((double)sx * px + (double)sz * pz) >= 0.0)) continue;
            }
            if (!ray_triangle(orig, dir, vload(verts + 12 * M16(tri)), vload(verts + 12 * M16(tri + 4)),
                              vload(verts + 12 * M16(tri + 2)), &t))
                continue;
            M8(frame - 5) = 1;
            if (!(t >= MF32(out))) MF32(out) = t;
        }
    }
    uint32_t children = M32(node + 0x20);
    int32_t n = (int32_t)M32(children + 8);
    for (int32_t i = 0; i < n; i++) {
        uint32_t child = list_at(children, i);
        if (height_node(c, child, from_t, to_t, frame)) continue;
        PUSH(frame);
        guest_call(c, f_00489ad0_lifted, 0x00489ad0u, 0x00489c89u, child, from_t, to_t);
        c->esp += 4;
    }
    c->esp = saved;
    return 1;
}

/* eax = node, edx = ray origin, ecx = a second point on the ray, stack: the model walker's frame. */
void f_00489ad0(CPU* RT_RESTRICT c, uint32_t entry, uint32_t ret) {
    uint32_t sp = c->esp;
    if (entry != 0x00489ad0u || !height_node(c, c->eax, c->edx, c->ecx, M32(sp + 4))) {
        f_00489ad0_lifted(c, entry, ret);
        return;
    }
    c->esp = sp + 4;
    if (M32(sp) != ret) rt_jump_ind(c, M32(sp), ret, 2);
}
