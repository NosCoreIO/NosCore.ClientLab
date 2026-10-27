/* Null Direct3D 9: a headless device that accepts every call. Resources get real memory so Lock/Unlock work;
   draws are dropped. COM objects live in guest memory; their vtables hold trampoline addresses. */
#include "port.h"
#include <math.h>
#include <stdlib.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define THIS ARG(0)

enum { O_D3D = 1, O_DEVICE, O_TEXTURE, O_SURFACE, O_VB, O_IB, O_OTHER, O_MATSTACK, O_SWAPCHAIN };
/* object layout: +0 vtbl +4 refs +8 kind +12 width +16 height +20 format +24 levels +28 data +32 pitch +36 size
   +40 surfaces (texture: array of level surfaces) +44 container +48 usage +52 pool */

typedef struct {
    const char* name;
    uint8_t argc;
    HleFn fn;
} Method;

static void m_ok(CPU* c) { RET(0); }
static void m_addref(CPU* c) { RET(++M32(THIS + 4)); }
static void m_release(CPU* c) { uint32_t r = M32(THIS + 4); RET(r ? --M32(THIS + 4) : 0); }

static uint32_t make_vtable(const char* iface, const Method* m, int n);
static uint32_t vt_d3d, vt_device, vt_texture, vt_surface, vt_vb, vt_ib, vt_other, vt_matstack, vt_swapchain;

static uint32_t make_obj(uint32_t vtbl, int kind) {
    uint32_t o = guest_alloc(64, 16);
    M32(o) = vtbl;
    M32(o + 4) = 1;
    M32(o + 8) = (uint32_t)kind;
    return o;
}

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
    if (fmt == 0x31545844 || fmt == 0x33545844 || fmt == 0x35545844) pitch = ((w + 3) / 4) * (fmt == 0x31545844 ? 8 : 16);
    uint32_t rows = (fmt == 0x31545844 || fmt == 0x33545844 || fmt == 0x35545844) ? (h + 3) / 4 : h;
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

static void fill_caps(uint32_t p) {
    memset(GPTR(p), 0, 304);
    M32(p) = 1; M32(p + 4) = 0;
    M32(p + 28) = 0x0001E000u;                 /* DevCaps */
    M32(p + 88) = 0xFFFFFFFFu; M32(p + 92) = 0xFFFFFFFFu; /* raster/zcmp caps (generous) */
    M32(p + 132) = 0xFFFFFFFFu; M32(p + 136) = 0xFFFFFFFFu; M32(p + 140) = 0xFFFFFFFFu;
    M32(p + 160) = 4096; M32(p + 164) = 4096;  /* MaxTextureWidth/Height */
    M32(p + 168) = 8192; M32(p + 172) = 8192;
    M32(p + 176) = 16;                          /* MaxAnisotropy */
    M32(p + 220) = 8; M32(p + 224) = 8;         /* MaxTextureBlendStages, MaxSimultaneousTextures */
    M32(p + 232) = 8;                           /* MaxActiveLights */
    M32(p + 248) = 0xFFFFF; M32(p + 252) = 0xFFFFF; /* MaxPrimitiveCount, MaxVertexIndex */
    M32(p + 256) = 16;                          /* MaxStreams */
    M32(p + 264) = 0xFFFE0200u;                 /* VertexShaderVersion vs_2_0 */
    M32(p + 268) = 256;
    M32(p + 272) = 0xFFFF0200u;                 /* PixelShaderVersion ps_2_0 */
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
    uint32_t pp = ARG(5), w = M32(pp), h = M32(pp + 4);
    if (!w) w = 1024;
    if (!h) h = 768;
    port_log("IDirect3D9::CreateDevice %ux%u format %u", w, h, M32(pp + 8));
    the_device = make_obj(vt_device, O_DEVICE);
    backbuffer = make_surface(w, h, M32(pp + 8) ? M32(pp + 8) : 22, 0);
    depthbuffer = make_surface(w, h, 75, 0);
    M32(ARG(6)) = the_device;
    RET(0);
}

static const Method d3d_methods[] = {
    {"QueryInterface", 3, 0}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
    {"RegisterSoftwareDevice", 2, 0}, {"GetAdapterCount", 1, d_GetAdapterCount},
    {"GetAdapterIdentifier", 4, d_GetAdapterIdentifier}, {"GetAdapterModeCount", 3, d_GetAdapterModeCount},
    {"EnumAdapterModes", 5, d_EnumAdapterModes}, {"GetAdapterDisplayMode", 3, d_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, 0}, {"CheckDeviceFormat", 7, 0}, {"CheckDeviceMultiSampleType", 7, 0},
    {"CheckDepthStencilMatch", 6, 0}, {"CheckDeviceFormatConversion", 4, 0}, {"GetDeviceCaps", 4, d_GetDeviceCaps},
    {"GetAdapterMonitor", 2, d_GetAdapterMonitor}, {"CreateDevice", 7, d_CreateDevice},
};

/* ---- IDirect3DDevice9 ---- */

static uint32_t frames;
static void v_TestCooperativeLevel(CPU* c) { RET(0); }
static void v_GetAvailableTextureMem(CPU* c) { RET(512u << 20); }
static void v_GetDirect3D(CPU* c) { M32(ARG(1)) = the_d3d; M32(the_d3d + 4)++; RET(0); }
static void v_GetDeviceCaps(CPU* c) { fill_caps(ARG(1)); RET(0); }
static void v_GetDisplayMode(CPU* c) { fill_mode(ARG(2)); RET(0); }
static void v_GetCreationParameters(CPU* c) { memset(GPTR(ARG(1)), 0, 16); RET(0); }
static void v_GetSwapChain(CPU* c) { M32(ARG(2)) = make_obj(vt_swapchain, O_SWAPCHAIN); RET(0); }
static void v_GetNumberOfSwapChains(CPU* c) { RET(1); }
static void v_Present(CPU* c) { if (++frames % 300 == 1) port_log("Present frame %u", frames); RET(0); }
static void v_GetBackBuffer(CPU* c) { M32(backbuffer + 4)++; M32(ARG(4)) = backbuffer; RET(0); }
static void v_GetRasterStatus(CPU* c) { memset(GPTR(ARG(2)), 0, 8); RET(0); }
static void v_CreateTexture(CPU* c) { M32(ARG(7)) = make_texture(ARG(1), ARG(2), ARG(3), ARG(5)); RET(0); }
static void v_CreateVolumeTexture(CPU* c) { M32(ARG(8)) = make_texture(ARG(1), ARG(2), ARG(4), ARG(6)); RET(0); }
static void v_CreateCubeTexture(CPU* c) { M32(ARG(6)) = make_texture(ARG(1), ARG(1), ARG(2), ARG(4)); RET(0); }
static void v_CreateVertexBuffer(CPU* c) { M32(ARG(5)) = make_buffer(vt_vb, O_VB, ARG(1), ARG(3)); RET(0); }
static void v_CreateIndexBuffer(CPU* c) { M32(ARG(5)) = make_buffer(vt_ib, O_IB, ARG(1), ARG(3)); RET(0); }
static void v_CreateRenderTarget(CPU* c) { M32(ARG(7)) = make_surface(ARG(1), ARG(2), ARG(3), 0); RET(0); }
static void v_CreateDepthStencilSurface(CPU* c) { M32(ARG(7)) = make_surface(ARG(1), ARG(2), ARG(3), 0); RET(0); }
static void v_CreateOffscreenPlainSurface(CPU* c) { M32(ARG(5)) = make_surface(ARG(1), ARG(2), ARG(3), 0); RET(0); }
static void v_GetRenderTarget(CPU* c) { M32(backbuffer + 4)++; M32(ARG(2)) = backbuffer; RET(0); }
static void v_GetDepthStencilSurface(CPU* c) { M32(depthbuffer + 4)++; M32(ARG(1)) = depthbuffer; RET(0); }
static void v_GetTransform(CPU* c) {
    memset(GPTR(ARG(2)), 0, 64);
    for (int k = 0; k < 4; k++) MF32(ARG(2) + 20 * k) = 1.0f;
    RET(0);
}
static void v_GetViewport(CPU* c) {
    uint32_t p = ARG(1);
    M32(p) = 0; M32(p + 4) = 0; M32(p + 8) = M32(backbuffer + 12); M32(p + 12) = M32(backbuffer + 16);
    MF32(p + 16) = 0.0f; MF32(p + 20) = 1.0f;
    RET(0);
}
static void v_zero_out1(CPU* c) { if (ARG(1)) M32(ARG(1)) = 0; RET(0); }
static void v_zero_out2(CPU* c) { if (ARG(2)) M32(ARG(2)) = 0; RET(0); }
static void v_zero_out3(CPU* c) { if (ARG(3)) M32(ARG(3)) = 0; RET(0); }
static void v_zero_struct(CPU* c) { if (ARG(1)) memset(GPTR(ARG(1)), 0, 68); RET(0); }
static void v_zero_struct2(CPU* c) { if (ARG(2)) memset(GPTR(ARG(2)), 0, 104); RET(0); }
static void v_new_other_out2(CPU* c) { M32(ARG(2)) = make_obj(vt_other, O_OTHER); RET(0); }
static void v_new_other_out1(CPU* c) { M32(ARG(1)) = make_obj(vt_other, O_OTHER); RET(0); }
static void v_ValidateDevice(CPU* c) { if (ARG(1)) M32(ARG(1)) = 1; RET(0); }

static const Method device_methods[] = {
    {"QueryInterface", 3, 0}, {"AddRef", 1, m_addref}, {"Release", 1, m_release},
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
    {"SetRenderTarget", 3, 0}, {"GetRenderTarget", 3, v_GetRenderTarget}, {"SetDepthStencilSurface", 2, 0},
    {"GetDepthStencilSurface", 2, v_GetDepthStencilSurface}, {"BeginScene", 1, 0}, {"EndScene", 1, 0},
    {"Clear", 7, 0}, {"SetTransform", 3, 0}, {"GetTransform", 3, v_GetTransform}, {"MultiplyTransform", 3, 0},
    {"SetViewport", 2, 0}, {"GetViewport", 2, v_GetViewport}, {"SetMaterial", 2, 0}, {"GetMaterial", 2, v_zero_struct},
    {"SetLight", 3, 0}, {"GetLight", 3, v_zero_struct2}, {"LightEnable", 3, 0}, {"GetLightEnable", 3, v_zero_out2},
    {"SetClipPlane", 3, 0}, {"GetClipPlane", 3, 0}, {"SetRenderState", 3, 0}, {"GetRenderState", 3, v_zero_out2},
    {"CreateStateBlock", 3, v_new_other_out2}, {"BeginStateBlock", 1, 0}, {"EndStateBlock", 2, v_new_other_out1},
    {"SetClipStatus", 2, 0}, {"GetClipStatus", 2, 0}, {"GetTexture", 3, v_zero_out2}, {"SetTexture", 3, 0},
    {"GetTextureStageState", 4, v_zero_out3}, {"SetTextureStageState", 4, 0}, {"GetSamplerState", 4, v_zero_out3},
    {"SetSamplerState", 4, 0}, {"ValidateDevice", 2, v_ValidateDevice}, {"SetPaletteEntries", 3, 0},
    {"GetPaletteEntries", 3, 0}, {"SetCurrentTexturePalette", 2, 0}, {"GetCurrentTexturePalette", 2, v_zero_out1},
    {"SetScissorRect", 2, 0}, {"GetScissorRect", 2, 0}, {"SetSoftwareVertexProcessing", 2, 0},
    {"GetSoftwareVertexProcessing", 1, 0}, {"SetNPatchMode", 2, 0}, {"GetNPatchMode", 1, 0},
    {"DrawPrimitive", 4, 0}, {"DrawIndexedPrimitive", 7, 0}, {"DrawPrimitiveUP", 5, 0},
    {"DrawIndexedPrimitiveUP", 9, 0}, {"ProcessVertices", 7, 0}, {"CreateVertexDeclaration", 3, v_new_other_out2},
    {"SetVertexDeclaration", 2, 0}, {"GetVertexDeclaration", 2, v_zero_out1}, {"SetFVF", 2, 0},
    {"GetFVF", 2, v_zero_out1}, {"CreateVertexShader", 3, v_new_other_out2}, {"SetVertexShader", 2, 0},
    {"GetVertexShader", 2, v_zero_out1}, {"SetVertexShaderConstantF", 4, 0}, {"GetVertexShaderConstantF", 4, 0},
    {"SetVertexShaderConstantI", 4, 0}, {"GetVertexShaderConstantI", 4, 0}, {"SetVertexShaderConstantB", 4, 0},
    {"GetVertexShaderConstantB", 4, 0}, {"SetStreamSource", 5, 0}, {"GetStreamSource", 5, v_zero_out2},
    {"SetStreamSourceFreq", 3, 0}, {"GetStreamSourceFreq", 3, v_zero_out2}, {"SetIndices", 2, 0},
    {"GetIndices", 2, v_zero_out1}, {"CreatePixelShader", 3, v_new_other_out2}, {"SetPixelShader", 2, 0},
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
static void t_LockRect(CPU* c) {
    uint32_t s = level(THIS, ARG(1));
    if (!s) { RET(0x8876086cu); return; }
    M32(ARG(2)) = M32(s + 32);
    M32(ARG(2) + 4) = M32(s + 28);
    RET(0);
}
static void s_GetDesc(CPU* c) { fill_surface_desc(THIS, ARG(1)); RET(0); }
static void s_LockRect(CPU* c) { M32(ARG(1)) = M32(THIS + 32); M32(ARG(1) + 4) = M32(THIS + 28); RET(0); }
static void s_GetContainer(CPU* c) { M32(ARG(2)) = M32(THIS + 44); RET(M32(THIS + 44) ? 0 : 0x80004002u); }
static void s_GetDC(CPU* c) { M32(ARG(1)) = 0x00300100u; RET(0); }
static void b_Lock(CPU* c) { M32(ARG(3)) = M32(THIS + 28) + ARG(1); RET(0); }
static void b_GetDesc(CPU* c) { memset(GPTR(ARG(1)), 0, 24); M32(ARG(1)) = M32(THIS + 20); M32(ARG(1) + 20) = M32(THIS + 36); RET(0); }

#define RESOURCE_METHODS \
    {"QueryInterface", 3, 0}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"GetDevice", 2, r_GetDevice}, \
    {"SetPrivateData", 5, 0}, {"GetPrivateData", 4, 0}, {"FreePrivateData", 2, 0}, {"SetPriority", 2, 0}, \
    {"GetPriority", 1, 0}, {"PreLoad", 1, 0}, {"GetType", 1, r_GetType}

static const Method texture_methods[] = {
    RESOURCE_METHODS, {"SetLOD", 2, 0}, {"GetLOD", 1, 0}, {"GetLevelCount", 1, t_GetLevelCount},
    {"SetAutoGenFilterType", 2, 0}, {"GetAutoGenFilterType", 1, 0}, {"GenerateMipSubLevels", 1, 0},
    {"GetLevelDesc", 3, t_GetLevelDesc}, {"GetSurfaceLevel", 3, t_GetSurfaceLevel}, {"LockRect", 5, t_LockRect},
    {"UnlockRect", 2, 0}, {"AddDirtyRect", 2, 0},
};
static const Method surface_methods[] = {
    RESOURCE_METHODS, {"GetContainer", 3, s_GetContainer}, {"GetDesc", 2, s_GetDesc}, {"LockRect", 4, s_LockRect},
    {"UnlockRect", 1, 0}, {"GetDC", 2, s_GetDC}, {"ReleaseDC", 2, 0},
};
static const Method vb_methods[] = {RESOURCE_METHODS, {"Lock", 5, b_Lock}, {"Unlock", 1, 0}, {"GetDesc", 2, b_GetDesc}};
static const Method ib_methods[] = {RESOURCE_METHODS, {"Lock", 5, b_Lock}, {"Unlock", 1, 0}, {"GetDesc", 2, b_GetDesc}};
static const Method other_methods[] = {
    {"QueryInterface", 3, 0}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"Method3", 1, 0},
    {"Method4", 1, 0}, {"Method5", 1, 0}, {"Method6", 1, 0}, {"Method7", 1, 0}, {"Method8", 1, 0},
};
static const Method swapchain_methods[] = {
    {"QueryInterface", 3, 0}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"Present", 6, 0},
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
    memset(GPTR(m), 0, 64);
    MF32(m) = 2 / w; MF32(m + 20) = 2 / h; MF32(m + 40) = 1 / (zn - zf); MF32(m + 56) = zn / (zn - zf); MF32(m + 60) = 1.0f;
    RET(m);
}
static void x_MatrixMultiply(CPU* c) { mat_mul(ARG(0), ARG(1), ARG(2)); RET(ARG(0)); }
static void x_MatrixIdentity(CPU* c) { mat_identity(ARG(0)); RET(ARG(0)); }
static void x_CreateTexture(CPU* c) { M32(ARG(7)) = make_texture(ARG(1), ARG(2), ARG(3), ARG(5) ? ARG(5) : 21); RET(0); }

/* ID3DXMatrixStack: +40 holds the stack array (32 matrices), +44 the depth. */
static uint32_t ms_top(uint32_t s) { return M32(s + 40) + 64 * M32(s + 44); }
static void ms_Push(CPU* c) { uint32_t t = ms_top(THIS); if (M32(THIS + 44) < 31) { M32(THIS + 44)++; memcpy(GPTR(ms_top(THIS)), GPTR(t), 64); } RET(0); }
static void ms_Pop(CPU* c) { if (M32(THIS + 44)) M32(THIS + 44)--; RET(0); }
static void ms_LoadIdentity(CPU* c) { mat_identity(ms_top(THIS)); RET(0); }
static void ms_LoadMatrix(CPU* c) { memcpy(GPTR(ms_top(THIS)), GPTR(ARG(1)), 64); RET(0); }
static void ms_MultMatrix(CPU* c) { mat_mul(ms_top(THIS), ms_top(THIS), ARG(1)); RET(0); }
static void ms_MultMatrixLocal(CPU* c) { mat_mul(ms_top(THIS), ARG(1), ms_top(THIS)); RET(0); }
static void ms_GetTop(CPU* c) { RET(ms_top(THIS)); }
static const Method matstack_methods[] = {
    {"QueryInterface", 3, 0}, {"AddRef", 1, m_addref}, {"Release", 1, m_release}, {"Pop", 1, ms_Pop},
    {"Push", 1, ms_Push}, {"LoadIdentity", 1, ms_LoadIdentity}, {"LoadMatrix", 2, ms_LoadMatrix},
    {"MultMatrix", 2, ms_MultMatrix}, {"MultMatrixLocal", 2, ms_MultMatrixLocal}, {"RotateAxis", 3, 0},
    {"RotateAxisLocal", 3, 0}, {"RotateYawPitchRoll", 4, 0}, {"RotateYawPitchRollLocal", 4, 0},
    {"Scale", 4, 0}, {"ScaleLocal", 4, 0}, {"Translate", 4, 0}, {"TranslateLocal", 4, 0}, {"GetTop", 1, ms_GetTop},
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
    {"d3dx9.dll", "D3DXCreateTexture", 8, x_CreateTexture},
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
