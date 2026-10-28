/* COM and OLE automation: apartment init counts, task memory, CLSID strings, IStream over global memory, BSTRs,
   SAFEARRAYs, VARIANT copy/clear/coercion, OLE drag and drop. No COM class is registered in the browser, so class
   lookups fail the way Windows does for an unregistered CLSID. */
#include "port.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define RET(v) (c->eax = (uint32_t)(v))

#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define STG_E_INVALIDFUNCTION 0x80030001u
#define REGDB_E_CLASSNOTREG 0x80040154u
#define CO_E_NOTINITIALIZED 0x800401f0u
#define MK_E_UNAVAILABLE 0x800401e3u
#define DV_E_NOIVIEWOBJECT 0x80040067u
#define DISP_E_TYPEMISMATCH 0x80020005u
#define DISP_E_BADVARTYPE 0x80020008u
#define DISP_E_OVERFLOW 0x8002000au
#define DISP_E_BADINDEX 0x8002000bu
#define DRAGDROP_S_DROP 0x00040100u
#define DRAGDROP_S_CANCEL 0x00040101u
#define DRAGDROP_S_USEDEFAULTCURSORS 0x00040102u

static const uint8_t IID_IUnknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t IID_IDispatch[16] = {0, 4, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t IID_IStream[16] = {0x0c, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t IID_ISequentialStream[16] = {0x30, 0x3a, 0x73, 0x0c, 0x1c, 0x2a, 0xce, 0x11, 0xad, 0xe5, 0, 0xaa, 0, 0x44, 0x77, 0x3d};
static const uint8_t IID_IViewObject[16] = {0x0d, 1, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};

/* Calls method n of a guest COM object. */
#define VCALL(obj, n, argc, ...) guest_call(c, M32(M32(obj) + 4u * (n)), (argc), (obj), __VA_ARGS__)
#define VCALL0(obj, n) guest_call(c, M32(M32(obj) + 4u * (n)), 1, (obj))

static uint32_t guid_in(CPU* c, uint32_t obj, const uint8_t* iid, uint32_t* out) {
    uint32_t g = guest_alloc(20, 16);
    memcpy(GPTR(g), iid, 16);
    M32(g + 16) = 0;
    uint32_t hr = VCALL(obj, 0, 3, g, g + 16);
    *out = hr ? 0 : M32(g + 16);
    guest_free(g);
    return hr;
}

/* ---- apartments and task memory ---- */

static int com_inits, ole_inits;

static void o_CoInitialize(CPU* c) { RET(com_inits++ ? S_FALSE : 0); }
static void o_CoInitializeEx(CPU* c) { RET(com_inits++ ? S_FALSE : 0); }
static void o_CoUninitialize(CPU* c) { if (com_inits) com_inits--; }
static void o_OleUninitialize(CPU* c) {
    if (ole_inits) ole_inits--;
    if (com_inits) com_inits--;
}
static void o_CoTaskMemAlloc(CPU* c) { RET(guest_alloc(ARG(0) ? ARG(0) : 1, 16)); }
static void o_CoTaskMemFree(CPU* c) { if (ARG(0)) guest_free(ARG(0)); }

static void o_IsEqualGUID(CPU* c) { RET(!memcmp(GPTR(ARG(0)), GPTR(ARG(1)), 16)); }
static void o_StringFromCLSID(CPU* c) {
    const uint8_t* g = GPTR(ARG(0));
    char s[40];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g[0] | g[1] << 8 | g[2] << 16 | (uint32_t)g[3] << 24,
             g[4] | g[5] << 8, g[6] | g[7] << 8, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    uint32_t w = guest_alloc(2 * 39, 16);
    for (int k = 0; k < 39; k++) M16(w + 2 * (uint32_t)k) = (uint8_t)s[k];
    M32(ARG(1)) = w;
    RET(0);
}

static void o_CoCreateInstance(CPU* c) {
    if (ARG(4)) M32(ARG(4)) = 0;
    RET(!ARG(4) ? E_POINTER : !com_inits ? CO_E_NOTINITIALIZED : REGDB_E_CLASSNOTREG);
}
static void o_CoGetClassObject(CPU* c) {
    if (ARG(4)) M32(ARG(4)) = 0;
    RET(!ARG(4) ? E_POINTER : !com_inits ? CO_E_NOTINITIALIZED : REGDB_E_CLASSNOTREG);
}
static void o_ProgIDFromCLSID(CPU* c) {
    if (ARG(1)) M32(ARG(1)) = 0;
    RET(REGDB_E_CLASSNOTREG);
}
/* Nothing registers in the running object table. */
static void o_GetActiveObject(CPU* c) {
    if (ARG(2)) M32(ARG(2)) = 0;
    RET(MK_E_UNAVAILABLE);
}
/* No IErrorInfo is ever set on this thread. */
static void o_GetErrorInfo(CPU* c) {
    if (ARG(1)) M32(ARG(1)) = 0;
    RET(S_FALSE);
}
/* No OLE menu descriptor can exist (nothing here creates one), so only removing a descriptor succeeds. */
static void o_OleSetMenuDescriptor(CPU* c) { RET(ARG(0) ? E_INVALIDARG : 0); }
/* No accelerator table can be created in this process, so no handle names one. */
static void o_IsAccelerator(CPU* c) { RET(0); }

static void o_OleDraw(CPU* c) {
    uint32_t view, rect = ARG(3);
    if (!ARG(0)) { RET(E_INVALIDARG); return; }
    if (guid_in(c, ARG(0), IID_IViewObject, &view) || !view) { RET(DV_E_NOIVIEWOBJECT); return; }
    uint32_t hr = guest_call(c, M32(M32(view) + 12), 11, view, ARG(1), 0xffffffffu, 0, 0, 0, ARG(2), rect, 0, 0, 0);
    VCALL0(view, 2);
    RET(hr);
}

/* ---- IStream on global memory ---- */

/* Object: vtable, refs, hglobal, position, logical size, delete-on-release. */
enum { ST_VT, ST_REFS = 4, ST_MEM = 8, ST_POS = 12, ST_SIZE = 16, ST_OWN = 20, ST_BYTES = 24 };
static uint32_t stream_vtable;

static uint32_t stream_new(uint32_t mem, uint32_t size, uint32_t own) {
    uint32_t s = guest_alloc(ST_BYTES, 16);
    M32(s + ST_VT) = stream_vtable;
    M32(s + ST_REFS) = 1;
    M32(s + ST_MEM) = mem;
    M32(s + ST_POS) = 0;
    M32(s + ST_SIZE) = size;
    M32(s + ST_OWN) = own;
    return s;
}
static int stream_reserve(uint32_t s, uint32_t size) {
    uint32_t mem = M32(s + ST_MEM), have = port_global_size(mem);
    if (size <= have) return 1;
    uint32_t grown = port_global_alloc(size + size / 2 + 64);
    if (!grown) return 0;
    memcpy(GPTR(grown), GPTR(mem), have);
    if (M32(s + ST_OWN)) port_global_free(mem);
    M32(s + ST_MEM) = grown;
    M32(s + ST_OWN) = 1;
    return 1;
}
static void st_QueryInterface(CPU* c) {
    uint32_t s = ARG(0), riid = ARG(1);
    const void* iid = GPTR(riid);
    if (!ARG(2)) { RET(E_POINTER); return; }
    if (memcmp(iid, IID_IUnknown, 16) && memcmp(iid, IID_IStream, 16) && memcmp(iid, IID_ISequentialStream, 16)) {
        M32(ARG(2)) = 0;
        RET(E_NOINTERFACE);
        return;
    }
    M32(s + ST_REFS)++;
    M32(ARG(2)) = s;
    RET(0);
}
static void st_AddRef(CPU* c) { RET(++M32(ARG(0) + ST_REFS)); }
static void st_Release(CPU* c) {
    uint32_t s = ARG(0), n = --M32(s + ST_REFS);
    if (!n) {
        if (M32(s + ST_OWN)) port_global_free(M32(s + ST_MEM));
        guest_free(s);
    }
    RET(n);
}
static void st_Read(CPU* c) {
    uint32_t s = ARG(0), pos = M32(s + ST_POS), size = M32(s + ST_SIZE), n = ARG(2);
    if (pos >= size) n = 0;
    else if (n > size - pos) n = size - pos;
    memcpy(GPTR(ARG(1)), GPTR(M32(s + ST_MEM) + pos), n);
    M32(s + ST_POS) = pos + n;
    if (ARG(3)) M32(ARG(3)) = n;
    RET(n < ARG(2) ? S_FALSE : 0);
}
static void st_Write(CPU* c) {
    uint32_t s = ARG(0), pos = M32(s + ST_POS), n = ARG(2);
    if (!stream_reserve(s, pos + n)) { RET(E_OUTOFMEMORY); return; }
    memcpy(GPTR(M32(s + ST_MEM) + pos), GPTR(ARG(1)), n);
    M32(s + ST_POS) = pos + n;
    if (pos + n > M32(s + ST_SIZE)) M32(s + ST_SIZE) = pos + n;
    if (ARG(3)) M32(ARG(3)) = n;
    RET(0);
}
/* Seek(this, LARGE_INTEGER move (two dwords), origin, ULARGE_INTEGER* new_pos). */
static void st_Seek(CPU* c) {
    uint32_t s = ARG(0), origin = ARG(3), out = ARG(4);
    int64_t move = (int64_t)((uint64_t)ARG(2) << 32 | ARG(1));
    int64_t base = origin == 0 ? 0 : origin == 1 ? M32(s + ST_POS) : origin == 2 ? M32(s + ST_SIZE) : -1;
    if (base < 0) { RET(STG_E_INVALIDFUNCTION); return; }
    int64_t pos = base + move;
    if (pos < 0 || pos > 0xffffffffll) { RET(STG_E_INVALIDFUNCTION); return; }
    M32(s + ST_POS) = (uint32_t)pos;
    if (out) { M32(out) = (uint32_t)pos; M32(out + 4) = 0; }
    RET(0);
}
static void st_SetSize(CPU* c) {
    uint32_t s = ARG(0), size = ARG(1);
    if (ARG(2)) { RET(STG_E_INVALIDFUNCTION); return; }
    if (!stream_reserve(s, size)) { RET(E_OUTOFMEMORY); return; }
    if (size > M32(s + ST_SIZE)) memset(GPTR(M32(s + ST_MEM) + M32(s + ST_SIZE)), 0, size - M32(s + ST_SIZE));
    M32(s + ST_SIZE) = size;
    RET(0);
}
/* CopyTo(this, dest, ULARGE_INTEGER cb, ULARGE_INTEGER* read, ULARGE_INTEGER* written). */
static void st_CopyTo(CPU* c) {
    uint32_t s = ARG(0), dst = ARG(1), pos = M32(s + ST_POS), size = M32(s + ST_SIZE);
    uint64_t want = (uint64_t)ARG(3) << 32 | ARG(2);
    uint32_t n = pos >= size ? 0 : (uint64_t)(size - pos) < want ? size - pos : (uint32_t)want;
    uint32_t done = guest_alloc(4, 4);
    M32(done) = 0;
    uint32_t hr = n ? guest_call(c, M32(M32(dst) + 16), 4, dst, M32(s + ST_MEM) + pos, n, done) : 0;
    uint32_t wrote = M32(done);
    guest_free(done);
    M32(s + ST_POS) = pos + n;
    if (ARG(4)) { M32(ARG(4)) = n; M32(ARG(4) + 4) = 0; }
    if (ARG(5)) { M32(ARG(5)) = wrote; M32(ARG(5) + 4) = 0; }
    RET(hr);
}
static void st_Commit(CPU* c) { RET(0); }
static void st_Revert(CPU* c) { RET(0); }
static void st_LockRegion(CPU* c) { RET(STG_E_INVALIDFUNCTION); }
/* STATSTG: name, type, cbSize, three FILETIMEs, mode, locks supported, clsid, state bits, reserved. */
static void st_Stat(CPU* c) {
    uint32_t s = ARG(0), out = ARG(1);
    if (!out) { RET(STG_E_INVALIDFUNCTION); return; }
    memset(GPTR(out), 0, 72);
    M32(out + 4) = 2;   /* STGTY_STREAM */
    M32(out + 8) = M32(s + ST_SIZE);
    M32(out + 40) = 2;  /* STGM_READWRITE */
    RET(0);
}
static void st_Clone(CPU* c) {
    uint32_t s = ARG(0), size = M32(s + ST_SIZE), mem = port_global_alloc(size ? size : 1);
    memcpy(GPTR(mem), GPTR(M32(s + ST_MEM)), size);
    uint32_t n = stream_new(mem, size, 1);
    M32(n + ST_POS) = M32(s + ST_POS);
    M32(ARG(1)) = n;
    RET(0);
}

static void o_CreateStreamOnHGlobal(CPU* c) {
    uint32_t mem = ARG(0), own = ARG(1), out = ARG(2);
    if (!out) { RET(E_INVALIDARG); return; }
    uint32_t size = mem ? port_global_size(mem) : 0;
    if (!mem) { mem = port_global_alloc(64); own = 1; }
    M32(out) = stream_new(mem, size, own);
    RET(0);
}

/* ---- BSTR (length prefix in bytes, then UTF-16, then NUL) ---- */

static uint32_t bstr_alloc(uint32_t src, uint32_t chars) {
    uint32_t p = guest_alloc(4 + 2 * chars + 2, 16);
    M32(p) = 2 * chars;
    if (src) memcpy(GPTR(p + 4), GPTR(src), 2 * chars);
    M16(p + 4 + 2 * chars) = 0;
    return p + 4;
}
static void bstr_free(uint32_t b) { if (b) guest_free(b - 4); }
static uint32_t bstr_len(uint32_t b) { return b ? M32(b - 4) / 2 : 0; }
static uint32_t bstr_from(const char* s) {
    uint32_t n = (uint32_t)strlen(s), b = bstr_alloc(0, n);
    for (uint32_t k = 0; k < n; k++) M16(b + 2 * k) = (uint8_t)s[k];
    return b;
}
static void o_SysAllocStringLen(CPU* c) { RET(bstr_alloc(ARG(0), ARG(1))); }
static void o_SysFreeString(CPU* c) { bstr_free(ARG(0)); }
static void o_SysReAllocStringLen(CPU* c) {
    uint32_t old = M32(ARG(0)), n = bstr_alloc(ARG(1), ARG(2));
    bstr_free(old);
    M32(ARG(0)) = n;
    RET(1);
}
static void o_SysStringLen(CPU* c) { RET(bstr_len(ARG(0))); }

/* ---- VARIANT and SAFEARRAY ---- */

enum {
    VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_CY = 6, VT_DATE = 7, VT_BSTR = 8,
    VT_DISPATCH = 9, VT_ERROR = 10, VT_BOOL = 11, VT_VARIANT = 12, VT_UNKNOWN = 13, VT_DECIMAL = 14, VT_I1 = 16,
    VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21, VT_INT = 22, VT_UINT = 23,
    VT_ARRAY = 0x2000, VT_BYREF = 0x4000
};
enum { FADF_HAVEVARTYPE = 0x80, FADF_BSTR = 0x100, FADF_UNKNOWN = 0x200, FADF_DISPATCH = 0x400, FADF_VARIANT = 0x800 };

static uint32_t vt_size(uint32_t vt) {
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_INT: case VT_UINT: case VT_ERROR: case VT_BSTR: case VT_DISPATCH:
    case VT_UNKNOWN: return 4;
    case VT_R8: case VT_CY: case VT_DATE: case VT_I8: case VT_UI8: return 8;
    case VT_VARIANT: case VT_DECIMAL: return 16;
    default: return 0;
    }
}

/* SAFEARRAY: cDims, fFeatures, cbElements, cLocks, pvData, then the bounds with the last dimension first. The
   element VARTYPE sits in the dword before the structure. */
static uint32_t sa_cells(uint32_t sa) {
    uint32_t n = 1;
    for (uint32_t d = 0; d < M16(sa); d++) n *= M32(sa + 16 + 8 * d);
    return n;
}
static void variant_clear(CPU* c, uint32_t v);
static uint32_t variant_copy(CPU* c, uint32_t dst, uint32_t src);

static void sa_destroy(CPU* c, uint32_t sa) {
    if (!sa) return;
    uint32_t feat = M16(sa + 2), size = M32(sa + 4), data = M32(sa + 12), n = sa_cells(sa);
    for (uint32_t k = 0; data && k < n; k++) {
        uint32_t e = data + k * size;
        if (feat & FADF_BSTR) bstr_free(M32(e));
        else if (feat & (FADF_UNKNOWN | FADF_DISPATCH)) { if (M32(e)) VCALL0(M32(e), 2); }
        else if (feat & FADF_VARIANT) variant_clear(c, e);
    }
    guest_free(data);
    guest_free(sa - 16);
}
static uint32_t sa_create(uint32_t vt, uint32_t dims, uint32_t bounds) {
    uint32_t size = vt_size(vt);
    if (!size || vt == VT_DECIMAL || !dims || dims > 64) return 0;
    uint32_t base = guest_alloc(16 + 16 + 8 * dims, 16), sa = base + 16;
    memset(GPTR(base), 0, 32 + 8 * dims);
    M32(sa - 4) = vt;
    M16(sa) = (uint16_t)dims;
    M16(sa + 2) = (uint16_t)(FADF_HAVEVARTYPE | (vt == VT_BSTR ? FADF_BSTR : vt == VT_UNKNOWN ? FADF_UNKNOWN
                                                 : vt == VT_DISPATCH ? FADF_DISPATCH : vt == VT_VARIANT ? FADF_VARIANT : 0));
    M32(sa + 4) = size;
    for (uint32_t d = 0; d < dims; d++) {
        M32(sa + 16 + 8 * d) = M32(bounds + 8 * (dims - 1 - d));
        M32(sa + 20 + 8 * d) = M32(bounds + 8 * (dims - 1 - d) + 4);
    }
    uint32_t bytes = sa_cells(sa) * size;
    M32(sa + 12) = guest_alloc(bytes ? bytes : 1, 16);
    memset(GPTR(M32(sa + 12)), 0, bytes);
    return sa;
}
static uint32_t sa_copy(CPU* c, uint32_t sa) {
    uint32_t dims = M16(sa), bounds = guest_alloc(8 * dims, 4);
    for (uint32_t d = 0; d < dims; d++) {
        M32(bounds + 8 * d) = M32(sa + 16 + 8 * (dims - 1 - d));
        M32(bounds + 8 * d + 4) = M32(sa + 20 + 8 * (dims - 1 - d));
    }
    uint32_t vt = M16(sa + 2) & FADF_HAVEVARTYPE ? M32(sa - 4) : M16(sa + 2) & FADF_BSTR ? VT_BSTR
                : M16(sa + 2) & FADF_VARIANT ? VT_VARIANT : M16(sa + 2) & FADF_DISPATCH ? VT_DISPATCH : VT_UNKNOWN;
    uint32_t r = sa_create(vt, dims, bounds);
    guest_free(bounds);
    if (!r) return 0;
    uint32_t size = M32(sa + 4), n = sa_cells(sa), from = M32(sa + 12), to = M32(r + 12);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t s = from + k * size, d = to + k * size;
        if (vt == VT_BSTR) M32(d) = M32(s) ? bstr_alloc(M32(s), bstr_len(M32(s))) : 0;
        else if (vt == VT_VARIANT) variant_copy(c, d, s);
        else {
            memcpy(GPTR(d), GPTR(s), size);
            if ((vt == VT_UNKNOWN || vt == VT_DISPATCH) && M32(s)) VCALL0(M32(s), 1);
        }
    }
    return r;
}
static void o_SafeArrayCreate(CPU* c) { RET(sa_create(ARG(0), ARG(1), ARG(2))); }
static void sa_bound(CPU* c, int upper) {
    uint32_t sa = ARG(0), dim = ARG(1);
    if (!sa || !ARG(2)) { RET(E_INVALIDARG); return; }
    if (!dim || dim > M16(sa)) { RET(DISP_E_BADINDEX); return; }
    uint32_t b = sa + 16 + 8 * (M16(sa) - dim);
    M32(ARG(2)) = upper ? M32(b + 4) + M32(b) - 1 : M32(b + 4);
    RET(0);
}
static void o_SafeArrayGetLBound(CPU* c) { sa_bound(c, 0); }
static void o_SafeArrayGetUBound(CPU* c) { sa_bound(c, 1); }
/* The first index varies fastest. */
static void o_SafeArrayPtrOfIndex(CPU* c) {
    uint32_t sa = ARG(0), idx = ARG(1), out = ARG(2);
    if (!sa || !idx || !out) { RET(E_INVALIDARG); return; }
    uint32_t dims = M16(sa), cell = 0, span = 1;
    for (uint32_t d = 0; d < dims; d++) {
        uint32_t b = sa + 16 + 8 * (dims - 1 - d);
        int32_t i = (int32_t)M32(idx + 4 * d) - (int32_t)M32(b + 4);
        if (i < 0 || (uint32_t)i >= M32(b)) { RET(DISP_E_BADINDEX); return; }
        cell += (uint32_t)i * span;
        span *= M32(b);
    }
    M32(out) = M32(sa + 12) + cell * M32(sa + 4);
    RET(0);
}

static void variant_clear(CPU* c, uint32_t v) {
    uint32_t vt = M16(v);
    if (!(vt & VT_BYREF)) {
        if (vt & VT_ARRAY) sa_destroy(c, M32(v + 8));
        else if (vt == VT_BSTR) bstr_free(M32(v + 8));
        else if ((vt == VT_UNKNOWN || vt == VT_DISPATCH) && M32(v + 8)) VCALL0(M32(v + 8), 2);
    }
    memset(GPTR(v), 0, 16);
}
static uint32_t variant_copy(CPU* c, uint32_t dst, uint32_t src) {
    if (dst == src) return 0;
    uint8_t s[16];
    memcpy(s, GPTR(src), 16);
    uint32_t vt = s[0] | s[1] << 8, val = s[8] | s[9] << 8 | s[10] << 16 | (uint32_t)s[11] << 24;
    if (!(vt & VT_BYREF)) {
        if (vt & VT_ARRAY) { if (val && !(val = sa_copy(c, val))) return E_OUTOFMEMORY; }
        else if (vt == VT_BSTR) { if (val) val = bstr_alloc(val, bstr_len(val)); }
        else if ((vt == VT_UNKNOWN || vt == VT_DISPATCH) && val) VCALL0(val, 1);
    }
    variant_clear(c, dst);
    memcpy(GPTR(dst), s, 16);
    if (!(vt & VT_BYREF) && (vt & VT_ARRAY || vt == VT_BSTR)) M32(dst + 8) = val;
    return 0;
}
static void o_VariantInit(CPU* c) { memset(GPTR(ARG(0)), 0, 16); }
static void o_VariantClear(CPU* c) { variant_clear(c, ARG(0)); RET(0); }
static void o_VariantCopy(CPU* c) { RET(variant_copy(c, ARG(0), ARG(1))); }

/* ---- VariantChangeType ---- */

/* DATE: days since 1899-12-30, the fraction is the time of day (for negative dates too). */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}
#define DATE_EPOCH (-25569)   /* 1899-12-30 relative to 1970-01-01 */

static void date_format(double date, char* out, size_t n) {
    double day = date < 0 ? ceil(date) : floor(date), frac = fabs(date - day);
    int64_t secs = (int64_t)floor(frac * 86400.0 + 0.5);
    if (secs >= 86400) secs = 86399;
    int y, m, d, h = (int)(secs / 3600), mi = (int)(secs / 60 % 60), s = (int)(secs % 60);
    civil_from_days((int64_t)day + DATE_EPOCH, &y, &m, &d);
    char t[32];
    snprintf(t, sizeof t, "%d:%02d:%02d %s", h % 12 ? h % 12 : 12, mi, s, h < 12 ? "AM" : "PM");
    if (!day && secs) snprintf(out, n, "%s", t);
    else if (!secs) snprintf(out, n, "%d/%d/%d", m, d, y);
    else snprintf(out, n, "%d/%d/%d %s", m, d, y, t);
}
/* M/D/YYYY, h:mm[:ss] [AM|PM], or both, as the en-US short formats. */
static int date_parse(const char* s, double* out) {
    int m = 0, d = 0, y = 0, h = 0, mi = 0, sec = 0, has_date = 0, has_time = 0, n = 0;
    while (*s == ' ') s++;
    if (sscanf(s, "%d/%d/%d%n", &m, &d, &y, &n) == 3) { has_date = 1; s += n; }
    else if (sscanf(s, "%d-%d-%d%n", &y, &m, &d, &n) == 3) { has_date = 1; s += n; }
    while (*s == ' ' || *s == 'T') s++;
    n = 0;
    if (sscanf(s, "%d:%d:%d%n", &h, &mi, &sec, &n) == 3 || (n = 0, sscanf(s, "%d:%d%n", &h, &mi, &n) == 2)) {
        has_time = 1;
        s += n;
        while (*s == ' ') s++;
        if (!strncasecmp(s, "PM", 2)) { if (h < 12) h += 12; s += 2; }
        else if (!strncasecmp(s, "AM", 2)) { if (h == 12) h = 0; s += 2; }
    }
    while (*s == ' ') s++;
    if (*s || (!has_date && !has_time)) return 0;
    if (has_date) {
        if (y < 100) y += y < 30 ? 2000 : 1900;
        if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    }
    if (h > 23 || mi > 59 || sec > 59) return 0;
    double day = has_date ? (double)(days_from_civil(y, m, d) - DATE_EPOCH) : 0, t = (h * 3600 + mi * 60 + sec) / 86400.0;
    *out = day < 0 ? day - t : day + t;
    return 1;
}

/* Scalars travel as either an exact integer or a double. */
typedef struct { int is_int; int64_t i; double f; } Num;

static int num_from(uint32_t vt, const uint8_t* p, Num* n) {
    int64_t i = 0;
    n->is_int = 1;
    switch (vt) {
    case VT_EMPTY: i = 0; break;
    case VT_I1: i = (int8_t)p[0]; break;
    case VT_UI1: i = p[0]; break;
    case VT_I2: case VT_BOOL: i = (int16_t)(p[0] | p[1] << 8); break;
    case VT_UI2: i = p[0] | p[1] << 8; break;
    case VT_I4: case VT_INT: { int32_t v; memcpy(&v, p, 4); i = v; break; }
    case VT_UI4: case VT_UINT: { uint32_t v; memcpy(&v, p, 4); i = v; break; }
    case VT_I8: memcpy(&i, p, 8); break;
    case VT_UI8: { uint64_t v; memcpy(&v, p, 8); if (v > INT64_MAX) { n->is_int = 0; n->f = (double)v; return 1; } i = (int64_t)v; break; }
    case VT_R4: { float v; memcpy(&v, p, 4); n->is_int = 0; n->f = v; return 1; }
    case VT_R8: case VT_DATE: memcpy(&n->f, p, 8); n->is_int = 0; return 1;
    case VT_CY: memcpy(&i, p, 8); n->is_int = 0; n->f = (double)i / 10000.0; return 1;
    default: return 0;
    }
    n->i = i;
    return 1;
}
static double num_f(Num* n) { return n->is_int ? (double)n->i : n->f; }
/* Floating to integer conversion rounds half to even, as OLE does. */
static int num_to_int(Num* n, int64_t lo, int64_t hi, int64_t* out) {
    if (n->is_int) {
        if (n->i < lo || n->i > hi) return 0;
        *out = n->i;
        return 1;
    }
    double r = nearbyint(n->f);
    if (isnan(r) || r < (double)lo || r > (double)hi) return 0;
    *out = (int64_t)r;
    return 1;
}
static uint32_t num_parse(const char* s, Num* n) {
    char buf[128], *e;
    size_t k = 0;
    while (*s == ' ') s++;
    for (; *s && k + 1 < sizeof buf; s++)
        if (*s != ',') buf[k++] = *s;
    while (k && buf[k - 1] == ' ') k--;
    buf[k] = 0;
    if (!k) return DISP_E_TYPEMISMATCH;
    double f = strtod(buf, &e);
    if (*e) return DISP_E_TYPEMISMATCH;
    long long i = strtoll(buf, &e, 10);
    if (!*e) { n->is_int = 1; n->i = i; }
    else { n->is_int = 0; n->f = f; }
    return 0;
}

static void bstr_to_ascii(uint32_t b, char* out, size_t n) {
    uint32_t len = bstr_len(b), k = 0;
    for (; k < len && k + 1 < n; k++) {
        uint16_t ch = M16(b + 2 * k);
        out[k] = ch < 256 ? (char)ch : '?';
    }
    out[k] = 0;
}

/* Writes the converted value into r (a 16-byte VARIANT image); objects are AddRef'd and strings allocated anew. */
static uint32_t coerce(CPU* c, const uint8_t* s, uint32_t to, uint32_t flags, uint8_t* r) {
    uint32_t from = s[0] | s[1] << 8;
    const uint8_t* p = s + 8;
    uint32_t ptr;
    memcpy(&ptr, p, 4);
    memset(r, 0, 16);
    r[0] = (uint8_t)to; r[1] = (uint8_t)(to >> 8);
    if (to & (VT_BYREF | VT_ARRAY) || from & VT_ARRAY) {
        if (from != to || to & VT_BYREF) return DISP_E_TYPEMISMATCH;
        uint32_t copy = ptr ? sa_copy(c, ptr) : 0;
        memcpy(r + 8, &copy, 4);
        return ptr && !copy ? E_OUTOFMEMORY : 0;
    }
    if (to == VT_VARIANT || (!vt_size(to) && to != VT_EMPTY && to != VT_NULL)) return DISP_E_BADVARTYPE;
    if (to == VT_EMPTY) return 0;
    if (to == VT_NULL || from == VT_NULL) return to == from ? 0 : DISP_E_TYPEMISMATCH;
    if (to == VT_ERROR || from == VT_ERROR) {
        if (to != from) return DISP_E_TYPEMISMATCH;
        memcpy(r + 8, p, 4);
        return 0;
    }
    if (to == VT_UNKNOWN || to == VT_DISPATCH) {
        if (from != VT_UNKNOWN && from != VT_DISPATCH) return DISP_E_TYPEMISMATCH;
        uint32_t obj = 0;
        if (ptr) {
            if (from == to) { VCALL0(ptr, 1); obj = ptr; }
            else if (guid_in(c, ptr, to == VT_DISPATCH ? IID_IDispatch : IID_IUnknown, &obj)) return DISP_E_TYPEMISMATCH;
        }
        memcpy(r + 8, &obj, 4);
        return 0;
    }
    if (from == VT_UNKNOWN || from == VT_DISPATCH) return DISP_E_TYPEMISMATCH;

    Num n;
    if (to == VT_BSTR) {
        char t[64];
        if (from == VT_BSTR || from == VT_EMPTY) {
            uint32_t b = from == VT_BSTR && ptr ? bstr_alloc(ptr, bstr_len(ptr)) : bstr_alloc(0, 0);
            memcpy(r + 8, &b, 4);
            return 0;
        }
        if (from == VT_DECIMAL) {
            uint64_t lo; uint32_t hi;
            memcpy(&hi, s + 4, 4); memcpy(&lo, s + 8, 8);
            double v = ((double)hi * 18446744073709551616.0 + (double)lo) / pow(10, s[2]);
            snprintf(t, sizeof t, "%s%.*f", s[3] & 0x80 ? "-" : "", s[2], v);
        } else if (from == VT_DATE) {
            double v; memcpy(&v, p, 8);
            date_format(v, t, sizeof t);
        } else if (from == VT_BOOL && (flags & 2)) {
            snprintf(t, sizeof t, "%s", p[0] | p[1] ? "True" : "False");
        } else if (from == VT_CY) {
            int64_t v; memcpy(&v, p, 8);
            uint64_t a = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
            int len = snprintf(t, sizeof t, "%s%llu.%04u", v < 0 ? "-" : "", (unsigned long long)(a / 10000), (unsigned)(a % 10000));
            while (t[len - 1] == '0') t[--len] = 0;
            if (t[len - 1] == '.') t[--len] = 0;
        } else {
            if (!num_from(from, p, &n)) return DISP_E_BADVARTYPE;
            if (n.is_int) snprintf(t, sizeof t, "%lld", (long long)n.i);
            else snprintf(t, sizeof t, from == VT_R4 ? "%.7G" : "%.15G", n.f);
        }
        uint32_t b = bstr_from(t);
        memcpy(r + 8, &b, 4);
        return 0;
    }

    if (from == VT_BSTR) {
        char t[128];
        bstr_to_ascii(ptr, t, sizeof t);
        if (to == VT_BOOL && (!strcasecmp(t, "True") || !strcasecmp(t, "False"))) { n.is_int = 1; n.i = !strcasecmp(t, "True"); }
        else if (to == VT_DATE) {
            double d;
            if (!date_parse(t, &d)) return DISP_E_TYPEMISMATCH;
            memcpy(r + 8, &d, 8);
            return 0;
        } else if (num_parse(t, &n)) return DISP_E_TYPEMISMATCH;
    } else if (from == VT_DECIMAL) {
        uint64_t lo; uint32_t hi;
        memcpy(&hi, s + 4, 4); memcpy(&lo, s + 8, 8);
        n.is_int = !hi && !s[2] && lo <= INT64_MAX;
        if (n.is_int) n.i = s[3] & 0x80 ? -(int64_t)lo : (int64_t)lo;
        else n.f = ((double)hi * 18446744073709551616.0 + (double)lo) / pow(10, s[2]) * (s[3] & 0x80 ? -1 : 1);
    } else if (!num_from(from, p, &n)) return DISP_E_BADVARTYPE;

    int64_t i;
    switch (to) {
    case VT_I1: if (!num_to_int(&n, -128, 127, &i)) return DISP_E_OVERFLOW; r[8] = (uint8_t)i; return 0;
    case VT_UI1: if (!num_to_int(&n, 0, 255, &i)) return DISP_E_OVERFLOW; r[8] = (uint8_t)i; return 0;
    case VT_I2: if (!num_to_int(&n, -32768, 32767, &i)) return DISP_E_OVERFLOW; r[8] = (uint8_t)i; r[9] = (uint8_t)(i >> 8); return 0;
    case VT_UI2: if (!num_to_int(&n, 0, 65535, &i)) return DISP_E_OVERFLOW; r[8] = (uint8_t)i; r[9] = (uint8_t)(i >> 8); return 0;
    case VT_I4: case VT_INT: if (!num_to_int(&n, INT32_MIN, INT32_MAX, &i)) return DISP_E_OVERFLOW; { int32_t v = (int32_t)i; memcpy(r + 8, &v, 4); } return 0;
    case VT_UI4: case VT_UINT: if (!num_to_int(&n, 0, UINT32_MAX, &i)) return DISP_E_OVERFLOW; { uint32_t v = (uint32_t)i; memcpy(r + 8, &v, 4); } return 0;
    case VT_I8: if (!num_to_int(&n, INT64_MIN, INT64_MAX, &i)) return DISP_E_OVERFLOW; memcpy(r + 8, &i, 8); return 0;
    case VT_UI8: {
        uint64_t v;
        if (n.is_int) { if (n.i < 0) return DISP_E_OVERFLOW; v = (uint64_t)n.i; }
        else { double f = nearbyint(n.f); if (!(f >= 0 && f < 18446744073709551616.0)) return DISP_E_OVERFLOW; v = (uint64_t)f; }
        memcpy(r + 8, &v, 8);
        return 0;
    }
    case VT_BOOL: { int16_t v = (n.is_int ? n.i != 0 : n.f != 0) ? -1 : 0; memcpy(r + 8, &v, 2); return 0; }
    case VT_R4: {
        double f = num_f(&n);
        if (fabs(f) > 3.402823466e38) return DISP_E_OVERFLOW;
        float v = (float)f;
        memcpy(r + 8, &v, 4);
        return 0;
    }
    case VT_R8: { double v = num_f(&n); memcpy(r + 8, &v, 8); return 0; }
    case VT_DATE: {
        double v = num_f(&n);
        if (v < -657434.0 || v >= 2958466.0) return DISP_E_OVERFLOW;
        memcpy(r + 8, &v, 8);
        return 0;
    }
    case VT_CY: {
        double f = n.is_int ? 0 : nearbyint(n.f * 10000.0);
        if (n.is_int ? (n.i > INT64_MAX / 10000 || n.i < INT64_MIN / 10000) : !(fabs(f) < 9.2233720368547758e18)) return DISP_E_OVERFLOW;
        int64_t v = n.is_int ? n.i * 10000 : (int64_t)f;
        memcpy(r + 8, &v, 8);
        return 0;
    }
    case VT_DECIMAL: {
        uint8_t scale = 0, neg;
        uint64_t lo;
        if (n.is_int) { neg = n.i < 0; lo = neg ? 0 - (uint64_t)n.i : (uint64_t)n.i; }
        else {
            double f = fabs(n.f);
            neg = n.f < 0;
            if (!(f < 18446744073709551616.0)) return DISP_E_OVERFLOW;
            while (scale < 15 && f != floor(f) && f * 10 < 18446744073709551616.0) { f *= 10; scale++; }
            lo = (uint64_t)nearbyint(f);
        }
        memset(r, 0, 16);
        r[0] = VT_DECIMAL;
        r[2] = scale;
        r[3] = neg ? 0x80 : 0;
        memcpy(r + 8, &lo, 8);
        return 0;
    }
    }
    return DISP_E_BADVARTYPE;
}

/* An object converted to a scalar yields its default property (DISPID_VALUE) unless VARIANT_NOVALUEPROP. */
static uint32_t default_value(CPU* c, uint32_t disp, uint32_t out) {
    uint32_t g = guest_alloc(64, 16);
    memset(GPTR(g), 0, 64);
    uint32_t hr = guest_call(c, M32(M32(disp) + 24), 9, disp, 0, g, 0x400, 2, g + 16, out, 0, 0);
    guest_free(g);
    return hr;
}
static void o_VariantChangeType(CPU* c) {
    uint32_t dst = ARG(0), src = ARG(1), flags = ARG(2), to = ARG(3) & 0xffff;
    uint8_t s[16], r[16];
    memcpy(s, GPTR(src), 16);
    uint32_t from = s[0] | s[1] << 8;
    if (from & VT_BYREF) {
        uint32_t base = from & ~(uint32_t)VT_BYREF, ref;
        memcpy(&ref, s + 8, 4);
        if (base == VT_VARIANT) memcpy(s, GPTR(ref), 16);
        else if (base == VT_DECIMAL) { memcpy(s, GPTR(ref), 16); s[0] = VT_DECIMAL; s[1] = 0; }
        else {
            uint32_t size = base & VT_ARRAY ? 4 : vt_size(base);
            if (!size) { RET(DISP_E_BADVARTYPE); return; }
            memset(s + 8, 0, 8);
            memcpy(s + 8, GPTR(ref), size);
            s[0] = (uint8_t)base; s[1] = (uint8_t)(base >> 8);
        }
        from = s[0] | s[1] << 8;
    }
    uint32_t hr, tmp = 0;
    uint32_t obj;
    memcpy(&obj, s + 8, 4);
    if ((from == VT_DISPATCH || from == VT_UNKNOWN) && to != VT_DISPATCH && to != VT_UNKNOWN && to != VT_EMPTY &&
        !(flags & 1) && obj) {
        uint32_t disp = obj;
        if (from == VT_UNKNOWN && guid_in(c, obj, IID_IDispatch, &disp)) { RET(DISP_E_TYPEMISMATCH); return; }
        tmp = guest_alloc(16, 16);
        memset(GPTR(tmp), 0, 16);
        hr = default_value(c, disp, tmp);
        if (from == VT_UNKNOWN) VCALL0(disp, 2);
        if (hr) { guest_free(tmp); RET(hr); return; }
        memcpy(s, GPTR(tmp), 16);
    }
    hr = coerce(c, s, to, flags, r);
    if (tmp) { variant_clear(c, tmp); guest_free(tmp); }
    if (hr) { RET(hr); return; }
    variant_clear(c, dst);
    memcpy(GPTR(dst), r, 16);
    RET(0);
}

/* ---- drag and drop ---- */

/* No window in this process registers as a drop target, so the loop asks the source whether to go on, shows the
   no-drop cursor its feedback asks for, and ends with no effect. */
static void o_DoDragDrop(CPU* c) {
    uint32_t source = ARG(1), effect = ARG(3);
    if (!ARG(0) || !source || !effect) { RET(E_INVALIDARG); return; }
    uint32_t msg = guest_alloc(32, 16);
    uint32_t hwnd = guest_call(c, hle_trampoline("user32.dll", "GetActiveWindow"), 0);
    uint32_t no_drop = guest_call(c, hle_trampoline("user32.dll", "LoadCursorA"), 2, 0, 32648);
    uint32_t old_cursor = guest_call(c, hle_trampoline("user32.dll", "GetCursor"), 0);
    if (hwnd) guest_call(c, hle_trampoline("user32.dll", "SetCapture"), 1, hwnd);
    uint32_t hr = 0, escape = 0;
    while (!hr) {
        static const uint8_t keys[6][2] = {{0x01, 1}, {0x02, 2}, {0x10, 4}, {0x11, 8}, {0x04, 0x10}, {0x12, 0x20}};
        uint32_t state = 0;
        for (int k = 0; k < 6; k++)
            if (guest_call(c, hle_trampoline("user32.dll", "GetKeyState"), 1, keys[k][0]) & 0x8000) state |= keys[k][1];
        hr = VCALL(source, 3, 3, escape, state);
        if (hr) break;
        if (VCALL(source, 4, 2, 0) == DRAGDROP_S_USEDEFAULTCURSORS)
            guest_call(c, hle_trampoline("user32.dll", "SetCursor"), 1, no_drop);
        if ((int32_t)guest_call(c, hle_trampoline("user32.dll", "GetMessageA"), 4, msg, 0, 0, 0) <= 0) {
            guest_call(c, hle_trampoline("user32.dll", "PostQuitMessage"), 1, M32(msg + 8));
            hr = DRAGDROP_S_CANCEL;
            break;
        }
        uint32_t m = M32(msg + 4);
        if (m == 0x100 && M32(msg + 8) == 0x1b) escape = 1;   /* WM_KEYDOWN VK_ESCAPE */
        if ((m < 0x100 || m > 0x109) && (m < 0x200 || m > 0x20d)) {
            guest_call(c, hle_trampoline("user32.dll", "TranslateMessage"), 1, msg);
            guest_call(c, hle_trampoline("user32.dll", "DispatchMessageA"), 1, msg);
        }
    }
    if (hwnd) guest_call(c, hle_trampoline("user32.dll", "ReleaseCapture"), 0);
    guest_call(c, hle_trampoline("user32.dll", "SetCursor"), 1, old_cursor);
    guest_free(msg);
    M32(effect) = 0;
    RET(hr);
}

/* ---- registration ---- */

#define O(n, a) {"ole32.dll", #n, a, o_##n}
#define A(n, a) {"oleaut32.dll", #n, a, o_##n}
#define IS(n, a) {"ole32.dll", "IStream::" #n, a, st_##n}
static const Hle ole_table[] = {
    O(CoInitialize, 1), O(CoInitializeEx, 2), O(CoUninitialize, 0), O(OleUninitialize, 0), O(CoTaskMemAlloc, 1), O(CoTaskMemFree, 1),
    O(IsEqualGUID, 2), O(StringFromCLSID, 2), O(CoCreateInstance, 5), O(CoGetClassObject, 5), O(ProgIDFromCLSID, 2),
    O(OleSetMenuDescriptor, 5), O(IsAccelerator, 4), O(OleDraw, 4), O(CreateStreamOnHGlobal, 3), O(DoDragDrop, 4),
    A(GetActiveObject, 3), A(GetErrorInfo, 2), A(SysAllocStringLen, 2), A(SysFreeString, 1),
    A(SysReAllocStringLen, 3), A(SysStringLen, 1), A(VariantInit, 1), A(VariantClear, 1), A(VariantCopy, 2),
    A(VariantChangeType, 4), A(SafeArrayCreate, 3), A(SafeArrayGetLBound, 3), A(SafeArrayGetUBound, 3),
    A(SafeArrayPtrOfIndex, 3),
    IS(QueryInterface, 3), IS(AddRef, 1), IS(Release, 1), IS(Read, 4), IS(Write, 4), IS(Seek, 5), IS(SetSize, 3),
    IS(CopyTo, 6), IS(Commit, 2), IS(Revert, 1), IS(LockRegion, 6), IS(Stat, 3), IS(Clone, 2),
};

void hle_ole_register(void) {
    static const char* methods[14] = {"IStream::QueryInterface", "IStream::AddRef", "IStream::Release", "IStream::Read",
                                      "IStream::Write", "IStream::Seek", "IStream::SetSize", "IStream::CopyTo",
                                      "IStream::Commit", "IStream::Revert", "IStream::LockRegion", "IStream::LockRegion",
                                      "IStream::Stat", "IStream::Clone"};
    hle_register(ole_table, sizeof ole_table / sizeof ole_table[0]);
    stream_vtable = guest_alloc(4 * 14, 16);
    for (int k = 0; k < 14; k++) M32(stream_vtable + 4 * (uint32_t)k) = hle_trampoline("ole32.dll", methods[k]);
}
