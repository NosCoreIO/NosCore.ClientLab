/* Direct3D 9 over render.c: resources live in guest memory, draws go to the WebGL renderer (or the software rasteriser
   under Node), and caps and format checks describe what that renderer supports. COM objects live in guest memory;
   their vtables hold trampoline addresses. */
#include "port.h"
#include "render.h"
#include "image_file.h"
#include <math.h>
#include <stdlib.h>
#include <strings.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define THIS ARG(0)

enum { O_D3D = 1, O_DEVICE, O_TEXTURE, O_SURFACE, O_VB, O_IB, O_OTHER, O_MATSTACK, O_SWAPCHAIN };
/* object layout: +0 vtbl +4 refs +8 kind +12 width +16 height +20 format +24 levels +28 data +32 pitch +36 size
   +40 surfaces (texture: array of level surfaces) +44 container +48 usage +52 pool +56 upload state +60 GetDC dc
   +64 locked +68 priority +72 LOD +76 autogen filter */
#define OBJ_SIZE 96

typedef struct {
    const char* name;
    uint8_t argc;
    HleFn fn;
} Method;

#define D3DERR_INVALIDCALL 0x8876086cu
#define D3DERR_NOTAVAILABLE 0x8876086au
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_MOREDATA 0x887600a3u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define FMT_DXT1 0x31545844u
#define FMT_DXT3 0x33545844u
#define FMT_DXT5 0x35545844u
#define POOL_MANAGED 1u

static void m_ok(CPU* c) { RET(0); }
static void m_addref(CPU* c) { RET(++M32(THIS + 4)); }
static void m_release(CPU* c) { uint32_t r = M32(THIS + 4); RET(r ? --M32(THIS + 4) : 0); }

static uint32_t make_vtable(const char* iface, const Method* m, int n);
static uint32_t vt_d3d, vt_device, vt_texture, vt_surface, vt_vb, vt_ib, vt_other, vt_matstack, vt_swapchain;

static uint32_t make_obj(uint32_t vtbl, int kind) {
    uint32_t o = guest_alloc(OBJ_SIZE, 16);
    memset(GPTR(o), 0, OBJ_SIZE);
    M32(o) = vtbl;
    M32(o + 4) = 1;
    M32(o + 8) = (uint32_t)kind;
    return o;
}

/* ---- QueryInterface: each object answers for the interfaces its kind implements ---- */

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
static const Guid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const Guid IID_IDirect3D9 = {0x81bdcbca, 0x64d4, 0x426d, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}};
static const Guid IID_IDirect3DDevice9 = {0xd0223b96, 0xbf7a, 0x43fd, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}};
static const Guid IID_IDirect3DResource9 = {0x05eec05d, 0x8f7d, 0x4362, {0xb9, 0x99, 0xd1, 0xba, 0xf3, 0x57, 0xc7, 0x04}};
static const Guid IID_IDirect3DBaseTexture9 = {0x580ca87e, 0x1d3c, 0x4d54, {0x99, 0x1d, 0xb7, 0xd3, 0xe3, 0xc2, 0x98, 0xce}};
static const Guid IID_IDirect3DTexture9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
static const Guid IID_IDirect3DSurface9 = {0x0cfbaf3a, 0x9ff6, 0x429a, {0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b}};
static const Guid IID_IDirect3DVertexBuffer9 = {0xb64bb1b5, 0xfd70, 0x4df6, {0xbf, 0x91, 0x19, 0xd0, 0xa1, 0x24, 0x54, 0xe3}};
static const Guid IID_IDirect3DIndexBuffer9 = {0x7c9dd65e, 0xd3f7, 0x4529, {0xac, 0xee, 0x78, 0x58, 0x30, 0xac, 0xde, 0x35}};
static const Guid IID_IDirect3DSwapChain9 = {0x794950f2, 0xadfc, 0x458a, {0x90, 0x5e, 0x10, 0xa1, 0x0b, 0x8b, 0x50, 0x3b}};
static const Guid IID_ID3DXMatrixStack = {0xc7885ba7, 0xf990, 0x4fe7, {0x92, 0x2d, 0x85, 0x15, 0xe4, 0x77, 0xdd, 0x85}};

static int implements(uint32_t kind, const void* iid) {
    const Guid* by_kind[9][4] = {
        [O_D3D] = {&IID_IDirect3D9}, [O_DEVICE] = {&IID_IDirect3DDevice9},
        [O_TEXTURE] = {&IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DTexture9},
        [O_SURFACE] = {&IID_IDirect3DResource9, &IID_IDirect3DSurface9},
        [O_VB] = {&IID_IDirect3DResource9, &IID_IDirect3DVertexBuffer9}, [O_IB] = {&IID_IDirect3DResource9, &IID_IDirect3DIndexBuffer9},
        [O_MATSTACK] = {&IID_ID3DXMatrixStack},
    };
    if (!memcmp(iid, &IID_IUnknown, 16)) return 1;
    if (kind == O_SWAPCHAIN) return !memcmp(iid, &IID_IDirect3DSwapChain9, 16);
    if (kind >= 9) return 0;
    for (int k = 0; k < 4 && by_kind[kind][k]; k++)
        if (!memcmp(iid, by_kind[kind][k], 16)) return 1;
    return 0;
}
static void m_QueryInterface(CPU* c) {
    uint32_t out = ARG(2);
    if (!out) { RET(E_POINTER); return; }
    if (ARG(1) && implements(M32(THIS + 8), GPTR(ARG(1)))) {
        M32(THIS + 4)++;
        M32(out) = THIS;
        RET(0);
        return;
    }
    M32(out) = 0;
    RET(E_NOINTERFACE);
}

static int block_format(uint32_t f) { return f == FMT_DXT1 || f == FMT_DXT3 || f == FMT_DXT5; }

static uint32_t fmt_bits(uint32_t f) {
    switch (f) {
    case 21: case 22: case 102: return 32;
    case 23: case 24: case 25: case 26: case 51: case 101: return 16;
    case 28: case 41: case 50: return 8;
    case 0x31545844: return 4;
    case 0x33545844: case 0x35545844: return 8;
    default: return 32;
    }
}

static uint32_t make_surface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t container) {
    uint32_t s = make_obj(vt_surface, O_SURFACE), pitch = (w * fmt_bits(fmt) + 7) / 8;
    if (block_format(fmt)) pitch = ((w + 3) / 4) * (fmt == FMT_DXT1 ? 8 : 16);
    uint32_t rows = block_format(fmt) ? (h + 3) / 4 : h;
    M32(s + 12) = w; M32(s + 16) = h; M32(s + 20) = fmt; M32(s + 32) = pitch; M32(s + 36) = pitch * rows;
    M32(s + 28) = guest_alloc(pitch * rows ? pitch * rows : 16, 16);
    M32(s + 44) = container;
    return s;
}

static uint32_t make_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt) {
    uint32_t t = make_obj(vt_texture, O_TEXTURE), n = 0, lw = w, lh = h;
    if (!levels) { levels = 1; while ((lw > 1 || lh > 1) && levels < 16) { lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; levels++; } }
    M32(t + 12) = w; M32(t + 16) = h; M32(t + 20) = fmt; M32(t + 24) = levels;
    uint32_t arr = guest_alloc(4 * levels, 16);
    for (lw = w, lh = h; n < levels; n++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1)
        M32(arr + 4 * n) = make_surface(lw, lh, fmt, t);
    M32(t + 40) = arr;
    M32(t + 56) = 1;
    return t;
}

static uint32_t make_buffer(uint32_t vt, int kind, uint32_t size, uint32_t fmt) {
    uint32_t b = make_obj(vt, kind);
    M32(b + 36) = size;
    M32(b + 20) = fmt;
    M32(b + 28) = guest_alloc(size ? size : 16, 16);
    return b;
}

/* ---- IDirect3D9 ---- */

/* D3DCAPS9 describing what the WebGL renderer does: fixed function only, two texture stages, eight lights, no
   stencil, culling, scissor, user clip planes, shaders, cube or volume textures. */
static void fill_caps(uint32_t p) {
    memset(GPTR(p), 0, 304);
    M32(p) = 1;                                 /* DeviceType HAL */
    M32(p + 12) = 0x40000000u;                  /* Caps2: CANAUTOGENMIPMAP */
    M32(p + 20) = 0x80000001u;                  /* PresentationIntervals: ONE, IMMEDIATE */
    M32(p + 28) = 0x0009A650u;                  /* DevCaps: system-memory vertices, video-memory textures, DrawPrimitives2(Ex), hardware T&L and rasterisation */
    M32(p + 32) = 0x00000812u;                  /* PrimitiveMiscCaps: MASKZ, CULLNONE, BLENDOP */
    M32(p + 36) = 0x00000090u;                  /* RasterCaps: ZTEST, FOGVERTEX */
    M32(p + 40) = 0xFF;                         /* ZCmpCaps */
    M32(p + 44) = M32(p + 48) = 0x1FFF;         /* Src/DestBlendCaps */
    M32(p + 52) = 0xFF;                         /* AlphaCmpCaps */
    M32(p + 56) = 0x00084008u;                  /* ShadeCaps: COLORGOURAUDRGB, ALPHAGOURAUDBLEND, FOGGOURAUD */
    M32(p + 60) = 0x00004005u;                  /* TextureCaps: PERSPECTIVE, ALPHA, MIPMAP */
    M32(p + 64) = 0x03030300u;                  /* TextureFilterCaps: MIN/MAG point+linear, MIP point+linear */
    M32(p + 76) = 0x17;                         /* TextureAddressCaps: WRAP, MIRROR, CLAMP, INDEPENDENTUV */
    M32(p + 84) = 0x1F;                         /* LineCaps */
    M32(p + 88) = M32(p + 92) = 4096;           /* MaxTextureWidth/Height */
    M32(p + 100) = 8192;                        /* MaxTextureRepeat */
    M32(p + 104) = 4096;                        /* MaxTextureAspectRatio */
    M32(p + 108) = 1;                           /* MaxAnisotropy */
    MF32(p + 112) = 1e10f;                      /* MaxVertexW */
    M32(p + 140) = 8;                           /* FVFCaps: eight texture coordinate sets */
    M32(p + 144) = 0x7FF;                       /* TextureOpCaps: DISABLE .. ADDSMOOTH */
    M32(p + 148) = M32(p + 152) = 2;            /* MaxTextureBlendStages, MaxSimultaneousTextures */
    M32(p + 156) = 0x19;                        /* VertexProcessingCaps: TEXGEN, directional and positional lights */
    M32(p + 160) = 8;                           /* MaxActiveLights */
    M32(p + 168) = 1;                           /* MaxVertexBlendMatrices: no blending */
    MF32(p + 176) = 1.0f;                       /* MaxPointSize */
    M32(p + 180) = 0xFFFFF;                     /* MaxPrimitiveCount */
    M32(p + 184) = 0xFFFFFF;                    /* MaxVertexIndex */
    M32(p + 188) = 1;                           /* MaxStreams */
    M32(p + 192) = 256;                         /* MaxStreamStride */
    M32(p + 232) = 1;                           /* NumberOfAdaptersInGroup */
    M32(p + 240) = 1;                           /* NumSimultaneousRTs */
}

/* ---- format support, matching what texel() decodes and what the canvas can display ---- */

static int display_format(uint32_t f) { return f == 22 || f == 23 || f == 24; }
static int backbuffer_format(uint32_t f) { return f >= 21 && f <= 25; }
static int texture_format(uint32_t f) {
    return (f >= 21 && f <= 26) || f == 28 || f == 50 || f == 51 || block_format(f);
}
static int depth_format(uint32_t f) { return f == 75 || f == 77 || f == 80; }
static uint32_t opaque_format(uint32_t f) { return f == 21 ? 22 : f == 25 ? 24 : f; }
static int adapter_ok(CPU* c) { return ARG(1) == 0; }

static void d_RegisterSoftwareDevice(CPU* c) { RET(D3DERR_INVALIDCALL); }   /* no native rasteriser can load */
static void d_CheckDeviceType(CPU* c) {
    uint32_t type = ARG(2), adapter_fmt = ARG(3), bb = ARG(4), windowed = ARG(5);
    if (!adapter_ok(c)) { RET(D3DERR_INVALIDCALL); return; }
    int ok = type == 1 && display_format(adapter_fmt) &&
             ((windowed && bb == 0) || (backbuffer_format(bb) && opaque_format(bb) == adapter_fmt));
    RET(ok ? 0 : D3DERR_NOTAVAILABLE);
}
static void d_CheckDeviceFormat(CPU* c) {
    uint32_t type = ARG(2), adapter_fmt = ARG(3), usage = ARG(4), rtype = ARG(5), f = ARG(6);
    if (!adapter_ok(c)) { RET(D3DERR_INVALIDCALL); return; }
    int ok = type == 1 && display_format(adapter_fmt);
    /* SRGBREAD, SRGBWRITE, VERTEXTEXTURE, LEGACYBUMPMAP, DMAP: no such sampling here. */
    if (usage & 0x0015C000u) ok = 0;
    if (usage & 1) ok = 0;   /* only the backbuffer is a render target */
    if (usage & 0x00080000u) ok = 0;   /* QUERY_POSTPIXELSHADER_BLENDING needs a render target */
    if (usage & 2) ok = ok && rtype == 1 && depth_format(f);
    else if (rtype == 1 || rtype == 3) ok = ok && (texture_format(f) || (rtype == 1 && f == 20));
    else if (rtype == 6) ok = ok && f == 100;
    else if (rtype == 7) ok = ok && (f == 101 || f == 102);
    else ok = 0;   /* volume and cube resources */
    RET(ok ? 0 : D3DERR_NOTAVAILABLE);
}
static void d_CheckDeviceMultiSampleType(CPU* c) {
    uint32_t type = ARG(2), f = ARG(3), ms = ARG(5), quality = ARG(6);
    if (!adapter_ok(c)) { RET(D3DERR_INVALIDCALL); return; }
    int ok = type == 1 && ms == 0 && (backbuffer_format(f) || depth_format(f) || texture_format(f));
    if (quality) M32(quality) = ok ? 1 : 0;
    RET(ok ? 0 : D3DERR_NOTAVAILABLE);
}
static void d_CheckDepthStencilMatch(CPU* c) {
    uint32_t type = ARG(2), adapter_fmt = ARG(3), rt = ARG(4), ds = ARG(5);
    if (!adapter_ok(c)) { RET(D3DERR_INVALIDCALL); return; }
    RET(type == 1 && display_format(adapter_fmt) && backbuffer_format(rt) && depth_format(ds) ? 0 : D3DERR_NOTAVAILABLE);
}
static void d_CheckDeviceFormatConversion(CPU* c) {
    uint32_t type = ARG(2), src = ARG(3), dst = ARG(4);
    if (!adapter_ok(c)) { RET(D3DERR_INVALIDCALL); return; }
    RET(type == 1 && backbuffer_format(src) && backbuffer_format(dst) ? 0 : D3DERR_NOTAVAILABLE);
}

static void d_GetAdapterCount(CPU* c) { RET(1); }
static void d_GetAdapterIdentifier(CPU* c) {
    uint32_t p = ARG(3);
    memset(GPTR(p), 0, 1100);
    strcpy(GSTR(p), "noscore");
    strcpy(GSTR(p + 512), "NosCore null Direct3D 9");
    M32(p + 1056) = 0x10de;
    RET(0);
}
static void fill_mode(uint32_t p) { M32(p) = 1920; M32(p + 4) = 1080; M32(p + 8) = 60; M32(p + 12) = 22; }
static void d_GetAdapterModeCount(CPU* c) { RET(1); }
static void d_EnumAdapterModes(CPU* c) { fill_mode(ARG(4)); RET(0); }
static void d_GetAdapterDisplayMode(CPU* c) { fill_mode(ARG(2)); RET(0); }
static void d_GetDeviceCaps(CPU* c) { fill_caps(ARG(3)); RET(0); }
static void d_GetAdapterMonitor(CPU* c) { RET(1); }
static uint32_t the_device, the_d3d, backbuffer, depthbuffer;
static void d_CreateDevice(CPU* c) {
    uint32_t pp = ARG(5), w = M32(pp), h = M32(pp + 4), hwnd = M32(pp + 28) ? M32(pp + 28) : ARG(3);
    if (!w || !h) hle_window_client_size(hwnd, &w, &h);
    if (!w) w = 1024;
    if (!h) h = 768;
    M32(pp) = w;
    M32(pp + 4) = h;
    port_log("IDirect3D9::CreateDevice %ux%u format %u", w, h, M32(pp + 8));
    the_device = make_obj(vt_device, O_DEVICE);
    backbuffer = make_surface(w, h, M32(pp + 8) ? M32(pp + 8) : 22, 0);
    depthbuffer = make_surface(w, h, 75, 0);
    r_init(backbuffer);
    M32(ARG(6)) = the_device;
    RET(0);
}

static const Method d3d_methods[] = {
    {"QueryInterface", 3, m_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"RegisterSoftwareDevice", 2, d_RegisterSoftwareDevice}, {"GetAdapterCount", 1, d_GetAdapterCount},
    {"GetAdapterIdentifier", 4, d_GetAdapterIdentifier}, {"GetAdapterModeCount", 3, d_GetAdapterModeCount},
    {"EnumAdapterModes", 5, d_EnumAdapterModes}, {"GetAdapterDisplayMode", 3, d_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, d_CheckDeviceType}, {"CheckDeviceFormat", 7, d_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 7, d_CheckDeviceMultiSampleType}, {"CheckDepthStencilMatch", 6, d_CheckDepthStencilMatch},
    {"CheckDeviceFormatConversion", 4, d_CheckDeviceFormatConversion}, {"GetDeviceCaps", 4, d_GetDeviceCaps},
    {"GetAdapterMonitor", 2, d_GetAdapterMonitor}, {"CreateDevice", 7, d_CreateDevice},
};

/* ---- IDirect3DDevice9 ---- */

static uint32_t frames;
uint32_t port_frames;
static void v_TestCooperativeLevel(CPU* c) { RET(0); }
static void v_GetAvailableTextureMem(CPU* c) { RET(512u << 20); }
static void v_GetDirect3D(CPU* c) { M32(ARG(1)) = the_d3d; M32(the_d3d + 4)++; RET(0); }
static void v_GetDeviceCaps(CPU* c) { fill_caps(ARG(1)); RET(0); }
static void v_GetDisplayMode(CPU* c) { fill_mode(ARG(2)); RET(0); }
static void v_GetCreationParameters(CPU* c) { memset(GPTR(ARG(1)), 0, 16); RET(0); }
static void v_GetSwapChain(CPU* c) { M32(ARG(2)) = make_obj(vt_swapchain, O_SWAPCHAIN); RET(0); }
static void v_GetNumberOfSwapChains(CPU* c) { RET(1); }
static void v_Present(CPU* c) {
    static const char* shot_dir;
    static int checked;
    if (!checked) { checked = 1; shot_dir = getenv("PORT_SHOT_DIR"); }
    port_frames++;
    /* PORT_TRACE_FRAME=n logs the next few thousand API calls once frame n is presented. */
    static long trace_frame = -2;
    if (trace_frame == -2) trace_frame = getenv("PORT_TRACE_FRAME") ? atol(getenv("PORT_TRACE_FRAME")) : -1;
    if ((long)port_frames == trace_frame) {
        port_trace_budget = getenv("PORT_TRACE_BUDGET") ? atoi(getenv("PORT_TRACE_BUDGET")) : 4000;
        port_trace_methods = !getenv("PORT_TRACE_NOMETHODS");
    }
    r_prof_frame();
    if (++frames % 300 == 1) port_log("Present frame %u", frames);
    if (!r_gl_present()) web_present(backbuffer);
    if (shot_dir && (frames == 1 || frames % 600 == 0)) {
        char path[400];
        snprintf(path, sizeof path, "%s/frame_%06u.bmp", shot_dir, frames);
        r_dump_bmp(backbuffer, path);
        r_stats();
    }
    RET(0);
}
static void v_GetBackBuffer(CPU* c) { M32(backbuffer + 4)++; M32(ARG(4)) = backbuffer; RET(0); }
static void v_GetRasterStatus(CPU* c) { memset(GPTR(ARG(2)), 0, 8); RET(0); }
static void v_CreateTexture(CPU* c) {
    uint32_t usage = ARG(4), f = ARG(5), out = ARG(7);
    if (!out || !ARG(1) || !ARG(2) || (usage & 3) || !texture_format(f)) {
        port_log("IDirect3DDevice9::CreateTexture %ux%u usage %x format %u refused", ARG(1), ARG(2), usage, f);
        RET(D3DERR_INVALIDCALL);
        return;
    }
    uint32_t t = make_texture(ARG(1), ARG(2), ARG(3), f);
    M32(t + 48) = usage;
    M32(t + 52) = ARG(6);
    M32(t + 76) = 2;   /* D3DTEXF_LINEAR */
    M32(out) = t;
    RET(0);
}
/* The caps advertise neither cube nor volume textures. */
static void v_CreateVolumeTexture(CPU* c) { if (ARG(8)) M32(ARG(8)) = 0; RET(D3DERR_INVALIDCALL); }
static void v_CreateCubeTexture(CPU* c) { if (ARG(6)) M32(ARG(6)) = 0; RET(D3DERR_INVALIDCALL); }
static void v_CreateVertexBuffer(CPU* c) {
    if (!ARG(5) || !ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t b = make_buffer(vt_vb, O_VB, ARG(1), ARG(3));
    M32(b + 48) = ARG(2);
    M32(b + 52) = ARG(4);
    M32(ARG(5)) = b;
    RET(0);
}
static void v_CreateIndexBuffer(CPU* c) {
    if (!ARG(5) || !ARG(1) || (ARG(3) != 101 && ARG(3) != 102)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t b = make_buffer(vt_ib, O_IB, ARG(1), ARG(3));
    M32(b + 48) = ARG(2);
    M32(b + 52) = ARG(4);
    M32(ARG(5)) = b;
    RET(0);
}
static void v_CreateRenderTarget(CPU* c) { M32(ARG(7)) = make_surface(ARG(1), ARG(2), ARG(3), 0); RET(0); }
static void v_CreateDepthStencilSurface(CPU* c) { M32(ARG(7)) = make_surface(ARG(1), ARG(2), ARG(3), 0); RET(0); }
static void v_CreateOffscreenPlainSurface(CPU* c) { M32(ARG(5)) = make_surface(ARG(1), ARG(2), ARG(3), 0); RET(0); }
static void v_GetRenderTarget(CPU* c) { M32(backbuffer + 4)++; M32(ARG(2)) = backbuffer; RET(0); }
static void v_GetDepthStencilSurface(CPU* c) { M32(depthbuffer + 4)++; M32(ARG(1)) = depthbuffer; RET(0); }
static void v_GetViewport(CPU* c) {
    uint32_t p = ARG(1);
    M32(p) = 0; M32(p + 4) = 0; M32(p + 8) = M32(backbuffer + 12); M32(p + 12) = M32(backbuffer + 16);
    MF32(p + 16) = 0.0f; MF32(p + 20) = 1.0f;
    RET(0);
}
static void v_zero_out1(CPU* c) { if (ARG(1)) M32(ARG(1)) = 0; RET(0); }
static void v_zero_out2(CPU* c) { if (ARG(2)) M32(ARG(2)) = 0; RET(0); }
static void v_new_other_out2(CPU* c) { M32(ARG(2)) = make_obj(vt_other, O_OTHER); RET(0); }
static void v_new_other_out1(CPU* c) { M32(ARG(1)) = make_obj(vt_other, O_OTHER); RET(0); }
static int in_scene;
static void v_BeginScene(CPU* c) {
    if (in_scene) { RET(D3DERR_INVALIDCALL); return; }
    in_scene = 1;
    RET(0);
}
static void v_EndScene(CPU* c) {
    if (!in_scene) { RET(D3DERR_INVALIDCALL); return; }
    in_scene = 0;
    RET(0);
}
static void v_ValidateDevice(CPU* c) { if (ARG(1)) M32(ARG(1)) = 1; RET(0); }

/* ---- pipeline state and draws go to the software rasteriser ---- */

static void v_SetFVF(CPU* c) { r_set_fvf(ARG(1)); RET(0); }
static void v_SetVertexDeclaration(CPU* c) { RET(0); }
static void v_SetTransform(CPU* c) { r_set_transform(ARG(1), ARG(2)); RET(0); }
static void v_GetTransformR(CPU* c) { r_get_transform(ARG(1), ARG(2)); RET(0); }
static void v_SetRenderState(CPU* c) { r_set_render_state(ARG(1), ARG(2)); RET(0); }
static void v_GetRenderStateR(CPU* c) { M32(ARG(2)) = r_get_render_state(ARG(1)); RET(0); }
static void v_SetTextureStageState(CPU* c) { r_set_tss(ARG(1), ARG(2), ARG(3)); RET(0); }
static void v_SetSamplerState(CPU* c) { r_set_sampler(ARG(1), ARG(2), ARG(3)); RET(0); }
static void v_GetSamplerStateR(CPU* c) { M32(ARG(3)) = r_get_sampler(ARG(1), ARG(2)); RET(0); }
static void v_GetTextureStageStateR(CPU* c) { M32(ARG(3)) = r_get_tss(ARG(1), ARG(2)); RET(0); }
static void v_SetTexture(CPU* c) { r_set_texture(ARG(1), ARG(2)); RET(0); }
static void v_GetTextureR(CPU* c) { M32(ARG(2)) = r_get_texture(ARG(1)); RET(0); }
static void v_SetStreamSource(CPU* c) { if (ARG(1) == 0) r_set_stream(ARG(2), ARG(3), ARG(4)); RET(0); }
static void v_SetIndices(CPU* c) { r_set_indices(ARG(1)); RET(0); }
static void v_SetViewport(CPU* c) { r_set_viewport(ARG(1)); RET(0); }
static void v_SetRenderTarget(CPU* c) { if (ARG(1) == 0) r_set_render_target(ARG(2) ? ARG(2) : backbuffer); RET(0); }
static void v_Clear(CPU* c) { uint32_t zb = ARG(5); float z; memcpy(&z, &zb, 4); r_clear(ARG(1), ARG(2), ARG(3), ARG(4), z); RET(0); }
static void v_DrawPrimitive(CPU* c) { r_draw(ARG(1), ARG(2), ARG(3)); RET(0); }
static void v_DrawPrimitiveUP(CPU* c) { r_draw_up(ARG(1), ARG(2), ARG(3), ARG(4)); RET(0); }
static void v_DrawIndexedPrimitive(CPU* c) { r_draw_indexed(ARG(1), (int32_t)ARG(2), ARG(5), ARG(6)); RET(0); }
static void v_DrawIndexedPrimitiveUP(CPU* c) { r_draw_indexed_up(ARG(1), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8)); RET(0); }
static void v_SetVertexShader(CPU* c) { RET(0); }
static void v_SetLight(CPU* c) { r_set_light(ARG(1), ARG(2)); RET(0); }
static void v_GetLightR(CPU* c) { r_get_light(ARG(1), ARG(2)); RET(0); }
static void v_LightEnable(CPU* c) { r_light_enable(ARG(1), ARG(2)); RET(0); }
static void v_GetLightEnableR(CPU* c) { M32(ARG(2)) = r_light_enabled(ARG(1)); RET(0); }
static void v_SetMaterial(CPU* c) { r_set_material(ARG(1)); RET(0); }
static void v_GetMaterialR(CPU* c) { r_get_material(ARG(1)); RET(0); }
static void v_SetPixelShader(CPU* c) { RET(0); }

static const Method device_methods[] = {
    {"QueryInterface", 3, m_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"TestCooperativeLevel", 1, v_TestCooperativeLevel}, {"GetAvailableTextureMem", 1, v_GetAvailableTextureMem},
    {"EvictManagedResources", 1, 0}, {"GetDirect3D", 2, v_GetDirect3D}, {"GetDeviceCaps", 2, v_GetDeviceCaps},
    {"GetDisplayMode", 3, v_GetDisplayMode}, {"GetCreationParameters", 2, v_GetCreationParameters},
    {"SetCursorProperties", 4, 0}, {"SetCursorPosition", 4, 0}, {"ShowCursor", 2, 0},
    {"CreateAdditionalSwapChain", 3, v_new_other_out2}, {"GetSwapChain", 3, v_GetSwapChain},
    {"GetNumberOfSwapChains", 1, v_GetNumberOfSwapChains}, {"Reset", 2, 0}, {"Present", 5, v_Present},
    {"GetBackBuffer", 5, v_GetBackBuffer}, {"GetRasterStatus", 3, v_GetRasterStatus}, {"SetDialogBoxMode", 2, 0},
    {"SetGammaRamp", 4, 0}, {"GetGammaRamp", 3, 0}, {"CreateTexture", 9, v_CreateTexture},
    {"CreateVolumeTexture", 10, v_CreateVolumeTexture}, {"CreateCubeTexture", 8, v_CreateCubeTexture},
    {"CreateVertexBuffer", 7, v_CreateVertexBuffer}, {"CreateIndexBuffer", 7, v_CreateIndexBuffer},
    {"CreateRenderTarget", 9, v_CreateRenderTarget}, {"CreateDepthStencilSurface", 9, v_CreateDepthStencilSurface},
    {"UpdateSurface", 5, 0}, {"UpdateTexture", 3, 0}, {"GetRenderTargetData", 3, 0}, {"GetFrontBufferData", 3, 0},
    {"StretchRect", 6, 0}, {"ColorFill", 4, 0}, {"CreateOffscreenPlainSurface", 7, v_CreateOffscreenPlainSurface},
    {"SetRenderTarget", 3, v_SetRenderTarget}, {"GetRenderTarget", 3, v_GetRenderTarget}, {"SetDepthStencilSurface", 2, 0},
    {"GetDepthStencilSurface", 2, v_GetDepthStencilSurface}, {"BeginScene", 1, v_BeginScene}, {"EndScene", 1, v_EndScene},
    {"Clear", 7, v_Clear}, {"SetTransform", 3, v_SetTransform}, {"GetTransform", 3, v_GetTransformR}, {"MultiplyTransform", 3, 0},
    {"SetViewport", 2, v_SetViewport}, {"GetViewport", 2, v_GetViewport}, {"SetMaterial", 2, v_SetMaterial}, {"GetMaterial", 2, v_GetMaterialR},
    {"SetLight", 3, v_SetLight}, {"GetLight", 3, v_GetLightR}, {"LightEnable", 3, v_LightEnable}, {"GetLightEnable", 3, v_GetLightEnableR},
    {"SetClipPlane", 3, 0}, {"GetClipPlane", 3, 0}, {"SetRenderState", 3, v_SetRenderState}, {"GetRenderState", 3, v_GetRenderStateR},
    {"CreateStateBlock", 3, v_new_other_out2}, {"BeginStateBlock", 1, 0}, {"EndStateBlock", 2, v_new_other_out1},
    {"SetClipStatus", 2, 0}, {"GetClipStatus", 2, 0}, {"GetTexture", 3, v_GetTextureR}, {"SetTexture", 3, v_SetTexture},
    {"GetTextureStageState", 4, v_GetTextureStageStateR}, {"SetTextureStageState", 4, v_SetTextureStageState}, {"GetSamplerState", 4, v_GetSamplerStateR},
    {"SetSamplerState", 4, v_SetSamplerState}, {"ValidateDevice", 2, v_ValidateDevice}, {"SetPaletteEntries", 3, 0},
    {"GetPaletteEntries", 3, 0}, {"SetCurrentTexturePalette", 2, 0}, {"GetCurrentTexturePalette", 2, v_zero_out1},
    {"SetScissorRect", 2, 0}, {"GetScissorRect", 2, 0}, {"SetSoftwareVertexProcessing", 2, 0},
    {"GetSoftwareVertexProcessing", 1, 0}, {"SetNPatchMode", 2, 0}, {"GetNPatchMode", 1, 0},
    {"DrawPrimitive", 4, v_DrawPrimitive}, {"DrawIndexedPrimitive", 7, v_DrawIndexedPrimitive}, {"DrawPrimitiveUP", 5, v_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, v_DrawIndexedPrimitiveUP}, {"ProcessVertices", 7, 0}, {"CreateVertexDeclaration", 3, v_new_other_out2},
    {"SetVertexDeclaration", 2, v_SetVertexDeclaration}, {"GetVertexDeclaration", 2, v_zero_out1}, {"SetFVF", 2, v_SetFVF},
    {"GetFVF", 2, v_zero_out1}, {"CreateVertexShader", 3, v_new_other_out2}, {"SetVertexShader", 2, v_SetVertexShader},
    {"GetVertexShader", 2, v_zero_out1}, {"SetVertexShaderConstantF", 4, 0}, {"GetVertexShaderConstantF", 4, 0},
    {"SetVertexShaderConstantI", 4, 0}, {"GetVertexShaderConstantI", 4, 0}, {"SetVertexShaderConstantB", 4, 0},
    {"GetVertexShaderConstantB", 4, 0}, {"SetStreamSource", 5, v_SetStreamSource}, {"GetStreamSource", 5, v_zero_out2},
    {"SetStreamSourceFreq", 3, 0}, {"GetStreamSourceFreq", 3, v_zero_out2}, {"SetIndices", 2, v_SetIndices},
    {"GetIndices", 2, v_zero_out1}, {"CreatePixelShader", 3, v_new_other_out2}, {"SetPixelShader", 2, v_SetPixelShader},
    {"GetPixelShader", 2, v_zero_out1}, {"SetPixelShaderConstantF", 4, 0}, {"GetPixelShaderConstantF", 4, 0},
    {"SetPixelShaderConstantI", 4, 0}, {"GetPixelShaderConstantI", 4, 0}, {"SetPixelShaderConstantB", 4, 0},
    {"GetPixelShaderConstantB", 4, 0}, {"DrawRectPatch", 4, 0}, {"DrawTriPatch", 4, 0}, {"DeletePatch", 2, 0},
    {"CreateQuery", 3, v_new_other_out2},
};

/* ---- resources ---- */

static void r_GetDevice(CPU* c) { M32(ARG(1)) = the_device; RET(0); }
static void r_GetType(CPU* c) { uint32_t k = M32(THIS + 8); RET(k == O_TEXTURE ? 3 : k == O_SURFACE ? 1 : k == O_VB ? 6 : k == O_IB ? 7 : 1); }
static void t_GetLevelCount(CPU* c) { RET(M32(THIS + 24)); }
static uint32_t level(uint32_t t, uint32_t l) { return l < M32(t + 24) ? M32(M32(t + 40) + 4 * l) : 0; }
static void fill_surface_desc(uint32_t s, uint32_t p) {
    M32(p) = M32(s + 20); M32(p + 4) = 1; M32(p + 8) = 0; M32(p + 12) = 0; M32(p + 16) = 0; M32(p + 20) = 0;
    M32(p + 24) = M32(s + 12); M32(p + 28) = M32(s + 16);
}
static void t_GetLevelDesc(CPU* c) { uint32_t s = level(THIS, ARG(1)); if (!s) { RET(0x8876086cu); return; } fill_surface_desc(s, ARG(2)); RET(0); }
static void t_GetSurfaceLevel(CPU* c) { uint32_t s = level(THIS, ARG(1)); if (!s) { RET(0x8876086cu); return; } M32(s + 4)++; M32(ARG(2)) = s; RET(0); }
/* Points the locked rectangle at rect's top-left (whole blocks for DXT) and marks the surface locked. A read-only lock
   leaves the uploaded texture alone. */
static uint32_t lock_surface(uint32_t s, uint32_t locked, uint32_t rect, uint32_t flags) {
    if (!locked) return D3DERR_INVALIDCALL;
    if (M32(s + 64)) return D3DERR_INVALIDCALL;
    uint32_t off = 0, f = M32(s + 20);
    if (rect) {
        int32_t l = (int32_t)M32(rect), t = (int32_t)M32(rect + 4), r = (int32_t)M32(rect + 8), b = (int32_t)M32(rect + 12);
        if (l < 0 || t < 0 || l >= r || t >= b || (uint32_t)r > M32(s + 12) || (uint32_t)b > M32(s + 16)) return D3DERR_INVALIDCALL;
        if (block_format(f)) off = (uint32_t)t / 4 * M32(s + 32) + (uint32_t)l / 4 * (f == FMT_DXT1 ? 8 : 16);
        else off = (uint32_t)t * M32(s + 32) + (uint32_t)l * fmt_bits(f) / 8;
    }
    r_read_back(s);
    M32(locked) = M32(s + 32);
    M32(locked + 4) = M32(s + 28) + off;
    M32(s + 64) = 1;
    if (!(flags & 0x10)) {   /* D3DLOCK_READONLY */
        M32(s + 56) = 1;
        if (M32(s + 44)) M32(M32(s + 44) + 56) = 1;
    }
    return 0;
}
static uint32_t unlock_surface(uint32_t s) {
    if (!s || !M32(s + 64)) return D3DERR_INVALIDCALL;
    M32(s + 64) = 0;
    return 0;
}
static void t_LockRect(CPU* c) {
    uint32_t s = level(THIS, ARG(1));
    RET(s ? lock_surface(s, ARG(2), ARG(3), ARG(4)) : D3DERR_INVALIDCALL);
}
static void t_UnlockRect(CPU* c) { RET(unlock_surface(level(THIS, ARG(1)))); }
static void s_GetDesc(CPU* c) { fill_surface_desc(THIS, ARG(1)); RET(0); }
static void s_LockRect(CPU* c) { RET(lock_surface(THIS, ARG(1), ARG(2), ARG(3))); }
static void s_UnlockRect(CPU* c) { RET(unlock_surface(THIS)); }
static void s_GetContainer(CPU* c) { M32(ARG(2)) = M32(THIS + 44); RET(M32(THIS + 44) ? 0 : 0x80004002u); }
/* GetDC hands out a memory DC on a 32bpp copy of the surface (+60 holds it while out); ReleaseDC writes the copy
   back. Only the RGB formats D3D allows GDI on qualify. */
static int gdi_format(uint32_t f) { return f >= 20 && f <= 25; }
static uint32_t surface_pixel(uint32_t s, uint32_t x, uint32_t y) {
    uint32_t row = M32(s + 28) + y * M32(s + 32);
    if (M32(s + 20) == 20) return 0xff000000u | M8(row + 3 * x) | M8(row + 3 * x + 1) << 8 | (uint32_t)M8(row + 3 * x + 2) << 16;
    return r_texel(s, x, y);
}
static void surface_store(uint32_t s, uint32_t x, uint32_t y, uint32_t p) {
    uint32_t row = M32(s + 28) + y * M32(s + 32), r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
    switch (M32(s + 20)) {
    case 20: M8(row + 3 * x) = (uint8_t)b; M8(row + 3 * x + 1) = (uint8_t)g; M8(row + 3 * x + 2) = (uint8_t)r; break;
    case 21: M32(row + 4 * x) = (M32(row + 4 * x) & 0xff000000u) | (p & 0xffffff); break;
    case 22: M32(row + 4 * x) = p | 0xff000000u; break;
    case 23: M16(row + 2 * x) = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); break;
    case 24: M16(row + 2 * x) = (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break;
    case 25: M16(row + 2 * x) = (uint16_t)((M16(row + 2 * x) & 0x8000) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break;
    }
}
static void s_GetDC(CPU* c) {
    uint32_t s = THIS, w = M32(s + 12), h = M32(s + 16);
    if (!ARG(1) || M32(s + 60) || !gdi_format(M32(s + 20))) { RET(D3DERR_INVALIDCALL); return; }
    r_read_back(s);
    uint32_t bmp = gdi_new_bitmap((int32_t)w, -(int32_t)h, 32);
    GdiBitmap b;
    gdi_bitmap(bmp, &b);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) M32(b.bits + y * b.stride + 4 * x) = surface_pixel(s, x, y);
    M32(s + 60) = hle_dc_memory(bmp);
    M32(ARG(1)) = M32(s + 60);
    RET(0);
}
static void s_ReleaseDC(CPU* c) {
    uint32_t s = THIS, dc = M32(s + 60), w = M32(s + 12), h = M32(s + 16);
    if (!dc || ARG(1) != dc) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t* px = malloc(4 * (size_t)w * h);
    hle_dc_read(dc, (int)w, (int)h, px);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) surface_store(s, x, y, px[y * w + x]);
    free(px);
    gdi_delete_bitmap(hle_dc_delete(dc));
    M32(s + 60) = 0;
    M32(s + 56) = 1;
    if (M32(s + 44)) M32(M32(s + 44) + 56) = 1;
    RET(0);
}
static void b_Lock(CPU* c) {
    uint32_t off = ARG(1), size = ARG(2), out = ARG(3);
    if (!out || off > M32(THIS + 36) || (size && off + size > M32(THIS + 36))) { RET(D3DERR_INVALIDCALL); return; }
    M32(out) = M32(THIS + 28) + off;
    M32(THIS + 64)++;   /* buffer locks nest */
    RET(0);
}
static void b_Unlock(CPU* c) {
    if (!M32(THIS + 64)) { RET(D3DERR_INVALIDCALL); return; }
    M32(THIS + 64)--;
    RET(0);
}
static void b_GetDesc(CPU* c) { memset(GPTR(ARG(1)), 0, 24); M32(ARG(1)) = M32(THIS + 20); M32(ARG(1) + 20) = M32(THIS + 36); RET(0); }

/* Private data is kept host-side per (object, GUID). D3DSPD_IUNKNOWN data is an interface pointer the resource holds a
   reference on. */
typedef struct PrivateData {
    uint32_t obj, flags, size;
    uint8_t guid[16];
    uint8_t* data;
    struct PrivateData* next;
} PrivateData;
static PrivateData* private_data;

static PrivateData** private_find(uint32_t obj, uint32_t guid) {
    PrivateData** at = &private_data;
    while (*at && ((*at)->obj != obj || memcmp((*at)->guid, GPTR(guid), 16))) at = &(*at)->next;
    return at;
}
static void unknown_call(CPU* c, uint32_t unk, int slot) { guest_call(c, M32(M32(unk) + 4 * (uint32_t)slot), 1, unk); }
static void private_free(CPU* c, PrivateData** at) {
    PrivateData* d = *at;
    if (d->flags & 1) unknown_call(c, M32((uint32_t)(uintptr_t)d->data), 2);
    *at = d->next;
    free(d->data);
    free(d);
}
static void r_SetPrivateData(CPU* c) {
    uint32_t guid = ARG(1), data = ARG(2), size = ARG(3), flags = ARG(4);
    if (!guid || (size && !data) || ((flags & 1) && size != 4)) { RET(D3DERR_INVALIDCALL); return; }
    PrivateData** at = private_find(THIS, guid);
    if (*at) private_free(c, at);
    PrivateData* d = calloc(1, sizeof *d);
    d->obj = THIS;
    d->flags = flags;
    d->size = size;
    memcpy(d->guid, GPTR(guid), 16);
    d->data = malloc(size ? size : 1);
    if (flags & 1) {
        memcpy(d->data, &data, 4);
        unknown_call(c, data, 1);
    } else if (size) memcpy(d->data, GPTR(data), size);
    d->next = private_data;
    private_data = d;
    RET(0);
}
static void r_GetPrivateData(CPU* c) {
    uint32_t data = ARG(2), size = ARG(3);
    if (!ARG(1) || !size) { RET(D3DERR_INVALIDCALL); return; }
    PrivateData* d = *private_find(THIS, ARG(1));
    if (!d) { RET(D3DERR_NOTFOUND); return; }
    uint32_t have = M32(size);
    M32(size) = d->size;
    if (!data) { RET(0); return; }
    if (have < d->size) { RET(D3DERR_MOREDATA); return; }
    memcpy(GPTR(data), d->data, d->size);
    if (d->flags & 1) unknown_call(c, M32(data), 1);
    RET(0);
}
static void r_FreePrivateData(CPU* c) {
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    PrivateData** at = private_find(THIS, ARG(1));
    if (!*at) { RET(D3DERR_NOTFOUND); return; }
    private_free(c, at);
    RET(0);
}
/* Priority and LOD only apply to managed resources; for the rest they read back as 0. */
static void r_SetPriority(CPU* c) {
    uint32_t old = M32(THIS + 68);
    if (M32(THIS + 52) == POOL_MANAGED) M32(THIS + 68) = ARG(1);
    RET(old);
}
static void r_GetPriority(CPU* c) { RET(M32(THIS + 68)); }
static void r_PreLoad(CPU* c) { if (M32(THIS + 8) == O_TEXTURE) r_preload(THIS); }
static void t_SetLOD(CPU* c) {
    uint32_t old = M32(THIS + 72), lod = ARG(1);
    if (M32(THIS + 52) == POOL_MANAGED) {
        M32(THIS + 72) = lod < M32(THIS + 24) ? lod : M32(THIS + 24) - 1;
        r_texture_lod(THIS, M32(THIS + 72));
    }
    RET(old);
}
static void t_GetLOD(CPU* c) { RET(M32(THIS + 72)); }
/* Mip levels are generated from level 0 whenever the texture is uploaded, so only the requested filter is kept. */
static void t_SetAutoGenFilterType(CPU* c) {
    uint32_t f = ARG(1);
    if (!(M32(THIS + 48) & 0x400) || f == 0 || f > 3) { RET(D3DERR_INVALIDCALL); return; }
    M32(THIS + 76) = f;
    RET(0);
}
static void t_GetAutoGenFilterType(CPU* c) { RET(M32(THIS + 76)); }
static void t_GenerateMipSubLevels(CPU* c) { if (M32(THIS + 48) & 0x400) M32(THIS + 56) = 1; }
static void t_AddDirtyRect(CPU* c) { M32(THIS + 56) = 1; RET(0); }

#define RESOURCE_METHODS \
    {"QueryInterface", 3, m_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"GetDevice", 2, r_GetDevice}, \
    {"SetPrivateData", 5, r_SetPrivateData}, {"GetPrivateData", 4, r_GetPrivateData}, {"FreePrivateData", 2, r_FreePrivateData}, \
    {"SetPriority", 2, r_SetPriority}, {"GetPriority", 1, r_GetPriority}, {"PreLoad", 1, r_PreLoad}, {"GetType", 1, r_GetType}

static const Method texture_methods[] = {
    RESOURCE_METHODS, {"SetLOD", 2, t_SetLOD}, {"GetLOD", 1, t_GetLOD}, {"GetLevelCount", 1, t_GetLevelCount},
    {"SetAutoGenFilterType", 2, t_SetAutoGenFilterType}, {"GetAutoGenFilterType", 1, t_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, t_GenerateMipSubLevels}, {"GetLevelDesc", 3, t_GetLevelDesc},
    {"GetSurfaceLevel", 3, t_GetSurfaceLevel}, {"LockRect", 5, t_LockRect}, {"UnlockRect", 2, t_UnlockRect},
    {"AddDirtyRect", 2, t_AddDirtyRect},
};
static const Method surface_methods[] = {
    RESOURCE_METHODS, {"GetContainer", 3, s_GetContainer}, {"GetDesc", 2, s_GetDesc}, {"LockRect", 4, s_LockRect},
    {"UnlockRect", 1, s_UnlockRect}, {"GetDC", 2, s_GetDC}, {"ReleaseDC", 2, s_ReleaseDC},
};
static const Method vb_methods[] = {RESOURCE_METHODS, {"Lock", 5, b_Lock}, {"Unlock", 1, b_Unlock}, {"GetDesc", 2, b_GetDesc}};
static const Method ib_methods[] = {RESOURCE_METHODS, {"Lock", 5, b_Lock}, {"Unlock", 1, b_Unlock}, {"GetDesc", 2, b_GetDesc}};
static const Method other_methods[] = {
    {"QueryInterface", 3, m_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"Method3", 1, 0},
    {"Method4", 1, 0}, {"Method5", 1, 0}, {"Method6", 1, 0}, {"Method7", 1, 0}, {"Method8", 1, 0},
};
static const Method swapchain_methods[] = {
    {"QueryInterface", 3, m_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"Present", 6, 0},
    {"GetFrontBufferData", 2, 0}, {"GetBackBuffer", 4, 0}, {"GetRasterStatus", 2, 0}, {"GetDisplayMode", 2, 0},
    {"GetDevice", 2, r_GetDevice}, {"GetPresentParameters", 2, 0},
};

/* ---- D3DX: matrices are real, textures are null-device textures ---- */

static void mat_identity(uint32_t m) {
    memset(GPTR(m), 0, 64);
    for (int k = 0; k < 4; k++) MF32(m + 20 * k) = 1.0f;
}
static void mat_mul(uint32_t out, uint32_t a, uint32_t b) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += MF32(a + 16 * i + 4 * k) * MF32(b + 16 * k + 4 * j);
            r[4 * i + j] = s;
        }
    memcpy(GPTR(out), r, 64);
}
static void x_MatrixPerspectiveFovRH(CPU* c) {
    uint32_t m = ARG(0);
    float fovy, aspect, zn, zf;
    uint32_t a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), a4 = ARG(4);
    memcpy(&fovy, &a1, 4); memcpy(&aspect, &a2, 4); memcpy(&zn, &a3, 4); memcpy(&zf, &a4, 4);
    float ys = 1.0f / tanf(fovy / 2), xs = ys / aspect;
    memset(GPTR(m), 0, 64);
    MF32(m) = xs; MF32(m + 20) = ys; MF32(m + 40) = zf / (zn - zf); MF32(m + 44) = -1.0f; MF32(m + 56) = zn * zf / (zn - zf);
    RET(m);
}
static void x_MatrixOrthoRH(CPU* c) {
    uint32_t m = ARG(0);
    float w, h, zn, zf;
    uint32_t a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), a4 = ARG(4);
    memcpy(&w, &a1, 4); memcpy(&h, &a2, 4); memcpy(&zn, &a3, 4); memcpy(&zf, &a4, 4);
    static int logged;
    if (logged++ < 3) port_log("D3DXMatrixOrthoRH w=%g h=%g zn=%g zf=%g (from %08x)", w, h, zn, zf, M32(c->esp));
    memset(GPTR(m), 0, 64);
    MF32(m) = 2 / w; MF32(m + 20) = 2 / h; MF32(m + 40) = 1 / (zn - zf); MF32(m + 56) = zn / (zn - zf); MF32(m + 60) = 1.0f;
    RET(m);
}
static void x_MatrixMultiply(CPU* c) { mat_mul(ARG(0), ARG(1), ARG(2)); RET(ARG(0)); }
static void x_MatrixIdentity(CPU* c) { mat_identity(ARG(0)); RET(ARG(0)); }

/* Cofactor expansion; returns the determinant. */
static double mat_inverse(const float* m, float* out) {
    double inv[16], det;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (out && det != 0)
        for (int k = 0; k < 16; k++) out[k] = (float)(inv[k] / det);
    return det;
}
static void read_mat(uint32_t p, float* m) { for (int k = 0; k < 16; k++) m[k] = MF32(p + 4 * (uint32_t)k); }

/* D3DXMatrixInverse(out, pDeterminant, m): NULL when the matrix is singular. */
static void x_MatrixInverse(CPU* c) {
    float m[16], r[16];
    read_mat(ARG(2), m);
    double det = mat_inverse(m, r);
    if (ARG(1)) MF32(ARG(1)) = (float)det;
    if (det == 0) { RET(0); return; }
    for (int k = 0; k < 16; k++) MF32(ARG(0) + 4 * (uint32_t)k) = r[k];
    RET(ARG(0));
}
/* Returns a float, so the result goes on the x87 stack. */
static void x_MatrixDeterminant(CPU* c) {
    float m[16];
    read_mat(ARG(0), m);
    FPUSH((double)(float)mat_inverse(m, 0));
}
static void x_CreateTexture(CPU* c) { M32(ARG(7)) = make_texture(ARG(1), ARG(2), ARG(3), ARG(5) ? ARG(5) : 21); RET(0); }

/* D3DXSaveSurfaceToFileA(file, format, surface, palette, rect): P8 surfaces take their colours from the palette
   (PALETTEENTRY: red, green, blue, flags). */
static void x_SaveSurfaceToFileA(CPU* c) {
    uint32_t s = ARG(2), pal = ARG(3), rect = ARG(4);
    if (!ARG(0) || !s || M32(s + 8) != O_SURFACE) { RET(D3DERR_INVALIDCALL); return; }
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)M32(s + 12), y1 = (int32_t)M32(s + 16);
    if (rect) {
        int32_t l = (int32_t)M32(rect), t = (int32_t)M32(rect + 4), r = (int32_t)M32(rect + 8), b = (int32_t)M32(rect + 12);
        if (l < 0 || t < 0 || r > x1 || b > y1 || l >= r || t >= b) { RET(D3DERR_INVALIDCALL); return; }
        x0 = l; y0 = t; x1 = r; y1 = b;
    }
    if (M32(s + 20) == 41 && !pal) { RET(D3DERR_INVALIDCALL); return; }
    r_read_back(s);
    int w = x1 - x0, h = y1 - y0;
    uint32_t* px = malloc(4 * (size_t)w * (size_t)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t X = (uint32_t)(x0 + x), Y = (uint32_t)(y0 + y);
            if (M32(s + 20) == 41) {
                uint32_t e = pal + 4 * M8(M32(s + 28) + Y * M32(s + 32) + X);
                px[y * w + x] = 0xff000000u | (uint32_t)M8(e) << 16 | (uint32_t)M8(e + 1) << 8 | M8(e + 2);
            } else px[y * w + x] = surface_pixel(s, X, Y);
        }
    char path[600];
    port_host_path(GSTR(ARG(0)), path, sizeof path);
    int ok = image_file_write(path, (int)ARG(1), w, h, px);
    free(px);
    RET(ok ? 0 : D3DERR_INVALIDCALL);
}

/* ewss.ews CaptureScene2(file, width, height, dc): the screenshot writer. The file's extension picks the format,
   JPEG unless it names another one. */
static void e_CaptureScene2(CPU* c) {
    int w = (int)ARG(1), h = (int)ARG(2);
    if (!ARG(0) || w <= 0 || h <= 0) { RET(0); return; }
    uint32_t* px = malloc(4 * (size_t)w * (size_t)h);
    int ok = hle_dc_read(ARG(3), w, h, px);
    if (ok) {
        const char* name = GSTR(ARG(0));
        const char* ext = strrchr(name, '.');
        int fmt = IMAGE_JPG;
        if (ext && !strcasecmp(ext, ".bmp")) fmt = IMAGE_BMP;
        else if (ext && !strcasecmp(ext, ".png")) fmt = IMAGE_PNG;
        else if (ext && !strcasecmp(ext, ".tga")) fmt = IMAGE_TGA;
        char path[600];
        port_host_path(name, path, sizeof path);
        ok = image_file_write(path, fmt, w, h, px);
        port_log("screenshot %s: %s", name, ok ? "saved" : "could not be written");
    }
    free(px);
    RET(ok);
}

/* ID3DXMatrixStack: +40 holds the stack array (32 matrices), +44 the depth. */
static uint32_t ms_top(uint32_t s) { return M32(s + 40) + 64 * M32(s + 44); }
static void ms_Push(CPU* c) { uint32_t t = ms_top(THIS); if (M32(THIS + 44) < 31) { M32(THIS + 44)++; memcpy(GPTR(ms_top(THIS)), GPTR(t), 64); } RET(0); }
static void ms_Pop(CPU* c) { if (M32(THIS + 44)) M32(THIS + 44)--; RET(0); }
static void ms_LoadIdentity(CPU* c) { mat_identity(ms_top(THIS)); RET(0); }
static void ms_LoadMatrix(CPU* c) { memcpy(GPTR(ms_top(THIS)), GPTR(ARG(1)), 64); RET(0); }
static void ms_MultMatrix(CPU* c) { mat_mul(ms_top(THIS), ms_top(THIS), ARG(1)); RET(0); }
static void ms_MultMatrixLocal(CPU* c) { mat_mul(ms_top(THIS), ARG(1), ms_top(THIS)); RET(0); }
static void ms_GetTop(CPU* c) { RET(ms_top(THIS)); }

static float fa(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static void mat_set(float* m, float a, float b, float cc, float d, float e, float f, float g, float h, float i, float j, float k,
                    float l, float mm, float n, float o, float p) {
    float t[16] = {a, b, cc, d, e, f, g, h, i, j, k, l, mm, n, o, p};
    memcpy(m, t, 64);
}
/* D3DX stack semantics: Op multiplies the top on the right (M = M * X), OpLocal on the left (M = X * M). */
static void ms_apply(CPU* c, const float* x, int local) {
    uint32_t tmp = guest_alloc(64, 16), top = ms_top(THIS);
    memcpy(GPTR(tmp), x, 64);
    if (local) mat_mul(top, tmp, top);
    else mat_mul(top, top, tmp);
    guest_free(tmp);
    RET(0);
}
static void translation(float* m, float x, float y, float z) { mat_set(m, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1); }
static void scaling(float* m, float x, float y, float z) { mat_set(m, x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1); }
static void rotation_axis(float* m, float x, float y, float z, float a) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    float s = sinf(a), co = cosf(a), t = 1 - co;
    mat_set(m, t * x * x + co, t * x * y + s * z, t * x * z - s * y, 0, t * x * y - s * z, t * y * y + co, t * y * z + s * x, 0,
            t * x * z + s * y, t * y * z - s * x, t * z * z + co, 0, 0, 0, 0, 1);
}
static void rotation_ypr(float* m, float yaw, float pitch, float roll) {
    float a[16], b[16], r[16], t[16];
    rotation_axis(a, 0, 0, 1, roll);
    rotation_axis(b, 1, 0, 0, pitch);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[4 * i + k] * b[4 * k + j]; t[4 * i + j] = s; }
    rotation_axis(r, 0, 1, 0, yaw);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += t[4 * i + k] * r[4 * k + j]; m[4 * i + j] = s; }
}
static void ms_Translate(CPU* c) { float m[16]; translation(m, fa(ARG(1)), fa(ARG(2)), fa(ARG(3))); ms_apply(c, m, 0); }
static void ms_TranslateLocal(CPU* c) { float m[16]; translation(m, fa(ARG(1)), fa(ARG(2)), fa(ARG(3))); ms_apply(c, m, 1); }
static void ms_Scale(CPU* c) { float m[16]; scaling(m, fa(ARG(1)), fa(ARG(2)), fa(ARG(3))); ms_apply(c, m, 0); }
static void ms_ScaleLocal(CPU* c) { float m[16]; scaling(m, fa(ARG(1)), fa(ARG(2)), fa(ARG(3))); ms_apply(c, m, 1); }
static void ms_RotateAxis(CPU* c) { float m[16]; uint32_t v = ARG(1); rotation_axis(m, MF32(v), MF32(v + 4), MF32(v + 8), fa(ARG(2))); ms_apply(c, m, 0); }
static void ms_RotateAxisLocal(CPU* c) { float m[16]; uint32_t v = ARG(1); rotation_axis(m, MF32(v), MF32(v + 4), MF32(v + 8), fa(ARG(2))); ms_apply(c, m, 1); }
static void ms_RotateYawPitchRoll(CPU* c) { float m[16]; rotation_ypr(m, fa(ARG(1)), fa(ARG(2)), fa(ARG(3))); ms_apply(c, m, 0); }
static void ms_RotateYawPitchRollLocal(CPU* c) { float m[16]; rotation_ypr(m, fa(ARG(1)), fa(ARG(2)), fa(ARG(3))); ms_apply(c, m, 1); }
static const Method matstack_methods[] = {
    {"QueryInterface", 3, m_QueryInterface}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"Pop", 1, ms_Pop},
    {"Push", 1, ms_Push}, {"LoadIdentity", 1, ms_LoadIdentity}, {"LoadMatrix", 2, ms_LoadMatrix},
    {"MultMatrix", 2, ms_MultMatrix}, {"MultMatrixLocal", 2, ms_MultMatrixLocal}, {"RotateAxis", 3, ms_RotateAxis},
    {"RotateAxisLocal", 3, ms_RotateAxisLocal}, {"RotateYawPitchRoll", 4, ms_RotateYawPitchRoll}, {"RotateYawPitchRollLocal", 4, ms_RotateYawPitchRollLocal},
    {"Scale", 4, ms_Scale}, {"ScaleLocal", 4, ms_ScaleLocal}, {"Translate", 4, ms_Translate}, {"TranslateLocal", 4, ms_TranslateLocal}, {"GetTop", 1, ms_GetTop},
};
static void x_CreateMatrixStack(CPU* c) {
    uint32_t s = make_obj(vt_matstack, O_MATSTACK);
    M32(s + 40) = guest_alloc(64 * 32, 16);
    M32(s + 44) = 0;
    mat_identity(M32(s + 40));
    M32(ARG(1)) = s;
    RET(0);
}

static void x_Direct3DCreate9(CPU* c) {
    the_d3d = make_obj(vt_d3d, O_D3D);
    port_log("Direct3DCreate9 -> null device");
    RET(the_d3d);
}

/* ---- registration: each interface method becomes an Hle entry "Interface::Method" ---- */

static Hle dyn[512];
static int dyn_count;

static uint32_t make_vtable(const char* iface, const Method* m, int n) {
    uint32_t vt = guest_alloc(4 * (uint32_t)n, 16);
    for (int k = 0; k < n; k++) {
        char* name = malloc(96);
        snprintf(name, 96, "%s::%s", iface, m[k].name);
        dyn[dyn_count] = (Hle){"d3d9.dll", name, m[k].argc, m[k].fn ? m[k].fn : m_ok};
        hle_register(&dyn[dyn_count++], 1);
        M32(vt + 4 * k) = hle_trampoline("d3d9.dll", name);
    }
    return vt;
}

static const Hle d3d9[] = {
    {"d3d9.dll", "Direct3DCreate9", 1, x_Direct3DCreate9},
    {"d3dx9.dll", "D3DXCreateMatrixStack", 2, x_CreateMatrixStack},
    {"d3dx9.dll", "D3DXMatrixPerspectiveFovRH", 5, x_MatrixPerspectiveFovRH},
    {"d3dx9.dll", "D3DXMatrixOrthoRH", 5, x_MatrixOrthoRH},
    {"d3dx9.dll", "D3DXMatrixMultiply", 3, x_MatrixMultiply},
    {"d3dx9.dll", "D3DXMatrixIdentity", 1, x_MatrixIdentity},
    {"d3dx9.dll", "D3DXMatrixInverse", 3, x_MatrixInverse},
    {"d3dx9.dll", "D3DXMatrixDeterminant", 1, x_MatrixDeterminant},
    {"d3dx9.dll", "D3DXCreateTexture", 8, x_CreateTexture},
    {"d3dx9.dll", "D3DXSaveSurfaceToFileA", 5, x_SaveSurfaceToFileA},
    {"ewss.ews", "CaptureScene2", 4, e_CaptureScene2},
};

#define VT(var, iface, arr) var = make_vtable(iface, arr, (int)(sizeof arr / sizeof arr[0]))

void hle_d3d9_register(void) {
    hle_register(d3d9, sizeof d3d9 / sizeof d3d9[0]);
    VT(vt_d3d, "IDirect3D9", d3d_methods);
    VT(vt_device, "IDirect3DDevice9", device_methods);
    VT(vt_texture, "IDirect3DTexture9", texture_methods);
    VT(vt_surface, "IDirect3DSurface9", surface_methods);
    VT(vt_vb, "IDirect3DVertexBuffer9", vb_methods);
    VT(vt_ib, "IDirect3DIndexBuffer9", ib_methods);
    VT(vt_other, "ID3DObject", other_methods);
    VT(vt_matstack, "ID3DXMatrixStack", matstack_methods);
    VT(vt_swapchain, "IDirect3DSwapChain9", swapchain_methods);
}
