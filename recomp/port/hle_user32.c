#include "port.h"
#include <ctype.h>
#include <stdlib.h>

#define RET(v) (c->eax = (uint32_t)(v))

/* ---- window classes and windows ---- */

typedef struct {
    char name[64];
    uint32_t wndproc, style, extra, wndextra, hinst, icon, cursor, brush, menu;
} WClass;
static WClass classes[128];
static int class_count;
static uint32_t atom_next = 0xC000;

typedef struct {
    int used;
    uint32_t wndproc, parent, style, exstyle, id, userdata, hinst, cls;
    int x, y, w, h, visible;
    char text[256];
    uint32_t extra[16];
} Win;
#define MAX_WIN 1024
#define HWND_BASE 0x00010000u
static Win wins[MAX_WIN];

static Win* win_get(uint32_t h) {
    uint32_t k = (h - HWND_BASE) / 4;
    if (h < HWND_BASE || k >= MAX_WIN || !wins[k].used) return 0;
    return &wins[k];
}

static WClass* class_find(const char* name) {
    for (int k = 0; k < class_count; k++)
        if (!strcasecmp(classes[k].name, name)) return &classes[k];
    return 0;
}

static const char* class_name(uint32_t p, char* tmp) {
    if (p < 0x10000) { sprintf(tmp, "#%u", p); return tmp; }
    return GSTR(p);
}

static uint32_t send_message(CPU* c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win* w = win_get(hwnd);
    if (!w || !w->wndproc) return 0;
    return guest_call(c, w->wndproc, 4, hwnd, msg, wp, lp);
}

static void u_RegisterClassA(CPU* c) {
    uint32_t wc = ARG(0);
    char tmp[16];
    WClass* k = &classes[class_count++];
    memset(k, 0, sizeof *k);
    k->style = M32(wc); k->wndproc = M32(wc + 4); k->extra = M32(wc + 8); k->wndextra = M32(wc + 12);
    k->hinst = M32(wc + 16); k->icon = M32(wc + 20); k->cursor = M32(wc + 24); k->brush = M32(wc + 28);
    k->menu = M32(wc + 32);
    snprintf(k->name, sizeof k->name, "%s", class_name(M32(wc + 36), tmp));
    RET(atom_next++);
}

static void u_GetClassInfoA(CPU* c) {
    char tmp[16];
    WClass* k = class_find(class_name(ARG(1), tmp));
    uint32_t wc = ARG(2);
    if (!k) { RET(0); return; }
    M32(wc) = k->style; M32(wc + 4) = k->wndproc; M32(wc + 8) = k->extra; M32(wc + 12) = k->wndextra;
    M32(wc + 16) = k->hinst; M32(wc + 20) = k->icon; M32(wc + 24) = k->cursor; M32(wc + 28) = k->brush;
    M32(wc + 32) = k->menu; M32(wc + 36) = ARG(1);
    RET(1);
}

static void u_UnregisterClassA(CPU* c) { RET(1); }

static void u_CreateWindowExA(CPU* c) {
    char tmp[16];
    uint32_t exstyle = ARG(0), cls = ARG(1), title = ARG(2), style = ARG(3);
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h = (int)ARG(7);
    uint32_t parent = ARG(8), menu = ARG(9), hinst = ARG(10), param = ARG(11);
    WClass* k = class_find(class_name(cls, tmp));
    int slot = -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (!wins[i].used) { slot = i; break; }
    if (slot < 0) port_fatal("out of windows");
    Win* win = &wins[slot];
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->wndproc = k ? k->wndproc : 0;
    win->parent = parent; win->style = style; win->exstyle = exstyle; win->id = menu; win->hinst = hinst;
    win->x = x == (int)0x80000000 ? 0 : x; win->y = y == (int)0x80000000 ? 0 : y;
    win->w = w == (int)0x80000000 ? 1024 : w; win->h = h == (int)0x80000000 ? 768 : h;
    if (title) snprintf(win->text, sizeof win->text, "%s", GSTR(title));
    uint32_t hwnd = HWND_BASE + (uint32_t)slot * 4;
    port_log("CreateWindowExA class=%s title=\"%s\" -> %08x", class_name(cls, tmp), win->text, hwnd);

    uint32_t cs = guest_alloc(48, 16);
    M32(cs) = param; M32(cs + 4) = hinst; M32(cs + 8) = menu; M32(cs + 12) = parent;
    M32(cs + 16) = (uint32_t)win->h; M32(cs + 20) = (uint32_t)win->w; M32(cs + 24) = (uint32_t)win->y; M32(cs + 28) = (uint32_t)win->x;
    M32(cs + 32) = style; M32(cs + 36) = title; M32(cs + 40) = cls; M32(cs + 44) = exstyle;
    if (!send_message(c, hwnd, 0x0081, 0, cs)) { port_log("  WM_NCCREATE refused"); }
    send_message(c, hwnd, 0x0001, 0, cs);
    guest_free(cs);
    if (style & 0x10000000u) win->visible = 1;
    RET(hwnd);
}

static void u_DestroyWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    if (w) { send_message(c, ARG(0), 0x0002, 0, 0); w->used = 0; }
    RET(1);
}

/* Windows reports moves and resizes as WM_WINDOWPOSCHANGING/CHANGED; DefWindowProc turns CHANGED into WM_MOVE + WM_SIZE.
   VCL (and NosTale's widget handler hooked on the main form) only learns its size this way. */
static void send_windowpos(CPU* c, uint32_t hwnd, uint32_t flags) {
    Win* w = win_get(hwnd);
    if (!w) return;
    uint32_t wp = guest_alloc(28, 16);
    M32(wp) = hwnd; M32(wp + 4) = 0; M32(wp + 8) = (uint32_t)w->x; M32(wp + 12) = (uint32_t)w->y;
    M32(wp + 16) = (uint32_t)w->w; M32(wp + 20) = (uint32_t)w->h; M32(wp + 24) = flags;
    send_message(c, hwnd, 0x0046, 0, wp);
    send_message(c, hwnd, 0x0047, 0, wp);
    guest_free(wp);
}

static void u_DefWindowProcA(CPU* c) {
    uint32_t hwnd = ARG(0), msg = ARG(1), lp = ARG(3);
    if (msg == 0x0047 && lp) {
        uint32_t x = M32(lp + 8), y = M32(lp + 12), cx = M32(lp + 16), cy = M32(lp + 20), f = M32(lp + 24);
        if (!(f & 2)) send_message(c, hwnd, 0x0003, 0, (y & 0xffff) << 16 | (x & 0xffff));
        if (!(f & 1)) send_message(c, hwnd, 0x0005, 0, (cy & 0xffff) << 16 | (cx & 0xffff));
        RET(0);
        return;
    }
    RET(msg == 0x0081 ? 1 : 0);
}

static void u_CallWindowProcA(CPU* c) { RET(guest_call(c, ARG(0), 4, ARG(1), ARG(2), ARG(3), ARG(4))); }

static uint32_t* win_long(Win* w, int32_t idx) {
    static uint32_t dummy;
    switch (idx) {
    case -4: return &w->wndproc;
    case -16: return &w->style;
    case -20: return &w->exstyle;
    case -12: return &w->id;
    case -21: return &w->userdata;
    case -6: return &w->hinst;
    case -8: return &w->parent;
    default: return idx >= 0 && idx < 64 ? &w->extra[idx / 4] : &dummy;
    }
}
static void u_SetWindowLongA(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    uint32_t* p = win_long(w, (int32_t)ARG(1)), old = *p;
    *p = ARG(2);
    RET(old);
}
static void u_GetWindowLongA(CPU* c) {
    Win* w = win_get(ARG(0));
    RET(w ? *win_long(w, (int32_t)ARG(1)) : 0);
}
static void u_SetClassLongA(CPU* c) { RET(0); }
static void u_GetClassLongA(CPU* c) { RET(0); }

static void u_SendMessageA(CPU* c) { RET(send_message(c, ARG(0), ARG(1), ARG(2), ARG(3))); }

typedef struct { uint32_t hwnd, msg, wp, lp; } QMsg;
static QMsg queue[1024];
static int q_head, q_tail;
static int quit_posted;

static void u_PostMessageA(CPU* c) {
    queue[q_tail] = (QMsg){ARG(0), ARG(1), ARG(2), ARG(3)};
    q_tail = (q_tail + 1) % 1024;
    RET(1);
}
static void u_PostQuitMessage(CPU* c) { quit_posted = 1; }

static int pop_msg(uint32_t m, int remove) {
    if (q_head == q_tail) return 0;
    QMsg* q = &queue[q_head];
    M32(m) = q->hwnd; M32(m + 4) = q->msg; M32(m + 8) = q->wp; M32(m + 12) = q->lp; M32(m + 16) = 0;
    if (remove) q_head = (q_head + 1) % 1024;
    return 1;
}

static uint32_t idle_peeks;
static void u_PeekMessageA(CPU* c) {
    int got = pop_msg(ARG(0), ARG(4) & 1);
    if (!got && ++idle_peeks % 10000 == 0) port_log("PeekMessage idle x%u", idle_peeks);
    RET(got);
}
static void u_GetMessageA(CPU* c) {
    if (quit_posted) { RET(0); return; }
    if (!pop_msg(ARG(0), 1)) port_fatal("GetMessage with an empty queue (nothing would wake the guest)");
    RET(1);
}
static void u_TranslateMessage(CPU* c) { RET(0); }
static void u_DispatchMessageA(CPU* c) {
    uint32_t m = ARG(0);
    RET(send_message(c, M32(m), M32(m + 4), M32(m + 8), M32(m + 12)));
}
static void u_WaitMessage(CPU* c) { RET(1); }
static void u_MsgWaitForMultipleObjects(CPU* c) { RET(0x102); }

static void u_ShowWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    int was = w ? w->visible : 0;
    if (w) w->visible = ARG(1) != 0;
    if (w) port_log("ShowWindow %08x \"%s\" %u (%dx%d)", ARG(0), w->text, ARG(1), w->w, w->h);
    if (w && ARG(1) && !was) {
        send_message(c, ARG(0), 0x0018, 1, 0);
        send_windowpos(c, ARG(0), 0x40);
    }
    RET(was);
}
static void u_IsWindowVisible(CPU* c) { Win* w = win_get(ARG(0)); RET(w && w->visible); }
static void u_IsWindow(CPU* c) { RET(win_get(ARG(0)) != 0); }
static void u_IsWindowEnabled(CPU* c) { RET(win_get(ARG(0)) != 0); }
static void u_EnableWindow(CPU* c) { RET(0); }
static void u_IsIconic(CPU* c) { RET(0); }
static void u_IsZoomed(CPU* c) { RET(0); }
static void u_GetParent(CPU* c) { Win* w = win_get(ARG(0)); RET(w ? w->parent : 0); }
static void u_SetParent(CPU* c) { Win* w = win_get(ARG(0)); uint32_t o = w ? w->parent : 0; if (w) w->parent = ARG(1); RET(o); }
static void u_SetWindowTextA(CPU* c) { Win* w = win_get(ARG(0)); if (w) snprintf(w->text, sizeof w->text, "%s", GSTR(ARG(1))); RET(1); }
static void u_GetWindowTextA(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w || !ARG(2)) { RET(0); return; }
    snprintf(GSTR(ARG(1)), ARG(2), "%s", w->text);
    RET(strlen(GSTR(ARG(1))));
}
static void rect_of(Win* w, uint32_t r, int client) {
    M32(r) = client ? 0 : (uint32_t)w->x; M32(r + 4) = client ? 0 : (uint32_t)w->y;
    M32(r + 8) = (uint32_t)(client ? w->w : w->x + w->w); M32(r + 12) = (uint32_t)(client ? w->h : w->y + w->h);
}
static void u_GetWindowRect(CPU* c) { Win* w = win_get(ARG(0)); if (w) rect_of(w, ARG(1), 0); RET(w != 0); }
static void u_GetClientRect(CPU* c) { Win* w = win_get(ARG(0)); if (w) rect_of(w, ARG(1), 1); RET(w != 0); }
static void u_SetWindowPos(CPU* c) {
    Win* w = win_get(ARG(0));
    uint32_t f = ARG(6);
    if (w && !(f & 2)) { w->x = (int)ARG(2); w->y = (int)ARG(3); }
    if (w && !(f & 1)) { w->w = (int)ARG(4); w->h = (int)ARG(5); }
    if (w && (f & 0x40)) w->visible = 1;
    if (w && (f & 0x80)) w->visible = 0;
    if (w && (f & 3) != 3) send_windowpos(c, ARG(0), f);
    RET(1);
}
static void u_MoveWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    if (w) { w->x = (int)ARG(1); w->y = (int)ARG(2); w->w = (int)ARG(3); w->h = (int)ARG(4); }
    if (w) send_windowpos(c, ARG(0), 0);
    RET(1);
}

/* ---- system, input, misc ---- */

static void u_GetSystemMetrics(CPU* c) {
    switch (ARG(0)) {
    case 0: RET(1920); break;
    case 1: RET(1080); break;
    case 4: RET(23); break;
    case 5: case 6: case 45: case 46: RET(1); break;
    case 2: case 3: case 20: case 21: RET(17); break;
    case 7: case 8: case 32: case 33: RET(4); break;
    case 11: case 12: RET(32); break;
    case 13: case 14: RET(32); break;
    case 15: RET(20); break;
    case 16: RET(1920); break;
    case 17: RET(1040); break;
    case 43: RET(3); break;
    case 49: case 50: RET(16); break;
    case 75: RET(1); break;
    case 80: RET(1); break;
    default: RET(0); break;
    }
}
static void u_SystemParametersInfoA(CPU* c) {
    if (ARG(2) && ARG(0) == 0x30) { M32(ARG(2)) = 0; M32(ARG(2) + 4) = 0; M32(ARG(2) + 8) = 1920; M32(ARG(2) + 12) = 1040; }
    else if (ARG(2) && ARG(1)) memset(GPTR(ARG(2)), 0, ARG(1) < 4096 ? ARG(1) : 4096);
    RET(1);
}
static void u_RegisterWindowMessageA(CPU* c) { RET(atom_next++); }
static void k_GlobalAddAtomA(CPU* c) { RET(atom_next++); }
static void k_GlobalDeleteAtom(CPU* c) { RET(0); }
static void k_GlobalFindAtomA(CPU* c) { RET(0); }
static void u_GetKeyboardLayout(CPU* c) { RET(0x04090409); }
static void u_GetKeyboardLayoutList(CPU* c) { if (ARG(0) && ARG(1)) M32(ARG(1)) = 0x04090409; RET(1); }
static void u_GetKeyboardState(CPU* c) { memset(GPTR(ARG(0)), 0, 256); RET(1); }
static void u_GetKeyState(CPU* c) { RET(0); }
static void u_GetAsyncKeyState(CPU* c) { RET(0); }
static void u_GetCursorPos(CPU* c) { M32(ARG(0)) = 512; M32(ARG(0) + 4) = 384; RET(1); }
static void u_SetCursorPos(CPU* c) { RET(1); }
static void u_ShowCursor(CPU* c) { RET(1); }
static void u_SetCursor(CPU* c) { RET(0); }
static void u_LoadIconA(CPU* c) { RET(0x00050000u + (ARG(1) & 0xffff)); }
static void u_LoadCursorA(CPU* c) { RET(0x00060000u + (ARG(1) & 0xffff)); }
static void u_GetDesktopWindow(CPU* c) { RET(0x00000010u); }
static void u_GetActiveWindow(CPU* c) { RET(0); }
static void u_GetForegroundWindow(CPU* c) { RET(0); }
static void u_SetActiveWindow(CPU* c) { RET(0); }
static void u_SetForegroundWindow(CPU* c) { RET(1); }
static void u_GetFocus(CPU* c) { RET(0); }
static void u_SetFocus(CPU* c) { RET(0); }
static void u_GetCapture(CPU* c) { RET(0); }
static void u_ReleaseCapture(CPU* c) { RET(1); }
static void u_GetSystemMenu(CPU* c) { RET(0x00070001u); }
static void u_DeleteMenu(CPU* c) { RET(1); }
static void u_EnableMenuItem(CPU* c) { RET(0); }
static void u_MessageBoxA(CPU* c) {
    port_log("MessageBoxA \"%s\": %s", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    RET(1);
}
static void u_MessageBeep(CPU* c) { RET(1); }
static void u_SetTimer(CPU* c) { RET(ARG(1) ? ARG(1) : 1); }
static void u_KillTimer(CPU* c) { RET(1); }
static void u_InvalidateRect(CPU* c) { RET(1); }
static void u_UpdateWindow(CPU* c) { RET(1); }
static void u_ValidateRect(CPU* c) { RET(1); }
static void u_ScreenToClient(CPU* c) { RET(1); }
static void u_ClientToScreen(CPU* c) { RET(1); }
static void u_WindowFromPoint(CPU* c) { RET(0); }
static void u_GetWindow(CPU* c) { RET(0); }
static void u_GetTopWindow(CPU* c) { RET(0); }
static void u_EnumWindows(CPU* c) { RET(1); }
static void u_EnumThreadWindows(CPU* c) { RET(1); }
static void u_GetWindowThreadProcessId(CPU* c) { if (ARG(1)) M32(ARG(1)) = 1234; RET(1); }
static void u_SetPropA(CPU* c) { RET(1); }
static void u_GetPropA(CPU* c) { RET(0); }
static void u_RemovePropA(CPU* c) { RET(0); }
static void u_FindWindowA(CPU* c) { RET(0); }
static void u_SetWindowsHookExA(CPU* c) { RET(0x00080001u); }
static void u_UnhookWindowsHookEx(CPU* c) { RET(1); }
static void u_CallNextHookEx(CPU* c) { RET(0); }
static void u_GetDoubleClickTime(CPU* c) { RET(500); }
static void u_GetCaretBlinkTime(CPU* c) { RET(530); }
static void u_GetSysColor(CPU* c) { RET(0x00c0c0c0u); }
static void u_GetSysColorBrush(CPU* c) { RET(0x00090000u + ARG(0)); }

static void u_CharNextA(CPU* c) { RET(M8(ARG(0)) ? ARG(0) + 1 : ARG(0)); }
static void u_CharPrevA(CPU* c) { RET(ARG(1) > ARG(0) ? ARG(1) - 1 : ARG(0)); }
static void u_CharLowerA(CPU* c) {
    uint32_t p = ARG(0);
    if (p < 0x10000) { RET(tolower((int)p)); return; }
    for (uint32_t k = p; M8(k); k++) M8(k) = (uint8_t)tolower(M8(k));
    RET(p);
}
static void u_CharUpperA(CPU* c) {
    uint32_t p = ARG(0);
    if (p < 0x10000) { RET(toupper((int)p)); return; }
    for (uint32_t k = p; M8(k); k++) M8(k) = (uint8_t)toupper(M8(k));
    RET(p);
}
static void u_CharLowerBuffA(CPU* c) { for (uint32_t k = 0; k < ARG(1); k++) M8(ARG(0) + k) = (uint8_t)tolower(M8(ARG(0) + k)); RET(ARG(1)); }
static void u_CharUpperBuffA(CPU* c) { for (uint32_t k = 0; k < ARG(1); k++) M8(ARG(0) + k) = (uint8_t)toupper(M8(ARG(0) + k)); RET(ARG(1)); }
static void u_OemToCharA(CPU* c) { if (ARG(0) != ARG(1)) strcpy(GSTR(ARG(1)), GSTR(ARG(0))); RET(1); }
static void u_CharToOemA(CPU* c) { if (ARG(0) != ARG(1)) strcpy(GSTR(ARG(1)), GSTR(ARG(0))); RET(1); }
static void u_IsCharAlphaA(CPU* c) { RET(isalpha((int)(ARG(0) & 0xff)) != 0); }
static void u_IsCharAlphaNumericA(CPU* c) { RET(isalnum((int)(ARG(0) & 0xff)) != 0); }
static void u_wsprintfA(CPU* c) {
    /* Only %s/%d/%u/%x/%c; enough for VCL's few uses. */
    char* out = GSTR(ARG(0));
    const char* f = GSTR(ARG(1));
    int a = 2;
    char* o = out;
    for (; *f; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        while (*f == '-' || *f == '0' || (*f >= '1' && *f <= '9') || *f == '.') f++;
        if (*f == 'l') f++;
        switch (*f) {
        case 's': o += sprintf(o, "%s", GSTR(ARG(a++))); break;
        case 'd': case 'i': o += sprintf(o, "%d", (int32_t)ARG(a++)); break;
        case 'u': o += sprintf(o, "%u", ARG(a++)); break;
        case 'x': o += sprintf(o, "%x", ARG(a++)); break;
        case 'X': o += sprintf(o, "%X", ARG(a++)); break;
        case 'c': *o++ = (char)ARG(a++); break;
        case '%': *o++ = '%'; break;
        }
    }
    *o = 0;
    RET(o - out);
}

/* ---- GDI: objects are handles only for now; nothing is drawn ---- */

static uint32_t gdi_next = 0x00200000u;
static void g_NewHandle(CPU* c) { RET(gdi_next += 4); }
static void g_True(CPU* c) { RET(1); }
static void g_Zero(CPU* c) { RET(0); }
static void g_GetStockObject(CPU* c) { RET(0x00100000u + ARG(0)); }
static void g_GetDeviceCaps(CPU* c) {
    switch (ARG(1)) {
    case 8: RET(1920); break;
    case 10: RET(1080); break;
    case 12: RET(32); break;
    case 14: RET(1); break;
    case 24: RET(-1); break;
    case 88: case 90: RET(96); break;
    case 104: RET(0); break;
    case 38: RET(0); break;
    default: RET(0); break;
    }
}
static void g_GetObjectA(CPU* c) { if (ARG(2)) memset(GPTR(ARG(2)), 0, ARG(1)); RET(ARG(1)); }
static void g_SelectObject(CPU* c) { RET(0x00100000u); }
static void g_GetTextMetricsA(CPU* c) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 56);
    M32(p) = 16; M32(p + 4) = 13; M32(p + 8) = 3; M32(p + 20) = 7; M32(p + 24) = 14;
    RET(1);
}
static void g_GetTextExtentPoint32A(CPU* c) { M32(ARG(3)) = 7 * ARG(2); M32(ARG(3) + 4) = 16; RET(1); }
static void g_GetDIBits(CPU* c) { RET(0); }
static void g_GetPaletteEntries(CPU* c) { if (ARG(3)) memset(GPTR(ARG(3)), 0, 4 * ARG(2)); RET(ARG(2)); }
static void g_GetSystemPaletteEntries(CPU* c) { if (ARG(3)) memset(GPTR(ARG(3)), 0, 4 * ARG(2)); RET(ARG(2)); }
static void g_GetNearestColor(CPU* c) { RET(ARG(1)); }
static void g_SetBkColor(CPU* c) { RET(0xffffff); }
static void g_SetTextColor(CPU* c) { RET(0); }
static void g_GetBkColor(CPU* c) { RET(0xffffff); }
static void u_GetDC(CPU* c) { RET(0x00300000u + (ARG(0) & 0xffff)); }
static void u_ReleaseDC(CPU* c) { RET(1); }
static void u_BeginPaint(CPU* c) { memset(GPTR(ARG(1)), 0, 64); M32(ARG(1)) = 0x00300001u; RET(0x00300001u); }
static void u_EndPaint(CPU* c) { RET(1); }
static void k_MulDiv(CPU* c) {
    int64_t r = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    int32_t d = (int32_t)ARG(2);
    RET(d ? (int32_t)((r + (r >= 0 ? d / 2 : -d / 2)) / d) : -1);
}

#define U(n, a) {"user32.dll", #n, a, u_##n}
#define G(n, a, f) {"gdi32.dll", #n, a, f}

static const Hle user32[] = {
    U(RegisterClassA, 1), U(GetClassInfoA, 3), U(UnregisterClassA, 2), U(CreateWindowExA, 12), U(DestroyWindow, 1),
    U(DefWindowProcA, 4), U(CallWindowProcA, 5), U(SetWindowLongA, 3), U(GetWindowLongA, 2), U(SetClassLongA, 3),
    U(GetClassLongA, 2), U(SendMessageA, 4), U(PostMessageA, 4), U(PostQuitMessage, 1), U(PeekMessageA, 5),
    U(GetMessageA, 4), U(TranslateMessage, 1), U(DispatchMessageA, 1), U(WaitMessage, 0),
    U(MsgWaitForMultipleObjects, 5), U(ShowWindow, 2), U(IsWindowVisible, 1), U(IsWindow, 1), U(IsWindowEnabled, 1),
    U(EnableWindow, 2), U(IsIconic, 1), U(IsZoomed, 1), U(GetParent, 1), U(SetParent, 2), U(SetWindowTextA, 2),
    U(GetWindowTextA, 3), U(GetWindowRect, 2), U(GetClientRect, 2), U(SetWindowPos, 7), U(MoveWindow, 6),
    U(GetSystemMetrics, 1), U(SystemParametersInfoA, 4), U(RegisterWindowMessageA, 1), U(GetKeyboardLayout, 1),
    U(GetKeyboardLayoutList, 2), U(GetKeyboardState, 1), U(GetKeyState, 1), U(GetAsyncKeyState, 1),
    U(GetCursorPos, 1), U(SetCursorPos, 2), U(ShowCursor, 1), U(SetCursor, 1), U(LoadIconA, 2), U(LoadCursorA, 2),
    U(GetDesktopWindow, 0), U(GetActiveWindow, 0), U(GetForegroundWindow, 0), U(SetActiveWindow, 1),
    U(SetForegroundWindow, 1), U(GetFocus, 0), U(SetFocus, 1), U(GetCapture, 0), U(ReleaseCapture, 0),
    U(GetSystemMenu, 2), U(DeleteMenu, 3), U(EnableMenuItem, 3), U(MessageBoxA, 4), U(MessageBeep, 1),
    U(SetTimer, 4), U(KillTimer, 2), U(InvalidateRect, 3), U(UpdateWindow, 1), U(ValidateRect, 2),
    U(ScreenToClient, 2), U(ClientToScreen, 2), U(WindowFromPoint, 2), U(GetWindow, 2), U(GetTopWindow, 1),
    U(EnumWindows, 2), U(EnumThreadWindows, 3), U(GetWindowThreadProcessId, 2), U(SetPropA, 3), U(GetPropA, 2),
    U(RemovePropA, 2), U(FindWindowA, 2), U(SetWindowsHookExA, 4), U(UnhookWindowsHookEx, 1),
    U(CallNextHookEx, 4), U(GetDoubleClickTime, 0), U(GetCaretBlinkTime, 0), U(GetSysColor, 1),
    U(GetSysColorBrush, 1), U(CharNextA, 1), U(CharPrevA, 2), U(CharLowerA, 1), U(CharUpperA, 1),
    U(CharLowerBuffA, 2), U(CharUpperBuffA, 2), U(OemToCharA, 2), U(CharToOemA, 2), U(IsCharAlphaA, 1),
    U(IsCharAlphaNumericA, 1), {"user32.dll", "wsprintfA", CDECL_ARGS, u_wsprintfA}, U(GetDC, 1), U(ReleaseDC, 2),
    U(BeginPaint, 2), U(EndPaint, 2),
    {"kernel32.dll", "GlobalAddAtomA", 1, k_GlobalAddAtomA}, {"kernel32.dll", "GlobalDeleteAtom", 1, k_GlobalDeleteAtom},
    {"kernel32.dll", "GlobalFindAtomA", 1, k_GlobalFindAtomA}, {"kernel32.dll", "MulDiv", 3, k_MulDiv},
    G(GetStockObject, 1, g_GetStockObject), G(GetDeviceCaps, 2, g_GetDeviceCaps), G(GetObjectA, 3, g_GetObjectA),
    G(SelectObject, 2, g_SelectObject), G(DeleteObject, 1, g_True), G(CreatePalette, 1, g_NewHandle),
    G(CreateFontIndirectA, 1, g_NewHandle), G(CreateSolidBrush, 1, g_NewHandle), G(CreatePen, 3, g_NewHandle),
    G(CreateBrushIndirect, 1, g_NewHandle), G(CreateCompatibleDC, 1, g_NewHandle),
    G(CreateCompatibleBitmap, 3, g_NewHandle), G(CreateDIBSection, 6, g_NewHandle), G(CreateBitmap, 5, g_NewHandle),
    G(DeleteDC, 1, g_True), G(SelectPalette, 3, g_Zero), G(RealizePalette, 1, g_Zero),
    G(GetTextMetricsA, 2, g_GetTextMetricsA), G(GetTextExtentPoint32A, 4, g_GetTextExtentPoint32A),
    G(GetDIBits, 7, g_GetDIBits), G(GetPaletteEntries, 4, g_GetPaletteEntries),
    G(GetSystemPaletteEntries, 4, g_GetSystemPaletteEntries), G(GetNearestColor, 2, g_GetNearestColor),
    G(SetBkColor, 2, g_SetBkColor), G(SetTextColor, 2, g_SetTextColor), G(GetBkColor, 1, g_GetBkColor),
    G(SetBkMode, 2, g_True), G(SetStretchBltMode, 2, g_True), G(SetMapMode, 2, g_True), G(SetROP2, 2, g_True),
    G(SetTextAlign, 2, g_Zero), G(BitBlt, 9, g_True), G(StretchBlt, 11, g_True), G(PatBlt, 6, g_True),
    G(TextOutA, 5, g_True), G(ExtTextOutA, 8, g_True), G(Rectangle, 5, g_True), G(FillRect, 3, g_True),
    G(MoveToEx, 4, g_True), G(LineTo, 3, g_True), G(SetWindowOrgEx, 4, g_True), G(SetViewportOrgEx, 4, g_True),
    G(IntersectClipRect, 5, g_True), G(ExcludeClipRect, 5, g_True), G(SaveDC, 1, g_True), G(RestoreDC, 2, g_True),
    G(UnrealizeObject, 1, g_True), G(GetClipBox, 2, g_True), G(SetDIBColorTable, 4, g_Zero),
    G(GetDIBColorTable, 4, g_Zero), G(GdiFlush, 0, g_True),
};

void hle_window_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h) {
    Win* win = win_get(hwnd);
    *w = win ? (uint32_t)win->w : 0;
    *h = win ? (uint32_t)win->h : 0;
}

void hle_user32_register(void) { hle_register(user32, sizeof user32 / sizeof user32[0]); }
