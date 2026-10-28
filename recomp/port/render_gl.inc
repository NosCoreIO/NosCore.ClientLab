/* WebGL2 backend, included at the end of render.c (it needs R, Vtx and texel). In the browser worker the vertices
   are still transformed by fetch(); the GPU rasterises, shades the two texture stages, blends and depth-tests. */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, js_gl_init, (int w, int h), {
    const report = (gl) => { const d = gl.getExtension('WEBGL_debug_renderer_info'); return d ? gl.getParameter(d.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER); };
    if (typeof OffscreenCanvas === 'undefined' || typeof importScripts !== 'function') return 0;
    const canvas = new OffscreenCanvas(w, h);
    const gl = canvas.getContext('webgl2', { alpha: false, antialias: false, preserveDrawingBuffer: false, depth: true });
    if (!gl) return 0;
    postMessage({ type: 'log', text: 'WebGL renderer: ' + report(gl) });
    const vs = '#version 300 es\n' +
        'in vec4 aPos; in vec4 aCol; in vec4 aUV; in float aFog;\n' +
        'out vec4 vCol; out vec4 vUV; out float vFog;\n' +
        'void main() { gl_Position = aPos; vCol = aCol.bgra; vUV = aUV; vFog = aFog; }\n';
    const fs = '#version 300 es\n' +
        'precision highp float; precision highp int;\n' +
        'in vec4 vCol; in vec4 vUV; in float vFog;\n' +
        'uniform sampler2D uTex0, uTex1;\n' +
        'uniform ivec4 uS0, uS1;\n' +
        'uniform ivec4 uA0, uA1;\n' +
        'uniform vec4 uTFactor;\n' +
        'uniform ivec2 uATest;\n' +
        'uniform float uARef;\n' +
        'uniform vec4 uFog;\n' +
        'out vec4 outColor;\n' +
        'vec4 arg(int a, vec4 tex, vec4 diff, vec4 cur) {\n' +
        '  int s = a & 15; vec4 v = s == 0 ? diff : s == 1 ? cur : s == 2 ? tex : s == 3 ? uTFactor : vec4(1.0);\n' +
        '  if ((a & 16) != 0) v = 1.0 - v;\n' +
        '  if ((a & 32) != 0) v = vec4(v.a);\n' +
        '  return v;\n' +
        '}\n' +
        'vec4 op(int o, vec4 a1, vec4 a2, vec4 cur) {\n' +
        '  if (o == 1) return cur;\n' +
        '  if (o == 2) return a1;\n' +
        '  if (o == 3) return a2;\n' +
        '  if (o == 5) return clamp(2.0 * a1 * a2, 0.0, 1.0);\n' +
        '  if (o == 6) return clamp(4.0 * a1 * a2, 0.0, 1.0);\n' +
        '  if (o == 7) return clamp(a1 + a2, 0.0, 1.0);\n' +
        '  if (o == 8) return clamp(a1 + a2 - 0.5, 0.0, 1.0);\n' +
        '  if (o == 9) return clamp(2.0 * (a1 + a2 - 0.5), 0.0, 1.0);\n' +
        '  if (o == 10) return a1 - a2;\n' +
        '  if (o == 11) return clamp(a1 + a2 - a1 * a2, 0.0, 1.0);\n' +
        '  return a1 * a2;\n' +
        '}\n' +
        'vec4 stage(ivec4 s, ivec4 a, vec4 tex, vec4 diff, vec4 cur) {\n' +
        '  vec3 c = op(s.x, arg(s.y, tex, diff, cur), arg(s.z, tex, diff, cur), cur).rgb;\n' +
        '  float al = s.w == 1 ? cur.a : op(s.w, arg(a.x, tex, diff, cur), arg(a.y, tex, diff, cur), cur).a;\n' +
        '  return vec4(c, al);\n' +
        '}\n' +
        'void main() {\n' +
        '  vec4 cur = vCol;\n' +
        '  if (uS0.x != 1) {\n' +
        '    vec4 t0 = uA0.z != 0 ? texture(uTex0, vUV.xy) : vec4(1.0);\n' +
        '    cur = stage(uS0, uA0, t0, vCol, cur);\n' +
        '    if (uS1.x != 1) {\n' +
        '      vec4 t1 = uA1.z != 0 ? texture(uTex1, vUV.zw) : vec4(1.0);\n' +
        '      cur = stage(uS1, uA1, t1, vCol, cur);\n' +
        '    }\n' +
        '  }\n' +
        '  if (uATest.x != 0) {\n' +
        '    float a = floor(cur.a * 255.0 + 0.5); int f = uATest.y;\n' +
        '    bool pass = f == 1 ? false : f == 2 ? a < uARef : f == 3 ? a == uARef : f == 4 ? a <= uARef :\n' +
        '                f == 5 ? a > uARef : f == 6 ? a != uARef : f == 7 ? a >= uARef : true;\n' +
        '    if (!pass) discard;\n' +
        '  }\n' +
        '  if (uFog.a > 0.5) cur.rgb = mix(uFog.rgb, cur.rgb, clamp(vFog, 0.0, 1.0));\n' +
        '  outColor = cur;\n' +
        '}\n';
    const sh = (type, src) => {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
        return s;
    };
    const prog = gl.createProgram();
    gl.attachShader(prog, sh(gl.VERTEX_SHADER, vs));
    gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fs));
    gl.bindAttribLocation(prog, 0, 'aPos');
    gl.bindAttribLocation(prog, 1, 'aCol');
    gl.bindAttribLocation(prog, 2, 'aUV');
    gl.bindAttribLocation(prog, 3, 'aFog');
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    const u = {};
    for (const n of ['uTex0', 'uTex1', 'uS0', 'uS1', 'uA0', 'uA1', 'uTFactor', 'uATest', 'uARef', 'uFog']) u[n] = gl.getUniformLocation(prog, n);
    gl.uniform1i(u.uTex0, 0);
    gl.uniform1i(u.uTex1, 1);
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 40, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.UNSIGNED_BYTE, true, 40, 16);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 40, 20);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 1, gl.FLOAT, false, 40, 36);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.enable(gl.SCISSOR_TEST);
    self.__gl = { canvas, gl, u, textures: new Map(), w, h, last: 0 };
    return 1;
});

EM_JS(void, js_gl_texture, (uint32_t id, int w, int h, const uint8_t* rgba), {
    const G = self.__gl, gl = G.gl;
    let t = G.textures.get(id);
    if (!t) { t = gl.createTexture(); G.textures.set(id, t); }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, HEAPU8.subarray(rgba, rgba + w * h * 4));
    gl.generateMipmap(gl.TEXTURE_2D);
    if (G.prev) { G.prev[12] = -1; G.prev[13] = -1; }
});

/* st: 0..5 stage 0 (colorop carg1 carg2 alphaop aarg1 aarg2), 6..11 stage 1, 12/13 texture ids, 14 tfactor,
   15 alpha test, 16 alpha func, 17 alpha ref, 18 blend, 19 src, 20 dst, 21 blend op, 22 z enable, 23 z write,
   24 z func, 25..28 scissor x y w h, 29..33 stage 0 sampler (mag min mip addressu addressv), 34..38 stage 1 */
EM_JS(void, js_gl_draw, (int prim, const uint8_t* verts, int n, const int32_t* st), {
    const G = self.__gl, gl = G.gl, u = G.u, base = st >> 2, S = (i) => HEAP32[base + i];
    const P = G.prev || (G.prev = new Int32Array(40).fill(-1));
    G.drawn = true;
    const changed = (from, to) => {
        let c = false;
        for (let i = from; i < to; i++) if (P[i] !== S(i)) { P[i] = S(i); c = true; }
        return c;
    };
    // Groups are tested one by one (no short-circuit) so the cache stays in step with GL.
    const s0 = changed(0, 6), t0 = P[12] !== S(12), s1 = changed(6, 12), t1 = P[13] !== S(13);
    if (s0 || t0) { gl.uniform4i(u.uS0, S(0), S(1), S(2), S(3)); gl.uniform4i(u.uA0, S(4), S(5), S(12) ? 1 : 0, 0); }
    if (s1 || t1) { gl.uniform4i(u.uS1, S(6), S(7), S(8), S(9)); gl.uniform4i(u.uA1, S(10), S(11), S(13) ? 1 : 0, 0); }
    if (changed(14, 15)) {
        const tf = S(14) >>> 0;
        gl.uniform4f(u.uTFactor, ((tf >>> 16) & 255) / 255, ((tf >>> 8) & 255) / 255, (tf & 255) / 255, (tf >>> 24) / 255);
    }
    if (changed(15, 18)) { gl.uniform2i(u.uATest, S(15), S(16)); gl.uniform1f(u.uARef, S(17) & 255); }
    const wrap = (a) => a === 2 ? gl.MIRRORED_REPEAT : (a === 3 || a === 4) ? gl.CLAMP_TO_EDGE : gl.REPEAT;
    for (let k = 0; k < 2; k++) {
        const b = 29 + 5 * k, texChanged = P[12 + k] !== S(12 + k), samplerChanged = changed(b, b + 5);
        P[12 + k] = S(12 + k);
        const id = S(12 + k) >>> 0;
        const t = id ? G.textures.get(id) : null;
        if (!t || (!texChanged && !samplerChanged)) continue;
        gl.activeTexture(gl.TEXTURE0 + k);
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, S(b) >= 2 ? gl.LINEAR : gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, S(b + 1) >= 2 ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap(S(b + 3)));
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap(S(b + 4)));
    }
    if (changed(18, 22)) {
        if (S(18)) {
            const f = [gl.ONE, gl.ZERO, gl.ONE, gl.SRC_COLOR, gl.ONE_MINUS_SRC_COLOR, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA,
                       gl.DST_ALPHA, gl.ONE_MINUS_DST_ALPHA, gl.DST_COLOR, gl.ONE_MINUS_DST_COLOR, gl.SRC_ALPHA_SATURATE];
            const eq = [gl.FUNC_ADD, gl.FUNC_ADD, gl.FUNC_SUBTRACT, gl.FUNC_REVERSE_SUBTRACT, gl.MIN, gl.MAX];
            gl.enable(gl.BLEND);
            gl.blendFunc(f[S(19)] ?? gl.ONE, f[S(20)] ?? gl.ZERO);
            gl.blendEquation(eq[S(21)] ?? gl.FUNC_ADD);
        } else gl.disable(gl.BLEND);
    }
    if (changed(22, 25)) {
        if (S(22)) {
            gl.enable(gl.DEPTH_TEST);
            gl.depthFunc(0x1FF + Math.min(Math.max(S(24), 1), 8));
            gl.depthMask(S(23) !== 0);
        } else gl.disable(gl.DEPTH_TEST);
    }
    if (changed(25, 29)) gl.scissor(S(25), G.h - S(26) - S(28), S(27), S(28));
    if (changed(38, 40)) {
        const fc = S(38) >>> 0;
        gl.uniform4f(u.uFog, ((fc >>> 16) & 255) / 255, ((fc >>> 8) & 255) / 255, (fc & 255) / 255, S(39) ? 1 : 0);
    }
    gl.bufferData(gl.ARRAY_BUFFER, HEAPU8.subarray(verts, verts + n * 40), gl.STREAM_DRAW);
    gl.drawArrays(prim, 0, n);
});

EM_JS(void, js_gl_clear, (int flags, uint32_t color, float z, int x, int y, int w, int h), {
    const G = self.__gl, gl = G.gl;
    G.drawn = true;
    gl.scissor(x, G.h - y - h, w, h);
    let bits = 0;
    if (flags & 1) {
        gl.clearColor(((color >>> 16) & 255) / 255, ((color >>> 8) & 255) / 255, (color & 255) / 255, 1);
        bits |= gl.COLOR_BUFFER_BIT;
    }
    if (flags & 2) { gl.depthMask(true); gl.clearDepth(z); bits |= gl.DEPTH_BUFFER_BIT; }
    if (bits) gl.clear(bits);
    if (G.prev) { G.prev[22] = G.prev[23] = G.prev[24] = -1; G.prev[25] = -1; }
});

/* The client never sleeps between frames: left alone it renders hundreds of frames a second, saturating the GPU
   process until the page itself stalls. Present holds it to about 60 frames a second, like vsync, with a real
   blocking wait (Atomics.wait is allowed in a worker). */
EM_JS(void, js_gl_present, (void), {
    const G = self.__gl;
    const pace = G.pace || (G.pace = new Int32Array(new SharedArrayBuffer(4)));
    const wait = 16.6 - (performance.now() - G.last);
    if (wait > 0) Atomics.wait(pace, 0, 0, wait);
    G.last = performance.now();
    const gl = G.gl;
    if (!G.kept) {
        G.keptRb = gl.createRenderbuffer();
        gl.bindRenderbuffer(gl.RENDERBUFFER, G.keptRb);
        gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, G.w, G.h);
        G.kept = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, G.kept);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, G.keptRb);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }
    gl.disable(gl.SCISSOR_TEST);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, G.kept);
    gl.blitFramebuffer(0, 0, G.w, G.h, 0, 0, G.w, G.h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.enable(gl.SCISSOR_TEST);
    G.drawn = false;
    const bmp = G.canvas.transferToImageBitmap();
    postMessage({ type: 'bitmap', bmp }, [bmp]);
});

/* The frame as 32-bit BGRA rows, top row first. Presenting hands the drawing buffer to the page and leaves it blank,
   so until the next draw the copy kept at the last present is the frame. */
EM_JS(void, js_gl_read, (int w, int h, uint8_t* out, int pitch), {
    const G = self.__gl, gl = G.gl, px = new Uint8Array(w * h * 4);
    if (!G.drawn && G.kept) gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.kept);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    for (let y = 0; y < h; y++)
        for (let x = 0, s = (h - 1 - y) * w * 4, d = out + y * pitch; x < w; x++, s += 4, d += 4) {
            HEAPU8[d] = px[s + 2]; HEAPU8[d + 1] = px[s + 1]; HEAPU8[d + 2] = px[s]; HEAPU8[d + 3] = 255;
        }
});

static uint32_t gl_backbuffer;
static void gl_flush(void);
static uint8_t* gl_verts;
static uint8_t* gl_rgba;
static uint32_t gl_rgba_size;

static void gl_start(uint32_t backbuffer) {
    gl_backbuffer = backbuffer;
    gl_on = js_gl_init((int)SW(backbuffer), (int)SH(backbuffer));
    if (gl_on) {
        gl_verts = malloc(65536 * 40);
        port_log("render: WebGL2");
    }
}

/* Textures are uploaded (level 0, then mipmapped) when Lock marked them dirty: +56 is 2 once uploaded. */
static uint32_t gl_texture(uint32_t tex) {
    if (!tex) return 0;
    uint32_t s = M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;
    if (!s || !SBITS(s)) return 0;
    if (M32(tex + 56) != 2) {
        gl_flush();
        double t0 = prof_now();
        uint32_t w = SW(s), h = SH(s);
        if (w * h * 4 > gl_rgba_size) { free(gl_rgba); gl_rgba_size = w * h * 4; gl_rgba = malloc(gl_rgba_size); }
        uint8_t* p = gl_rgba;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++, p += 4) {
                uint32_t t = texel(s, x, y);
                p[0] = (uint8_t)(t >> 16); p[1] = (uint8_t)(t >> 8); p[2] = (uint8_t)t; p[3] = (uint8_t)(t >> 24);
            }
        js_gl_texture(tex, (int)w, (int)h, gl_rgba);
        M32(tex + 56) = 2;
        prof_upload += prof_now() - t0;
    }
    return tex;
}

/* Draws with identical state are merged into one triangle list: the client issues thousands of tiny draws per frame
   and each call into JavaScript costs far more than the vertices it carries. */
#define BATCH_VERTS 65535
static int32_t batch_st[40];
static uint32_t batch_n;

static void gl_flush(void) {
    if (!batch_n) return;
    double t = prof_now();
    js_gl_draw(4, gl_verts, (int)batch_n, batch_st);
    prof_submit += prof_now() - t;
    batch_n = 0;
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
    log_stage_setup();
    log_raster_setup();
    memset(st, 0, 40 * sizeof *st);
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
    st[22] = (int32_t)R.rs[7]; st[23] = (int32_t)R.rs[14]; st[24] = (int32_t)R.rs[23];
    st[25] = (int32_t)(R.vp_set ? R.vp_x : 0); st[26] = (int32_t)(R.vp_set ? R.vp_y : 0);
    st[27] = (int32_t)(R.vp_set ? R.vp_w : SW(R.target)); st[28] = (int32_t)(R.vp_set ? R.vp_h : SH(R.target));
    st[38] = (int32_t)R.rs[34];
    st[39] = (int32_t)(R.rs[28] != 0);
}

static void gl_draw_list(uint32_t prim, Vtx* v, uint32_t n) {
    if (R.target != gl_backbuffer || n == 0) return;
    float tw = (float)SW(R.target), th = (float)SH(R.target);
    int32_t st[40];
    gl_state(st);
    if (prim < 4 || prim > 6) {
        gl_flush();
        for (uint32_t k = 0; k < n && k < BATCH_VERTS; k++) put_vertex(gl_verts + 40 * k, &v[k], tw, th);
        js_gl_draw((int)prim, gl_verts, (int)(n < BATCH_VERTS ? n : BATCH_VERTS), st);
        return;
    }
    uint32_t tris = prim == 4 ? n / 3 : n - 2;
    if (batch_n && (memcmp(st, batch_st, sizeof st) || batch_n + 3 * tris > BATCH_VERTS)) gl_flush();
    memcpy(batch_st, st, sizeof st);
    for (uint32_t t = 0; t < tris; t++) {
        if (batch_n + 3 > BATCH_VERTS) gl_flush();
        uint32_t a, b, c;
        if (prim == 4) { a = 3 * t; b = a + 1; c = a + 2; }
        else if (prim == 5) { a = t; b = t + 1; c = t + 2; }
        else { a = 0; b = t + 1; c = t + 2; }
        put_vertex(gl_verts + 40 * batch_n++, &v[a], tw, th);
        put_vertex(gl_verts + 40 * batch_n++, &v[b], tw, th);
        put_vertex(gl_verts + 40 * batch_n++, &v[c], tw, th);
    }
}

static void gl_clear(int flags, uint32_t color, float z, int x, int y, int w, int h) {
    gl_flush();
    js_gl_clear(flags, color, z, x, y, w, h);
}

void r_read_back(uint32_t surface) {
    if (!gl_on || surface != gl_backbuffer || (SFMT(surface) != 21 && SFMT(surface) != 22)) return;
    gl_flush();
    js_gl_read((int)SW(surface), (int)SH(surface), GPTR(SBITS(surface)), (int)SPITCH(surface));
}

int r_gl_present(void) {
    if (!gl_on) return 0;
    double t = prof_now();
    gl_flush();
    js_gl_present();
    prof_present += prof_now() - t;
    return 1;
}
#else
static void gl_start(uint32_t backbuffer) { (void)backbuffer; }
static void gl_draw_list(uint32_t prim, Vtx* v, uint32_t n) { (void)prim; (void)v; (void)n; }
static void gl_clear(int flags, uint32_t color, float z, int x, int y, int w, int h) {
    (void)flags; (void)color; (void)z; (void)x; (void)y; (void)w; (void)h;
}
int r_gl_present(void) { return 0; }
void r_read_back(uint32_t surface) { (void)surface; }
#endif
