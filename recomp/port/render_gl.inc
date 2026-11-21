/* WebGL2 backend, included at the end of render.c (it needs R, Vtx and texel). In the browser worker the vertices
   are still transformed by fetch(); the GPU rasterises, shades the two texture stages, blends and depth-tests. */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* The WebGL calls run on the page's renderer worker (web/gl.js, which documents the commands). This thread records a
   frame's drawing into glq and posts it once per Present, so the game never waits on WebGL and the two threads run in
   parallel. A command is never split between posts. */
EM_JS(int, js_gl_available, (void), {
    if (typeof OffscreenCanvas === 'undefined' || typeof importScripts !== 'function' || !self.glPort) return 0;
    const probe = new OffscreenCanvas(1, 1).getContext('webgl2');
    if (!probe) return 0;
    self.glCommandPoolSupported = false;
    if (typeof SharedArrayBuffer === 'function' && ENV.PORT_GL_COMMAND_POOL !== '0') {
        try {
            const bytes = new Uint8Array(new SharedArrayBuffer(4));
            probe.bindBuffer(probe.ARRAY_BUFFER, probe.createBuffer());
            probe.bufferData(probe.ARRAY_BUFFER, bytes, probe.STREAM_DRAW);
            let supported = probe.getError() === 0;
            probe.bufferSubData(probe.ARRAY_BUFFER, 0, bytes, 0, 4);
            supported &&= probe.getError() === 0;
            probe.bindTexture(probe.TEXTURE_2D, probe.createTexture());
            probe.texImage2D(probe.TEXTURE_2D, 0, probe.RGBA, 1, 1, 0, probe.RGBA, probe.UNSIGNED_BYTE, bytes, 0);
            supported &&= probe.getError() === 0;
            probe.texSubImage2D(probe.TEXTURE_2D, 0, 0, 0, 1, 1, probe.RGBA, probe.UNSIGNED_BYTE, bytes, 0);
            self.glCommandPoolSupported = supported && probe.getError() === 0;
        } catch { /* Use ordinary transferable buffers on implementations without shared uploads. */ }
    }
    const lose = probe.getExtension('WEBGL_lose_context');
    if (lose) lose.loseContext();
    return 1;
});

EM_JS(void, js_gl_submit, (const uint8_t* q, int len), {
    const slotBytes = 8 * 1024 * 1024;
    if (self.glCommandPoolSupported && len <= slotBytes && ENV.PORT_GL_COMMAND_POOL !== '0') {
        let pool = self.glCommandPool;
        if (!pool) {
            pool = self.glCommandPool = { states: new Int32Array(new SharedArrayBuffer(12)), bytes: new Uint8Array(new SharedArrayBuffer(3 * slotBytes)), next: 0 };
            self.glPort.postMessage({ type: 'command-pool', states: pool.states.buffer, bytes: pool.bytes.buffer, slotBytes });
        }
        for (let n = 0; n < 3; n++) {
            const slot = (pool.next + n) % 3;
            if (Atomics.compareExchange(pool.states, slot, 0, 1) !== 0) continue;
            pool.bytes.set(HEAPU8.subarray(q, q + len), slot * slotBytes);
            Atomics.store(pool.states, slot, 2);
            self.glPort.postMessage({ type: 'commands', slot, bytes: len });
            pool.next = (slot + 1) % 3;
            return;
        }
    }
    // Oversized/loading bursts and a busy pool retain the original ordered
    // transfer path. The game never waits for a pool slot to become available.
    const copy = HEAPU8.slice(q, q + len).buffer;
    self.glPort.postMessage(copy, [copy]);
});

/* The client never sleeps between frames: left alone it renders hundreds of frames a second, saturating the GPU
   process until the page itself stalls. The page shows one frame per display refresh from a queue of two and counts
   the frames it has shown in vsync[0]; after posting a frame, Present waits while more than two are in flight (one on
   the renderer thread, two queued), which paces the game to the display and lets one slow frame be absorbed by the
   spare instead of repeating a refresh. A renderer slower than the game (a software GL) still shows frames, so Present
   keeps waiting while the count moves: resynchronising after one short wait let the queue grow without bound, so the
   display fell further behind until the page ran out of memory. Only a count that stops for a second (page hidden or
   covered) resynchronises it, and until it moves again each frame waits one refresh at most, which keeps the game
   near 60 frames a second. */
EM_JS(void, js_gl_frame_posted, (void), {
    const V = self.vsync;
    self.glPosted = (self.glPosted || 0) + 1;
    if (!V) return;
    let shown = Atomics.load(V, 0);
    if (self.glPosted - shown <= 2) return;
    const patience = self.glStalledAt === shown ? 16 : 1000;
    let deadline = performance.now() + patience;
    for (;;) {
        const left = deadline - performance.now();
        if (left <= 0) break;
        Atomics.wait(V, 0, shown, Math.min(left, 16));
        const now = Atomics.load(V, 0);
        if (now !== shown) { shown = now; deadline = performance.now() + patience; }
        if (self.glPosted - shown <= 2) return;
    }
    self.glStalledAt = shown;
    self.glPosted = shown + 2;
});

/* The frame as 32-bit BGRA rows, top row first, read by the renderer thread after everything posted before. */
EM_JS(void, js_gl_read, (int w, int h, uint8_t* out, int pitch), {
    const done = new Int32Array(new SharedArrayBuffer(4)), pixels = new SharedArrayBuffer(w * h * 4);
    self.glPort.postMessage({ type: 'read', w, h, done, pixels });
    for (let waited = 0; Atomics.load(done, 0) === 0 && waited < 10; waited++) Atomics.wait(done, 0, 0, 1000);
    const px = new Uint8Array(pixels);
    for (let y = 0; y < h; y++)
        for (let x = 0, s = (h - 1 - y) * w * 4, d = out + y * pitch; x < w; x++, s += 4, d += 4) {
            HEAPU8[d] = px[s + 2]; HEAPU8[d + 1] = px[s + 1]; HEAPU8[d + 2] = px[s]; HEAPU8[d + 3] = 255;
        }
});

static uint8_t* glq;
static uint32_t glq_len, glq_cap;
#define GLQ_POST_AT (16u << 20)   /* post early while loading pours in textures */

static uint8_t* glq_take(uint32_t bytes) {
    bytes = (bytes + 3) & ~3u;
    if (glq_len + bytes > glq_cap) {
        uint32_t cap = glq_cap ? glq_cap : 4u << 20;
        while (cap < glq_len + bytes) cap *= 2;
        glq = realloc(glq, cap);
        glq_cap = cap;
    }
    uint8_t* p = glq + glq_len;
    glq_len += bytes;
    return p;
}
static void glq_word(int32_t v) { memcpy(glq_take(4), &v, 4); }
static void glq_data(const void* src, uint32_t n) {
    uint8_t* p = glq_take(n);
    memcpy(p, src, n);
    memset(p + n, 0, ((n + 3) & ~3u) - n);
}
static void glq_post(void) {
    if (!glq_len) return;
    js_gl_submit(glq, (int)glq_len);
    glq_len = 0;
}
static void glq_op(int32_t op) {
    if (glq_len > GLQ_POST_AT) glq_post();
    glq_word(op);
}

static uint32_t gl_backbuffer;
static void gl_flush(void);
#define FRAME_VERTS (1u << 18)
static uint8_t* gl_verts;

static int gl_tnl_primed;   /* 0 until the GPU-path program has had every uniform and the layout set */
static uint32_t gl_batch_verts, gl_batch_bytes;   /* small draws merged so far (gl_tnl_batch) */

static void gl_start(uint32_t backbuffer) {
    gl_backbuffer = backbuffer;
    gl_tnl_primed = 0;
    gl_batch_verts = gl_batch_bytes = 0;
    gl_on = js_gl_available();
    if (gl_on) {
        if (!gl_verts) gl_verts = malloc(FRAME_VERTS * 40);
        glq_op(1); glq_word((int32_t)SW(backbuffer)); glq_word((int32_t)SH(backbuffer));
        port_log("render: WebGL2 on the renderer thread");
    }
}

/* Textures are uploaded (level 0, then mipmapped) when something changed them: +56 is 2 once uploaded. When only
   rectangles of level 0 changed since (hle_d3d9.c changed()), only their bounding box is sent. */
static uint32_t gl_texture(uint32_t tex) {
    if (!tex) return 0;
    uint32_t s = M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;
    if (!s || !SBITS(s)) return 0;
    if (M32(tex + 56) != 2) {
        gl_flush();
        double t0 = prof_mark();
        uint32_t w = SW(s), h = SH(s), size = w | h << 16;
        if (M32(tex + 88) || M32(tex + 92) != size) {
            glq_op(2); glq_word((int32_t)tex); glq_word((int32_t)w); glq_word((int32_t)h);
            surface_rgba(s, glq_take(w * h * 4));
        } else {
            uint32_t l = M32(tex + 80) & 0xffff, t = M32(tex + 80) >> 16, r = M32(tex + 84) & 0xffff, b = M32(tex + 84) >> 16;
            if (SFMT(s) == FMT_DXT1 || SFMT(s) == FMT_DXT3 || SFMT(s) == FMT_DXT5) { l &= ~3u; t &= ~3u; r = (r + 3) & ~3u; b = (b + 3) & ~3u; }
            if (r > w) r = w;
            if (b > h) b = h;
            if (l < r && t < b) {
                glq_op(10); glq_word((int32_t)tex); glq_word((int32_t)l); glq_word((int32_t)t); glq_word((int32_t)(r - l)); glq_word((int32_t)(b - t));
                surface_rgba_rect(s, l, t, r - l, b - t, glq_take((r - l) * (b - t) * 4));
            }
        }
        M32(tex + 56) = 2;
        M32(tex + 88) = 0;
        M32(tex + 92) = size;
        prof_upload += prof_mark() - t0;
    }
    return tex;
}

/* A frame's draws are recorded and handed to JavaScript in one call: the client issues thousands of tiny draws per
   frame and each call into JavaScript costs far more than the vertices it carries. Consecutive triangle draws with
   identical state share one command. Anything that must see the drawing so far (a texture upload, a clear, a
   readback, present) runs the list first. */
#define MAX_CMDS 8192
typedef struct { int32_t prim, first, count, st[41]; } GlCmd;
static GlCmd gl_cmds[MAX_CMDS];
static uint32_t gl_ncmds, gl_nverts;

static void gl_tnl_flush_batch(void);

static void gl_flush_list(void) {
    if (!gl_ncmds) return;
    double t = prof_mark();
    glq_op(4); glq_word((int32_t)gl_nverts); glq_word((int32_t)gl_ncmds);
    glq_data(gl_verts, gl_nverts * 40);
    glq_data(gl_cmds, gl_ncmds * (uint32_t)sizeof *gl_cmds);
    prof_submit += prof_mark() - t;
    prof_gl_draws += gl_ncmds;
    gl_ncmds = gl_nverts = 0;
}

static void gl_flush(void) {
    gl_flush_list();
    gl_tnl_flush_batch();
}

/* The command a draw appends to: the current one when it is a triangle list with the same state, else a new one. */
static GlCmd* gl_command(int32_t prim, const int32_t* st, uint32_t verts) {
    gl_tnl_flush_batch();
    if (gl_nverts + verts > FRAME_VERTS || gl_ncmds == MAX_CMDS) gl_flush_list();
    GlCmd* last = gl_ncmds ? &gl_cmds[gl_ncmds - 1] : 0;
    if (last && prim == 4 && last->prim == 4 && r_words_equal(last->st, st, 41)) return last;
    GlCmd* c = &gl_cmds[gl_ncmds++];
    c->prim = prim;
    c->first = (int32_t)gl_nverts;
    c->count = 0;
    memcpy(c->st, st, sizeof c->st);
    return c;
}

static void put_vertex(uint8_t* dst, const Vtx* v, float tw, float th) {
    float* f = (float*)dst;
    /* The real w, negative behind the camera, so the GPU clips triangles that cross the camera plane. */
    float w = v->w != 0 ? v->w : 1e-6f;
    f[0] = ((v->x + 0.5f) / tw * 2 - 1) * w;
    f[1] = (1 - (v->y + 0.5f) / th * 2) * w;
    f[2] = (v->z * 2 - 1) * w;
    f[3] = w;
    memcpy(dst + 16, &v->diffuse, 4);
    memcpy(dst + 20, v->uv, 16);
    memcpy(dst + 36, &v->fog, 4);
}

/* Each distinct texture-stage setup is logged once (first 48), to spot fixed-function features not emulated. */
static void log_stage_setup(void) {
    if (prof_detail <= 0) return;   /* a search per draw; only when profiling (PORT_PROFILE=1) */
    static uint32_t seen[48][10];
    static int nseen;
    uint32_t key[10] = {R.tss[0][1], R.tss[0][2] << 8 | R.tss[0][3], R.tss[0][4], R.tss[0][11], R.tss[0][24],
                        R.tss[1][1], R.tss[1][2] << 8 | R.tss[1][3], R.tss[1][4], R.tss[1][11], R.tss[1][24]};
    for (int k = 0; k < nseen; k++) if (!memcmp(seen[k], key, sizeof key)) return;
    if (nseen >= 48) return;
    memcpy(seen[nseen++], key, sizeof key);
    port_log("stage setup: s0 op %u args %04x aop %u tci %x ttff %x | s1 op %u args %04x aop %u tci %x ttff %x | fvf %x",
             key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], key[8], key[9], R.fvf);
}

/* Each distinct blend / alpha-test / depth / texture-format combination is logged once (first 64). */
static void log_raster_setup(void) {
    if (prof_detail <= 0) return;   /* a search per draw; only when profiling (PORT_PROFILE=1) */
    static uint32_t seen[64][6];
    static int nseen;
    uint32_t tex = R.tex_obj[0], s = tex && M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;
    uint32_t key[6] = {R.rs[27] << 16 | R.rs[19] << 8 | R.rs[20], R.rs[15] << 16 | R.rs[25] << 8 | (R.rs[24] & 255),
                       R.rs[7] << 16 | R.rs[14] << 8 | R.rs[23], s ? SFMT(s) : 0, R.rs[28], R.rs[22]};
    for (int k = 0; k < nseen; k++) if (!memcmp(seen[k], key, sizeof key)) return;
    if (nseen >= 64) return;
    memcpy(seen[nseen++], key, sizeof key);
    port_log("raster setup: blend %u %u->%u | alpha test %u func %u ref %u | z %u write %u func %u | tex fmt %u | fog %u cull %u",
             R.rs[27], R.rs[19], R.rs[20], R.rs[15], R.rs[25], R.rs[24] & 255, R.rs[7], R.rs[14], R.rs[23], key[3], R.rs[28], R.rs[22]);
}

static void gl_state(int32_t* st) {
    static int32_t cached[41];
    log_stage_setup();
    log_raster_setup();
    if (!gl_state_dirty) {
        memcpy(st, cached, sizeof cached);
        // Texture contents/validity can change without changing the binding.
        // Keep checking uploads in draw order even when scalar state is cached.
        st[12] = (int32_t)gl_texture(R.tex_obj[0]);
        st[13] = (int32_t)gl_texture(R.tex_obj[1]);
        return;
    }
    memset(st, 0, 41 * sizeof *st);
    for (int s = 0; s < 2; s++) {
        for (int k = 0; k < 6; k++) st[6 * s + k] = (int32_t)R.tss[s][1 + k];
        st[12 + s] = (int32_t)gl_texture(R.tex_obj[s]);
        st[29 + 5 * s] = (int32_t)R.samp[s][5];
        st[29 + 5 * s + 1] = (int32_t)R.samp[s][6];
        st[29 + 5 * s + 2] = (int32_t)R.samp[s][7];
        st[29 + 5 * s + 3] = (int32_t)R.samp[s][1];
        st[29 + 5 * s + 4] = (int32_t)R.samp[s][2];
    }
    st[14] = (int32_t)R.rs[60]; st[15] = (int32_t)R.rs[15]; st[16] = (int32_t)R.rs[25]; st[17] = (int32_t)R.rs[24];
    st[18] = (int32_t)R.rs[27]; st[19] = (int32_t)R.rs[19]; st[20] = (int32_t)R.rs[20];
    st[21] = (int32_t)(R.rs[171] ? R.rs[171] : 1);
    st[22] = has_depth ? (int32_t)R.rs[7] : 0; st[23] = (int32_t)R.rs[14]; st[24] = (int32_t)R.rs[23];
    draw_area(&st[25], &st[26], &st[27], &st[28]);
    st[38] = (int32_t)R.rs[34];
    st[39] = (int32_t)(R.rs[28] != 0);
    st[40] = (int32_t)R.rs[22];
    memcpy(cached, st, sizeof cached);
    gl_state_dirty = 0;
}

static void gl_draw_list(uint32_t prim, Vtx* v, uint32_t n) {
    if (R.target != gl_backbuffer || n == 0) return;
    float tw = (float)SW(R.target), th = (float)SH(R.target);
    int32_t st[41];
    gl_state(st);
    if (prim < 4 || prim > 6) {
        static const int32_t gl_prim[4] = {0, 0, 1, 3};   /* POINTS, LINES, LINE_STRIP */
        if (n > FRAME_VERTS) n = FRAME_VERTS;
        GlCmd* c = gl_command(gl_prim[prim & 3], st, n);
        for (uint32_t k = 0; k < n; k++) put_vertex(gl_verts + 40 * gl_nverts++, &v[k], tw, th);
        c->count += (int32_t)n;
        return;
    }
    uint32_t tris = prim == 4 ? n / 3 : n - 2;
    GlCmd* cmd = 0;
    for (uint32_t t = 0; t < tris; t++) {
        if (!cmd || gl_nverts + 3 > FRAME_VERTS) cmd = gl_command(4, st, 3 * (tris - t));
        uint32_t a, b, c;
        if (prim == 4) { a = 3 * t; b = a + 1; c = a + 2; }
        else if (prim == 5) { a = t + (t & 1); b = t + 1 - (t & 1); c = t + 2; }   /* odd strip triangles keep the winding */
        else { a = 0; b = t + 1; c = t + 2; }
        put_vertex(gl_verts + 40 * gl_nverts++, &v[a], tw, th);
        put_vertex(gl_verts + 40 * gl_nverts++, &v[b], tw, th);
        put_vertex(gl_verts + 40 * gl_nverts++, &v[c], tw, th);
        cmd->count += 3;
    }
}

/* Vertex-buffer draws go to the GPU with the game's own buffers: a map's scenery is hundreds of thousands of triangles
   a frame, which transforming, lighting and re-uploading on the CPU (fetch(), put_vertex()) cannot keep up with. The
   vertex shader does what fetch() does; draws using texture-coordinate generation or texture transforms stay on the
   CPU path. Parameter block (int32 unless noted):
   0 GL primitive, 1 vertex/index count, 2 indexed, 3 first vertex or index byte offset, 4 index type,
   5..8 vertex buffer id, data, size, dirty, 9..12 index buffer id, data, size, dirty, 13 stride, 14 attribute base,
   15 normal offset, 16 diffuse offset, 17..20 stage 0 and 1 texture coordinate offset and size (-1: none),
   21 lit, 22..24 diffuse/ambient/emissive from the vertex colour, 25 lights, 26 fog mode,
   floats: 27 world, 43 view, 59 projection, 75 viewport x y w h, 79 target w h, 81 material diffuse,
   85 ambient, 89 emissive, 93 global ambient, 96 fog start end density, 99 lights (20 floats each),
   259..299 the 41 state words, 300/301 stage 0/1 coordinate generation, 302/303 texture transform flags,
   304 has a normal, floats: 305 stage 0 and 321 stage 1 texture matrix, 337 groups changed since the last draw. */
static int32_t gl_tnl_params[338], gl_tnl_last[338];

/* Which uniform groups and vertex layout differ from the previous GPU draw (see tnl() in web/gl.js). */
static int32_t gl_tnl_changes(const int32_t* p) {
    if (!gl_tnl_primed) { gl_tnl_primed = 1; return 255; }
#define TNL_DIFF(from, to) (!r_words_equal(p + (from), gl_tnl_last + (from), (to) - (from)))
    int32_t mask = 0;
    if (p[5] != gl_tnl_last[5] || TNL_DIFF(13, 21)) mask |= 1;
    if (TNL_DIFF(27, 43)) mask |= 2;
    if (TNL_DIFF(43, 75)) mask |= 4;
    if (TNL_DIFF(75, 81)) mask |= 8;
    if (!r_tnl_material_equal(p, gl_tnl_last)) mask |= 16;
    if (TNL_DIFF(26, 27) || TNL_DIFF(96, 99)) mask |= 32;
    if (TNL_DIFF(300, 337)) mask |= 64;
    if (TNL_DIFF(259, 300)) mask |= 128;
#undef TNL_DIFF
    return mask;
}

static int gl_tnl_reject(int reason) { prof_cpu_reason[reason]++; return 0; }

static void gl_tnl_submit(int32_t* p) {
    p[337] = gl_tnl_changes(p);
    r_tnl_copy(gl_tnl_last, p);
    prof_gl_draws++;
    double t = prof_mark();
    /* Words 0..26, then the groups that changed in gl.js's GROUPS order, then data the renderer must upload. */
    static const int32_t groups[][3] = {{2, 27, 43}, {4, 43, 75}, {8, 75, 81}, {16, 81, 96}, {32, 96, 99}, {64, 300, 337}, {128, 259, 300}};
    int32_t mask = p[337];
    glq_op(5); glq_word(mask);
    glq_data(p, 27 * 4);
    for (size_t g = 0; g < sizeof groups / sizeof *groups; g++)
        if (mask & groups[g][0]) glq_data(p + groups[g][1], 4u * (uint32_t)(groups[g][2] - groups[g][1]));
    if (mask & 16) glq_data(p + 99, 80u * (uint32_t)p[25]);
    if (p[5] == -3 || p[8]) glq_data(GPTR((uint32_t)p[6]), (uint32_t)p[7]);
    if (p[2] && p[12]) glq_data(GPTR((uint32_t)p[10]), (uint32_t)p[11]);
    prof_submit += prof_mark() - t;
}

/* Most of a frame's draws are a few vertices each (sprites, billboards, every glyph of text), and each GL draw costs
   far more than its vertices. A small draw is transformed by its world matrix here, which is what the vertex shader
   does with uWorld for positions and normals, and appended as a triangle list; consecutive ones whose parameters
   otherwise match go to the GPU as one draw with an identity world. */
#define BATCH_DRAW_VERTS 64
#define BATCH_BYTES (1u << 20)
static uint8_t gl_batch[BATCH_BYTES];
static int32_t gl_batch_key[338], gl_batch_params[338];

static void gl_tnl_flush_batch(void) {
    if (!gl_batch_verts) return;
    int32_t* p = gl_batch_params;
    float* f = (float*)p;
    r_tnl_copy(p, gl_batch_key);
    p[0] = 4; p[1] = (int32_t)gl_batch_verts; p[2] = 0; p[3] = 0;
    p[5] = -3; p[6] = (int32_t)(uintptr_t)gl_batch; p[7] = (int32_t)gl_batch_bytes; p[8] = 2;
    p[14] = 0;
    memset(f + 27, 0, 64);
    f[27] = f[32] = f[37] = f[42] = 1;
    gl_batch_verts = gl_batch_bytes = 0;
    gl_tnl_submit(p);
}

/* Everything but the draw's buffers, its position in them and the world matrix. */
static int gl_batch_matches(const int32_t* p) {
    return r_tnl_batch_equal(p, gl_batch_key);
}

static int gl_tnl_batch(const int32_t* p, uint32_t prim, uint32_t n, int indexed, int idx32, uint32_t data, uint32_t base,
                        uint32_t size, uint32_t idata, uint32_t isize, uint32_t start) {
    const float* m = R.world.m;
    uint32_t stride = (uint32_t)p[13], isz = idx32 ? 4 : 2;
    uint32_t tris = prim == 4 ? n / 3 : n >= 3 ? n - 2 : 0, bytes = 3 * tris * stride;
    if (n > BATCH_DRAW_VERTS || !tris || bytes > BATCH_BYTES || m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return 0;
    if (indexed && (uint64_t)(start + n) * isz > isize) return 0;
    uint32_t verts[BATCH_DRAW_VERTS];
    for (uint32_t k = 0; k < n; k++) {
        uint32_t v = indexed ? (idx32 ? M32(idata + (start + k) * 4) : M16(idata + (start + k) * 2)) : start + k;
        if ((uint64_t)base + ((uint64_t)v + 1) * stride > size) return 0;
        verts[k] = data + base + v * stride;
    }
    if (gl_batch_verts && (!gl_batch_matches(p) || gl_batch_bytes + bytes > BATCH_BYTES)) gl_tnl_flush_batch();
    if (!gl_batch_verts) r_tnl_copy(gl_batch_key, p);
    int32_t normal = p[15];
    for (uint32_t t = 0; t < tris; t++)
        for (int j = 0; j < 3; j++) {
            uint32_t k = prim == 4 ? 3 * t + (uint32_t)j
                       : prim == 5 ? (j == 0 ? t + (t & 1) : j == 1 ? t + 1 - (t & 1) : t + 2)   /* odd strip triangles keep the winding */
                       : (j == 0 ? 0 : t + (uint32_t)j);
            uint8_t* out = gl_batch + gl_batch_bytes;
            memcpy(out, GPTR(verts[k]), stride);
            float x, y, z;
            memcpy(&x, out, 4); memcpy(&y, out + 4, 4); memcpy(&z, out + 8, 4);
            float w[3] = {x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13], x * m[2] + y * m[6] + z * m[10] + m[14]};
            memcpy(out, w, 12);
            if (normal >= 0) {
                memcpy(&x, out + normal, 4); memcpy(&y, out + normal + 4, 4); memcpy(&z, out + normal + 8, 4);
                float nw[3] = {x * m[0] + y * m[4] + z * m[8], x * m[1] + y * m[5] + z * m[9], x * m[2] + y * m[6] + z * m[10]};
                memcpy(out + normal, nw, 12);
            }
            gl_batch_bytes += stride;
            gl_batch_verts++;
        }
    return 1;
}

/* up_data != 0: a DrawPrimitiveUP / DrawIndexedPrimitiveUP from the game's memory, streamed for this draw only. */
static int gl_tnl_draw(uint32_t prim, uint32_t count, int indexed, int32_t base_vertex, uint32_t start,
                       uint32_t up_data, uint32_t up_stride, uint32_t up_indices, int up_idx32) {
    if (R.target != gl_backbuffer) return gl_tnl_reject(0);
    if (prim < 4 || prim > 6) return gl_tnl_reject(1);
    if (LY.pretransformed || !LY.pos) return gl_tnl_reject(2);
    for (int s = 0; s < STAGES; s++)
        if (LY.stage_gen[s] > 2) return gl_tnl_reject(3);   /* only camera-space normal and position are generated */
    int up = up_data != 0;
    uint32_t n = vertex_count(prim, count), vb = R.vb, ib = R.ib, stride = up ? up_stride : R.vb_stride;
    if (!n || !stride || (stride & 3)) return gl_tnl_reject(4);
    uint32_t data, size, idata = 0, isize = 0;
    int idx32;
    int64_t base = 0;
    if (up) {
        idx32 = up_idx32;
        uint32_t hi = 0;
        if (indexed)
            for (uint32_t k = 0; k < n; k++) {
                uint32_t i = idx32 ? M32(up_indices + 4 * k) : M16(up_indices + 2 * k);
                if (i > hi) hi = i;
            }
        data = up_data;
        size = indexed ? (hi + 1) * stride : n * stride;
        idata = up_indices;
        isize = n * (idx32 ? 4 : 2);
        start = 0;
    } else {
        if (!vb || !M32(vb + 28)) return gl_tnl_reject(4);
        if (indexed && (!ib || !M32(ib + 28))) return gl_tnl_reject(4);
        idx32 = indexed && M32(ib + 20) == 102;
        data = M32(vb + 28);
        size = M32(vb + 36);
        base = (int64_t)R.vb_offset + (int64_t)base_vertex * stride;
        if (base < 0 || (base & 3)) return gl_tnl_reject(5);
    }
    int32_t st[41];
    gl_state(st);   /* may upload a texture, which first runs the recorded list */
    gl_flush_list();   /* the CPU-path draws recorded so far come first */
    int32_t* p = gl_tnl_params;
    float* f = (float*)gl_tnl_params;
    p[0] = (int32_t)prim; p[1] = (int32_t)n; p[2] = indexed;
    p[3] = indexed ? (int32_t)(start * (idx32 ? 4 : 2)) : (int32_t)start;
    p[4] = idx32 ? 0x1405 : 0x1403;   /* UNSIGNED_INT, UNSIGNED_SHORT */
    /* Buffer dirty flag: 1 upload again, 2 stream (user-pointer data, never cached). */
    p[5] = up ? (indexed ? -1 : -3) : (int32_t)vb; p[6] = (int32_t)data; p[7] = (int32_t)size; p[8] = up ? 2 : M32(vb + 56) != 2;
    if (indexed && up) { p[9] = -2; p[10] = (int32_t)idata; p[11] = (int32_t)isize; p[12] = 2; }
    else if (indexed) { p[9] = (int32_t)ib; p[10] = (int32_t)M32(ib + 28); p[11] = (int32_t)M32(ib + 36); p[12] = M32(ib + 56) != 2; }
    p[13] = (int32_t)stride; p[14] = (int32_t)base;
    p[15] = (R.fvf & 0x10) ? (int32_t)LY.normal_at : -1;
    p[16] = LY.has_diffuse ? (int32_t)LY.diffuse_at : -1;
    for (int s = 0; s < 2; s++) {
        int set = LY.stage_set[s];
        p[17 + 2 * s] = LY.tex_n[set] ? (int32_t)LY.tex_at[set] : -1;
        p[18 + 2 * s] = (int32_t)LY.tex_n[set];
    }
    int colorvertex = R.rs[141] && LY.has_diffuse;
    p[21] = LY.lit;
    p[22] = colorvertex && R.rs[145] == 1; p[23] = colorvertex && R.rs[147] == 1; p[24] = colorvertex && R.rs[148] == 1;
    p[26] = LY.fog_mode;
    memcpy(f + 27, R.world.m, 64); memcpy(f + 43, R.view.m, 64); memcpy(f + 59, R.proj.m, 64);
    f[75] = LY.vx; f[76] = LY.vy; f[77] = LY.vw; f[78] = LY.vh;
    f[79] = (float)SW(R.target); f[80] = (float)SH(R.target);
    memcpy(f + 81, R.material, 16); memcpy(f + 85, R.material + 4, 16); memcpy(f + 89, R.material + 12, 16);
    uint32_t amb = R.rs[139];
    f[93] = ((amb >> 16) & 255) / 255.0f; f[94] = ((amb >> 8) & 255) / 255.0f; f[95] = (amb & 255) / 255.0f;
    f[96] = LY.fog_start; f[97] = LY.fog_end; f[98] = LY.fog_density;
    int lights = 0;
    for (int i = 0; i < 8; i++) {
        if (!R.light_on[i]) continue;
        const float* L = R.light[i];
        float* o = f + 99 + 20 * lights++;
        o[0] = L[1]; o[1] = L[2]; o[2] = L[3]; o[3] = L[0];          /* diffuse, type */
        o[4] = L[9]; o[5] = L[10]; o[6] = L[11]; o[7] = L[19];       /* ambient, range */
        o[8] = L[13]; o[9] = L[14]; o[10] = L[15]; o[11] = L[21];    /* position, attenuation 0 */
        o[12] = L[16]; o[13] = L[17]; o[14] = L[18]; o[15] = L[22];  /* direction, attenuation 1 */
        o[16] = L[23]; o[17] = o[18] = o[19] = 0;                    /* attenuation 2 */
    }
    p[25] = lights;
    memcpy(p + 259, st, sizeof st);
    for (int s = 0; s < 2; s++) {
        p[300 + s] = LY.stage_gen[s];
        p[302 + s] = (int32_t)LY.ttff[s];
        memcpy(f + 305 + 16 * s, R.tex[s].m, 64);
    }
    p[304] = (R.fvf & 0x10) != 0;
    if (dump_state == 2) {
        Vtx first;
        fetch(data + (uint32_t)base + (indexed ? 0 : start * stride), &first);
        dump_draw(prim, &first, n);
    }
    prof_gpu_draws++;
    S.draws++;
    /* Vertex-buffer draws are merged too: a WebGL draw call costs far more than re-sending a few dozen vertices. */
    if (gl_tnl_batch(p, prim, n, indexed, idx32, data, (uint32_t)base, size, indexed ? (up ? idata : M32(ib + 28)) : 0,
                     indexed ? (up ? isize : M32(ib + 36)) : 0, start)) {
        prof_fetch += prof_mark() - fetch_t0;
        return 1;
    }
    if (!up) M32(vb + 56) = 2;
    if (indexed && !up) M32(ib + 56) = 2;
    prof_fetch += prof_mark() - fetch_t0;
    gl_tnl_flush_batch();
    gl_tnl_submit(p);
    return 1;
}

static void gl_clear(int flags, uint32_t color, float z, int x, int y, int w, int h) {
    gl_flush();
    int32_t zbits;
    memcpy(&zbits, &z, 4);
    glq_op(6); glq_word(flags); glq_word((int32_t)color); glq_word(zbits);
    glq_word(x); glq_word(y); glq_word(w); glq_word(h);
}

void r_read_back(uint32_t surface) {
    if (!gl_on || surface != gl_backbuffer || (SFMT(surface) != 21 && SFMT(surface) != 22)) return;
    gl_flush();
    glq_post();
    js_gl_read((int)SW(surface), (int)SH(surface), GPTR(SBITS(surface)), (int)SPITCH(surface));
}

static void gl_resize(uint32_t backbuffer) {
    gl_flush();
    gl_backbuffer = backbuffer;
    if (gl_on) { glq_op(8); glq_word((int32_t)SW(backbuffer)); glq_word((int32_t)SH(backbuffer)); }
}

/* Puts what the host wrote into the backbuffer's memory (a lock, a GDI DC, a surface copy) onto the drawing buffer. */
void r_write_back(uint32_t surface) {
    if (!gl_on || surface != gl_backbuffer) return;
    gl_flush();
    uint32_t w = SW(surface), h = SH(surface);
    glq_op(7); glq_word(0); glq_word(0); glq_word((int32_t)w); glq_word((int32_t)h);
    uint8_t* p = glq_take(w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++, p += 4) {
            uint32_t t = texel(surface, x, y);
            p[0] = (uint8_t)(t >> 16); p[1] = (uint8_t)(t >> 8); p[2] = (uint8_t)t; p[3] = 255;
        }
}
int r_on_gpu(uint32_t surface) { return gl_on && surface == gl_backbuffer; }
void r_fill(uint32_t color, int x, int y, int w, int h) { gl_clear(1, color, 0, x, y, w, h); }

void r_preload(uint32_t tex) { if (gl_on) gl_texture(tex); }
void r_texture_lod(uint32_t tex, uint32_t lod) {
    if (!gl_on) return;
    gl_flush();
    glq_op(3); glq_word((int32_t)tex); glq_word((int32_t)lod);
}

int r_gl_present(void) {
    if (!gl_on) return 0;
    double t = prof_now();
    gl_flush();
    glq_op(9);
    glq_post();
    js_gl_frame_posted();
    prof_present += prof_now() - t;
    return 1;
}
#else
static void gl_start(uint32_t backbuffer) { (void)backbuffer; }
static void gl_resize(uint32_t backbuffer) { (void)backbuffer; }
void r_write_back(uint32_t surface) { (void)surface; }
int r_on_gpu(uint32_t surface) { (void)surface; return 0; }
void r_fill(uint32_t color, int x, int y, int w, int h) { (void)color; (void)x; (void)y; (void)w; (void)h; }
static void gl_draw_list(uint32_t prim, Vtx* v, uint32_t n) { (void)prim; (void)v; (void)n; }
static int gl_tnl_draw(uint32_t prim, uint32_t count, int indexed, int32_t base_vertex, uint32_t start,
                       uint32_t up_data, uint32_t up_stride, uint32_t up_indices, int up_idx32) {
    (void)prim; (void)count; (void)indexed; (void)base_vertex; (void)start;
    (void)up_data; (void)up_stride; (void)up_indices; (void)up_idx32;
    return 0;
}
static void gl_clear(int flags, uint32_t color, float z, int x, int y, int w, int h) {
    (void)flags; (void)color; (void)z; (void)x; (void)y; (void)w; (void)h;
}
int r_gl_present(void) { return 0; }
void r_preload(uint32_t tex) { (void)tex; }
void r_texture_lod(uint32_t tex, uint32_t lod) { (void)tex; (void)lod; }
void r_read_back(uint32_t surface) { (void)surface; }
#endif
