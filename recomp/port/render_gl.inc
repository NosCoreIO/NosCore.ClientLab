/* WebGL2 backend, included at the end of render.c (it needs R, Vtx and texel). On a GPU, draws from vertex buffers
   are transformed, lit and fogged by the vertex shader (gl_buffer_draw); everything else, and every draw under a
   software rasteriser, is transformed by fetch(). The GPU rasterises, shades the two texture stages, blends and
   depth-tests. */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, js_gl_init, (int w, int h), {
    const report = (gl) => { const d = gl.getExtension('WEBGL_debug_renderer_info'); return d ? gl.getParameter(d.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER); };
    if (typeof OffscreenCanvas === 'undefined' || typeof importScripts !== 'function') return 0;
    const canvas = new OffscreenCanvas(w, h);
    const gl = canvas.getContext('webgl2', { alpha: false, antialias: false, preserveDrawingBuffer: false, depth: true });
    if (!gl) return 0;
    const renderer = report(gl);
    postMessage({ type: 'log', text: 'WebGL renderer: ' + renderer });
    const STREAM = 1 << 24;
    const vsStream = '#version 300 es\n' +
        'in vec4 aPos; in vec4 aCol; in vec4 aUV; in float aFog;\n' +
        'out vec4 vCol; out vec4 vUV; out float vFog;\n' +
        'void main() { gl_Position = aPos; gl_PointSize = 1.0; vCol = aCol.bgra; vUV = aUV; vFog = aFog; }\n';
    // Fixed-function vertex processing for draws from a vertex buffer, the same arithmetic as render.c fetch(). The
    // key (gl_buffer_draw) is baked in as constants; the block C holds the matrices, viewport, fog, material and lights.
    const vsBuffer = (k) => '#version 300 es\n' +
        'precision highp float; precision highp int;\n' +
        'const bool LIT = ' + ((k & 1) !== 0) + ', DIFFUSE = ' + ((k >> 15 & 1) !== 0) + ', NORMAL = ' + ((k >> 16 & 1) !== 0) + ';\n' +
        'const bool DVC = ' + ((k >> 17 & 1) !== 0) + ', AVC = ' + ((k >> 18 & 1) !== 0) + ', EVC = ' + ((k >> 19 & 1) !== 0) + ';\n' +
        'const int FOG = ' + (k >> 1 & 3) + ', LIGHTS = ' + (k >> 20 & 15) + ';\n' +
        'const ivec2 GEN = ivec2(' + (k >> 3 & 3) + ', ' + (k >> 5 & 3) + ');\n' +
        'const ivec2 TT = ivec2(' + (k >> 7 & 7) + ', ' + (k >> 11 & 7) + ');\n' +
        'const bvec2 PROJ = bvec2(' + ((k >> 10 & 1) !== 0) + ', ' + ((k >> 14 & 1) !== 0) + ');\n' +
        'const ivec2 TN = ivec2(' + (k >> 25 & 7) + ', ' + (k >> 28 & 7) + ');\n' +
        'in vec3 aPos; in vec4 aCol; in vec4 aT0; in vec4 aT1; in vec3 aNrm;\n' +
        'layout(std140) uniform C { vec4 uC[75]; };\n' +
        'out vec4 vCol; out vec4 vUV; out float vFog;\n' +
        'mat4 M(int i) { return mat4(uC[i], uC[i + 1], uC[i + 2], uC[i + 3]); }\n' +
        'vec4 coords(vec4 t, int n) {\n' +
        '  return n == 1 ? vec4(t.x, 0.0, 1.0, 0.0) : n == 2 ? vec4(t.xy, 1.0, 0.0) : n == 3 ? vec4(t.xyz, 1.0) : n == 4 ? t : vec4(0.0, 0.0, 1.0, 0.0);\n' +
        '}\n' +
        'vec2 stageUV(int st, vec4 t, vec4 cam, vec4 camN) {\n' +
        '  vec4 tc = GEN[st] == 2 ? cam : GEN[st] == 1 ? camN : coords(t, TN[st]);\n' +
        '  if (TT[st] == 0) return tc.xy;\n' +
        '  vec4 r = M(12 + 4 * st) * tc;\n' +
        '  float q = PROJ[st] ? r[TT[st] - 1] : 1.0;\n' +
        '  return r.xy / q;\n' +
        '}\n' +
        'vec4 light(vec4 p, vec4 vc) {\n' +
        '  vec3 n = mat3(M(8)) * aNrm;\n' +
        '  float len = length(n);\n' +
        '  if (len > 0.0) n /= len;\n' +
        '  vec3 wp = (M(8) * p).xyz;\n' +
        '  vec4 dsrc = DVC ? vc : uC[23], asrc = AVC ? vc : uC[24], esrc = EVC ? vc : uC[25];\n' +
        '  vec3 o = esrc.rgb + uC[26].rgb * asrc.rgb;\n' +
        '  for (int i = 0; i < LIGHTS; i++) {\n' +
        '    int b = 27 + 6 * i;\n' +
        '    vec3 dir; float atten = 1.0;\n' +
        '    if (uC[b].x == 3.0) dir = -uC[b + 4].xyz;\n' +
        '    else {\n' +
        '      dir = uC[b + 3].xyz - wp;\n' +
        '      float d = length(dir);\n' +
        '      if (uC[b].y > 0.0 && d > uC[b].y) continue;\n' +
        '      float a = uC[b + 5].x + uC[b + 5].y * d + uC[b + 5].z * d * d;\n' +
        '      atten = a > 0.0 ? 1.0 / a : 1.0;\n' +
        '    }\n' +
        '    float dl = length(dir);\n' +
        '    float ndotl = dl > 0.0 ? max(dot(n, dir) / dl, 0.0) : 0.0;\n' +
        '    o += atten * (uC[b + 2].rgb * asrc.rgb + ndotl * uC[b + 1].rgb * dsrc.rgb);\n' +
        '  }\n' +
        '  return vec4(clamp(o, 0.0, 1.0), clamp(dsrc.a, 0.0, 1.0));\n' +
        '}\n' +
        'void main() {\n' +
        '  vec4 p = vec4(aPos, 1.0);\n' +
        '  vec4 cam = M(4) * p, camN = NORMAL ? M(4) * vec4(aNrm, 0.0) : vec4(0.0);\n' +
        '  vec4 vc = DIFFUSE ? aCol.bgra : vec4(1.0);\n' +
        '  vCol = LIT ? light(p, vc) : vc;\n' +
        '  float d = abs(cam.z), f = 1.0;\n' +
        '  if (FOG == 3) f = uC[22].y != uC[22].x ? (uC[22].y - d) / (uC[22].y - uC[22].x) : 1.0;\n' +
        '  else if (FOG == 1) f = exp(-uC[22].z * d);\n' +
        '  else if (FOG == 2) f = exp(-(uC[22].z * d) * (uC[22].z * d));\n' +
        '  vFog = clamp(f, 0.0, 1.0);\n' +
        '  vUV = vec4(stageUV(0, aT0, cam, camN), stageUV(1, aT1, cam, camN));\n' +
        '  vec4 t = M(0) * p;\n' +
        '  float w = t.w != 0.0 ? t.w : 1.0;\n' +
        '  vec2 s = vec2(uC[20].x + (t.x / w + 1.0) * 0.5 * uC[20].z, uC[20].y + (1.0 - t.y / w) * 0.5 * uC[20].w);\n' +
        '  gl_Position = vec4(((s.x + 0.5) / uC[21].x * 2.0 - 1.0) * w, (1.0 - (s.y + 0.5) / uC[21].y * 2.0) * w, (t.z / w * 2.0 - 1.0) * w, w);\n' +
        '  gl_PointSize = 1.0;\n' +
        '}\n';
    // The texture stage, alpha test and fog setup are constants of each program, one program per distinct setup:
    // a software rasteriser runs every branch of a shader that reads them from uniforms, several times slower.
    const fsHead = (k) => '#version 300 es\n' +
        'precision highp float; precision highp int;\n' +
        'in vec4 vCol; in vec4 vUV; in float vFog;\n' +
        'uniform sampler2D uTex0, uTex1;\n' +
        'const ivec4 uS0 = ivec4(' + k.slice(0, 4) + '), uS1 = ivec4(' + k.slice(6, 10) + ');\n' +
        'const ivec4 uA0 = ivec4(' + [k[4], k[5], k[12], 0] + '), uA1 = ivec4(' + [k[10], k[11], k[13], 0] + ');\n' +
        'const ivec2 uATest = ivec2(' + k.slice(14, 16) + ');\n' +
        'const bool uFogOn = ' + (k[16] ? 'true' : 'false') + ';\n' +
        'uniform vec4 uTFactor;\n' +
        'uniform float uARef;\n' +
        'uniform vec3 uFog;\n' +
        'out vec4 outColor;\n';
    const fsBody =
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
        '  if (uFogOn) cur.rgb = mix(uFog, cur.rgb, clamp(vFog, 0.0, 1.0));\n' +
        '  outColor = cur;\n' +
        '}\n';
    const sh = (type, src) => {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
        return s;
    };
    const vertexShaders = new Map();
    const vertexShader = (k) => {
        let v = vertexShaders.get(k);
        if (!v) vertexShaders.set(k, v = sh(gl.VERTEX_SHADER, k === STREAM ? vsStream : vsBuffer(k)));
        return v;
    };
    // k: the fragment setup (17 values), then the vertex key (STREAM for recorded vertices).
    const programs = new Map();
    const program = (k) => {
        const key = k.join(',');
        let p = programs.get(key);
        if (p) return p;
        const prog = gl.createProgram();
        gl.attachShader(prog, vertexShader(k[17]));
        gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fsHead(k) + fsBody));
        gl.bindAttribLocation(prog, 0, 'aPos');
        gl.bindAttribLocation(prog, 1, 'aCol');
        gl.bindAttribLocation(prog, 2, k[17] === STREAM ? 'aUV' : 'aT0');
        gl.bindAttribLocation(prog, 3, k[17] === STREAM ? 'aFog' : 'aT1');
        gl.bindAttribLocation(prog, 4, 'aNrm');
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
        gl.useProgram(prog);
        p = { prog, u: {}, tex0: k[12] === 1, tex1: k[13] === 1, vkey: k[17] };
        for (const n of ['uTex0', 'uTex1', 'uTFactor', 'uARef', 'uFog']) p.u[n] = gl.getUniformLocation(prog, n);
        if (k[17] !== STREAM) gl.uniformBlockBinding(prog, gl.getUniformBlockIndex(prog, 'C'), 0);
        gl.uniform1i(p.u.uTex0, 0);
        gl.uniform1i(p.u.uTex1, 1);
        programs.set(key, p);
        if (G.cur) gl.useProgram(G.cur.prog);
        return p;
    };
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 40, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.UNSIGNED_BYTE, true, 40, 16);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 40, 20);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 1, gl.FLOAT, false, 40, 36);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.enable(gl.SCISSOR_TEST);
    const G = self.__gl = { canvas, gl, program, STREAM, vbo, cur: null, textures: new Map(), buffers: [new Map(), new Map()],
                            vao: gl.createVertexArray(), ubo: gl.createBuffer(), w, h, last: 0 };
    // Constant blocks sit 256 bytes apart (gl_buffer_draw); a GPU that needs a coarser offset keeps the CPU path.
    if (gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT) > 256) return 1;
    // 2: a GPU, which takes the vertex work too; 1: a software rasteriser, where the CPU path is faster on small maps.
    return /SwiftShader|llvmpipe|lavapipe|Software|Basic Render/i.test(renderer) ? 1 : 2;
});

/* A vertex (index 0) or index (1) buffer's contents, keyed by its object. */
EM_JS(void, js_gl_buffer, (uint32_t id, int index, const uint8_t* data, int size), {
    const G = self.__gl, gl = G.gl, map = G.buffers[index];
    let b = map.get(id);
    if (!b) { b = gl.createBuffer(); map.set(id, b); }
    const target = index ? gl.ELEMENT_ARRAY_BUFFER : gl.ARRAY_BUFFER;
    if (index) gl.bindVertexArray(G.vao);
    gl.bindBuffer(target, b);
    gl.bufferData(target, HEAPU8.subarray(data, data + size), gl.STATIC_DRAW);
    if (index) gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo);
    G.vaoOn = false;
});

EM_JS(void, js_gl_texture, (uint32_t id, int w, int h, const uint8_t* rgba), {
    const G = self.__gl, gl = G.gl;
    // Textures the client rewrites every frame (text, the minimap) keep their storage, so an upload never reallocates.
    let t = G.textures.get(id);
    if (!t || t.w !== w || t.h !== h) {
        if (t) gl.deleteTexture(t);
        t = gl.createTexture();
        t.w = w;
        t.h = h;
        G.textures.set(id, t);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texStorage2D(gl.TEXTURE_2D, Math.floor(Math.log2(Math.max(w, h))) + 1, gl.RGBA8, w, h);
    } else {
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, t);
    }
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, HEAPU8.subarray(rgba, rgba + w * h * 4));
    gl.generateMipmap(gl.TEXTURE_2D);
    if (G.lods && G.lods.has(id)) gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, G.lods.get(id));
    if (G.prev) { G.prev[12] = -1; G.prev[13] = -1; }
});

/* SetLOD: the most detailed mip level sampled. */
EM_JS(void, js_gl_texture_lod, (uint32_t id, int lod), {
    const G = self.__gl, gl = G.gl, t = G.textures.get(id);
    G.lods = G.lods || new Map();
    G.lods.set(id, lod);
    if (!t) return;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, lod);
    if (G.prev) G.prev[12] = -1;
});

/* st: 0..5 stage 0 (colorop carg1 carg2 alphaop aarg1 aarg2), 6..11 stage 1, 12/13 texture ids, 14 tfactor,
   15 alpha test, 16 alpha func, 17 alpha ref, 18 blend, 19 src, 20 dst, 21 blend op, 22 z enable, 23 z write,
   24 z func, 25..28 scissor x y w h, 29..33 stage 0 sampler (mag min mip addressu addressv), 34..38 stage 1,
   38 fog colour, 39 fog on, 40 cull mode */
/* Runs one recorded command list: the vertices of all its draws are uploaded at once, then each command (GL
   primitive, first vertex, count, 41 state words, then 16 words for a draw from a vertex buffer: see gl_buffer_draw)
   sets what changed and draws. */
EM_JS(void, js_gl_run, (const uint8_t* verts, int nverts, const int32_t* cmds, int ncmds, const uint8_t* consts, int cbytes), {
    const G = self.__gl, gl = G.gl;
    const P = G.prev || (G.prev = new Int32Array(41).fill(-1));
    const A = G.attribs || (G.attribs = new Int32Array(11).fill(-1));
    G.drawn = true;
    G.run = (G.run || 0) + 1;
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo);
    G.vaoOn = false;
    // One large buffer takes every list; when it fills up it is orphaned, so the GPU never waits on a range still
    // being read.
    const RING = 1 << 24, bytes = nverts * 40;
    if (G.ring === undefined || G.ring + bytes > RING) { gl.bufferData(gl.ARRAY_BUFFER, RING, gl.STREAM_DRAW); G.ring = 0; }
    gl.bufferSubData(gl.ARRAY_BUFFER, G.ring, HEAPU8, verts, bytes);
    const first = G.ring / 40;
    G.ring += bytes;
    // Every constant block of the run goes up in one upload; each draw then binds its range, instead of thousands of
    // uniform calls that filled the GPU process's command buffer.
    if (cbytes) {
        gl.bindBuffer(gl.UNIFORM_BUFFER, G.ubo);
        gl.bufferData(gl.UNIFORM_BUFFER, HEAPU8.subarray(consts, consts + cbytes), gl.STREAM_DRAW);
    }
    let block = -1;
    let base = 0;
    const S = (i) => HEAP32[base + i];
    const changed = (from, to) => {
        let c = false;
        for (let i = from; i < to; i++) if (P[i] !== S(i)) { P[i] = S(i); c = true; }
        return c;
    };
    const wrap = (a) => a === 2 ? gl.MIRRORED_REPEAT : (a === 3 || a === 4) ? gl.CLAMP_TO_EDGE : gl.REPEAT;
    const f = [gl.ONE, gl.ZERO, gl.ONE, gl.SRC_COLOR, gl.ONE_MINUS_SRC_COLOR, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA,
               gl.DST_ALPHA, gl.ONE_MINUS_DST_ALPHA, gl.DST_COLOR, gl.ONE_MINUS_DST_COLOR, gl.SRC_ALPHA_SATURATE];
    const eq = [gl.FUNC_ADD, gl.FUNC_ADD, gl.FUNC_SUBTRACT, gl.FUNC_REVERSE_SUBTRACT, gl.MIN, gl.MAX];
    for (let k = 0; k < ncmds; k++) {
        const cmd = (cmds >> 2) + 60 * k, g = cmd + 44, buffered = HEAP32[g] !== 0;
        const vkey = buffered ? HEAP32[g + 14] : G.STREAM;
        base = cmd + 3;
        // Groups are tested one by one (no short-circuit) so the cache stays in step with GL.
        const s0 = changed(0, 6), s1 = changed(6, 12), at = changed(15, 17), fogOn = changed(39, 40);
        let switched = false;
        if (s0 || s1 || at || fogOn || !G.cur || vkey !== G.cur.vkey || (S(12) !== 0) !== G.cur.tex0 || (S(13) !== 0) !== G.cur.tex1) {
            const setup = [S(0), S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11),
                           S(12) ? 1 : 0, S(13) ? 1 : 0, S(15), S(16), S(39) ? 1 : 0, vkey];
            const p = G.program(setup);
            if (p !== G.cur) {
                gl.useProgram(p.prog);
                G.cur = p;
                switched = true;
            }
        }
        const u = G.cur.u;
        // A program keeps its own uniforms, so a switch sets them all again.
        if (changed(14, 15) || switched) {
            const tf = S(14) >>> 0;
            gl.uniform4f(u.uTFactor, ((tf >>> 16) & 255) / 255, ((tf >>> 8) & 255) / 255, (tf & 255) / 255, (tf >>> 24) / 255);
        }
        if (changed(17, 18) || switched) gl.uniform1f(u.uARef, S(17) & 255);
        for (let j = 0; j < 2; j++) {
            const b = 29 + 5 * j, texChanged = P[12 + j] !== S(12 + j), samplerChanged = changed(b, b + 5);
            P[12 + j] = S(12 + j);
            const id = S(12 + j) >>> 0;
            const t = id ? G.textures.get(id) : null;
            if (!t || (!texChanged && !samplerChanged)) continue;
            gl.activeTexture(gl.TEXTURE0 + j);
            gl.bindTexture(gl.TEXTURE_2D, t);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, S(b) >= 2 ? gl.LINEAR : gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, S(b + 1) >= 2 ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap(S(b + 3)));
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap(S(b + 4)));
        }
        if (changed(18, 22)) {
            if (S(18)) {
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
        if (changed(38, 39) || switched) {
            const fc = S(38) >>> 0;
            gl.uniform3f(u.uFog, ((fc >>> 16) & 255) / 255, ((fc >>> 8) & 255) / 255, (fc & 255) / 255);
        }
        // Screen-clockwise (y down) stays clockwise in clip space (y up): a GL back face.
        if (changed(40, 41)) {
            if (S(40) === 2 || S(40) === 3) { gl.enable(gl.CULL_FACE); gl.cullFace(S(40) === 2 ? gl.BACK : gl.FRONT); }
            else gl.disable(gl.CULL_FACE);
        }
        if (!buffered) {
            if (G.vaoOn) { gl.bindVertexArray(null); gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo); G.vaoOn = false; }
            gl.drawArrays(HEAP32[cmd], first + HEAP32[cmd + 1], HEAP32[cmd + 2]);
            continue;
        }
        const vb = G.buffers[0].get(HEAP32[g + 1] >>> 0), ib = HEAP32[g + 2] ? G.buffers[1].get(HEAP32[g + 2] >>> 0) : null;
        if (!vb || (HEAP32[g + 2] && !ib)) continue;
        if (block !== HEAP32[g + 13]) { block = HEAP32[g + 13]; gl.bindBufferRange(gl.UNIFORM_BUFFER, 0, G.ubo, block, 1200); }
        if (!G.vaoOn) { gl.bindVertexArray(G.vao); G.vaoOn = true; A.fill(-1); }
        let same = true;
        for (let i = 0; i < 11; i++) if (A[i] !== HEAP32[g + 1 + i]) { A[i] = HEAP32[g + 1 + i]; same = false; }
        if (!same) {
            const at = HEAP32[g + 4], stride = HEAP32[g + 5];
            gl.bindBuffer(gl.ARRAY_BUFFER, vb);
            gl.enableVertexAttribArray(0);
            gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, at);
            const attr = (loc, off, size, type, norm, x, y, z, w) => {
                if (off >= 0) { gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, size, type, norm, stride, at + off); }
                else { gl.disableVertexAttribArray(loc); gl.vertexAttrib4f(loc, x, y, z, w); }
            };
            attr(1, HEAP32[g + 7], 4, gl.UNSIGNED_BYTE, true, 1, 1, 1, 1);
            attr(2, HEAP32[g + 8], HEAP32[g + 9], gl.FLOAT, false, 0, 0, 1, 0);
            attr(3, HEAP32[g + 10], HEAP32[g + 11], gl.FLOAT, false, 0, 0, 1, 0);
            attr(4, HEAP32[g + 6], 3, gl.FLOAT, false, 0, 0, 0, 0);
            if (ib) gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
        }
        if (ib) gl.drawElements(HEAP32[cmd], HEAP32[cmd + 2], HEAP32[g + 3] ? gl.UNSIGNED_INT : gl.UNSIGNED_SHORT, HEAP32[g + 12]);
        else gl.drawArrays(HEAP32[cmd], HEAP32[cmd + 1], HEAP32[cmd + 2]);
    }
    if (G.vaoOn) { gl.bindVertexArray(null); gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo); G.vaoOn = false; }
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

/* Writes rows of RGBA (top row first) into the drawing buffer at x, y. */
EM_JS(void, js_gl_write, (int x, int y, int w, int h, const uint8_t* rgba), {
    const G = self.__gl, gl = G.gl;
    if (!G.wtex) { G.wtex = gl.createTexture(); G.wfb = gl.createFramebuffer(); }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, G.wtex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, HEAPU8.subarray(rgba, rgba + w * h * 4));
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.wfb);
    gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, G.wtex, 0);
    gl.disable(gl.SCISSOR_TEST);
    gl.blitFramebuffer(0, 0, w, h, x, G.h - y, x + w, G.h - y - h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.enable(gl.SCISSOR_TEST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    G.drawn = true;
    if (G.prev) { G.prev[12] = -1; G.prev[25] = -1; }
});

EM_JS(void, js_gl_resize, (int w, int h), {
    const G = self.__gl, gl = G.gl;
    G.canvas.width = w;
    G.canvas.height = h;
    G.w = w;
    G.h = h;
    gl.viewport(0, 0, w, h);
    if (G.kept) { gl.deleteFramebuffer(G.kept); gl.deleteRenderbuffer(G.keptRb); G.kept = null; }
    if (G.prev) G.prev[25] = -1;
});

/* The client never sleeps between frames: left alone it renders hundreds of frames a second, saturating the GPU
   process until the page itself stalls. The page shows one frame per display refresh from a queue of two and counts
   the frames it has shown in vsync[0]; Present waits while two frames are already queued, which paces the game to the
   display and lets one slow frame be absorbed by the spare instead of repeating a refresh. A notify wakes it; the
   timeout only matters when the page stops refreshing (hidden or covered), where it keeps the game near 60 frames a
   second, and the count is then resynchronised so a page that refreshes again does not leave every frame waiting. */
EM_JS(void, js_gl_present, (void), {
    const G = self.__gl, V = self.vsync;
    if (V) {
        G.posted = G.posted || 0;
        let shown = Atomics.load(V, 0);
        if (G.posted - shown >= 2) {
            Atomics.wait(V, 0, shown, 16);
            shown = Atomics.load(V, 0);
            if (G.posted - shown >= 2) G.posted = shown + 1;
        }
    }
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
    // Frames go to the page as pixels in shared memory, four slots used in turn: an ImageBitmap per frame stays in GPU
    // memory until the page collects garbage, which piled up gigabytes and stalled the GPU, and the worker never
    // yields for a canvas transferred from the page to show anything. Each frame is read into one of two pixel buffers
    // and fetched a frame later, by when the GPU has finished it, so the read does not wait for the frame just drawn.
    const size = G.w * G.h * 4;
    if (!G.slots || G.slotW !== G.w || G.slotH !== G.h) {
        G.slots = [0, 1, 2, 3].map(() => new Uint8Array(new SharedArrayBuffer(size)));
        G.owner = new Int32Array(new SharedArrayBuffer(16));   /* per slot: 0 free, 1 the page copying it, 2 being written */
        G.slotW = G.w; G.slotH = G.h;
        if (!G.flip) { G.flip = gl.createFramebuffer(); G.flipRb = gl.createRenderbuffer(); G.pbo = [gl.createBuffer(), gl.createBuffer()]; }
        gl.bindRenderbuffer(gl.RENDERBUFFER, G.flipRb);
        gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, G.w, G.h);
        gl.bindFramebuffer(gl.FRAMEBUFFER, G.flip);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, G.flipRb);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        for (const b of G.pbo) { gl.bindBuffer(gl.PIXEL_PACK_BUFFER, b); gl.bufferData(gl.PIXEL_PACK_BUFFER, size, gl.STREAM_READ); }
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
        G.pending = -1;
        postMessage({ type: 'slots', slots: G.slots.map((v) => v.buffer), owner: G.owner, w: G.w, h: G.h });
    }
    gl.disable(gl.SCISSOR_TEST);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, G.kept);
    gl.blitFramebuffer(0, 0, G.w, G.h, 0, 0, G.w, G.h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, G.flip);
    gl.blitFramebuffer(0, 0, G.w, G.h, 0, G.h, G.w, 0, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.flip);
    const into = G.pending === 0 ? 1 : 0;
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, G.pbo[into]);
    gl.readPixels(0, 0, G.w, G.h, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    gl.enable(gl.SCISSOR_TEST);
    G.drawn = false;
    if (G.pending >= 0) {
        let slot = (G.posted || 0) & 3;
        while (Atomics.compareExchange(G.owner, slot, 0, 2) !== 0) slot = (slot + 1) & 3;
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, G.pbo[G.pending]);
        gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, G.slots[slot]);
        Atomics.store(G.owner, slot, 0);
        postMessage({ type: 'pixels', slot });
        G.posted = (G.posted || 0) + 1;
    }
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    G.pending = into;
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
#define FRAME_VERTS (1u << 18)
static uint8_t* gl_verts;
static uint8_t* gl_rgba;
static uint32_t gl_rgba_size;

static int gl_gpu_vertices;

static void gl_start(uint32_t backbuffer) {
    gl_backbuffer = backbuffer;
    int r = js_gl_init((int)SW(backbuffer), (int)SH(backbuffer));
    gl_on = r != 0;
    gl_gpu_vertices = r == 2;
    if (gl_on) {
        gl_verts = malloc(FRAME_VERTS * 40);
        port_log("render: WebGL2, vertices on the %s", gl_gpu_vertices ? "GPU" : "CPU");
    }
}

/* Textures are uploaded (level 0, then mipmapped) when Lock marked them dirty: +56 is 2 once uploaded. */
static uint32_t gl_texture(uint32_t tex) {
    if (!tex) return 0;
    uint32_t s = M32(tex + 8) == 3 ? M32(M32(tex + 40)) : tex;
    if (!s || !SBITS(s)) return 0;
    if (M32(tex + 56) != 2) {
        gl_flush();
        double t0 = prof_mark();
        uint32_t w = SW(s), h = SH(s);
        if (w * h * 4 > gl_rgba_size) { free(gl_rgba); gl_rgba_size = w * h * 4; gl_rgba = malloc(gl_rgba_size); }
        surface_rgba(s, gl_rgba);
        js_gl_texture(tex, (int)w, (int)h, gl_rgba);
        M32(tex + 56) = 2;
        prof_upload += prof_mark() - t0;
    }
    return tex;
}

/* A frame's draws are recorded and handed to JavaScript in one call: the client issues thousands of tiny draws per
   frame and each call into JavaScript costs far more than the vertices it carries. Consecutive triangle draws with
   identical state share one command. Anything that must see the drawing so far (a texture upload, a clear, a
   readback, present) runs the list first. */
#define MAX_CMDS 8192
/* g: a draw straight from a vertex buffer (gl_buffer_draw), all zero for one from the recorded vertices. */
typedef struct { int32_t prim, first, count, st[41], g[16]; } GlCmd;
static GlCmd gl_cmds[MAX_CMDS];
static uint32_t gl_ncmds, gl_nverts;
/* Vertex-shader constants of buffer draws: one block of up to GL_CONSTS vec4s per draw, each starting on a 256-byte
   boundary so a draw can bind its own range, and followed by enough room for the full block the shader declares. A draw
   whose constants match the previous block's reuses it. */
#define GL_CONSTS 75
#define GL_CONST_SIZE (GL_CONSTS * 16)
static float gl_consts[(MAX_CMDS * 1280 + GL_CONST_SIZE) / 4] __attribute__((aligned(16)));
static uint32_t gl_cbytes, gl_clast, gl_clen;

static void gl_flush(void) {
    if (!gl_ncmds) return;
    double t = prof_mark();
    js_gl_run(gl_verts, (int)gl_nverts, (const int32_t*)gl_cmds, (int)gl_ncmds, (const uint8_t*)gl_consts,
              gl_cbytes ? (int)(gl_cbytes + GL_CONST_SIZE) : 0);
    prof_submit += prof_mark() - t;
    gl_ncmds = gl_nverts = gl_cbytes = gl_clen = 0;
}

/* The command a draw appends to: the current one when it is a triangle list with the same state, else a new one. */
static GlCmd* gl_command(int32_t prim, const int32_t* st, uint32_t verts) {
    if (gl_nverts + verts > FRAME_VERTS || gl_ncmds == MAX_CMDS) gl_flush();
    GlCmd* last = gl_ncmds ? &gl_cmds[gl_ncmds - 1] : 0;
    if (last && prim == 4 && last->prim == 4 && !last->g[0] && !memcmp(last->st, st, sizeof last->st)) return last;
    GlCmd* c = &gl_cmds[gl_ncmds++];
    c->prim = prim;
    c->first = (int32_t)gl_nverts;
    c->count = 0;
    memcpy(c->st, st, sizeof c->st);
    memset(c->g, 0, sizeof c->g);
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

/* A vertex or index buffer's copy on the GPU, uploaded again after a Lock that could write it: +56 is 2 while current. */
static void gl_buffer(uint32_t b, int index) {
    if (M32(b + 56) == 2) return;
    gl_flush();
    double t0 = prof_mark();
    js_gl_buffer(b, index, GPTR(M32(b + 28)), (int)M32(b + 36));
    M32(b + 56) = 2;
    prof_upload += prof_mark() - t0;
}

static void put_mat(float* c, const Mat* m) { memcpy(c, m->m, sizeof m->m); }
static void put_color(float* c, uint32_t argb) {
    c[0] = ((argb >> 16) & 255) / 255.0f; c[1] = ((argb >> 8) & 255) / 255.0f; c[2] = (argb & 255) / 255.0f; c[3] = (argb >> 24) / 255.0f;
}

/* Draws from the bound vertex (and index) buffer with the transform, lighting, fog and texture coordinates done by
   the vertex shader, so static geometry is uploaded once instead of transformed on the CPU every frame. Returns 0 for
   a draw this path does not cover, which then goes through fetch(). */
static int gl_buffer_draw(uint32_t prim, uint32_t n, uint32_t first, int32_t base_vertex, int indexed) {
    if (!gl_gpu_vertices || R.target != gl_backbuffer || dump_state == 2 || LY.pos != 2 || prim < 1 || prim > 6) return 0;
    uint32_t vb = R.vb, ib = indexed ? R.ib : 0, stride = R.vb_stride;
    if (!stride || M32(vb + 64) || (M32(vb + 48) & 0x200) || stride & 3) return 0;   /* GL reads stride 0 as packed */
    if (ib && (M32(ib + 64) || (M32(ib + 48) & 0x200))) return 0;
    int64_t vbase = (int64_t)R.vb_offset + (int64_t)base_vertex * stride;
    if (vbase < 0 || (vbase & 3)) return 0;
    if (LY.fog_mode > 3) return 0;
    int idx32 = ib && M32(ib + 20) == 102;
    int32_t key = LY.lit | LY.fog_mode << 1;
    int32_t at[2] = {-1, -1}, tn[2] = {0, 0};
    for (int st = 0; st < STAGES; st++) {
        if (LY.stage_gen[st] > 2) return 0;
        uint32_t ttff = LY.ttff[st], count = ttff & 7, set = (uint32_t)LY.stage_set[st];
        if (count > 4) return 0;
        key |= LY.stage_gen[st] << (3 + 2 * st) | (int32_t)(count | (ttff & 256 ? 8 : 0)) << (7 + 4 * st);
        if (!LY.stage_gen[st] && LY.tex_n[set]) { at[st] = (int32_t)LY.tex_at[set]; tn[st] = (int32_t)LY.tex_n[set]; }
        key |= tn[st] << (25 + 3 * st);
    }
    int colorvertex = R.rs[141] && LY.has_diffuse;
    int nlights = 0;
    for (int i = 0; i < 8; i++) nlights += LY.lit && R.light_on[i];
    key |= LY.has_diffuse << 15 | ((R.fvf & 0x10) != 0) << 16 | (colorvertex && R.rs[145] == 1) << 17 |
           (colorvertex && R.rs[147] == 1) << 18 | (colorvertex && R.rs[148] == 1) << 19 | nlights << 20;

    gl_buffer(vb, 0);
    if (ib) gl_buffer(ib, 1);
    int32_t st[41];
    gl_state(st);
    if (gl_ncmds == MAX_CMDS) gl_flush();

    float* c = gl_consts + gl_cbytes / 4;
    memset(c, 0, GL_CONST_SIZE);
    put_mat(c, &wvp); put_mat(c + 16, &wv); put_mat(c + 32, &R.world); put_mat(c + 48, &R.tex[0]); put_mat(c + 64, &R.tex[1]);
    c[80] = LY.vx; c[81] = LY.vy; c[82] = LY.vw; c[83] = LY.vh;
    c[84] = (float)SW(R.target); c[85] = (float)SH(R.target);
    c[88] = LY.fog_start; c[89] = LY.fog_end; c[90] = LY.fog_density;
    memcpy(c + 92, R.material + 0, 16); memcpy(c + 96, R.material + 4, 16); memcpy(c + 100, R.material + 12, 16);
    put_color(c + 104, R.rs[139]);
    for (int i = 0, k = 0; i < 8 && LY.lit; i++) {
        if (!R.light_on[i]) continue;
        const float* L = R.light[i];
        float* d = c + 108 + 24 * k++;
        d[0] = L[0]; d[1] = L[19];
        memcpy(d + 4, L + 1, 12); memcpy(d + 8, L + 9, 12); memcpy(d + 12, L + 13, 12); memcpy(d + 16, L + 16, 12);
        memcpy(d + 20, L + 21, 12);
    }
    uint32_t len = (27u + 6u * (uint32_t)nlights) * 16u, block = gl_clast;
    if (len != gl_clen || memcmp(c, gl_consts + gl_clast / 4, len)) {
        block = gl_clast = gl_cbytes;
        gl_clen = len;
        gl_cbytes += (len + 255) & ~255u;
    }

    GlCmd* cmd = &gl_cmds[gl_ncmds++];
    static const int32_t gl_prim[7] = {0, 0, 1, 3, 4, 5, 6};   /* POINTS LINES LINE_STRIP TRIANGLES _STRIP _FAN */
    cmd->prim = gl_prim[prim];
    cmd->first = (int32_t)first;
    cmd->count = (int32_t)n;
    memcpy(cmd->st, st, sizeof cmd->st);
    int32_t* g = cmd->g;
    g[0] = 1; g[1] = (int32_t)vb; g[2] = (int32_t)ib; g[3] = idx32; g[4] = (int32_t)vbase; g[5] = (int32_t)stride;
    g[6] = R.fvf & 0x10 ? (int32_t)LY.normal_at : -1;
    g[7] = LY.has_diffuse ? (int32_t)LY.diffuse_at : -1;
    g[8] = at[0]; g[9] = tn[0]; g[10] = at[1]; g[11] = tn[1];
    g[12] = (int32_t)(first * (idx32 ? 4u : 2u));
    g[13] = (int32_t)block; g[14] = key;
    S.draws++;
    prof_fetch += prof_mark() - fetch_t0;
    return 1;
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

static void gl_resize(uint32_t backbuffer) {
    gl_flush();
    gl_backbuffer = backbuffer;
    if (gl_on) js_gl_resize((int)SW(backbuffer), (int)SH(backbuffer));
}

/* Puts what the host wrote into the backbuffer's memory (a lock, a GDI DC, a surface copy) onto the drawing buffer. */
void r_write_back(uint32_t surface) {
    if (!gl_on || surface != gl_backbuffer) return;
    gl_flush();
    uint32_t w = SW(surface), h = SH(surface);
    if (w * h * 4 > gl_rgba_size) { free(gl_rgba); gl_rgba_size = w * h * 4; gl_rgba = malloc(gl_rgba_size); }
    uint8_t* p = gl_rgba;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++, p += 4) {
            uint32_t t = texel(surface, x, y);
            p[0] = (uint8_t)(t >> 16); p[1] = (uint8_t)(t >> 8); p[2] = (uint8_t)t; p[3] = 255;
        }
    js_gl_write(0, 0, (int)w, (int)h, gl_rgba);
}
int r_on_gpu(uint32_t surface) { return gl_on && surface == gl_backbuffer; }
void r_fill(uint32_t color, int x, int y, int w, int h) { gl_clear(1, color, 0, x, y, w, h); }

void r_preload(uint32_t tex) { if (gl_on) gl_texture(tex); }
void r_texture_lod(uint32_t tex, uint32_t lod) { if (gl_on) js_gl_texture_lod(tex, (int)lod); }

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
static void gl_resize(uint32_t backbuffer) { (void)backbuffer; }
void r_write_back(uint32_t surface) { (void)surface; }
int r_on_gpu(uint32_t surface) { (void)surface; return 0; }
void r_fill(uint32_t color, int x, int y, int w, int h) { (void)color; (void)x; (void)y; (void)w; (void)h; }
static void gl_draw_list(uint32_t prim, Vtx* v, uint32_t n) { (void)prim; (void)v; (void)n; }
static int gl_buffer_draw(uint32_t prim, uint32_t n, uint32_t first, int32_t base_vertex, int indexed) {
    (void)prim; (void)n; (void)first; (void)base_vertex; (void)indexed;
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
