// The game's WebGL2 renderer, on its own thread. The game worker (render_gl.inc) records each frame's drawing as a
// command stream and posts it here once per Present, so its thread spends no time in WebGL calls and the two run in
// parallel; this worker draws the frame and hands the page an ImageBitmap. A synchronous read of the frame arrives as
// a separate message with a SharedArrayBuffer that the game worker waits on.
//
// Stream: little-endian int32 words, each command an opcode then its operands; byte payloads are padded to 4 bytes.
//   1 init w h | 2 texture id w h rgba | 3 lod id level | 4 list nverts ncmds verts(40 bytes each) cmds(44 words each)
//   5 tnl mask words... [vertices] [indices] | 6 clear flags color z x y w h | 7 write x y w h rgba | 8 resize w h
//   9 present
let G = null, port = null, commandPool = null;

function report(gl) {
    const d = gl.getExtension('WEBGL_debug_renderer_info');
    return d ? gl.getParameter(d.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
}

function init(w, h) {
    const canvas = new OffscreenCanvas(w, h);
    const gl = canvas.getContext('webgl2', { alpha: false, antialias: false, preserveDrawingBuffer: false, depth: true });
    if (!gl) { postMessage({ type: 'log', text: 'WebGL2 is unavailable in the renderer worker' }); return; }
    postMessage({ type: 'log', text: 'WebGL renderer: ' + report(gl) + ' (renderer thread)' });
    const vs = '#version 300 es\n' +
        'in vec4 aPos; in vec4 aCol; in vec4 aUV; in float aFog;\n' +
        'out vec4 vCol; out vec4 vUV; out float vFog;\n' +
        'void main() { gl_Position = aPos; gl_PointSize = 1.0; vCol = aCol.bgra; vUV = aUV; vFog = aFog; }\n';
    // fetch() and light_vertex() of render.c, term for term, for vertices read straight from the game's buffers: the
    // same screen mapping as put_vertex(), the same lighting sum and colour sources, the same fog from camera depth.
    const tnlVs = '#version 300 es\n' +
        'in vec3 aP; in vec3 aN; in vec4 aD; in vec4 aT0; in vec4 aT1;\n' +
        // Per-draw transforms come from one uniform buffer per frame (see frameData), bound with one call per draw.
        'layout(std140) uniform Transform { mat4 uWorld, uView, uProj, uTexM0, uTexM1; vec4 uVP; vec4 uTarget4; ivec4 uTexA, uTexB; };\n' +
        '#define uTarget uTarget4.xy\n#define uTN uTexA.xy\n#define uGen uTexA.zw\n#define uTTF uTexB.xy\n#define uHasN uTexB.z\n' +
        'uniform int uLit, uNumLights, uFogMode;\n' +
        'uniform ivec3 uSrc;\n' +
        'uniform vec4 uMatD, uMatA, uMatE; uniform vec3 uGAmb, uFogP;\n' +
        'uniform vec4 uLight[40];\n' +
        'out vec4 vCol; out vec4 vUV; out float vFog;\n' +
        // A stage's coordinates as fetch() builds them: missing components of a set are (.., 1, 0) as in D3D,
        // camera-space position or normal when generated, then the texture matrix with an optional projective divide.
        'vec2 coord(vec4 a, int n, int gen, int ttf, mat4 m, vec4 camPos, vec4 camNrm) {\n' +
        '  vec4 tc = gen == 2 ? camPos : gen == 1 ? camNrm\n' +
        '    : vec4(n > 0 ? a.x : 0.0, n > 1 ? a.y : 0.0, n > 2 ? a.z : 1.0, n > 3 ? a.w : (n == 3 ? 1.0 : 0.0));\n' +
        '  if ((ttf & 7) == 0) return tc.xy;\n' +
        '  vec4 r = m * tc;\n' +
        '  float q = (ttf & 256) != 0 ? r[(ttf & 7) - 1] : 1.0;\n' +
        '  return r.xy / q;\n' +
        '}\n' +
        'void main() {\n' +
        '  vec4 p = vec4(aP, 1.0), wpos = uWorld * p, camPos = uView * wpos, c = uProj * camPos;\n' +
        '  float w = c.w != 0.0 ? c.w : 1.0;\n' +
        '  float sx = uVP.x + (c.x / w + 1.0) * 0.5 * uVP.z, sy = uVP.y + (1.0 - c.y / w) * 0.5 * uVP.w;\n' +
        '  gl_Position = vec4(((sx + 0.5) / uTarget.x * 2.0 - 1.0) * w, (1.0 - (sy + 0.5) / uTarget.y * 2.0) * w, (c.z / w * 2.0 - 1.0) * w, w);\n' +
        '  gl_PointSize = 1.0;\n' +
        '  vec4 vc = aD.bgra;\n' +
        '  if (uLit != 0) {\n' +
        '    vec3 wp = wpos.xyz, n = mat3(uWorld) * aN;\n' +
        '    float len = length(n);\n' +
        '    if (len > 0.0) n /= len;\n' +
        '    vec4 ds = uSrc.x != 0 ? vc : uMatD, am = uSrc.y != 0 ? vc : uMatA, em = uSrc.z != 0 ? vc : uMatE;\n' +
        '    vec3 o = em.rgb + uGAmb * am.rgb;\n' +
        '    for (int k = 0; k < 8; k++) {\n' +
        '      if (k >= uNumLights) break;\n' +
        '      vec4 a = uLight[5 * k], b = uLight[5 * k + 1], q = uLight[5 * k + 2], d = uLight[5 * k + 3], e = uLight[5 * k + 4];\n' +
        '      vec3 dir; float att = 1.0;\n' +
        '      if (int(a.w) == 3) dir = -d.xyz;\n' +
        '      else {\n' +
        '        dir = q.xyz - wp;\n' +
        '        float d2 = dot(dir, dir), dist = sqrt(d2);\n' +
        '        if (b.w > 0.0 && dist > b.w) continue;\n' +
        '        float f = q.w + d.w * dist + e.x * d2;\n' +
        '        att = f > 0.0 ? 1.0 / f : 1.0;\n' +
        '      }\n' +
        '      float dl = length(dir), ndl = dl > 0.0 ? max(dot(n, dir) / dl, 0.0) : 0.0;\n' +
        '      o += att * (b.rgb * am.rgb + ndl * a.rgb * ds.rgb);\n' +
        '    }\n' +
        '    vCol = vec4(clamp(o, 0.0, 1.0), clamp(ds.a, 0.0, 1.0));\n' +
        '  } else vCol = vc;\n' +
        '  vec4 camNrm = uHasN != 0 ? uView * (uWorld * vec4(aN, 0.0)) : vec4(0.0);\n' +
        '  vUV = vec4(coord(aT0, uTN.x, uGen.x, uTTF.x, uTexM0, camPos, camNrm), coord(aT1, uTN.y, uGen.y, uTTF.y, uTexM1, camPos, camNrm));\n' +
        '  vFog = 1.0;\n' +
        '  if (uFogMode != 0) {\n' +
        '    float d = abs(camPos.z);\n' +
        '    vFog = uFogMode == 3 ? (uFogP.y != uFogP.x ? (uFogP.y - d) / (uFogP.y - uFogP.x) : 1.0)\n' +
        '         : uFogMode == 1 ? exp(-uFogP.z * d) : exp(-(uFogP.z * d) * (uFogP.z * d));\n' +
        '    vFog = clamp(vFog, 0.0, 1.0);\n' +
        '  }\n' +
        '}\n';
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
    const program = (vsrc, attribs, extra) => {
        const prog = gl.createProgram();
        gl.attachShader(prog, sh(gl.VERTEX_SHADER, vsrc));
        gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fs));
        attribs.forEach((name, loc) => gl.bindAttribLocation(prog, loc, name));
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
        gl.useProgram(prog);
        const u = {};
        for (const n of ['uTex0', 'uTex1', 'uS0', 'uS1', 'uA0', 'uA1', 'uTFactor', 'uATest', 'uARef', 'uFog', ...extra]) u[n] = gl.getUniformLocation(prog, n);
        gl.uniform1i(u.uTex0, 0);
        gl.uniform1i(u.uTex1, 1);
        const block = gl.getUniformBlockIndex(prog, 'Transform');
        if (block !== gl.INVALID_INDEX) gl.uniformBlockBinding(prog, block, 0);
        return { prog, u, U: new Int32Array(41).fill(-1), vao: gl.createVertexArray() };
    };
    const tnl = program(tnlVs, ['aP', 'aN', 'aD', 'aT0', 'aT1'],
        ['uLit', 'uNumLights', 'uFogMode', 'uSrc', 'uMatD', 'uMatA', 'uMatE', 'uGAmb', 'uFogP', 'uLight']);
    gl.bindVertexArray(tnl.vao);
    gl.enableVertexAttribArray(0);
    const cpu = program(vs, ['aPos', 'aCol', 'aUV', 'aFog'], []);
    gl.bindVertexArray(cpu.vao);
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 40, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.UNSIGNED_BYTE, true, 40, 16);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 40, 20);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 1, gl.FLOAT, false, 40, 36);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.enable(gl.SCISSOR_TEST);
    // P mirrors the game's TNL parameter block (gl_tnl_draw): each tnl command carries only the groups that changed.
    const P = new Int32Array(338), PF = new Float32Array(P.buffer), Q = new Int32Array(338), QF = new Float32Array(Q.buffer);
    const align = gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT) / 4, scratch = new Float32Array(BLOCK_FLOATS);
    const frame = { stride: Math.ceil(BLOCK_FLOATS / align) * align, ubo: new Float32Array(1 << 18), scratch, scratchInts: new Int32Array(scratch.buffer),
                    slot: new Int32Array(1 << 14), first: new Int32Array(1 << 14), vertices: new Uint8Array(1 << 22), draws: 0, slots: 0, stream: 0, next: 0 };
    G = { canvas, gl, cpu, tnl, cur: cpu, vao: cpu.vao, vbo, vbs: new Map(), ibs: new Map(), textures: new Map(), lods: new Map(), mipDirty: new Set(), w, h,
          P, PF, Q, QF, frame, ubo: gl.createBuffer(), ring: gl.createBuffer(), boundSlot: -1,
          views: new Map(), prev: new Int32Array(41).fill(-1), tnlState: false, drawn: false, listRing: undefined };
    const wrap = (a) => a === 2 ? gl.MIRRORED_REPEAT : (a === 3 || a === 4) ? gl.CLAMP_TO_EDGE : gl.REPEAT;
    const f = [gl.ONE, gl.ZERO, gl.ONE, gl.SRC_COLOR, gl.ONE_MINUS_SRC_COLOR, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA,
               gl.DST_ALPHA, gl.ONE_MINUS_DST_ALPHA, gl.DST_COLOR, gl.ONE_MINUS_DST_COLOR, gl.SRC_ALPHA_SATURATE];
    const eq = [gl.FUNC_ADD, gl.FUNC_ADD, gl.FUNC_SUBTRACT, gl.FUNC_REVERSE_SUBTRACT, gl.MIN, gl.MAX];
    // Applies 41 state words (S[base..base+40], layout in render_gl.inc above gl_state) for a program. Uniform values
    // belong to each program, so they are cached per program; blending, depth, scissor, culling and texture bindings
    // are context state, cached once. Groups are tested one by one (no short-circuit) so the caches stay in step.
    G.apply = (p, A, base) => {
        const S = (i) => A[base + i], U = p.U, u = p.u, P = G.prev;
        const uniforms = (from, to) => { let c = false; for (let i = from; i < to; i++) if (U[i] !== S(i)) { U[i] = S(i); c = true; } return c; };
        const state = (from, to) => { let c = false; for (let i = from; i < to; i++) if (P[i] !== S(i)) { P[i] = S(i); c = true; } return c; };
        const s0 = uniforms(0, 6), t0 = uniforms(12, 13), s1 = uniforms(6, 12), t1 = uniforms(13, 14);
        if (s0 || t0) { gl.uniform4i(u.uS0, S(0), S(1), S(2), S(3)); gl.uniform4i(u.uA0, S(4), S(5), S(12) ? 1 : 0, 0); }
        if (s1 || t1) { gl.uniform4i(u.uS1, S(6), S(7), S(8), S(9)); gl.uniform4i(u.uA1, S(10), S(11), S(13) ? 1 : 0, 0); }
        if (uniforms(14, 15)) {
            const tf = S(14) >>> 0;
            gl.uniform4f(u.uTFactor, ((tf >>> 16) & 255) / 255, ((tf >>> 8) & 255) / 255, (tf & 255) / 255, (tf >>> 24) / 255);
        }
        if (uniforms(15, 18)) { gl.uniform2i(u.uATest, S(15), S(16)); gl.uniform1f(u.uARef, S(17) & 255); }
        for (let j = 0; j < 2; j++) {
            const b = 29 + 5 * j, texChanged = P[12 + j] !== S(12 + j), samplerChanged = state(b, b + 5);
            P[12 + j] = S(12 + j);
            const id = S(12 + j) >>> 0;
            const t = id ? G.textures.get(id) : null;
            if (!t || (!texChanged && !samplerChanged)) continue;
            gl.activeTexture(gl.TEXTURE0 + j);
            gl.bindTexture(gl.TEXTURE_2D, t);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, S(b) >= 2 ? gl.LINEAR : gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, minFilter(gl, S(b + 1), S(b + 2)));
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap(S(b + 3)));
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap(S(b + 4)));
            if (S(b + 2) || G.lods.get(id) > 0) ensureMipmaps(id);
        }
        if (state(18, 22)) {
            if (S(18)) {
                gl.enable(gl.BLEND);
                gl.blendFunc(f[S(19)] ?? gl.ONE, f[S(20)] ?? gl.ZERO);
                gl.blendEquation(eq[S(21)] ?? gl.FUNC_ADD);
            } else gl.disable(gl.BLEND);
        }
        if (state(22, 25)) {
            if (S(22)) {
                gl.enable(gl.DEPTH_TEST);
                gl.depthFunc(0x1FF + Math.min(Math.max(S(24), 1), 8));
                gl.depthMask(S(23) !== 0);
            } else gl.disable(gl.DEPTH_TEST);
        }
        if (state(25, 29)) gl.scissor(S(25), G.h - S(26) - S(28), S(27), S(28));
        if (uniforms(38, 40)) {
            const fc = S(38) >>> 0;
            gl.uniform4f(u.uFog, ((fc >>> 16) & 255) / 255, ((fc >>> 8) & 255) / 255, (fc & 255) / 255, S(39) ? 1 : 0);
        }
        // Screen-clockwise (y down) stays clockwise in clip space (y up): a GL back face.
        if (state(40, 41)) {
            if (S(40) === 2 || S(40) === 3) { gl.enable(gl.CULL_FACE); gl.cullFace(S(40) === 2 ? gl.BACK : gl.FRONT); }
            else gl.disable(gl.CULL_FACE);
        }
    };
}

// D3D separates the minification filter from whether/how mip levels are sampled.
function minFilter(gl, min, mip) {
    if (!mip) return min >= 2 ? gl.LINEAR : gl.NEAREST;
    if (mip === 1) return min >= 2 ? gl.LINEAR_MIPMAP_NEAREST : gl.NEAREST_MIPMAP_NEAREST;
    return min >= 2 ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST_MIPMAP_LINEAR;
}

// The caller has bound this texture. Updates invalidate lower levels, but a
// non-mipmapped draw never needs to rebuild them (notably the text scratchpad).
function ensureMipmaps(id) {
    if (!G.mipDirty.has(id)) return;
    const gl = G.gl, level = G.lods.get(id) || 0;
    if (level) gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, 0);
    gl.generateMipmap(gl.TEXTURE_2D);
    if (level) gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, level);
    G.mipDirty.delete(id);
}

function texture(id, w, h, B, at) {
    const gl = G.gl;
    let t = G.textures.get(id);
    if (!t) { t = gl.createTexture(); G.textures.set(id, t); }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, B, at);
    G.mipDirty.add(id);
    if (G.lods.has(id)) gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, G.lods.get(id));
    G.prev[12] = -1; G.prev[13] = -1;
    G.tnlState = false;
}

// The changed part of a texture already uploaded whole.
function textureRect(id, x, y, w, h, B, at) {
    const gl = G.gl, t = G.textures.get(id);
    if (!t) return;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, w, h, gl.RGBA, gl.UNSIGNED_BYTE, B, at);
    G.mipDirty.add(id);
    G.prev[12] = -1; G.prev[13] = -1;
    G.tnlState = false;
}

// SetLOD: the most detailed mip level sampled.
function lod(id, level) {
    const gl = G.gl, t = G.textures.get(id);
    G.lods.set(id, level);
    if (!t) return;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, t);
    if (level > 0) ensureMipmaps(id);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, level);
    G.prev[12] = -1;
    G.tnlState = false;
}

// One recorded CPU-path list: its vertices go into one large buffer (orphaned when full, so the GPU never waits on a
// range still being read), then each command (GL primitive, first vertex, count, 41 state words) sets what changed.
function list(B, W, at, nverts, cmds, ncmds) {
    const gl = G.gl, C = G.cpu;
    G.drawn = true;
    G.tnlState = false;
    if (G.cur !== C) { gl.useProgram(C.prog); G.cur = C; }
    if (G.vao !== C.vao) { gl.bindVertexArray(C.vao); G.vao = C.vao; }
    gl.bindBuffer(gl.ARRAY_BUFFER, G.vbo);
    const RING = 1 << 24, bytes = nverts * 40;
    if (G.listRing === undefined || G.listRing + bytes > RING) { gl.bufferData(gl.ARRAY_BUFFER, RING, gl.STREAM_DRAW); G.listRing = 0; }
    gl.bufferSubData(gl.ARRAY_BUFFER, G.listRing, B, at, bytes);
    const first = G.listRing / 40;
    G.listRing += bytes;
    for (let k = 0; k < ncmds; k++) {
        const cmd = cmds + 44 * k;
        G.apply(C, W, cmd + 3);
        gl.drawArrays(W[cmd], first + W[cmd + 1], W[cmd + 2]);
    }
}

// Groups of the parameter block a tnl command may carry, in stream order, by the bit of word 337 that sends them.
const GROUPS = [[2, 27, 43], [4, 43, 75], [8, 75, 81], [16, 81, 96], [32, 96, 99], [64, 300, 337], [128, 259, 300]];
const padded = (n) => (n + 3) & ~3;

// Reads one tnl command into the parameter mirror M; returns its change mask and where its payload starts.
function decode(W, at, M) {
    const mask = W[at++];
    M.set(W.subarray(at, at + 27), 0); at += 27;
    for (const [bit, from, to] of GROUPS) if (mask & bit) { M.set(W.subarray(at, at + to - from), from); at += to - from; }
    if (mask & 16) { const n = 20 * M[25]; M.set(W.subarray(at, at + n), 99); at += n; }
    return [mask, at];
}
const payloadEnd = (M, at) => at + ((M[5] === -3 || M[8]) ? padded(M[7]) >> 2 : 0) + ((M[2] && M[12]) ? padded(M[11]) >> 2 : 0);

// The Transform block (std140): world, view, projection, two texture matrices, viewport, target size, then the
// texture-coordinate ints (count per stage, generation, transform flags, has-normal).
const BLOCK_FLOATS = 96;
function transform(M, MF, out) {
    out.set(MF.subarray(27, 75), 0);       // world, view, projection
    out.set(MF.subarray(305, 337), 48);    // texture matrices
    out.set(MF.subarray(75, 79), 80);      // viewport
    out[84] = MF[79]; out[85] = MF[80]; out[86] = 0; out[87] = 0;
    const I = G.frame.scratchInts;
    I[88] = M[18]; I[89] = M[20]; I[90] = M[300]; I[91] = M[301]; I[92] = M[302]; I[93] = M[303]; I[94] = M[304]; I[95] = 0;
}

// First pass over a posted stream, before any draw: every draw's transforms go into one uniform buffer (a new slot
// only when they change) and every streamed vertex block into one vertex buffer, each uploaded with a single call.
// The second pass then binds a slot per draw instead of re-sending up to five matrices.
function frameData(W, start) {
    const gl = G.gl, F = G.frame, Q = G.Q, QF = G.QF, scratch = F.scratch, scratchInts = F.scratchInts;
    F.draws = 0; F.slots = 0; F.stream = 0; F.next = 0;
    for (let at = start; at < W.length;) {
        const op = W[at++];
        switch (op) {
            case 1: case 3: case 8: at += 2; break;
            case 2: at += 3 + W[at + 1] * W[at + 2]; break;
            case 4: at += 2 + W[at] * 10 + W[at + 1] * 44; break;
            case 5: {
                const [mask, data] = decode(W, at, Q);
                if (!F.slots || (mask & 79)) {
                    transform(Q, QF, scratch);
                    let same = F.slots > 0;
                    if (same) {
                        const last = new Int32Array(F.ubo.buffer, (F.slots - 1) * F.stride * 4, BLOCK_FLOATS);
                        for (let k = 0; k < BLOCK_FLOATS; k++) if (scratchInts[k] !== last[k]) { same = false; break; }
                    }
                    if (!same) {
                        if ((F.slots + 1) * F.stride > F.ubo.length) { const grown = new Float32Array(F.ubo.length * 2); grown.set(F.ubo); F.ubo = grown; }
                        F.ubo.set(scratch, F.slots * F.stride);
                        F.slots++;
                    }
                }
                if (F.draws >= F.slot.length) {
                    const n = F.slot.length * 2, slot = new Int32Array(n), first = new Int32Array(n);
                    slot.set(F.slot); first.set(F.first); F.slot = slot; F.first = first;
                }
                F.slot[F.draws] = F.slots - 1;
                if (Q[5] === -3) {
                    const stride = Q[13], size = Q[7];
                    const offset = Math.ceil(F.stream / stride) * stride;
                    if (offset + size > F.vertices.length) { const grown = new Uint8Array(Math.max(F.vertices.length * 2, offset + size)); grown.set(F.vertices); F.vertices = grown; }
                    F.vertices.set(new Uint8Array(W.buffer, W.byteOffset + data * 4, size), offset);
                    F.first[F.draws] = offset / stride;
                    F.stream = offset + size;
                }
                F.draws++;
                at = payloadEnd(Q, data);
                break;
            }
            case 6: at += 7; break;
            case 7: at += 4 + W[at + 2] * W[at + 3]; break;
            case 9: break;
            case 10: at += 5 + W[at + 3] * W[at + 4]; break;
            default: throw new Error('renderer: unknown command ' + op);
        }
    }
    if (F.slots) {
        gl.bindBuffer(gl.UNIFORM_BUFFER, G.ubo);
        gl.bufferData(gl.UNIFORM_BUFFER, F.ubo.subarray(0, F.slots * F.stride), gl.STREAM_DRAW);
    }
    if (F.stream) {
        gl.bindBuffer(gl.ARRAY_BUFFER, G.ring);
        gl.bufferData(gl.ARRAY_BUFFER, F.vertices.subarray(0, F.stream), gl.STREAM_DRAW);
    }
    G.boundSlot = -1;
}

// One draw transformed, lit and fogged on the GPU from the game's vertex (and index) buffer, which is cached by object
// and sent again only when the game has locked it since. Returns the word after the command.
function tnl(B, W, at) {
    const gl = G.gl, T = G.tnl, P = G.P, F = G.frame, draw = F.next++;
    const [mask, data] = decode(W, at, P);
    at = data;
    const I = (k) => P[k];
    const PF = (k, n) => {
        const key = k * 1024 + n;
        let v = G.views.get(key);
        if (!v) { v = G.PF.subarray(k, k + n); G.views.set(key, v); }
        return v;
    };
    G.drawn = true;
    if (G.cur !== T) { gl.useProgram(T.prog); G.cur = T; }
    if (G.vao !== T.vao) { gl.bindVertexArray(T.vao); G.vao = T.vao; }
    const upload = (cache, target, id, size, dirty, offset) => {
        let b = cache.get(id);
        if (!b) { b = { buffer: gl.createBuffer(), size: -1 }; cache.set(id, b); }
        gl.bindBuffer(target, b.buffer);
        // Streamed data is given fresh storage each time, so the GPU never waits on a draw still reading it.
        if (dirty === 2) { gl.bufferData(target, B.subarray(offset, offset + size), gl.STREAM_DRAW); b.size = -1; return; }
        if (b.size !== size) { gl.bufferData(target, size, gl.STATIC_DRAW); b.size = size; }
        if (dirty) gl.bufferSubData(target, 0, B, offset, size);
    };
    // Buffer -3 is streamed, non-indexed data (merged small draws, DrawPrimitiveUP), all of it uploaded by frameData.
    let first = I(3);
    if (I(5) === -3) {
        first = F.first[draw];
        at += padded(I(7)) >> 2;
        if (mask & 1) gl.bindBuffer(gl.ARRAY_BUFFER, G.ring);
    } else if (I(8) || (mask & 1)) {
        upload(G.vbs, gl.ARRAY_BUFFER, I(5) >>> 0, I(7), I(8), at * 4);
        if (I(8)) at += padded(I(7)) >> 2;
    }
    if (mask & 1) {
        const stride = I(13), base = I(14);
        gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, base);
        const attrib = (loc, offset, n, type, normalized, x, y, z, w) => {
            if (offset < 0) { gl.disableVertexAttribArray(loc); gl.vertexAttrib4f(loc, x, y, z, w); }
            else { gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, n, type, normalized, stride, base + offset); }
        };
        attrib(1, I(15), 3, gl.FLOAT, false, 0, 0, 1, 0);
        attrib(2, I(16), 4, gl.UNSIGNED_BYTE, true, 1, 1, 1, 1);
        attrib(3, I(17), I(18), gl.FLOAT, false, 0, 0, 0, 1);
        attrib(4, I(19), I(20), gl.FLOAT, false, 0, 0, 0, 1);
    }
    const slot = F.slot[draw];
    if (slot !== G.boundSlot) { gl.bindBufferRange(gl.UNIFORM_BUFFER, 0, G.ubo, slot * F.stride * 4, BLOCK_FLOATS * 4); G.boundSlot = slot; }
    const u = T.u;
    if (mask & 16) {
        gl.uniform1i(u.uLit, I(21));
        gl.uniform3i(u.uSrc, I(22), I(23), I(24));
        gl.uniform1i(u.uNumLights, I(25));
        gl.uniform4fv(u.uMatD, PF(81, 4));
        gl.uniform4fv(u.uMatA, PF(85, 4));
        gl.uniform4fv(u.uMatE, PF(89, 4));
        gl.uniform3fv(u.uGAmb, PF(93, 3));
        if (I(25) > 0) gl.uniform4fv(u.uLight, PF(99, 20 * I(25)));
    }
    if (mask & 32) { gl.uniform1i(u.uFogMode, I(26)); gl.uniform3fv(u.uFogP, PF(96, 3)); }
    // Bit 128: the 41 state words changed. Everything else that touches GL state clears G.tnlState.
    if ((mask & 128) || !G.tnlState) { G.apply(T, P, 259); G.tnlState = true; }
    if (I(2)) {
        // The index buffer binding is part of the VAO, which only these draws use.
        if (I(12) || T.ib !== I(9)) { upload(G.ibs, gl.ELEMENT_ARRAY_BUFFER, I(9) >>> 0, I(11), I(12), at * 4); T.ib = I(9); }
        if (I(12)) at += padded(I(11)) >> 2;
        gl.drawElements(I(0), I(1), I(4), I(3));
    } else gl.drawArrays(I(0), first, I(1));
    return at;
}

function clear(flags, color, z, x, y, w, h) {
    const gl = G.gl;
    G.drawn = true;
    gl.scissor(x, G.h - y - h, w, h);
    let bits = 0;
    if (flags & 1) {
        gl.clearColor(((color >>> 16) & 255) / 255, ((color >>> 8) & 255) / 255, (color & 255) / 255, 1);
        bits |= gl.COLOR_BUFFER_BIT;
    }
    if (flags & 2) { gl.depthMask(true); gl.clearDepth(z); bits |= gl.DEPTH_BUFFER_BIT; }
    if (bits) gl.clear(bits);
    G.prev[22] = G.prev[23] = G.prev[24] = -1; G.prev[25] = -1;
    G.tnlState = false;
}

// Rows of RGBA (top row first) written into the drawing buffer at x, y.
function write(x, y, w, h, B, at) {
    const gl = G.gl;
    if (!G.wtex) { G.wtex = gl.createTexture(); G.wfb = gl.createFramebuffer(); }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, G.wtex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, B, at);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.wfb);
    gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, G.wtex, 0);
    gl.disable(gl.SCISSOR_TEST);
    gl.blitFramebuffer(0, 0, w, h, x, G.h - y, x + w, G.h - y - h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.enable(gl.SCISSOR_TEST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    G.drawn = true;
    G.prev[12] = -1; G.prev[25] = -1;
    G.tnlState = false;
}

function resize(w, h) {
    const gl = G.gl;
    G.canvas.width = w;
    G.canvas.height = h;
    G.w = w;
    G.h = h;
    gl.viewport(0, 0, w, h);
    if (G.kept) { gl.deleteFramebuffer(G.kept); gl.deleteRenderbuffer(G.keptRb); G.kept = null; }
    G.prev[25] = -1;
    G.tnlState = false;
}

// Keeps a copy of the frame (a read before the next draw sees it) and hands the drawing buffer to the page.
function present() {
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
}

// The frame as RGBA rows, bottom row first, into the game worker's buffer; [0] of its header flips to 1 when done.
function read(m) {
    const gl = G.gl, out = new Uint8Array(m.pixels);
    if (!G.drawn && G.kept) gl.bindFramebuffer(gl.READ_FRAMEBUFFER, G.kept);
    const px = new Uint8Array(m.w * m.h * 4);
    gl.readPixels(0, 0, m.w, m.h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    out.set(px);
    Atomics.store(m.done, 0, 1);
    Atomics.notify(m.done, 0);
}

// Every 5 s: how long this thread was busy per frame, which bounds the frame rate as much as the game thread does.
const load = { busy: 0, frames: 0, draws: 0, bytes: 0, since: performance.now(), ops: new Float64Array(11), textures: new Map() };
// Texture uploads by texture, for the profile line: which ones the client keeps changing.
function uploaded(id, w, h, pixels) {
    const t = load.textures.get(id);
    if (t) { t.n++; t.pixels += pixels; } else load.textures.set(id, { w, h, n: 1, pixels });
}
function reportLoad() {
    const now = performance.now();
    if (now - load.since < 5000 || !load.frames) return;
    postMessage({ type: 'log', text: `profile: renderer thread ${(load.busy / load.frames).toFixed(1)} ms/frame busy, ` +
        `${Math.round(load.draws / load.frames)} WebGL draws, ${Math.round(load.bytes / load.frames / 1024)} KB of commands per frame; ` +
        ['', 'init', 'textures', 'lod', 'lists', 'draws', 'clears', 'writes', 'resize', 'present', 'texture parts']
            .map((name, op) => load.ops[op] >= load.frames * 0.1 ? `${name} ${(load.ops[op] / load.frames).toFixed(1)}` : '').filter(Boolean).join(', ') +
        uploadReport() });
    Object.assign(load, { busy: 0, frames: 0, draws: 0, bytes: 0, since: now });
    load.textures.clear();
    load.ops.fill(0);
}

function uploadReport() {
    const top = [...load.textures.entries()].sort((a, b) => b[1].pixels - a[1].pixels).slice(0, 4)
        .filter(([, t]) => t.pixels * 4 >= load.frames * 64 * 1024);
    if (!top.length) return '';
    return '; most uploaded: ' + top.map(([id, t]) => `${id.toString(16)} ${t.w}x${t.h} ${(t.n / load.frames).toFixed(1)}/frame ` +
        `${Math.round(t.pixels * 4 / load.frames / 1024)} KB`).join(', ');
}

function run(buffer, offset = 0, length = buffer.byteLength - offset) {
    const started = performance.now();
    try { execute(buffer, offset, length); } finally { load.busy += performance.now() - started; load.bytes += length; }
    reportLoad();
}

function execute(buffer, offset = 0, length = buffer.byteLength - offset) {
    const W = new Int32Array(buffer, offset, length / 4), B = new Uint8Array(buffer, offset, length), F32 = new Float32Array(buffer, offset, length / 4);
    if (G) frameData(W, 0);
    const detailed = !!globalThis.renderProfileDetailed;
    for (let at = 0, last = detailed ? performance.now() : 0, op = 0; at < W.length;) {
        if (detailed) { const now = performance.now(); load.ops[op] += now - last; last = now; }
        op = W[at++];
        if (op !== 1 && !G) return;
        switch (op) {
            case 1: init(W[at], W[at + 1]); at += 2; if (G) frameData(W, at); break;
            case 2: { const id = W[at] >>> 0, w = W[at + 1], h = W[at + 2]; texture(id, w, h, B, (at + 3) * 4); uploaded(id, w, h, w * h); at += 3 + w * h; break; }
            case 3: lod(W[at] >>> 0, W[at + 1]); at += 2; break;
            case 4: {
                const nverts = W[at], ncmds = W[at + 1], verts = at + 2, cmds = verts + nverts * 10;
                list(B, W, verts * 4, nverts, cmds, ncmds);
                load.draws += ncmds;
                at = cmds + ncmds * 44;
                break;
            }
            case 5: at = tnl(B, W, at); load.draws++; break;
            case 6: clear(W[at], W[at + 1] >>> 0, F32[at + 2], W[at + 3], W[at + 4], W[at + 5], W[at + 6]); at += 7; break;
            case 7: { const w = W[at + 2], h = W[at + 3]; write(W[at], W[at + 1], w, h, B, (at + 4) * 4); at += 4 + w * h; break; }
            case 8: resize(W[at], W[at + 1]); at += 2; break;
            case 9: present(); load.frames++; break;
            case 10: {
                const id = W[at] >>> 0, w = W[at + 3], h = W[at + 4];
                textureRect(id, W[at + 1], W[at + 2], w, h, B, (at + 5) * 4);
                uploaded(id, w, h, w * h);
                at += 5 + w * h;
                break;
            }
            default: throw new Error('renderer: unknown command ' + op);
        }
    }
}

onmessage = (e) => {
    if (e.data.type === 'profile') { globalThis.renderProfileDetailed = !!e.data.enabled; return; }
    if (e.data.type !== 'port') return;
    port = e.data.port;
    port.onmessage = (m) => {
        try {
            if (m.data instanceof ArrayBuffer) run(m.data);
            else if (m.data.type === 'command-pool') {
                const pool = m.data;
                if (!(pool.states instanceof SharedArrayBuffer) || !(pool.bytes instanceof SharedArrayBuffer) ||
                    pool.states.byteLength !== 12 || !Number.isInteger(pool.slotBytes) || pool.slotBytes <= 0 ||
                    pool.slotBytes > 16 * 1024 * 1024 || (pool.slotBytes & 3) || pool.bytes.byteLength !== 3 * pool.slotBytes)
                    throw Error('Invalid shared command pool');
                commandPool = { states: new Int32Array(pool.states), bytes: pool.bytes, slotBytes: pool.slotBytes };
            }
            else if (m.data.type === 'commands') {
                const pool = commandPool, { slot, bytes } = m.data;
                if (!pool || !Number.isInteger(slot) || slot < 0 || slot >= 3 || !Number.isInteger(bytes) ||
                    bytes <= 0 || bytes > pool.slotBytes || (bytes & 3) || Atomics.load(pool.states, slot) !== 2)
                    throw Error('Invalid shared command span');
                try { run(pool.bytes, slot * pool.slotBytes, bytes); }
                finally { Atomics.store(pool.states, slot, 0); }
            }
            else if (m.data.type === 'read') read(m.data);
        } catch (error) {
            postMessage({ type: 'log', text: 'FATAL: renderer thread: ' + (error.stack || error) });
        }
    };
};
