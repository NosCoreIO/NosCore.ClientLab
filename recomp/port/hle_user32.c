#include "port.h"
#include "render.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <wctype.h>

#define RET(v) (c->eax = (uint32_t)(v))

enum {
    WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_MOVE = 0x03, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
    WM_KILLFOCUS = 0x08, WM_ENABLE = 0x0A, WM_SETTEXT = 0x0C, WM_GETTEXT = 0x0D, WM_GETTEXTLENGTH = 0x0E,
    WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_QUERYENDSESSION = 0x11, WM_QUIT = 0x12, WM_QUERYOPEN = 0x13,
    WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_CANCELMODE = 0x1F, WM_SETCURSOR = 0x20,
    WM_MOUSEACTIVATE = 0x21, WM_GETMINMAXINFO = 0x24, WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47,
    WM_CONTEXTMENU = 0x7B, WM_STYLECHANGING = 0x7C, WM_STYLECHANGED = 0x7D, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82,
    WM_NCCALCSIZE = 0x83, WM_NCHITTEST = 0x84, WM_NCPAINT = 0x85, WM_NCACTIVATE = 0x86, WM_GETDLGCODE = 0x87,
    WM_NCMOUSEMOVE = 0xA0, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104,
    WM_SYSKEYUP = 0x105, WM_SYSCHAR = 0x106, WM_COMMAND = 0x111, WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113,
    WM_CTLCOLORMSGBOX = 0x132, WM_CTLCOLORSTATIC = 0x138, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201,
    WM_MOUSEWHEEL = 0x20A, WM_PARENTNOTIFY = 0x210, WM_CAPTURECHANGED = 0x215,
};
enum {
    WS_POPUP = 0x80000000u, WS_CHILD = 0x40000000u, WS_MINIMIZE = 0x20000000u, WS_VISIBLE = 0x10000000u,
    WS_DISABLED = 0x08000000u, WS_MAXIMIZE = 0x01000000u, WS_CAPTION = 0x00C00000u, WS_BORDER = 0x00800000u,
    WS_DLGFRAME = 0x00400000u, WS_VSCROLL = 0x00200000u, WS_HSCROLL = 0x00100000u, WS_THICKFRAME = 0x00040000u,
    WS_TABSTOP = 0x00010000u, WS_CLIPCHILDREN = 0x02000000u,
    WS_EX_DLGMODALFRAME = 0x1, WS_EX_NOPARENTNOTIFY = 0x4, WS_EX_TOPMOST = 0x8, WS_EX_TOOLWINDOW = 0x80,
    WS_EX_CLIENTEDGE = 0x200, WS_EX_STATICEDGE = 0x20000, WS_EX_APPWINDOW = 0x40000,
};
enum {
    SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOZORDER = 0x4, SWP_NOREDRAW = 0x8, SWP_NOACTIVATE = 0x10,
    SWP_FRAMECHANGED = 0x20, SWP_SHOWWINDOW = 0x40, SWP_HIDEWINDOW = 0x80, SWP_NOOWNERZORDER = 0x200,
};
enum { HTERROR = -2, HTTRANSPARENT = -1, HTNOWHERE = 0, HTCLIENT = 1, HTCAPTION = 2, HTMENU = 5, HTHSCROLL = 6,
       HTVSCROLL = 7, HTBORDER = 18 };

static const char* class_label(uint32_t p, char* tmp);

/* ---- atoms: one table serves the global atoms, class names, window messages, clipboard formats and props ---- */

typedef struct { int refs; char name[256]; } Atom;
static Atom atoms[1024];

static int atom_is_int(const char* s, uint32_t* v) {
    if (s[0] != '#') return 0;
    char* end;
    unsigned long n = strtoul(s + 1, &end, 10);
    if (*end || !s[1] || n == 0 || n >= 0xC000) return 0;
    *v = (uint32_t)n;
    return 1;
}

uint32_t hle_atom_find(const char* name) {
    uint32_t v;
    if (atom_is_int(name, &v)) return v;
    for (int k = 0; k < 1024; k++)
        if (atoms[k].refs && !strcasecmp(atoms[k].name, name)) return 0xC000u + (uint32_t)k;
    return 0;
}

uint32_t hle_atom_add(const char* name) {
    uint32_t v = hle_atom_find(name);
    if (v >= 0xC000) { atoms[v - 0xC000].refs++; return v; }
    if (v) return v;
    if (!*name || strlen(name) > 255) { set_last_error(123); return 0; }
    for (int k = 0; k < 1024; k++)
        if (!atoms[k].refs) {
            atoms[k].refs = 1;
            snprintf(atoms[k].name, sizeof atoms[k].name, "%s", name);
            return 0xC000u + (uint32_t)k;
        }
    set_last_error(8);
    return 0;
}

static void atom_delete(uint32_t a) {
    if (a >= 0xC000 && a < 0xC000 + 1024 && atoms[a - 0xC000].refs) atoms[a - 0xC000].refs--;
}

static const char* atom_name(uint32_t a, char* tmp) {
    if (a >= 0xC000 && a < 0xC000 + 1024 && atoms[a - 0xC000].refs) return atoms[a - 0xC000].name;
    if (a && a < 0xC000) { sprintf(tmp, "#%u", a); return tmp; }
    return 0;
}

/* A name argument is either a string or an integer atom in the low word. */
static uint32_t atom_arg(uint32_t p, int add) {
    if (p < 0x10000) return p;
    return add ? hle_atom_add(GSTR(p)) : hle_atom_find(GSTR(p));
}

static void k_GlobalAddAtomA(CPU* c) { RET(atom_arg(ARG(0), 1)); }
static void k_GlobalFindAtomA(CPU* c) { RET(atom_arg(ARG(0), 0)); }
static void k_GlobalDeleteAtom(CPU* c) { atom_delete(ARG(0)); RET(0); }
static void k_GlobalGetAtomNameA(CPU* c) {
    char tmp[16];
    const char* n = atom_name(ARG(0), tmp);
    if (!n || !ARG(2)) { set_last_error(6); RET(0); return; }
    snprintf(GSTR(ARG(1)), ARG(2), "%s", n);
    RET(strlen(GSTR(ARG(1))));
}
static void u_RegisterWindowMessageA(CPU* c) { RET(ARG(0) ? hle_atom_add(GSTR(ARG(0))) : 0); }

/* ---- classes ---- */

typedef struct {
    int used;
    uint32_t atom, wndproc, style, cls_extra, wnd_extra, hinst, icon, cursor, brush, menu, icon_sm;
    uint8_t* extra;
} WClass;
static WClass classes[128];

static WClass* class_get(uint32_t name) {
    uint32_t a = name < 0x10000 ? name : hle_atom_find(GSTR(name));
    if (!a) return 0;
    for (int k = 0; k < 128; k++)
        if (classes[k].used && classes[k].atom == a) return &classes[k];
    return 0;
}

static const char* class_label(uint32_t p, char* tmp) {
    if (p < 0x10000) { sprintf(tmp, "#%u", p); return tmp; }
    return GSTR(p);
}

static void u_RegisterClassA(CPU* c) {
    uint32_t wc = ARG(0), name = M32(wc + 36);
    if (class_get(name)) { set_last_error(1410); RET(0); return; }
    WClass* k = 0;
    for (int i = 0; i < 128 && !k; i++)
        if (!classes[i].used) k = &classes[i];
    if (!k) { set_last_error(8); RET(0); return; }
    memset(k, 0, sizeof *k);
    k->atom = name < 0x10000 ? name : hle_atom_add(GSTR(name));
    if (!k->atom) { RET(0); return; }
    k->used = 1;
    k->style = M32(wc); k->wndproc = M32(wc + 4); k->cls_extra = M32(wc + 8); k->wnd_extra = M32(wc + 12);
    k->hinst = M32(wc + 16); k->icon = M32(wc + 20); k->cursor = M32(wc + 24); k->brush = M32(wc + 28);
    k->menu = M32(wc + 32);
    k->extra = calloc(1, k->cls_extra + 4);
    RET(k->atom);
}

static void u_GetClassInfoA(CPU* c) {
    WClass* k = class_get(ARG(1));
    uint32_t wc = ARG(2);
    if (!k) { set_last_error(1411); RET(0); return; }
    M32(wc) = k->style; M32(wc + 4) = k->wndproc; M32(wc + 8) = k->cls_extra; M32(wc + 12) = k->wnd_extra;
    M32(wc + 16) = k->hinst; M32(wc + 20) = k->icon; M32(wc + 24) = k->cursor; M32(wc + 28) = k->brush;
    M32(wc + 32) = k->menu; M32(wc + 36) = ARG(1);
    RET(k->atom);
}

/* ---- windows ---- */

typedef struct { uint32_t key, value; } Prop;
typedef struct { int32_t min, max, page, pos, track; uint32_t disabled; } Scroll;

typedef struct {
    int used;
    uint32_t wndproc, parent, owner, style, exstyle, id, userdata, hinst, menu, sysmenu, last_popup;
    WClass* cls;
    int x, y, w, h;                 /* window rectangle, in the parent's client coordinates (screen for top level) */
    int cl, ct, cr, cb;             /* client rectangle, relative to the window rectangle */
    int nx, ny, nw, nh;             /* restored rectangle while minimized or maximized */
    double z;
    char* text;
    uint8_t* extra;
    Prop* props;
    int nprops;
    int inval, erase, frame, internal;
    int il, it, ir, ib;             /* bounding box of the update region, client coordinates */
    Scroll sb[3];
} Win;

#define MAX_WIN 1024
#define HWND_BASE 0x00010000u
#define HWND_DESKTOP 0x00000010u
#define SCREEN_W 1920
#define SCREEN_H 1080
#define WORK_H 1040
static Win wins[MAX_WIN];
static int next_slot;
static double z_top, z_bottom;

static Win* win_get(uint32_t h) {
    uint32_t k = (h - HWND_BASE) / 4;
    if (h < HWND_BASE || (h & 3) || k >= MAX_WIN || !wins[k].used) return 0;
    return &wins[k];
}
static uint32_t win_handle(Win* w) { return HWND_BASE + (uint32_t)(w - wins) * 4; }
static int is_child(Win* w) { return (w->style & WS_CHILD) != 0; }

static int visible_chain(Win* w) {
    for (; w; w = is_child(w) ? win_get(w->parent) : 0)
        if (!(w->style & WS_VISIBLE)) return 0;
    return 1;
}

static int is_descendant(uint32_t parent, uint32_t hwnd) {
    Win* w = win_get(hwnd);
    for (int guard = 0; w && is_child(w) && guard < MAX_WIN; guard++) {
        if (w->parent == parent) return 1;
        w = win_get(w->parent);
    }
    return 0;
}

static uint32_t root_of(uint32_t hwnd) {
    Win* w = win_get(hwnd);
    while (w && is_child(w) && win_get(w->parent)) { hwnd = w->parent; w = win_get(hwnd); }
    return hwnd;
}

/* Screen position of the client area's top-left corner. */
static void client_origin(Win* w, int* x, int* y) {
    int ox = 0, oy = 0;
    for (int guard = 0; w && guard < 64; guard++) {
        ox += w->x + w->cl;
        oy += w->y + w->ct;
        w = is_child(w) ? win_get(w->parent) : 0;
    }
    *x = ox;
    *y = oy;
}
static void parent_origin(Win* w, int* x, int* y) {
    Win* p = is_child(w) ? win_get(w->parent) : 0;
    if (p) client_origin(p, x, y);
    else *x = *y = 0;
}

/* Siblings sharing a parent (0 for top-level windows), topmost first. */
static int siblings(uint32_t parent, Win** out) {
    int n = 0;
    for (int k = 0; k < MAX_WIN; k++) {
        Win* w = &wins[k];
        if (!w->used) continue;
        if (parent ? !(is_child(w) && w->parent == parent) : is_child(w)) continue;
        int at = n++;
        while (at > 0) {
            Win* o = out[at - 1];
            int wt = (w->exstyle & WS_EX_TOPMOST) != 0, ot = (o->exstyle & WS_EX_TOPMOST) != 0;
            if (wt < ot || (wt == ot && w->z <= o->z)) break;
            out[at] = o;
            at--;
        }
        out[at] = w;
    }
    return n;
}
static uint32_t sibling_key(Win* w) { return is_child(w) ? w->parent : 0; }

static void place_z(Win* w, uint32_t after) {
    static Win* list[MAX_WIN];
    if (after == 0) w->z = ++z_top;
    else if (after == 1) w->z = --z_bottom;
    else if (after == 0xFFFFFFFFu) { w->exstyle |= WS_EX_TOPMOST; w->z = ++z_top; }
    else if (after == 0xFFFFFFFEu) { w->exstyle &= ~(uint32_t)WS_EX_TOPMOST; w->z = ++z_top; }
    else {
        Win* a = win_get(after);
        if (!a || a == w) return;
        int n = siblings(sibling_key(w), list);
        double below = a->z - 2;
        for (int k = 0; k + 1 < n; k++)
            if (list[k] == a) { below = list[k + 1] == w ? (k + 2 < n ? list[k + 2]->z : a->z - 2) : list[k + 1]->z; break; }
        w->z = (a->z + below) / 2;
    }
}

static int metric(int i);

/* Thickness of the non-client area that the default window procedure adds for a style. */
static void nc_borders(uint32_t style, uint32_t exstyle, int menu, int* l, int* t, int* r, int* b) {
    int bx = 0, by = 0;
    if ((style & WS_THICKFRAME)) { bx = metric(32); by = metric(33); }
    else if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_DLGFRAME) || (exstyle & WS_EX_DLGMODALFRAME)) { bx = metric(7); by = metric(8); }
    else if (style & WS_BORDER) { bx = metric(5); by = metric(6); }
    if (exstyle & WS_EX_CLIENTEDGE) { bx += metric(45); by += metric(46); }
    if (exstyle & WS_EX_STATICEDGE) { bx += metric(5); by += metric(6); }
    *l = bx; *r = bx; *t = by; *b = by;
    if ((style & WS_CAPTION) == WS_CAPTION) *t += (exstyle & WS_EX_TOOLWINDOW) ? metric(51) : metric(4);
    if (menu && !(style & WS_CHILD)) *t += metric(15);
}

static uint32_t send_message(CPU* c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

/* ---- hooks ---- */

enum { WH_MSGFILTER = -1, WH_KEYBOARD = 2, WH_GETMESSAGE = 3, WH_CALLWNDPROC = 4, WH_CBT = 5, WH_SYSMSGFILTER = 6,
       WH_MOUSE = 7, WH_DEBUG = 9, WH_SHELL = 10, WH_FOREGROUNDIDLE = 11, WH_CALLWNDPROCRET = 12,
       WH_KEYBOARD_LL = 13, WH_MOUSE_LL = 14 };
typedef struct { int used, type; uint32_t proc, seq; } Hook;
static Hook hooks[64];
static uint32_t hook_seq;
#define HHOOK_BASE 0x00080000u

static Hook* next_hook(int type, uint32_t below_seq) {
    Hook* best = 0;
    for (int k = 0; k < 64; k++)
        if (hooks[k].used && hooks[k].type == type && hooks[k].seq < below_seq && (!best || hooks[k].seq > best->seq))
            best = &hooks[k];
    return best;
}

static uint32_t hook_invoke(CPU* c, Hook* h, int code, uint32_t wp, uint32_t lp);

static uint32_t call_hooks(CPU* c, int type, int code, uint32_t wp, uint32_t lp) {
    Hook* h = next_hook(type, UINT32_MAX);
    return h ? hook_invoke(c, h, code, wp, lp) : 0;
}

static uint32_t hook_invoke(CPU* c, Hook* h, int code, uint32_t wp, uint32_t lp) {
    if (h->type != WH_DEBUG && next_hook(WH_DEBUG, UINT32_MAX)) {
        uint32_t info = guest_alloc(20, 4);
        M32(info) = 1; M32(info + 4) = 1; M32(info + 8) = lp; M32(info + 12) = wp; M32(info + 16) = (uint32_t)code;
        uint32_t skip = call_hooks(c, WH_DEBUG, 0, (uint32_t)h->type, info);
        guest_free(info);
        if (skip) return 0;
    }
    return guest_call(c, h->proc, 3, (uint32_t)code, wp, lp);
}

static void u_SetWindowsHookExA(CPU* c) {
    int type = (int)ARG(0);
    if (type == 0 || type == 1) { set_last_error(5); RET(0); return; }   /* journal hooks need UI access */
    if (type < WH_MSGFILTER || type > WH_MOUSE_LL || !ARG(1)) { set_last_error(1426); RET(0); return; }
    for (int k = 0; k < 64; k++)
        if (!hooks[k].used) {
            hooks[k] = (Hook){1, type, ARG(1), ++hook_seq};
            RET(HHOOK_BASE + (uint32_t)k + 1);
            return;
        }
    set_last_error(8);
    RET(0);
}
static Hook* hook_get(uint32_t h) {
    uint32_t k = h - HHOOK_BASE - 1;
    return k < 64 && hooks[k].used ? &hooks[k] : 0;
}
static void u_UnhookWindowsHookEx(CPU* c) {
    Hook* h = hook_get(ARG(0));
    if (h) h->used = 0;
    else set_last_error(1404);
    RET(h != 0);
}
static void u_CallNextHookEx(CPU* c) {
    Hook* h = hook_get(ARG(0));
    Hook* n = h ? next_hook(h->type, h->seq) : 0;
    RET(n ? hook_invoke(c, n, (int)ARG(1), ARG(2), ARG(3)) : 0);
}

static uint32_t send_message(CPU* c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win* w = win_get(hwnd);
    if (!w || !w->wndproc) return 0;
    int hooked = next_hook(WH_CALLWNDPROC, UINT32_MAX) || next_hook(WH_CALLWNDPROCRET, UINT32_MAX);
    uint32_t cwp = 0;
    if (hooked) {
        cwp = guest_alloc(20, 4);
        M32(cwp) = lp; M32(cwp + 4) = wp; M32(cwp + 8) = msg; M32(cwp + 12) = hwnd;
        call_hooks(c, WH_CALLWNDPROC, 0, 1, cwp);
    }
    uint32_t r = guest_call(c, w->wndproc, 4, hwnd, msg, wp, lp);
    if (hooked) {
        M32(cwp) = r; M32(cwp + 4) = lp; M32(cwp + 8) = wp; M32(cwp + 12) = msg; M32(cwp + 16) = hwnd;
        call_hooks(c, WH_CALLWNDPROCRET, 0, 1, cwp);
        guest_free(cwp);
    }
    return r;
}
uint32_t hle_send(CPU* c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { return send_message(c, hwnd, msg, wp, lp); }

static uint32_t cbt(CPU* c, int code, uint32_t wp, uint32_t lp) { return call_hooks(c, WH_CBT, code, wp, lp); }

/* ---- the message queue ---- */

typedef struct { uint32_t hwnd, msg, wp, lp, time, ch; int px, py; } QMsg;
#define QCAP 4096
static QMsg queue[QCAP];
static int qn;
static int quit_posted;
static uint32_t quit_code;
static int cursor_x, cursor_y;
static uint32_t last_msg_time;
static int last_msg_x, last_msg_y;

static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (qn == QCAP) { port_log("message queue full, dropping %04x", msg); return; }
    queue[qn++] = (QMsg){hwnd, msg, wp, lp, (uint32_t)port_now_ms(), 0, cursor_x, cursor_y};
}
void hle_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { post(hwnd, msg, wp, lp); }

static void u_PostMessageA(CPU* c) {
    if (ARG(0) == 0xFFFF) {
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && !is_child(&wins[k])) post(win_handle(&wins[k]), ARG(1), ARG(2), ARG(3));
        RET(1);
        return;
    }
    if (ARG(0) && !win_get(ARG(0))) { set_last_error(1400); RET(0); return; }
    post(ARG(0), ARG(1), ARG(2), ARG(3));
    RET(1);
}
static void u_PostQuitMessage(CPU* c) { quit_posted = 1; quit_code = ARG(0); }

static void u_SendMessageA(CPU* c) {
    if (ARG(0) == 0xFFFF) {
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && !is_child(&wins[k])) send_message(c, win_handle(&wins[k]), ARG(1), ARG(2), ARG(3));
        RET(0);
        return;
    }
    if (!win_get(ARG(0))) { set_last_error(1400); RET(0); return; }
    RET(send_message(c, ARG(0), ARG(1), ARG(2), ARG(3)));
}

static void queue_remove(int k) {
    memmove(&queue[k], &queue[k + 1], sizeof(QMsg) * (size_t)(qn - k - 1));
    qn--;
}

static void purge_window(uint32_t hwnd) {
    int o = 0;
    for (int k = 0; k < qn; k++)
        if (queue[k].hwnd != hwnd) queue[o++] = queue[k];
    qn = o;
}

/* ---- timers: due ones are posted as WM_TIMER when the guest polls its queue ---- */

typedef struct { int used, pending; uint32_t hwnd, id, proc; double interval, next; } Timer;
static Timer timers[128];
static uint32_t timer_next_id = 0x7000;

static void pump_timers(void) {
    double now = port_now_ms();
    for (int k = 0; k < 128; k++) {
        Timer* t = &timers[k];
        if (!t->used || t->pending || now < t->next) continue;
        t->pending = 1;
        t->next = now + t->interval;
        post(t->hwnd, WM_TIMER, t->id, t->proc);
    }
}
static double next_timer_ms(void) {
    double soonest = 0;
    for (int k = 0; k < 128; k++)
        if (timers[k].used && !timers[k].pending && (!soonest || timers[k].next < soonest)) soonest = timers[k].next;
    return soonest;
}
static void timer_delivered(uint32_t hwnd, uint32_t id) {
    for (int k = 0; k < 128; k++)
        if (timers[k].used && timers[k].hwnd == hwnd && timers[k].id == id) timers[k].pending = 0;
}
static void u_SetTimer(CPU* c) {
    uint32_t hwnd = ARG(0), id = ARG(1);
    if (hwnd && !win_get(hwnd)) { set_last_error(1400); RET(0); return; }
    Timer* t = 0;
    for (int k = 0; k < 128 && hwnd; k++)
        if (timers[k].used && timers[k].hwnd == hwnd && timers[k].id == id) t = &timers[k];
    for (int k = 0; k < 128 && !t; k++)
        if (!timers[k].used) t = &timers[k];
    if (!t) { set_last_error(8); RET(0); return; }
    if (!hwnd) id = timer_next_id++;
    *t = (Timer){1, 0, hwnd, id, ARG(3), ARG(2) < 10 ? 10 : ARG(2), 0};
    t->next = port_now_ms() + t->interval;
    RET(id ? id : 1);
}
static void u_KillTimer(CPU* c) {
    int found = 0;
    for (int k = 0; k < 128; k++)
        if (timers[k].used && timers[k].hwnd == ARG(0) && timers[k].id == ARG(1)) timers[k].used = 0, found = 1;
    for (int k = qn - 1; k >= 0; k--)
        if (queue[k].msg == WM_TIMER && queue[k].hwnd == ARG(0) && queue[k].wp == ARG(1)) queue_remove(k);
    if (!found) set_last_error(1402);
    RET(found);
}

/* ---- painting: each window keeps the bounding box of its update region ---- */

static void invalidate(Win* w, int l, int t, int r, int b, int erase, int children);

static void client_size(Win* w, int* cw, int* ch) {
    *cw = w->cr - w->cl;
    *ch = w->cb - w->ct;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

static void invalidate(Win* w, int l, int t, int r, int b, int erase, int children) {
    int cw, ch;
    client_size(w, &cw, &ch);
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > cw) r = cw;
    if (b > ch) b = ch;
    if (r > l && b > t) {
        if (!w->inval) { w->il = l; w->it = t; w->ir = r; w->ib = b; }
        else {
            if (l < w->il) w->il = l;
            if (t < w->it) w->it = t;
            if (r > w->ir) w->ir = r;
            if (b > w->ib) w->ib = b;
        }
        w->inval = 1;
        if (erase) w->erase = 1;
    }
    if (!children) return;
    uint32_t hwnd = win_handle(w);
    for (int k = 0; k < MAX_WIN; k++) {
        Win* ch2 = &wins[k];
        if (!ch2->used || !is_child(ch2) || ch2->parent != hwnd || !(ch2->style & WS_VISIBLE)) continue;
        int cl = l - ch2->x - ch2->cl, ct = t - ch2->y - ch2->ct, cr = r - ch2->x - ch2->cl, cb = b - ch2->y - ch2->ct;
        invalidate(ch2, cl, ct, cr, cb, erase, 1);
    }
}

static void invalidate_all(Win* w, int erase) {
    int cw, ch;
    client_size(w, &cw, &ch);
    invalidate(w, 0, 0, cw, ch, erase, !(w->style & WS_CLIPCHILDREN));
}

static void validate(Win* w) { w->inval = w->erase = w->internal = 0; }

/* Exposes the area a window covered in its parent (or, for a top-level window, in the windows below it). */
static void expose_under(Win* w) {
    if (is_child(w)) {
        Win* p = win_get(w->parent);
        if (p) invalidate(p, w->x, w->y, w->x + w->w, w->y + w->h, 1, 1);
        return;
    }
    for (int k = 0; k < MAX_WIN; k++) {
        Win* o = &wins[k];
        if (!o->used || o == w || is_child(o) || !(o->style & WS_VISIBLE)) continue;
        int ox = o->x + o->cl, oy = o->y + o->ct;
        invalidate(o, w->x - ox, w->y - oy, w->x + w->w - ox, w->y + w->h - oy, 1, 1);
    }
}

static uint32_t api(const char* dll, const char* name) { return hle_trampoline(dll, name); }

uint32_t hle_window_begin_paint(CPU* c, uint32_t hwnd, uint32_t dc, uint32_t rect) {
    Win* w = win_get(hwnd);
    if (!w) { memset(GPTR(rect), 0, 16); return 0; }
    if (w->frame) { w->frame = 0; send_message(c, hwnd, WM_NCPAINT, 1, 0); }
    uint32_t erase = 0;
    if (w->erase) {
        w->erase = 0;
        erase = send_message(c, hwnd, WM_ERASEBKGND, dc, 0) == 0;
    }
    M32(rect) = (uint32_t)(w->inval ? w->il : 0); M32(rect + 4) = (uint32_t)(w->inval ? w->it : 0);
    M32(rect + 8) = (uint32_t)(w->inval ? w->ir : 0); M32(rect + 12) = (uint32_t)(w->inval ? w->ib : 0);
    validate(w);
    return erase;
}

static void paint_now(CPU* c, Win* w) {
    if (!(w->inval || w->internal) || !visible_chain(w)) return;
    send_message(c, win_handle(w), WM_PAINT, 0, 0);
}

static void u_InvalidateRect(CPU* c) {
    uint32_t r = ARG(1);
    if (!ARG(0)) {
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && !is_child(&wins[k])) invalidate_all(&wins[k], ARG(2) != 0), wins[k].frame = 1;
        RET(1);
        return;
    }
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    int children = !(w->style & WS_CLIPCHILDREN);
    if (r) invalidate(w, (int)M32(r), (int)M32(r + 4), (int)M32(r + 8), (int)M32(r + 12), ARG(2) != 0, children);
    else invalidate(w, 0, 0, INT_MAX / 2, INT_MAX / 2, ARG(2) != 0, children);
    RET(1);
}
static void u_ValidateRect(CPU* c) {
    Win* w = win_get(ARG(0));
    uint32_t r = ARG(1);
    if (!w) { RET(0); return; }
    if (!r || ((int)M32(r) <= w->il && (int)M32(r + 4) <= w->it && (int)M32(r + 8) >= w->ir && (int)M32(r + 12) >= w->ib))
        validate(w);
    RET(1);
}
static void u_GetUpdateRect(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    if (ARG(1)) {
        M32(ARG(1)) = (uint32_t)(w->inval ? w->il : 0); M32(ARG(1) + 4) = (uint32_t)(w->inval ? w->it : 0);
        M32(ARG(1) + 8) = (uint32_t)(w->inval ? w->ir : 0); M32(ARG(1) + 12) = (uint32_t)(w->inval ? w->ib : 0);
    }
    if (w->inval && w->erase && ARG(2)) {
        uint32_t dc = guest_call(c, api("user32.dll", "GetDC"), 1, ARG(0));
        w->erase = send_message(c, ARG(0), WM_ERASEBKGND, dc, 0) == 0;
        guest_call(c, api("user32.dll", "ReleaseDC"), 2, ARG(0), dc);
    }
    RET(w->inval);
}
static void u_UpdateWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    paint_now(c, w);
    RET(1);
}

enum { RDW_INVALIDATE = 1, RDW_INTERNALPAINT = 2, RDW_ERASE = 4, RDW_VALIDATE = 8, RDW_NOINTERNALPAINT = 0x10,
       RDW_NOERASE = 0x20, RDW_NOCHILDREN = 0x40, RDW_ALLCHILDREN = 0x80, RDW_UPDATENOW = 0x100,
       RDW_ERASENOW = 0x200, RDW_FRAME = 0x400 };

static void redraw(CPU* c, Win* w, int l, int t, int r, int b, uint32_t f) {
    int cw, ch;
    client_size(w, &cw, &ch);
    if (f & RDW_INVALIDATE) {
        invalidate(w, l, t, r, b, (f & RDW_ERASE) != 0, 0);
        if (f & RDW_FRAME) w->frame = 1;
    }
    if (f & RDW_INTERNALPAINT) w->internal = 1;
    if (f & RDW_VALIDATE) {
        if (l <= 0 && t <= 0 && r >= cw && b >= ch) validate(w);
        if (f & RDW_NOERASE) w->erase = 0;
    }
    if (f & RDW_NOINTERNALPAINT) w->internal = 0;
    int children = (f & RDW_ALLCHILDREN) || (!(f & RDW_NOCHILDREN) && !(w->style & WS_CLIPCHILDREN));
    if (children) {
        uint32_t hwnd = win_handle(w);
        for (int k = 0; k < MAX_WIN; k++) {
            Win* ch2 = &wins[k];
            if (!ch2->used || !is_child(ch2) || ch2->parent != hwnd || !(ch2->style & WS_VISIBLE)) continue;
            int ox = ch2->x + ch2->cl, oy = ch2->y + ch2->ct;
            if (r - ox <= 0 || b - oy <= 0 || l - ox >= ch2->cr - ch2->cl || t - oy >= ch2->cb - ch2->ct) continue;
            redraw(c, ch2, l - ox, t - oy, r - ox, b - oy, f & ~(uint32_t)(RDW_UPDATENOW | RDW_ERASENOW));
        }
    }
    if (f & RDW_ERASENOW && w->erase) {
        uint32_t hwnd = win_handle(w);
        uint32_t dc = guest_call(c, api("user32.dll", "GetDC"), 1, hwnd);
        w->erase = send_message(c, hwnd, WM_ERASEBKGND, dc, 0) == 0;
        guest_call(c, api("user32.dll", "ReleaseDC"), 2, hwnd, dc);
    }
    if (f & RDW_UPDATENOW) paint_now(c, w);
}

static void u_RedrawWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    uint32_t r = ARG(1), rgn = ARG(2), f = ARG(3);
    if (!w) { set_last_error(1400); RET(0); return; }
    int l = 0, t = 0, rr = INT_MAX / 2, b = INT_MAX / 2;
    if (r) { l = (int)M32(r); t = (int)M32(r + 4); rr = (int)M32(r + 8); b = (int)M32(r + 12); }
    else if (rgn) {
        uint32_t box = guest_alloc(16, 4);
        guest_call(c, api("gdi32.dll", "GetRgnBox"), 2, rgn, box);
        l = (int)M32(box); t = (int)M32(box + 4); rr = (int)M32(box + 8); b = (int)M32(box + 12);
        guest_free(box);
    }
    redraw(c, w, l, t, rr, b, f);
    RET(1);
}

/* ---- geometry ---- */

static void fill_windowpos(uint32_t wp, uint32_t hwnd, uint32_t after, Win* w, int x, int y, int cx, int cy, uint32_t f) {
    M32(wp) = hwnd; M32(wp + 4) = after; M32(wp + 8) = (uint32_t)x; M32(wp + 12) = (uint32_t)y;
    M32(wp + 16) = (uint32_t)cx; M32(wp + 20) = (uint32_t)cy; M32(wp + 24) = f;
    (void)w;
}

/* Lets the window (or the default procedure) size its client area: WM_NCCALCSIZE gets the window rectangle in
   parent coordinates and returns the client rectangle. */
static void calc_client(CPU* c, Win* w) {
    uint32_t r = guest_alloc(16, 4);
    M32(r) = (uint32_t)w->x; M32(r + 4) = (uint32_t)w->y;
    M32(r + 8) = (uint32_t)(w->x + w->w); M32(r + 12) = (uint32_t)(w->y + w->h);
    send_message(c, win_handle(w), WM_NCCALCSIZE, 0, r);
    w->cl = (int)M32(r) - w->x; w->ct = (int)M32(r + 4) - w->y;
    w->cr = (int)M32(r + 8) - w->x; w->cb = (int)M32(r + 12) - w->y;
    if (w->cr < w->cl) w->cr = w->cl;
    if (w->cb < w->ct) w->cb = w->ct;
    guest_free(r);
}

static uint32_t focus_hwnd, active_hwnd, capture_hwnd, display_hwnd;
static int app_active;
static uint32_t set_active(CPU* c, uint32_t hwnd, int mouse);
static uint32_t set_focus(CPU* c, uint32_t hwnd);

static void pick_display(void) {
    Win* cur = win_get(display_hwnd);
    if (cur && cur->style & WS_VISIBLE) return;
    display_hwnd = 0;
    int best = 0;
    for (int k = 0; k < MAX_WIN; k++) {
        Win* w = &wins[k];
        if (w->used && !is_child(w) && (w->style & WS_VISIBLE) && w->w * w->h > best) { best = w->w * w->h; display_hwnd = win_handle(w); }
    }
}
static void consider_display(Win* w) {
    Win* cur = win_get(display_hwnd);
    if (is_child(w) || !(w->style & WS_VISIBLE)) return;
    if (!cur || !(cur->style & WS_VISIBLE) || w->w * w->h > cur->w * cur->h) display_hwnd = win_handle(w);
}

/* SetWindowPos as Windows does it: WM_WINDOWPOSCHANGING may rewrite the request, then the client area is recomputed
   and WM_WINDOWPOSCHANGED reports the result (the default procedure turns it into WM_MOVE and WM_SIZE). */
static int set_window_pos(CPU* c, uint32_t hwnd, uint32_t after, int x, int y, int cx, int cy, uint32_t f) {
    Win* w = win_get(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    if (f & SWP_NOMOVE) { x = w->x; y = w->y; }
    if (f & SWP_NOSIZE) { cx = w->w; cy = w->h; }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    uint32_t wp = guest_alloc(28, 4);
    fill_windowpos(wp, hwnd, after, w, x, y, cx, cy, f);
    send_message(c, hwnd, WM_WINDOWPOSCHANGING, 0, wp);
    w = win_get(hwnd);
    if (!w) { guest_free(wp); return 0; }
    after = M32(wp + 4); x = (int)M32(wp + 8); y = (int)M32(wp + 12); cx = (int)M32(wp + 16); cy = (int)M32(wp + 20);
    f = M32(wp + 24);
    int was_visible = (w->style & WS_VISIBLE) != 0;
    int moved = x != w->x || y != w->y, sized = cx != w->w || cy != w->h;
    if (!moved) f |= SWP_NOMOVE;
    if (!sized) f |= SWP_NOSIZE;
    if (was_visible && (moved || sized || (f & SWP_HIDEWINDOW)) && !(f & SWP_NOREDRAW)) expose_under(w);
    int old_cw, old_ch;
    client_size(w, &old_cw, &old_ch);
    w->x = x; w->y = y; w->w = cx; w->h = cy;
    if (sized || moved || (f & SWP_FRAMECHANGED)) calc_client(c, w);
    if (!(f & SWP_NOZORDER)) place_z(w, after);
    if (f & SWP_SHOWWINDOW) w->style |= WS_VISIBLE;
    if (f & SWP_HIDEWINDOW) w->style &= ~(uint32_t)WS_VISIBLE;
    int cw, ch;
    client_size(w, &cw, &ch);
    if (!(f & SWP_NOREDRAW) && (w->style & WS_VISIBLE)) {
        if (!was_visible || (f & SWP_FRAMECHANGED)) { invalidate_all(w, 1); w->frame = 1; }
        else if (cw != old_cw || ch != old_ch) {
            uint32_t cs = w->cls ? w->cls->style : 0;
            if ((cs & 3) == 3 || ((cs & 1) && ch != old_ch) || ((cs & 2) && cw != old_cw)) invalidate_all(w, 1);
            else {
                if (cw > old_cw) invalidate(w, old_cw, 0, cw, ch, 1, 1);
                if (ch > old_ch) invalidate(w, 0, old_ch, cw, ch, 1, 1);
            }
        }
    }
    M32(wp + 8) = (uint32_t)x; M32(wp + 12) = (uint32_t)y; M32(wp + 16) = (uint32_t)cx; M32(wp + 20) = (uint32_t)cy;
    M32(wp + 24) = f;
    send_message(c, hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    guest_free(wp);
    w = win_get(hwnd);
    if (!w) return 1;
    if (f & SWP_SHOWWINDOW) consider_display(w);
    if (f & SWP_HIDEWINDOW) {
        if (is_descendant(hwnd, focus_hwnd) || focus_hwnd == hwnd) set_focus(c, is_child(w) ? w->parent : 0);
        if (active_hwnd == hwnd) set_active(c, w->owner, 0);
        pick_display();
    } else if (!is_child(w) && !(f & SWP_NOACTIVATE) && (w->style & WS_VISIBLE) && active_hwnd != hwnd) {
        set_active(c, hwnd, 0);
    }
    return 1;
}

static void u_SetWindowPos(CPU* c) {
    RET(set_window_pos(c, ARG(0), ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), ARG(6)));
}
static void u_MoveWindow(CPU* c) {
    RET(set_window_pos(c, ARG(0), 0, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4),
                       SWP_NOZORDER | SWP_NOACTIVATE | (ARG(5) ? 0 : SWP_NOREDRAW)));
}

static void minmax_info(CPU* c, Win* w, uint32_t mmi) {
    int l, t, r, b;
    nc_borders(w->style & ~(uint32_t)WS_CAPTION, w->exstyle, 0, &l, &t, &r, &b);
    memset(GPTR(mmi), 0, 40);
    M32(mmi + 8) = (uint32_t)(SCREEN_W + l + r); M32(mmi + 12) = (uint32_t)(WORK_H + t + b);
    M32(mmi + 16) = (uint32_t)-l; M32(mmi + 20) = (uint32_t)-t;
    M32(mmi + 24) = (uint32_t)metric(34); M32(mmi + 28) = (uint32_t)metric(35);
    M32(mmi + 32) = (uint32_t)metric(59); M32(mmi + 36) = (uint32_t)metric(60);
    send_message(c, win_handle(w), WM_GETMINMAXINFO, 0, mmi);
}

enum { SIZE_RESTORED = 0, SIZE_MINIMIZED = 1, SIZE_MAXIMIZED = 2 };

static uint32_t show_window(CPU* c, uint32_t hwnd, uint32_t cmd) {
    Win* w = win_get(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    int was = (w->style & WS_VISIBLE) != 0;
    uint32_t f = SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE;
    int activate = !is_child(w) && (cmd == 1 || cmd == 2 || cmd == 3 || cmd == 5 || cmd == 9 || cmd == 10);
    int x = w->x, y = w->y, cx = w->w, cy = w->h;
    int minimize = cmd == 2 || cmd == 6 || cmd == 7 || cmd == 11, maximize = cmd == 3;
    int restore = cmd == 1 || cmd == 9 || cmd == 10;
    if (cmd == 0) {
        if (!was) return 0;
        f |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
    } else {
        f |= SWP_SHOWWINDOW;
        if (!activate) f |= SWP_NOACTIVATE;
        if (cmd == 6 || cmd == 7) f |= SWP_NOACTIVATE;
    }
    if (minimize && !(w->style & WS_MINIMIZE)) {
        if (cbt(c, 1, hwnd, cmd)) return was;
        if (!(w->style & WS_MAXIMIZE)) { w->nx = w->x; w->ny = w->y; w->nw = w->w; w->nh = w->h; }
        w->style = (w->style | WS_MINIMIZE) & ~(uint32_t)WS_MAXIMIZE;
        x = -32000; y = -32000; cx = metric(57); cy = metric(58);
        f &= ~(uint32_t)(SWP_NOMOVE | SWP_NOSIZE);
        f |= SWP_FRAMECHANGED;
    } else if (maximize && !(w->style & WS_MAXIMIZE)) {
        if (cbt(c, 1, hwnd, cmd)) return was;
        if (!(w->style & WS_MINIMIZE)) { w->nx = w->x; w->ny = w->y; w->nw = w->w; w->nh = w->h; }
        uint32_t mmi = guest_alloc(40, 4);
        minmax_info(c, w, mmi);
        x = (int)M32(mmi + 16); y = (int)M32(mmi + 20); cx = (int)M32(mmi + 8); cy = (int)M32(mmi + 12);
        guest_free(mmi);
        w->style = (w->style | WS_MAXIMIZE) & ~(uint32_t)WS_MINIMIZE;
        f &= ~(uint32_t)(SWP_NOMOVE | SWP_NOSIZE);
        f |= SWP_FRAMECHANGED;
    } else if (restore && (w->style & (WS_MINIMIZE | WS_MAXIMIZE))) {
        if (cbt(c, 1, hwnd, cmd)) return was;
        w->style &= ~(uint32_t)(WS_MINIMIZE | WS_MAXIMIZE);
        x = w->nx; y = w->ny; cx = w->nw; cy = w->nh;
        f &= ~(uint32_t)(SWP_NOMOVE | SWP_NOSIZE);
        f |= SWP_FRAMECHANGED;
    }
    if ((cmd != 0) != was) send_message(c, hwnd, WM_SHOWWINDOW, cmd != 0, 0);
    if (!win_get(hwnd)) return was;
    if (activate) f &= ~(uint32_t)SWP_NOZORDER;
    set_window_pos(c, hwnd, 0, x, y, cx, cy, f);
    w = win_get(hwnd);
    if (w) port_log("ShowWindow %08x \"%s\" %u: %dx%d at %d,%d, client %dx%d", hwnd, w->text ? w->text : "", cmd, w->w, w->h,
                    w->x, w->y, w->cr - w->cl, w->cb - w->ct);
    if (w && cmd != 0 && is_child(w) && !(f & SWP_NOACTIVATE) && activate) set_focus(c, hwnd);
    return was;
}
static void u_ShowWindow(CPU* c) { RET(show_window(c, ARG(0), ARG(1))); }

static void u_ShowOwnedPopups(CPU* c) {
    for (int k = 0; k < MAX_WIN; k++) {
        Win* w = &wins[k];
        if (!w->used || is_child(w) || w->owner != ARG(0) || !(w->style & WS_POPUP)) continue;
        int vis = (w->style & WS_VISIBLE) != 0;
        if (vis == (ARG(1) != 0)) continue;
        send_message(c, win_handle(w), WM_SHOWWINDOW, ARG(1) != 0, ARG(1) ? 3 : 1);
        set_window_pos(c, win_handle(w), 0, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | (ARG(1) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    RET(1);
}

static void u_GetWindowPlacement(CPU* c) {
    Win* w = win_get(ARG(0));
    uint32_t p = ARG(1);
    if (!w) { set_last_error(1400); RET(0); return; }
    int normal = !(w->style & (WS_MINIMIZE | WS_MAXIMIZE));
    M32(p + 4) = 0;
    M32(p + 8) = (w->style & WS_MINIMIZE) ? 2 : (w->style & WS_MAXIMIZE) ? 3 : 1;
    M32(p + 12) = (uint32_t)-1; M32(p + 16) = (uint32_t)-1; M32(p + 20) = (uint32_t)-1; M32(p + 24) = (uint32_t)-1;
    M32(p + 28) = (uint32_t)(normal ? w->x : w->nx); M32(p + 32) = (uint32_t)(normal ? w->y : w->ny);
    M32(p + 36) = (uint32_t)(normal ? w->x + w->w : w->nx + w->nw); M32(p + 40) = (uint32_t)(normal ? w->y + w->h : w->ny + w->nh);
    RET(1);
}
static void u_SetWindowPlacement(CPU* c) {
    Win* w = win_get(ARG(0));
    uint32_t p = ARG(1);
    if (!w) { set_last_error(1400); RET(0); return; }
    int l = (int)M32(p + 28), t = (int)M32(p + 32), r = (int)M32(p + 36), b = (int)M32(p + 40);
    uint32_t cmd = M32(p + 8);
    if (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) { w->nx = l; w->ny = t; w->nw = r - l; w->nh = b - t; }
    else set_window_pos(c, ARG(0), 0, l, t, r - l, b - t, SWP_NOZORDER | SWP_NOACTIVATE);
    show_window(c, ARG(0), cmd);
    RET(1);
}

/* ---- creation and destruction ---- */

static uint32_t create_window(CPU* c, uint32_t exstyle, uint32_t cls_name, uint32_t title, uint32_t style, int x, int y,
                              int cx, int cy, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t param) {
    char tmp[16];
    WClass* k = class_get(cls_name);
    if (!k) {
        port_log("CreateWindowExA: class %s is not registered", class_label(cls_name, tmp));
        set_last_error(1407);
        return 0;
    }
    if ((style & WS_CHILD) && !win_get(parent)) { set_last_error(1406); return 0; }
    int slot = -1;
    for (int i = 0; i < MAX_WIN && slot < 0; i++) {
        int s = (next_slot + i) % MAX_WIN;
        if (!wins[s].used) slot = s;
    }
    if (slot < 0) { set_last_error(1158); return 0; }
    next_slot = slot + 1;
    Win* w = &wins[slot];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->cls = k;
    w->wndproc = k->wndproc;
    w->hinst = hinst;
    if (style & WS_CHILD) { w->parent = parent; w->id = menu; }
    else {
        w->owner = parent ? root_of(parent) : 0;
        w->menu = menu ? menu : 0;
        if (!(style & WS_POPUP)) style |= WS_CAPTION;
    }
    w->style = style & ~(uint32_t)WS_VISIBLE;
    w->exstyle = exstyle;
    w->extra = calloc(1, k->wnd_extra + 4);
    uint32_t hwnd = win_handle(w);

    int top = !(style & WS_CHILD);
    if (x == (int)0x80000000) {
        if (top) { x = 26 * (slot % 8); y = 26 * (slot % 8); } else { x = 0; y = 0; }
    }
    if (cx == (int)0x80000000) {
        if (top) { cx = SCREEN_W * 3 / 4; cy = WORK_H * 3 / 4; } else { cx = 0; cy = 0; }
    }
    w->x = x; w->y = y; w->w = cx; w->h = cy;
    w->nx = x; w->ny = y; w->nw = cx; w->nh = cy;
    w->z = top ? ++z_top : --z_bottom;
    w->sb[0].max = w->sb[1].max = 100;
    w->sb[2].max = 100;

    uint32_t cs = guest_alloc(48, 4);
    M32(cs) = param; M32(cs + 4) = hinst; M32(cs + 8) = menu; M32(cs + 12) = parent;
    M32(cs + 16) = (uint32_t)cy; M32(cs + 20) = (uint32_t)cx; M32(cs + 24) = (uint32_t)y; M32(cs + 28) = (uint32_t)x;
    M32(cs + 32) = style; M32(cs + 36) = title; M32(cs + 40) = cls_name; M32(cs + 44) = exstyle;
    uint32_t cbtc = guest_alloc(8, 4);
    M32(cbtc) = cs; M32(cbtc + 4) = 0;
    if (cbt(c, 3, hwnd, cbtc)) {
        guest_free(cbtc); guest_free(cs);
        free(w->extra);
        w->used = 0;
        set_last_error(1400);
        return 0;
    }
    guest_free(cbtc);
    if (top && ((style & WS_THICKFRAME) || !(style & (WS_POPUP | WS_CHILD)))) {
        uint32_t mmi = guest_alloc(40, 4);
        minmax_info(c, w, mmi);
        if (w->w > (int)M32(mmi + 32)) w->w = (int)M32(mmi + 32);
        if (w->h > (int)M32(mmi + 36)) w->h = (int)M32(mmi + 36);
        guest_free(mmi);
        M32(cs + 16) = (uint32_t)w->h; M32(cs + 20) = (uint32_t)w->w;
    }
    port_log("CreateWindowExA class=%s title=\"%s\" style=%08x ex=%08x %dx%d -> %08x", class_label(cls_name, tmp),
             title ? GSTR(title) : "", style, exstyle, w->w, w->h, hwnd);
    if (!send_message(c, hwnd, WM_NCCREATE, 0, cs)) {
        port_log("  WM_NCCREATE refused");
        guest_free(cs);
        if (win_get(hwnd)) { free(w->extra); free(w->text); w->used = 0; }
        set_last_error(1400);
        return 0;
    }
    calc_client(c, w);
    if ((int32_t)send_message(c, hwnd, WM_CREATE, 0, cs) == -1) {
        guest_free(cs);
        guest_call(c, api("user32.dll", "DestroyWindow"), 1, hwnd);
        return 0;
    }
    guest_free(cs);
    w = win_get(hwnd);
    if (!w) return 0;
    uint32_t wpos = guest_alloc(28, 4);
    fill_windowpos(wpos, hwnd, 0, w, w->x, w->y, w->w, w->h, SWP_NOZORDER | SWP_NOACTIVATE);
    send_message(c, hwnd, WM_WINDOWPOSCHANGED, 0, wpos);
    guest_free(wpos);
    w = win_get(hwnd);
    if (!w) return 0;
    if (is_child(w) && !(w->exstyle & WS_EX_NOPARENTNOTIFY)) {
        uint32_t p = w->parent;
        for (Win* pw = win_get(p); pw; pw = is_child(pw) ? win_get(pw->parent) : 0) {
            send_message(c, p, WM_PARENTNOTIFY, WM_CREATE | (w->id & 0xffff) << 16, hwnd);
            if (!is_child(pw) || (pw->exstyle & WS_EX_NOPARENTNOTIFY)) break;
            p = pw->parent;
        }
    }
    if (!is_child(w) && !w->owner) call_hooks(c, WH_SHELL, 1, hwnd, 0);
    if (style & WS_VISIBLE) {
        uint32_t cmd = (style & WS_MAXIMIZE) ? 3 : (style & WS_MINIMIZE) ? 6 : 5;
        if (style & WS_MAXIMIZE) w->style &= ~(uint32_t)WS_MAXIMIZE;
        if (style & WS_MINIMIZE) w->style &= ~(uint32_t)WS_MINIMIZE;
        show_window(c, hwnd, cmd);
    }
    return win_get(hwnd) ? hwnd : 0;
}

static void u_CreateWindowExA(CPU* c) {
    RET(create_window(c, ARG(0), ARG(1), ARG(2), ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), (int)ARG(7), ARG(8),
                      ARG(9), ARG(10), ARG(11)));
}

static void free_window(uint32_t hwnd) {
    Win* w = win_get(hwnd);
    if (!w) return;
    for (int k = 0; k < 128; k++)
        if (timers[k].used && timers[k].hwnd == hwnd) timers[k].used = 0;
    purge_window(hwnd);
    for (int k = 0; k < w->nprops; k++) atom_delete(w->props[k].key);
    free(w->props);
    free(w->text);
    free(w->extra);
    w->used = 0;
}

static void destroy_tree(CPU* c, uint32_t hwnd) {
    send_message(c, hwnd, WM_DESTROY, 0, 0);
    for (int k = 0; k < MAX_WIN; k++)
        if (wins[k].used && is_child(&wins[k]) && wins[k].parent == hwnd) destroy_tree(c, win_handle(&wins[k]));
}
static void ncdestroy_tree(CPU* c, uint32_t hwnd) {
    for (int k = 0; k < MAX_WIN; k++)
        if (wins[k].used && is_child(&wins[k]) && wins[k].parent == hwnd) ncdestroy_tree(c, win_handle(&wins[k]));
    send_message(c, hwnd, WM_NCDESTROY, 0, 0);
    free_window(hwnd);
}

static int destroy_window(CPU* c, uint32_t hwnd) {
    Win* w = win_get(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    if (cbt(c, 4, hwnd, 0)) return 0;
    for (int k = 0; k < MAX_WIN; k++)
        if (wins[k].used && !is_child(&wins[k]) && wins[k].owner == hwnd) destroy_window(c, win_handle(&wins[k]));
    w = win_get(hwnd);
    if (!w) return 1;
    if (is_child(w) && !(w->exstyle & WS_EX_NOPARENTNOTIFY))
        send_message(c, w->parent, WM_PARENTNOTIFY, WM_DESTROY | (w->id & 0xffff) << 16, hwnd);
    if (w->style & WS_VISIBLE)
        set_window_pos(c, hwnd, 0, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (capture_hwnd == hwnd || is_descendant(hwnd, capture_hwnd)) {
        uint32_t old = capture_hwnd;
        capture_hwnd = 0;
        send_message(c, old, WM_CAPTURECHANGED, 0, 0);
    }
    if (focus_hwnd == hwnd || is_descendant(hwnd, focus_hwnd)) {
        Win* fw = win_get(hwnd);
        set_focus(c, fw && is_child(fw) ? fw->parent : 0);
    }
    if (active_hwnd == hwnd) {
        Win* aw = win_get(hwnd);
        uint32_t next = aw ? aw->owner : 0;
        if (!next)
            for (int k = 0; k < MAX_WIN && !next; k++)
                if (wins[k].used && &wins[k] != aw && !is_child(&wins[k]) && (wins[k].style & WS_VISIBLE) &&
                    !(wins[k].style & WS_DISABLED))
                    next = win_handle(&wins[k]);
        set_active(c, next, 0);
    }
    w = win_get(hwnd);
    if (w && !is_child(w) && !w->owner) call_hooks(c, WH_SHELL, 2, hwnd, 0);
    destroy_tree(c, hwnd);
    ncdestroy_tree(c, hwnd);
    if (display_hwnd == hwnd) { display_hwnd = 0; pick_display(); }
    return 1;
}
static void u_DestroyWindow(CPU* c) { RET(destroy_window(c, ARG(0))); }

static void u_UnregisterClassA(CPU* c) {
    WClass* k = class_get(ARG(0));
    if (!k) { set_last_error(1411); RET(0); return; }
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].cls == k) { set_last_error(1412); RET(0); return; }
    atom_delete(k->atom);
    free(k->extra);
    k->used = 0;
    RET(1);
}

/* ---- focus, activation, capture ---- */

static uint32_t set_focus(CPU* c, uint32_t hwnd) {
    uint32_t old = focus_hwnd;
    if (hwnd == old) return old;
    if (hwnd && !win_get(hwnd)) { set_last_error(1400); return 0; }
    if (cbt(c, 9, hwnd, old)) return 0;
    if (hwnd) {
        uint32_t root = root_of(hwnd);
        if (root != active_hwnd) set_active(c, root, 0);
        /* Activating the window may already have focused it through WM_ACTIVATE. */
        if (focus_hwnd == hwnd) return old;
        old = focus_hwnd;
    }
    focus_hwnd = hwnd;
    if (old && win_get(old)) send_message(c, old, WM_KILLFOCUS, hwnd, 0);
    if (hwnd && focus_hwnd == hwnd) send_message(c, hwnd, WM_SETFOCUS, old, 0);
    return old;
}

static uint32_t set_active(CPU* c, uint32_t hwnd, int mouse) {
    uint32_t old = active_hwnd;
    if (hwnd) hwnd = root_of(hwnd);
    if (hwnd == old) return old;
    Win* w = win_get(hwnd);
    if (hwnd && (!w || (w->style & WS_DISABLED))) return 0;
    uint32_t cas = guest_alloc(8, 4);
    M32(cas) = (uint32_t)mouse; M32(cas + 4) = old;
    uint32_t refuse = hwnd ? cbt(c, 5, hwnd, cas) : 0;
    guest_free(cas);
    if (refuse) return 0;
    if (hwnd && !app_active) {
        app_active = 1;
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && !is_child(&wins[k])) send_message(c, win_handle(&wins[k]), WM_ACTIVATEAPP, 1, 1);
    }
    active_hwnd = hwnd;
    if (old && win_get(old)) {
        send_message(c, old, WM_NCACTIVATE, 0, 0);
        send_message(c, old, WM_ACTIVATE, 0 | (win_get(old)->style & WS_MINIMIZE ? 0x10000u : 0), hwnd);
    }
    if (!hwnd) {
        if (focus_hwnd) set_focus(c, 0);
        return old;
    }
    w = win_get(hwnd);
    if (!w) return old;
    place_z(w, (w->exstyle & WS_EX_TOPMOST) ? 0xFFFFFFFFu : 0);
    Win* owner = win_get(w->owner);
    if (owner) owner->last_popup = hwnd;
    call_hooks(c, WH_SHELL, 4, hwnd, 0);
    send_message(c, hwnd, WM_NCACTIVATE, 1, 0);
    send_message(c, hwnd, WM_ACTIVATE, (mouse ? 2u : 1u) | ((w->style & WS_MINIMIZE) ? 0x10000u : 0), old);
    if (focus_hwnd && root_of(focus_hwnd) != hwnd && active_hwnd == hwnd) set_focus(c, 0);
    return old;
}

static void u_SetFocus(CPU* c) { RET(set_focus(c, ARG(0))); }
static void u_GetFocus(CPU* c) { RET(focus_hwnd); }
static void u_SetActiveWindow(CPU* c) { RET(set_active(c, ARG(0), 0)); }
static void u_GetActiveWindow(CPU* c) { RET(active_hwnd); }
static void u_GetForegroundWindow(CPU* c) { RET(active_hwnd); }
static void u_SetForegroundWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    set_active(c, ARG(0), 0);
    RET(1);
}
static void u_GetLastActivePopup(CPU* c) {
    Win* w = win_get(ARG(0));
    Win* p = w ? win_get(w->last_popup) : 0;
    RET(p && (p->style & WS_VISIBLE) && !(p->style & WS_DISABLED) && p->owner == ARG(0) ? w->last_popup : ARG(0));
}

static void u_SetCapture(CPU* c) {
    uint32_t old = capture_hwnd;
    if (!win_get(ARG(0))) { RET(old); return; }
    capture_hwnd = ARG(0);
    if (old && old != capture_hwnd) send_message(c, old, WM_CAPTURECHANGED, 0, capture_hwnd);
    RET(old);
}
static void u_GetCapture(CPU* c) { RET(capture_hwnd); }
static void u_ReleaseCapture(CPU* c) {
    uint32_t old = capture_hwnd;
    capture_hwnd = 0;
    if (old) send_message(c, old, WM_CAPTURECHANGED, 0, 0);
    RET(1);
}

static void u_EnableWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    uint32_t was_disabled = (w->style & WS_DISABLED) != 0;
    if (ARG(1) && was_disabled) {
        w->style &= ~(uint32_t)WS_DISABLED;
        send_message(c, ARG(0), WM_ENABLE, 1, 0);
    } else if (!ARG(1) && !was_disabled) {
        send_message(c, ARG(0), WM_CANCELMODE, 0, 0);
        w = win_get(ARG(0));
        if (!w) { RET(0); return; }
        w->style |= WS_DISABLED;
        if (focus_hwnd == ARG(0) || is_descendant(ARG(0), focus_hwnd)) set_focus(c, 0);
        if (capture_hwnd == ARG(0)) { capture_hwnd = 0; send_message(c, ARG(0), WM_CAPTURECHANGED, 0, 0); }
        send_message(c, ARG(0), WM_ENABLE, 0, 0);
    }
    RET(was_disabled);
}
static void u_IsWindowEnabled(CPU* c) { Win* w = win_get(ARG(0)); RET(w && !(w->style & WS_DISABLED)); }

/* ---- window data ---- */

static void set_text(Win* w, const char* s) {
    free(w->text);
    w->text = s ? strdup(s) : 0;
}

static uint32_t* win_long(Win* w, int32_t idx) {
    switch (idx) {
    case -4: return &w->wndproc;
    case -6: return &w->hinst;
    case -8: return is_child(w) ? &w->parent : &w->owner;
    case -12: return &w->id;
    case -16: return &w->style;
    case -20: return &w->exstyle;
    case -21: return &w->userdata;
    default: return 0;
    }
}
static uint32_t set_window_long(CPU* c, uint32_t hwnd, int32_t idx, uint32_t v) {
    Win* w = win_get(hwnd);
    if (!w) { set_last_error(1400); return 0; }
    if (idx >= 0) {
        if ((uint32_t)idx + 4 > w->cls->wnd_extra) { set_last_error(1413); return 0; }
        uint32_t old;
        memcpy(&old, w->extra + idx, 4);
        memcpy(w->extra + idx, &v, 4);
        return old;
    }
    uint32_t* p = win_long(w, idx);
    if (!p) { set_last_error(1413); return 0; }
    uint32_t old = *p;
    if (idx == -16 || idx == -20) {
        uint32_t ss = guest_alloc(8, 4);
        M32(ss) = old; M32(ss + 4) = v;
        send_message(c, hwnd, WM_STYLECHANGING, (uint32_t)idx, ss);
        w = win_get(hwnd);
        if (!w) { guest_free(ss); return 0; }
        v = M32(ss + 4);
        p = win_long(w, idx);
        *p = v;
        send_message(c, hwnd, WM_STYLECHANGED, (uint32_t)idx, ss);
        guest_free(ss);
        return old;
    }
    *p = v;
    return old;
}
static void u_SetWindowLongA(CPU* c) { RET(set_window_long(c, ARG(0), (int32_t)ARG(1), ARG(2))); }
static void u_SetWindowLongW(CPU* c) { RET(set_window_long(c, ARG(0), (int32_t)ARG(1), ARG(2))); }
static void u_GetWindowLongA(CPU* c) {
    Win* w = win_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    if (!w) { set_last_error(1400); RET(0); return; }
    if (idx >= 0) {
        uint32_t v = 0;
        if ((uint32_t)idx + 4 > w->cls->wnd_extra) { set_last_error(1413); RET(0); return; }
        memcpy(&v, w->extra + idx, 4);
        RET(v);
        return;
    }
    uint32_t* p = win_long(w, idx);
    if (!p) { set_last_error(1413); RET(0); return; }
    RET(*p);
}

static uint32_t* class_long(WClass* k, int32_t idx) {
    switch (idx) {
    case -8: return &k->menu;
    case -10: return &k->brush;
    case -12: return &k->cursor;
    case -14: return &k->icon;
    case -16: return &k->hinst;
    case -18: return &k->wnd_extra;
    case -20: return &k->cls_extra;
    case -24: return &k->wndproc;
    case -26: return &k->style;
    case -32: return &k->atom;
    case -34: return &k->icon_sm;
    default: return 0;
    }
}
static void u_GetClassLongA(CPU* c) {
    Win* w = win_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    if (!w) { set_last_error(1400); RET(0); return; }
    if (idx >= 0) {
        uint32_t v = 0;
        if ((uint32_t)idx + 4 > w->cls->cls_extra) { set_last_error(1413); RET(0); return; }
        memcpy(&v, w->cls->extra + idx, 4);
        RET(v);
        return;
    }
    uint32_t* p = class_long(w->cls, idx);
    if (!p) { set_last_error(1413); RET(0); return; }
    RET(*p);
}
static void u_SetClassLongA(CPU* c) {
    Win* w = win_get(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    if (!w) { set_last_error(1400); RET(0); return; }
    if (idx >= 0) {
        uint32_t v = ARG(2), old = 0;
        if ((uint32_t)idx + 4 > w->cls->cls_extra) { set_last_error(1413); RET(0); return; }
        memcpy(&old, w->cls->extra + idx, 4);
        memcpy(w->cls->extra + idx, &v, 4);
        RET(old);
        return;
    }
    uint32_t* p = class_long(w->cls, idx);
    if (!p || idx == -32 || idx == -18 || idx == -20) { set_last_error(1413); RET(0); return; }
    uint32_t old = *p;
    *p = ARG(2);
    RET(old);
}
static void u_GetClassNameA(CPU* c) {
    Win* w = win_get(ARG(0));
    char tmp[16];
    const char* n = w ? atom_name(w->cls->atom, tmp) : 0;
    if (!n || !ARG(2)) { set_last_error(1400); RET(0); return; }
    snprintf(GSTR(ARG(1)), ARG(2), "%s", n);
    RET(strlen(GSTR(ARG(1))));
}

static void u_SetPropA(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    uint32_t key = ARG(1) < 0x10000 ? ARG(1) : hle_atom_find(GSTR(ARG(1)));
    for (int k = 0; key && k < w->nprops; k++)
        if (w->props[k].key == key) { w->props[k].value = ARG(2); RET(1); return; }
    if (ARG(1) >= 0x10000) key = hle_atom_add(GSTR(ARG(1)));
    else if (key >= 0xC000) atoms[key - 0xC000].refs++;
    if (!key) { RET(0); return; }
    w->props = realloc(w->props, sizeof(Prop) * (size_t)(w->nprops + 1));
    w->props[w->nprops++] = (Prop){key, ARG(2)};
    RET(1);
}
static Prop* prop_find(Win* w, uint32_t name) {
    uint32_t key = name < 0x10000 ? name : hle_atom_find(GSTR(name));
    for (int k = 0; key && k < w->nprops; k++)
        if (w->props[k].key == key) return &w->props[k];
    return 0;
}
static void u_GetPropA(CPU* c) {
    Win* w = win_get(ARG(0));
    Prop* p = w ? prop_find(w, ARG(1)) : 0;
    RET(p ? p->value : 0);
}
static void u_RemovePropA(CPU* c) {
    Win* w = win_get(ARG(0));
    Prop* p = w ? prop_find(w, ARG(1)) : 0;
    if (!p) { RET(0); return; }
    uint32_t v = p->value;
    atom_delete(p->key);
    *p = w->props[--w->nprops];
    RET(v);
}

static void u_SetWindowTextA(CPU* c) {
    if (!win_get(ARG(0))) { set_last_error(1400); RET(0); return; }
    RET(send_message(c, ARG(0), WM_SETTEXT, 0, ARG(1)) != 0);
}
static void u_GetWindowTextA(CPU* c) {
    if (!win_get(ARG(0)) || !ARG(2)) { RET(0); return; }
    M8(ARG(1)) = 0;
    RET(send_message(c, ARG(0), WM_GETTEXT, ARG(2), ARG(1)));
}
static void u_GetWindowTextLengthA(CPU* c) {
    RET(win_get(ARG(0)) ? send_message(c, ARG(0), WM_GETTEXTLENGTH, 0, 0) : 0);
}

static void u_GetParent(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    RET(is_child(w) ? w->parent : (w->style & WS_POPUP) ? w->owner : 0);
}
static void u_SetParent(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    uint32_t np = ARG(1) == HWND_DESKTOP ? 0 : ARG(1);
    if (np && (!win_get(np) || np == ARG(0) || is_descendant(ARG(0), np))) { set_last_error(1400); RET(0); return; }
    uint32_t old = is_child(w) ? w->parent : HWND_DESKTOP;
    int vis = (w->style & WS_VISIBLE) != 0;
    if (vis) set_window_pos(c, ARG(0), 0, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    w->parent = np;
    w->z = is_child(w) ? --z_bottom : ++z_top;
    if (vis) set_window_pos(c, ARG(0), 0, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RET(old);
}
static void u_IsChild(CPU* c) { RET(is_descendant(ARG(0), ARG(1))); }
static void u_IsWindow(CPU* c) { RET(win_get(ARG(0)) != 0); }
static void u_IsWindowVisible(CPU* c) { Win* w = win_get(ARG(0)); RET(w && visible_chain(w)); }
static void u_IsIconic(CPU* c) { Win* w = win_get(ARG(0)); RET(w && (w->style & WS_MINIMIZE)); }
static void u_IsZoomed(CPU* c) { Win* w = win_get(ARG(0)); RET(w && (w->style & WS_MAXIMIZE)); }

static void u_GetWindowRect(CPU* c) {
    uint32_t r = ARG(1);
    if (ARG(0) == HWND_DESKTOP) { M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = SCREEN_W; M32(r + 12) = SCREEN_H; RET(1); return; }
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    int ox, oy;
    parent_origin(w, &ox, &oy);
    M32(r) = (uint32_t)(ox + w->x); M32(r + 4) = (uint32_t)(oy + w->y);
    M32(r + 8) = (uint32_t)(ox + w->x + w->w); M32(r + 12) = (uint32_t)(oy + w->y + w->h);
    RET(1);
}
static void u_GetClientRect(CPU* c) {
    uint32_t r = ARG(1);
    if (ARG(0) == HWND_DESKTOP) { M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = SCREEN_W; M32(r + 12) = SCREEN_H; RET(1); return; }
    Win* w = win_get(ARG(0));
    if (!w) { set_last_error(1400); RET(0); return; }
    int cw, ch;
    client_size(w, &cw, &ch);
    M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = (uint32_t)cw; M32(r + 12) = (uint32_t)ch;
    RET(1);
}

static int origin_of(uint32_t hwnd, int* x, int* y) {
    Win* w = win_get(hwnd);
    if (w) { client_origin(w, x, y); return 1; }
    *x = *y = 0;
    return !hwnd || hwnd == HWND_DESKTOP;
}
static void u_ClientToScreen(CPU* c) {
    int x, y;
    if (!origin_of(ARG(0), &x, &y)) { RET(0); return; }
    M32(ARG(1)) += (uint32_t)x; M32(ARG(1) + 4) += (uint32_t)y;
    RET(1);
}
static void u_ScreenToClient(CPU* c) {
    int x, y;
    if (!origin_of(ARG(0), &x, &y)) { RET(0); return; }
    M32(ARG(1)) -= (uint32_t)x; M32(ARG(1) + 4) -= (uint32_t)y;
    RET(1);
}
static void u_MapWindowPoints(CPU* c) {
    int fx, fy, tx, ty;
    if (!origin_of(ARG(0), &fx, &fy) || !origin_of(ARG(1), &tx, &ty)) { set_last_error(1400); RET(0); return; }
    int dx = fx - tx, dy = fy - ty;
    for (uint32_t k = 0; k < ARG(3); k++) { M32(ARG(2) + k * 8) += (uint32_t)dx; M32(ARG(2) + k * 8 + 4) += (uint32_t)dy; }
    RET(((uint32_t)dy & 0xffff) << 16 | ((uint32_t)dx & 0xffff));
}

static void u_AdjustWindowRectEx(CPU* c) {
    uint32_t r = ARG(0);
    int l, t, rr, b;
    nc_borders(ARG(1), ARG(3), ARG(2) != 0, &l, &t, &rr, &b);
    M32(r) -= (uint32_t)l; M32(r + 4) -= (uint32_t)t; M32(r + 8) += (uint32_t)rr; M32(r + 12) += (uint32_t)b;
    RET(1);
}

/* Topmost visible window containing a screen point, descending into children. */
static uint32_t window_from_point(int x, int y, int skip_disabled) {
    static Win* list[MAX_WIN];
    uint32_t parent = 0, found = 0;
    for (;;) {
        int n = siblings(parent, list), px = 0, py = 0;
        Win* hit = 0;
        if (parent) client_origin(win_get(parent), &px, &py);
        for (int k = 0; k < n && !hit; k++) {
            Win* w = list[k];
            if (!(w->style & WS_VISIBLE)) continue;
            if (x >= px + w->x && x < px + w->x + w->w && y >= py + w->y && y < py + w->y + w->h) hit = w;
        }
        if (!hit) return found;
        if (skip_disabled && (hit->style & WS_DISABLED)) return found ? found : win_handle(hit);
        found = win_handle(hit);
        int cx, cy;
        client_origin(hit, &cx, &cy);
        if (x < cx || y < cy || x >= cx + hit->cr - hit->cl || y >= cy + hit->cb - hit->ct) return found;
        parent = found;
    }
}
static void u_WindowFromPoint(CPU* c) { RET(window_from_point((int)ARG(0), (int)ARG(1), 0)); }

enum { GW_HWNDFIRST, GW_HWNDLAST, GW_HWNDNEXT, GW_HWNDPREV, GW_OWNER, GW_CHILD, GW_ENABLEDPOPUP };
static uint32_t get_window(uint32_t hwnd, uint32_t cmd) {
    static Win* list[MAX_WIN];
    Win* w = win_get(hwnd);
    if (cmd == GW_CHILD) {
        int n = siblings(hwnd == HWND_DESKTOP ? 0 : hwnd, list);
        if (!w && hwnd != HWND_DESKTOP) return 0;
        return n ? win_handle(list[0]) : 0;
    }
    if (!w) return 0;
    if (cmd == GW_OWNER) return is_child(w) ? 0 : w->owner;
    if (cmd == GW_ENABLEDPOPUP) {
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && wins[k].owner == hwnd && (wins[k].style & WS_VISIBLE) && !(wins[k].style & WS_DISABLED))
                return win_handle(&wins[k]);
        return 0;
    }
    int n = siblings(sibling_key(w), list), at = 0;
    for (int k = 0; k < n; k++)
        if (list[k] == w) at = k;
    switch (cmd) {
    case GW_HWNDFIRST: return win_handle(list[0]);
    case GW_HWNDLAST: return win_handle(list[n - 1]);
    case GW_HWNDNEXT: return at + 1 < n ? win_handle(list[at + 1]) : 0;
    case GW_HWNDPREV: return at > 0 ? win_handle(list[at - 1]) : 0;
    }
    return 0;
}
static void u_GetWindow(CPU* c) { RET(get_window(ARG(0), ARG(1))); }
static void u_GetTopWindow(CPU* c) { RET(get_window(ARG(0) ? ARG(0) : HWND_DESKTOP, GW_CHILD)); }
static void u_GetDesktopWindow(CPU* c) { RET(HWND_DESKTOP); }

static void u_EnumWindows(CPU* c) {
    static Win* list[MAX_WIN];
    uint32_t h[MAX_WIN];
    int n = siblings(0, list);
    for (int k = 0; k < n; k++) h[k] = win_handle(list[k]);
    for (int k = 0; k < n; k++)
        if (win_get(h[k]) && !guest_call(c, ARG(0), 2, h[k], ARG(1))) break;
    RET(1);
}
static void u_EnumThreadWindows(CPU* c) {
    static Win* list[MAX_WIN];
    uint32_t h[MAX_WIN];
    int n = siblings(0, list);
    for (int k = 0; k < n; k++) h[k] = win_handle(list[k]);
    for (int k = 0; k < n; k++)
        if (win_get(h[k]) && !guest_call(c, ARG(1), 2, h[k], ARG(2))) break;
    RET(n > 0);
}
static void u_GetWindowThreadProcessId(CPU* c) {
    if (!win_get(ARG(0))) { RET(0); return; }
    if (ARG(1)) M32(ARG(1)) = 1234;
    RET(1);
}

static int window_matches(Win* w, uint32_t cls, uint32_t title) {
    if (cls) {
        WClass* k = class_get(cls);
        if (!k || w->cls != k) return 0;
    }
    if (title && strcmp(w->text ? w->text : "", GSTR(title))) return 0;
    return 1;
}
static void u_FindWindowA(CPU* c) {
    static Win* list[MAX_WIN];
    int n = siblings(0, list);
    for (int k = 0; k < n; k++)
        if (window_matches(list[k], ARG(0), ARG(1))) { RET(win_handle(list[k])); return; }
    set_last_error(1400);
    RET(0);
}
static void u_FindWindowExA(CPU* c) {
    static Win* list[MAX_WIN];
    uint32_t parent = ARG(0) == HWND_DESKTOP ? 0 : ARG(0);
    if (ARG(0) == 0xFFFFFFFDu) { RET(0); return; }   /* HWND_MESSAGE: no message-only windows exist */
    int n = siblings(parent, list), k = 0;
    if (ARG(1))
        for (; k < n; k++)
            if (win_handle(list[k]) == ARG(1)) { k++; break; }
    for (; k < n; k++)
        if (window_matches(list[k], ARG(2), ARG(3))) { RET(win_handle(list[k])); return; }
    RET(0);
}

/* ---- scroll bars: per-window state; frames are never composited, so they have no pixels of their own ---- */

enum { SB_HORZ, SB_VERT, SB_CTL, SB_BOTH };
static Scroll* scroll_get(uint32_t hwnd, uint32_t bar) {
    Win* w = win_get(hwnd);
    return w && bar <= SB_CTL ? &w->sb[bar] : 0;
}
static void show_bar(CPU* c, uint32_t hwnd, uint32_t bar, int show) {
    Win* w = win_get(hwnd);
    if (!w || bar == SB_CTL) return;
    uint32_t bits = bar == SB_BOTH ? WS_HSCROLL | WS_VSCROLL : bar == SB_HORZ ? WS_HSCROLL : WS_VSCROLL;
    uint32_t ns = show ? w->style | bits : w->style & ~bits;
    if (ns == w->style) return;
    w->style = ns;
    set_window_pos(c, hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
static int32_t scroll_set(CPU* c, uint32_t hwnd, uint32_t bar, uint32_t si) {
    Scroll* s = scroll_get(hwnd, bar);
    if (!s) return 0;
    uint32_t mask = M32(si + 4);
    if (mask & 1) { s->min = (int32_t)M32(si + 8); s->max = (int32_t)M32(si + 12); }
    if (mask & 2) s->page = (int32_t)M32(si + 16);
    if (mask & 4) s->pos = (int32_t)M32(si + 20);
    int32_t range = s->max - s->min + 1;
    if (s->page < 0) s->page = 0;
    if (s->page > range) s->page = range > 0 ? range : 0;
    int32_t hi = s->max - (s->page ? s->page - 1 : 0);
    if (s->pos > hi) s->pos = hi;
    if (s->pos < s->min) s->pos = s->min;
    if (mask & 3) {
        int needed = s->min < s->max && s->page <= s->max - s->min;
        if (!(mask & 8)) show_bar(c, hwnd, bar, needed);
        else { show_bar(c, hwnd, bar, 1); s->disabled = needed ? 0 : 3; }
    }
    return s->pos;
}
static void u_SetScrollInfo(CPU* c) { RET(scroll_set(c, ARG(0), ARG(1), ARG(2))); }
static void u_GetScrollInfo(CPU* c) {
    Scroll* s = scroll_get(ARG(0), ARG(1));
    uint32_t si = ARG(2), mask = M32(si + 4);
    if (!s) { RET(0); return; }
    if (mask & 1) { M32(si + 8) = (uint32_t)s->min; M32(si + 12) = (uint32_t)s->max; }
    if (mask & 2) M32(si + 16) = (uint32_t)s->page;
    if (mask & 4) M32(si + 20) = (uint32_t)s->pos;
    if (mask & 0x10) M32(si + 24) = (uint32_t)s->track;
    RET(1);
}
static void u_SetScrollPos(CPU* c) {
    Scroll* s = scroll_get(ARG(0), ARG(1));
    if (!s) { RET(0); return; }
    int32_t old = s->pos;
    uint32_t si = guest_alloc(28, 4);
    M32(si + 4) = 4; M32(si + 20) = ARG(2);
    scroll_set(c, ARG(0), ARG(1), si);
    guest_free(si);
    RET(old);
}
static void u_GetScrollPos(CPU* c) { Scroll* s = scroll_get(ARG(0), ARG(1)); RET(s ? s->pos : 0); }
static void u_SetScrollRange(CPU* c) {
    if (!scroll_get(ARG(0), ARG(1))) { RET(0); return; }
    uint32_t si = guest_alloc(28, 4);
    M32(si + 4) = 1; M32(si + 8) = ARG(2); M32(si + 12) = ARG(3);
    scroll_set(c, ARG(0), ARG(1), si);
    guest_free(si);
    RET(1);
}
static void u_GetScrollRange(CPU* c) {
    Scroll* s = scroll_get(ARG(0), ARG(1));
    if (!s) { RET(0); return; }
    M32(ARG(2)) = (uint32_t)s->min; M32(ARG(3)) = (uint32_t)s->max;
    RET(1);
}
static void u_ShowScrollBar(CPU* c) {
    if (!win_get(ARG(0))) { RET(0); return; }
    show_bar(c, ARG(0), ARG(1), ARG(2) != 0);
    RET(1);
}
static void u_EnableScrollBar(CPU* c) {
    uint32_t bars[2] = {ARG(1) == SB_BOTH ? SB_HORZ : ARG(1), ARG(1) == SB_BOTH ? SB_VERT : ARG(1)};
    int changed = 0;
    for (int k = 0; k < 2; k++) {
        Scroll* s = scroll_get(ARG(0), bars[k]);
        if (s && s->disabled != ARG(2)) { s->disabled = ARG(2); changed = 1; }
    }
    RET(changed);
}
static void u_ScrollWindow(CPU* c) {
    Win* w = win_get(ARG(0));
    int dx = (int)ARG(1), dy = (int)ARG(2);
    uint32_t r = ARG(3), clip = ARG(4);
    if (!w) { RET(0); return; }
    int cw, ch;
    client_size(w, &cw, &ch);
    int l = 0, t = 0, rr = cw, b = ch;
    if (r) { l = (int)M32(r); t = (int)M32(r + 4); rr = (int)M32(r + 8); b = (int)M32(r + 12); }
    if (clip) {
        if ((int)M32(clip) > l) l = (int)M32(clip);
        if ((int)M32(clip + 4) > t) t = (int)M32(clip + 4);
        if ((int)M32(clip + 8) < rr) rr = (int)M32(clip + 8);
        if ((int)M32(clip + 12) < b) b = (int)M32(clip + 12);
    }
    hle_dc_scroll_window(ARG(0), dx, dy, l, t, rr, b);
    if (w->inval) { w->il += dx; w->ir += dx; w->it += dy; w->ib += dy; }
    if (dx > 0) invalidate(w, l, t, l + dx, b, 1, 0);
    if (dx < 0) invalidate(w, rr + dx, t, rr, b, 1, 0);
    if (dy > 0) invalidate(w, l, t, rr, t + dy, 1, 0);
    if (dy < 0) invalidate(w, l, b + dy, rr, b, 1, 0);
    if (!r) {
        uint32_t hwnd = ARG(0);
        for (int k = 0; k < MAX_WIN; k++)
            if (wins[k].used && is_child(&wins[k]) && wins[k].parent == hwnd)
                set_window_pos(c, win_handle(&wins[k]), 0, wins[k].x + dx, wins[k].y + dy, 0, 0,
                               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    RET(1);
}

/* Flat scroll bars share the window's scroll state; their properties only change how a bar would look. */
static struct { uint32_t index, value; } flatsb_props[] = {
    {0x1, 17}, {0x2, 17}, {0x4, 17}, {0x8, 17}, {0x10, 17}, {0x20, 17}, {0x40, 0xFFFFFFFFu}, {0x80, 0xFFFFFFFFu},
    {0x100, 0}, {0x200, 0}, {0x800, 0},
};
static void c_InitializeFlatSB(CPU* c) { RET(win_get(ARG(0)) != 0); }
static void c_UninitializeFlatSB(CPU* c) { RET(0); }
static void c_FlatSB_GetScrollProp(CPU* c) {
    Win* w = win_get(ARG(0));
    if (!w || !ARG(2)) { RET(0); return; }
    if (ARG(1) == 0x400) { M32(ARG(2)) = w->style; RET(1); return; }
    for (size_t k = 0; k < sizeof flatsb_props / sizeof flatsb_props[0]; k++)
        if (flatsb_props[k].index == ARG(1)) { M32(ARG(2)) = flatsb_props[k].value; RET(1); return; }
    RET(0);
}
static void c_FlatSB_SetScrollProp(CPU* c) {
    for (size_t k = 0; k < sizeof flatsb_props / sizeof flatsb_props[0]; k++)
        if (flatsb_props[k].index == ARG(1)) { flatsb_props[k].value = ARG(2); RET(1); return; }
    RET(0);
}

/* ---- the default window procedure ---- */

static int hit_test(Win* w, int sx, int sy) {
    int ox, oy;
    parent_origin(w, &ox, &oy);
    int x = sx - ox - w->x, y = sy - oy - w->y;
    if (x < 0 || y < 0 || x >= w->w || y >= w->h) return HTNOWHERE;
    if (x >= w->cl && x < w->cr && y >= w->ct && y < w->cb) return HTCLIENT;
    if ((w->style & WS_VSCROLL) && x >= w->cr && y >= w->ct && y < w->cb) return HTVSCROLL;
    if ((w->style & WS_HSCROLL) && y >= w->cb && x >= w->cl && x < w->cr) return HTHSCROLL;
    int l, t, r, b;
    nc_borders(w->style & ~(uint32_t)(WS_CAPTION), w->exstyle, 0, &l, &t, &r, &b);
    if (x < l || x >= w->w - r || y < t || y >= w->h - b) {
        if (!(w->style & WS_THICKFRAME)) return HTBORDER;
        int left = x < l + 16, right = x >= w->w - r - 16, top = y < t + 16, bottom = y >= w->h - b - 16;
        if (top) return left ? 13 : right ? 14 : 12;
        if (bottom) return left ? 16 : right ? 17 : 15;
        return left ? 10 : 11;
    }
    if ((w->style & WS_CAPTION) == WS_CAPTION && y < w->ct - (w->menu && !is_child(w) ? metric(15) : 0)) return HTCAPTION;
    if (w->menu && !is_child(w)) return HTMENU;
    return HTNOWHERE;
}

static void u_DefWindowProcA(CPU* c) {
    uint32_t hwnd = ARG(0), msg = ARG(1), wp = ARG(2), lp = ARG(3);
    Win* w = win_get(hwnd);
    if (!w) { RET(0); return; }
    switch (msg) {
    case WM_NCCREATE:
        if (M32(lp + 36)) set_text(w, GSTR(M32(lp + 36)));
        RET(1);
        return;
    case WM_NCCALCSIZE: {
        int l, t, r, b;
        nc_borders(w->style, w->exstyle, w->menu != 0, &l, &t, &r, &b);
        if (w->style & WS_VSCROLL) r += metric(2);
        if (w->style & WS_HSCROLL) b += metric(3);
        M32(lp) += (uint32_t)l; M32(lp + 4) += (uint32_t)t; M32(lp + 8) -= (uint32_t)r; M32(lp + 12) -= (uint32_t)b;
        if ((int32_t)M32(lp + 8) < (int32_t)M32(lp)) M32(lp + 8) = M32(lp);
        if ((int32_t)M32(lp + 12) < (int32_t)M32(lp + 4)) M32(lp + 12) = M32(lp + 4);
        RET(0);
        return;
    }
    case WM_NCHITTEST: RET((uint32_t)hit_test(w, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16))); return;
    case WM_WINDOWPOSCHANGING: {
        uint32_t f = M32(lp + 24);
        if (!(f & SWP_NOSIZE) && !is_child(w) && ((w->style & WS_THICKFRAME) || !(w->style & WS_POPUP)) &&
            !(w->style & (WS_MINIMIZE | WS_MAXIMIZE))) {
            uint32_t mmi = guest_alloc(40, 4);
            minmax_info(c, w, mmi);
            int32_t cx = (int32_t)M32(lp + 16), cy = (int32_t)M32(lp + 20);
            int32_t mnx = (int32_t)M32(mmi + 24), mny = (int32_t)M32(mmi + 28), mxx = (int32_t)M32(mmi + 32), mxy = (int32_t)M32(mmi + 36);
            if (cx > mxx) cx = mxx;
            if (cy > mxy) cy = mxy;
            if (cx < mnx && (w->style & WS_THICKFRAME)) cx = mnx;
            if (cy < mny && (w->style & WS_THICKFRAME)) cy = mny;
            M32(lp + 16) = (uint32_t)cx; M32(lp + 20) = (uint32_t)cy;
            guest_free(mmi);
        }
        RET(0);
        return;
    }
    case WM_WINDOWPOSCHANGED: {
        uint32_t f = M32(lp + 24);
        if (!(f & SWP_NOMOVE)) {
            int x = w->x + w->cl, y = w->y + w->ct;
            send_message(c, hwnd, WM_MOVE, 0, ((uint32_t)y & 0xffff) << 16 | ((uint32_t)x & 0xffff));
        }
        w = win_get(hwnd);
        if (w && (!(f & SWP_NOSIZE) || (f & SWP_FRAMECHANGED))) {
            int cw, ch;
            client_size(w, &cw, &ch);
            uint32_t type = (w->style & WS_MINIMIZE) ? SIZE_MINIMIZED : (w->style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : SIZE_RESTORED;
            send_message(c, hwnd, WM_SIZE, type, ((uint32_t)ch & 0xffff) << 16 | ((uint32_t)cw & 0xffff));
        }
        RET(0);
        return;
    }
    case WM_SETTEXT:
        set_text(w, lp ? GSTR(lp) : "");
        RET(1);
        return;
    case WM_GETTEXT: {
        if (!wp) { RET(0); return; }
        snprintf(GSTR(lp), wp, "%s", w->text ? w->text : "");
        RET(strlen(GSTR(lp)));
        return;
    }
    case WM_GETTEXTLENGTH: RET(w->text ? strlen(w->text) : 0); return;
    case WM_ERASEBKGND: {
        uint32_t brush = w->cls->brush;
        if (!brush) { RET(0); return; }
        int cw, ch;
        client_size(w, &cw, &ch);
        hle_dc_fill(wp, 0, 0, cw, ch, brush);
        RET(1);
        return;
    }
    case WM_PAINT: {
        uint32_t ps = guest_alloc(64, 4);
        guest_call(c, api("user32.dll", "BeginPaint"), 2, hwnd, ps);
        guest_call(c, api("user32.dll", "EndPaint"), 2, hwnd, ps);
        guest_free(ps);
        RET(0);
        return;
    }
    case WM_CLOSE: destroy_window(c, hwnd); RET(0); return;
    case WM_QUERYENDSESSION: case WM_QUERYOPEN: case WM_NCACTIVATE: RET(1); return;
    case WM_ACTIVATE:
        if ((wp & 0xffff) && !(wp >> 16)) set_focus(c, hwnd);
        RET(0);
        return;
    case WM_MOUSEACTIVATE:
        if (is_child(w)) {
            uint32_t r = send_message(c, w->parent, WM_MOUSEACTIVATE, wp, lp);
            if (r) { RET(r); return; }
        }
        RET(1);
        return;
    case WM_SETCURSOR: {
        if (is_child(w) && send_message(c, w->parent, WM_SETCURSOR, wp, lp)) { RET(1); return; }
        int hit = (int16_t)(lp & 0xffff);
        if (hit == HTERROR && (lp >> 16) >= WM_LBUTTONDOWN && (lp >> 16) <= 0x209) guest_call(c, api("user32.dll", "MessageBeep"), 1, 0);
        if (hit == HTCLIENT) {
            if (!w->cls->cursor) { RET(0); return; }
            guest_call(c, api("user32.dll", "SetCursor"), 1, w->cls->cursor);
            RET(1);
            return;
        }
        if (hit >= HTCAPTION && hit <= 17) {
            guest_call(c, api("user32.dll", "SetCursor"), 1, guest_call(c, api("user32.dll", "LoadCursorA"), 2, 0, 32512));
            RET(1);
            return;
        }
        RET(0);
        return;
    }
    case WM_MOUSEWHEEL: case WM_CONTEXTMENU:
        RET(is_child(w) ? send_message(c, w->parent, msg, wp, lp) : 0);
        return;
    case WM_SYSKEYDOWN:
        if (wp == 0x73 && (lp & 0x20000000u)) post(root_of(hwnd), WM_SYSCOMMAND, 0xF060, 0);
        RET(0);
        return;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case 0xF060: send_message(c, hwnd, WM_CLOSE, 0, 0); break;
        case 0xF020: show_window(c, hwnd, 6); break;
        case 0xF030: show_window(c, hwnd, 3); break;
        case 0xF120: show_window(c, hwnd, 9); break;
        }
        RET(0);
        return;
    case WM_GETMINMAXINFO: case WM_NCPAINT: RET(0); return;
    default:
        if (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) {
            int edit = msg == 0x133 || msg == 0x134;
            guest_call(c, api("gdi32.dll", "SetTextColor"), 2, wp, guest_call(c, api("user32.dll", "GetSysColor"), 1, 8));
            guest_call(c, api("gdi32.dll", "SetBkColor"), 2, wp, guest_call(c, api("user32.dll", "GetSysColor"), 1, edit ? 5 : 15));
            RET(guest_call(c, api("user32.dll", "GetSysColorBrush"), 1, edit ? 5 : 15));
            return;
        }
        RET(0);
        return;
    }
}
static void u_CallWindowProcA(CPU* c) { RET(guest_call(c, ARG(0), 4, ARG(1), ARG(2), ARG(3), ARG(4))); }

/* MDI needs the MDICLIENT system class, which is not provided, so a frame has no client to forward to. */
static void u_DefFrameProcA(CPU* c) {
    uint32_t hwnd = ARG(0), client = ARG(1), msg = ARG(2), wp = ARG(3), lp = ARG(4);
    if (msg == WM_SIZE && win_get(client))
        set_window_pos(c, client, 0, 0, 0, (int)(lp & 0xffff), (int)(lp >> 16), SWP_NOZORDER | SWP_NOACTIVATE);
    RET(guest_call(c, api("user32.dll", "DefWindowProcA"), 4, hwnd, msg, wp, lp));
}
static void u_DefMDIChildProcA(CPU* c) {
    RET(guest_call(c, api("user32.dll", "DefWindowProcA"), 4, ARG(0), ARG(1), ARG(2), ARG(3)));
}
static void u_TranslateMDISysAccel(CPU* c) { RET(0); }

/* ---- keyboard ---- */

typedef struct { uint8_t vk, scan, ext; const char* name; } Key;
static const Key keys[] = {
    {0x08, 0x0E, 0, "Backspace"}, {0x09, 0x0F, 0, "Tab"}, {0x0D, 0x1C, 0, "Enter"}, {0x10, 0x2A, 0, "Shift"},
    {0x11, 0x1D, 0, "Ctrl"}, {0x12, 0x38, 0, "Alt"}, {0x13, 0x45, 0, "Pause"}, {0x14, 0x3A, 0, "Caps Lock"},
    {0x1B, 0x01, 0, "Esc"}, {0x20, 0x39, 0, "Space"}, {0x21, 0x49, 1, "Page Up"}, {0x22, 0x51, 1, "Page Down"},
    {0x23, 0x4F, 1, "End"}, {0x24, 0x47, 1, "Home"}, {0x25, 0x4B, 1, "Left"}, {0x26, 0x48, 1, "Up"},
    {0x27, 0x4D, 1, "Right"}, {0x28, 0x50, 1, "Down"}, {0x2C, 0x37, 1, "Prnt Scrn"}, {0x2D, 0x52, 1, "Insert"},
    {0x2E, 0x53, 1, "Delete"},
    {'0', 0x0B, 0, 0}, {'1', 0x02, 0, 0}, {'2', 0x03, 0, 0}, {'3', 0x04, 0, 0}, {'4', 0x05, 0, 0}, {'5', 0x06, 0, 0},
    {'6', 0x07, 0, 0}, {'7', 0x08, 0, 0}, {'8', 0x09, 0, 0}, {'9', 0x0A, 0, 0},
    {'A', 0x1E, 0, 0}, {'B', 0x30, 0, 0}, {'C', 0x2E, 0, 0}, {'D', 0x20, 0, 0}, {'E', 0x12, 0, 0}, {'F', 0x21, 0, 0},
    {'G', 0x22, 0, 0}, {'H', 0x23, 0, 0}, {'I', 0x17, 0, 0}, {'J', 0x24, 0, 0}, {'K', 0x25, 0, 0}, {'L', 0x26, 0, 0},
    {'M', 0x32, 0, 0}, {'N', 0x31, 0, 0}, {'O', 0x18, 0, 0}, {'P', 0x19, 0, 0}, {'Q', 0x10, 0, 0}, {'R', 0x13, 0, 0},
    {'S', 0x1F, 0, 0}, {'T', 0x14, 0, 0}, {'U', 0x16, 0, 0}, {'V', 0x2F, 0, 0}, {'W', 0x11, 0, 0}, {'X', 0x2D, 0, 0},
    {'Y', 0x15, 0, 0}, {'Z', 0x2C, 0, 0},
    {0x5B, 0x5B, 1, "Left Windows"}, {0x5C, 0x5C, 1, "Right Windows"}, {0x5D, 0x5D, 1, "Application"},
    {0x60, 0x52, 0, "Num 0"}, {0x61, 0x4F, 0, "Num 1"}, {0x62, 0x50, 0, "Num 2"}, {0x63, 0x51, 0, "Num 3"},
    {0x64, 0x4B, 0, "Num 4"}, {0x65, 0x4C, 0, "Num 5"}, {0x66, 0x4D, 0, "Num 6"}, {0x67, 0x47, 0, "Num 7"},
    {0x68, 0x48, 0, "Num 8"}, {0x69, 0x49, 0, "Num 9"}, {0x6A, 0x37, 0, "Num *"}, {0x6B, 0x4E, 0, "Num +"},
    {0x6D, 0x4A, 0, "Num -"}, {0x6E, 0x53, 0, "Num Del"}, {0x6F, 0x35, 1, "Num /"},
    {0x70, 0x3B, 0, "F1"}, {0x71, 0x3C, 0, "F2"}, {0x72, 0x3D, 0, "F3"}, {0x73, 0x3E, 0, "F4"}, {0x74, 0x3F, 0, "F5"},
    {0x75, 0x40, 0, "F6"}, {0x76, 0x41, 0, "F7"}, {0x77, 0x42, 0, "F8"}, {0x78, 0x43, 0, "F9"}, {0x79, 0x44, 0, "F10"},
    {0x7A, 0x57, 0, "F11"}, {0x7B, 0x58, 0, "F12"}, {0x90, 0x45, 1, "Num Lock"}, {0x91, 0x46, 0, "Scroll Lock"},
    {0xA0, 0x2A, 0, "Shift"}, {0xA1, 0x36, 0, "Right Shift"}, {0xA2, 0x1D, 0, "Ctrl"}, {0xA3, 0x1D, 1, "Right Ctrl"},
    {0xA4, 0x38, 0, "Alt"}, {0xA5, 0x38, 1, "Right Alt"},
    {0xBA, 0x27, 0, ";"}, {0xBB, 0x0D, 0, "="}, {0xBC, 0x33, 0, ","}, {0xBD, 0x0C, 0, "-"}, {0xBE, 0x34, 0, "."},
    {0xBF, 0x35, 0, "/"}, {0xC0, 0x29, 0, "`"}, {0xDB, 0x1A, 0, "["}, {0xDC, 0x2B, 0, "\\"}, {0xDD, 0x1B, 0, "]"},
    {0xDE, 0x28, 0, "'"},
};
#define NKEYS (sizeof keys / sizeof keys[0])
static const Key* key_by_vk(uint32_t vk) {
    for (size_t k = 0; k < NKEYS; k++)
        if (keys[k].vk == vk) return &keys[k];
    return 0;
}
static const Key* key_by_scan(uint32_t scan, int ext) {
    for (size_t k = 0; k < NKEYS; k++)
        if (keys[k].scan == scan && keys[k].ext == ext) return &keys[k];
    for (size_t k = 0; k < NKEYS; k++)
        if (keys[k].scan == scan) return &keys[k];
    return 0;
}
static const char vk_chars[][2] = {{(char)0xBA, ';'}, {(char)0xBB, '='}, {(char)0xBC, ','}, {(char)0xBD, '-'}, {(char)0xBE, '.'},
                                   {(char)0xBF, '/'}, {(char)0xC0, '`'}, {(char)0xDB, '['}, {(char)0xDC, '\\'}, {(char)0xDD, ']'},
                                   {(char)0xDE, '\''}};
static uint32_t vk_char(uint32_t vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + vk - 0x60;
    switch (vk) {
    case 0x08: return 8;
    case 0x09: return 9;
    case 0x0D: return 13;
    case 0x1B: return 27;
    case 0x20: return ' ';
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    }
    for (size_t k = 0; k < sizeof vk_chars / sizeof vk_chars[0]; k++)
        if ((uint8_t)vk_chars[k][0] == vk) return (uint8_t)vk_chars[k][1];
    return 0;
}

static void u_MapVirtualKeyA(CPU* c) {
    uint32_t code = ARG(0);
    const Key* k;
    switch (ARG(1)) {
    case 0: k = key_by_vk(code == 0x10 ? 0xA0 : code == 0x11 ? 0xA2 : code == 0x12 ? 0xA4 : code); RET(k ? k->scan : 0); return;
    case 1: k = key_by_scan(code, 0); RET(k ? (k->vk >= 0xA0 && k->vk <= 0xA5 ? 0x10 + (k->vk - 0xA0) / 2 : k->vk) : 0); return;
    case 2: RET(vk_char(code)); return;
    case 3: k = key_by_scan(code & 0xff, (code >> 8) == 0xE0); RET(k ? k->vk : 0); return;
    }
    RET(0);
}
static void u_GetKeyNameTextA(CPU* c) {
    uint32_t lp = ARG(0), scan = (lp >> 16) & 0xff;
    int ext = (lp >> 24) & 1;
    const Key* k = key_by_scan(scan, ext);
    if (!k || !ARG(2)) { RET(0); return; }
    char one[2] = {(char)k->vk, 0};
    const char* name = k->name ? k->name : one;
    if ((lp & 0x02000000u) && k->vk >= 0xA0 && k->vk <= 0xA5) name = k->vk < 0xA2 ? "Shift" : k->vk < 0xA4 ? "Ctrl" : "Alt";
    snprintf(GSTR(ARG(1)), ARG(2), "%s", name);
    RET(strlen(GSTR(ARG(1))));
}

static uint8_t key_down[256], key_pressed[256], key_toggled[256];
static uint32_t layouts[16] = {0x04090409};
static int nlayouts = 1, cur_layout;

static void u_GetKeyboardLayout(CPU* c) { RET(layouts[cur_layout]); }
static void u_GetKeyboardLayoutList(CPU* c) {
    if (ARG(0) && ARG(1))
        for (int k = 0; k < nlayouts && k < (int)ARG(0); k++) M32(ARG(1) + 4 * k) = layouts[k];
    RET(nlayouts);
}
static void u_LoadKeyboardLayoutA(CPU* c) {
    uint32_t lang = (uint32_t)strtoul(GSTR(ARG(0)), 0, 16) & 0xffff;
    if (!lang) { RET(0); return; }
    uint32_t hkl = lang << 16 | lang;
    int at = -1;
    for (int k = 0; k < nlayouts; k++)
        if (layouts[k] == hkl) at = k;
    if (at < 0 && nlayouts < 16) layouts[at = nlayouts++] = hkl;
    if (at >= 0 && (ARG(1) & 1)) cur_layout = at;
    RET(at >= 0 ? hkl : 0);
}
static void u_ActivateKeyboardLayout(CPU* c) {
    uint32_t old = layouts[cur_layout], h = ARG(0);
    if (h == 0 || h == 1) cur_layout = (cur_layout + (h ? 1 : nlayouts - 1)) % nlayouts;
    else {
        int at = -1;
        for (int k = 0; k < nlayouts; k++)
            if (layouts[k] == h) at = k;
        if (at < 0) { set_last_error(87); RET(0); return; }
        cur_layout = at;
    }
    RET(old);
}

static void u_GetKeyboardState(CPU* c) {
    for (int k = 0; k < 256; k++) M8(ARG(0) + k) = (uint8_t)((key_down[k] ? 0x80 : 0) | key_toggled[k]);
    RET(1);
}
static void u_GetKeyState(CPU* c) {
    uint32_t k = ARG(0) & 0xff;
    RET((key_down[k] ? 0xFFFF8000u : 0) | key_toggled[k]);
}
static void u_GetAsyncKeyState(CPU* c) {
    uint32_t k = ARG(0) & 0xff, r = (key_down[k] ? 0x8000u : 0) | key_pressed[k];
    key_pressed[k] = 0;
    RET(r);
}

static uint32_t mouse_keys(void) {
    return (key_down[1] ? 1 : 0) | (key_down[2] ? 2 : 0) | (key_down[0x10] ? 4 : 0) | (key_down[0x11] ? 8 : 0) |
           (key_down[4] ? 0x10 : 0);
}

static void set_key(uint32_t vk, int down) {
    vk &= 0xff;
    if (down && !key_down[vk]) { key_pressed[vk] = 1; key_toggled[vk] ^= 1; }
    key_down[vk] = (uint8_t)down;
    if (vk == 0x10 || vk == 0x11 || vk == 0x12) {
        uint32_t left = 0xA0 + (vk - 0x10) * 2;
        if (down && !key_down[left]) key_pressed[left] = 1;
        key_down[left] = (uint8_t)down;
    }
}

/* Keyboard input goes to the focus window; with none, the active window gets it as system keys. */
static void key_event(CPU* c, uint32_t vk, int down, uint32_t ext) {
    const Key* k = key_by_vk(vk);
    uint32_t scan = k ? k->scan : 0;
    int repeat = down && key_down[vk & 0xff];
    set_key(vk, down);
    int alt = key_down[0x12] && !key_down[0x11];
    uint32_t target = focus_hwnd ? focus_hwnd : active_hwnd;
    if (!target) return;
    int sys = alt || vk == 0x79 || !focus_hwnd || (vk == 0x12 && !down);
    if (vk == 0x12 && !down && key_down[0x11]) sys = 0;
    uint32_t lp = 1u | scan << 16 | ((ext || (k && k->ext)) ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) |
                  (repeat || !down ? 1u << 30 : 0) | (down ? 0 : 1u << 31);
    uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    if (next_hook(WH_KEYBOARD_LL, UINT32_MAX)) {
        uint32_t ll = guest_alloc(20, 4);
        M32(ll) = vk; M32(ll + 4) = scan; M32(ll + 8) = (down ? 0 : 0x80) | (alt ? 0x20 : 0) | ((k && k->ext) ? 1 : 0);
        M32(ll + 12) = (uint32_t)port_now_ms(); M32(ll + 16) = 0;
        uint32_t eat = call_hooks(c, WH_KEYBOARD_LL, 0, msg, ll);
        guest_free(ll);
        if (eat) return;
    }
    post(target, msg, vk, lp);
}

static void u_keybd_event(CPU* c) { key_event(c, ARG(0) & 0xff, !(ARG(2) & 2), ARG(2) & 1); }

/* The page sends the character a key press produced (it knows the user's layout); TranslateMessage hands it out
   for the key-down message that carried it. */
static struct { uint32_t hwnd, msg, vk, lp, ch; } last_key;

static void u_TranslateMessage(CPU* c) {
    uint32_t m = ARG(0), msg = M32(m + 4);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYUP) { RET(0); return; }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && last_key.ch && last_key.hwnd == M32(m) && last_key.vk == M32(m + 8) &&
        last_key.msg == msg) {
        post(M32(m), msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, last_key.ch, M32(m + 12));
        last_key.ch = 0;
    }
    RET(1);
}

/* ---- input from the page: events arrive in a SharedArrayBuffer ring and become window messages ---- */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, js_input_poll, (int* out), {
    const ring = globalThis.inputRing;
    if (!ring) return 0;
    const head = Atomics.load(ring, 0), tail = Atomics.load(ring, 1);
    if (head === tail) return 0;
    const cap = (ring.length - 2) >> 2, at = 2 + (tail % cap) * 4;
    for (let i = 0; i < 4; i++) HEAP32[(out >> 2) + i] = ring[at + i];
    Atomics.store(ring, 1, tail + 1);
    return 1;
});
EM_JS(int, js_input_peek_change, (void), {
    const ring = globalThis.inputRing;
    if (!ring) return 0;
    const head = Atomics.load(ring, 0), tail = Atomics.load(ring, 1);
    if (head === tail) return 0;
    const type = ring[2 + (tail % ((ring.length - 2) >> 2)) * 4];
    return type === 2 || type === 3 || type === 5 || type === 6 ? 1 : 0;
});
/* Blocks until the page writes an input event or the timeout passes; returns 0 when there is no ring to wait on. */
EM_JS(int, js_input_wait, (int ms), {
    const ring = globalThis.inputRing;
    if (!ring) return 0;
    const head = Atomics.load(ring, 0);
    if (head !== Atomics.load(ring, 1)) return 1;
    Atomics.wait(ring, 0, head, ms);
    return 1;
});
#else
static int js_input_poll(int* out) { (void)out; return 0; }
static int js_input_peek_change(void) { return 0; }
static int js_input_wait(int ms) { (void)ms; return 0; }
#endif

static void page_to_screen(int px, int py, int* sx, int* sy) {
    Win* d = win_get(display_hwnd);
    int ox = 0, oy = 0;
    if (d) client_origin(d, &ox, &oy);
    *sx = ox + px;
    *sy = oy + py;
}
void hle_screen_to_page(int* x, int* y) {
    Win* d = win_get(display_hwnd);
    int ox = 0, oy = 0;
    if (d) client_origin(d, &ox, &oy);
    *x -= ox;
    *y -= oy;
}

static struct { uint32_t hwnd, msg, time; int x, y; } last_click;

/* Mouse input goes to the capture window or the one under the cursor, after the hit test and cursor messages. */
static void mouse_event(CPU* c, int kind, int button, int sx, int sy, int wheel) {
    static const uint32_t vk[3] = {1, 4, 2}, down_msg[3] = {0x201, 0x207, 0x204};
    cursor_x = sx;
    cursor_y = sy;
    int b = button >= 0 && button < 3 ? button : 0;
    uint32_t msg = kind == 1 ? WM_MOUSEMOVE : kind == 2 ? down_msg[b] : kind == 3 ? down_msg[b] + 1 : WM_MOUSEWHEEL;
    if (kind == 2) set_key(vk[b], 1);
    if (kind == 3) set_key(vk[b], 0);
    if (next_hook(WH_MOUSE_LL, UINT32_MAX)) {
        uint32_t ll = guest_alloc(24, 4);
        M32(ll) = (uint32_t)sx; M32(ll + 4) = (uint32_t)sy; M32(ll + 8) = kind == 4 ? (uint32_t)wheel << 16 : 0;
        M32(ll + 12) = 0; M32(ll + 16) = (uint32_t)port_now_ms(); M32(ll + 20) = 0;
        uint32_t eat = call_hooks(c, WH_MOUSE_LL, 0, msg, ll);
        guest_free(ll);
        if (eat) return;
    }
    if (kind == 4) {
        uint32_t target = focus_hwnd ? focus_hwnd : active_hwnd;
        if (target) post(target, WM_MOUSEWHEEL, (uint32_t)(wheel & 0xffff) << 16 | mouse_keys(), ((uint32_t)sy & 0xffff) << 16 | ((uint32_t)sx & 0xffff));
        return;
    }
    uint32_t target = capture_hwnd;
    int hit = HTCLIENT;
    if (!target) {
        target = window_from_point(sx, sy, 0);
        Win* t = win_get(target);
        if (!t) return;
        if (t->style & WS_DISABLED) {
            if (kind == 2) {
                uint32_t root = root_of(target);
                Win* r = win_get(root);
                Win* popup = r ? win_get(r->last_popup) : 0;
                if (popup && (popup->style & WS_VISIBLE)) set_active(c, r->last_popup, 1);
                guest_call(c, api("user32.dll", "MessageBeep"), 1, 0);
            }
            return;
        }
        hit = (int16_t)send_message(c, target, WM_NCHITTEST, 0, ((uint32_t)sy & 0xffff) << 16 | ((uint32_t)sx & 0xffff));
        if (!win_get(target)) return;
        send_message(c, target, WM_SETCURSOR, target, (uint32_t)(uint16_t)hit | msg << 16);
        if (!win_get(target)) return;
        if (kind == 2 && root_of(target) != active_hwnd) {
            uint32_t r = send_message(c, target, WM_MOUSEACTIVATE, root_of(target), (uint32_t)(uint16_t)hit | msg << 16);
            if (r == 1 || r == 2) set_active(c, target, 1);
            if (r == 2 || r == 4) return;
        }
    }
    Win* t = win_get(target);
    if (!t) return;
    if (next_hook(WH_MOUSE, UINT32_MAX)) {
        uint32_t mh = guest_alloc(20, 4);
        M32(mh) = (uint32_t)sx; M32(mh + 4) = (uint32_t)sy; M32(mh + 8) = target; M32(mh + 12) = (uint32_t)hit; M32(mh + 16) = 0;
        uint32_t eat = call_hooks(c, WH_MOUSE, 0, msg, mh);
        guest_free(mh);
        if (eat) return;
    }
    if (hit == HTCLIENT) {
        int ox, oy;
        client_origin(t, &ox, &oy);
        uint32_t now = (uint32_t)port_now_ms();
        if (kind == 2 && (t->cls->style & 8) && last_click.hwnd == target && last_click.msg == msg &&
            now - last_click.time <= 500 && abs(sx - last_click.x) <= 2 && abs(sy - last_click.y) <= 2) {
            msg += 2;
            last_click.hwnd = 0;
        } else if (kind == 2) {
            last_click.hwnd = target; last_click.msg = msg; last_click.time = now; last_click.x = sx; last_click.y = sy;
        }
        post(target, msg, mouse_keys(), ((uint32_t)(sy - oy) & 0xffff) << 16 | ((uint32_t)(sx - ox) & 0xffff));
    } else if (hit != HTNOWHERE && hit != HTTRANSPARENT) {
        post(target, msg - WM_MOUSEMOVE + WM_NCMOUSEMOVE, (uint32_t)hit, ((uint32_t)sy & 0xffff) << 16 | ((uint32_t)sx & 0xffff));
    }
}

enum { IN_MOVE = 1, IN_DOWN, IN_UP, IN_WHEEL, IN_KEYDOWN, IN_KEYUP, IN_CHAR, IN_DUMP_FRAME };

/* A button or key press and its release must not land in the same frame: the game also polls the key state once a
   frame, so only one state change goes through per Present. */
static uint32_t last_change_frame = 0xffffffffu;

static void pump_input(CPU* c) {
    int e[4];
    while (display_hwnd) {
        int change = js_input_peek_change();
        if (change && last_change_frame == port_frames) break;
        if (!js_input_poll(e)) break;
        if (change) last_change_frame = port_frames;
        static int logged;
        if (e[0] != IN_MOVE && e[0] != IN_CHAR && logged++ < 10) port_log("input %d %d (%d,%d) -> %08x", e[0], e[1], e[2], e[3], display_hwnd);
        int sx, sy;
        page_to_screen(e[2], e[3], &sx, &sy);
        switch (e[0]) {
        case IN_MOVE: case IN_UP: case IN_WHEEL: mouse_event(c, e[0], e[1], sx, sy, e[1]); break;
        case IN_DOWN:
            if (getenv("PORT_TRACE_CLICK")) { port_trace_budget = 1500; port_fn_trace_start(c); }
            mouse_event(c, e[0], e[1], sx, sy, 0);
            break;
        case IN_KEYDOWN: key_event(c, (uint32_t)e[1] & 0xff, 1, 0); break;
        case IN_KEYUP: key_event(c, (uint32_t)e[1] & 0xff, 0, 0); break;
        case IN_CHAR:
            for (int k = qn - 1; k >= 0; k--)
                if (queue[k].msg == WM_KEYDOWN || queue[k].msg == WM_SYSKEYDOWN) {
                    if (!queue[k].ch) queue[k].ch = (uint32_t)e[1];
                    break;
                }
            break;
        case IN_DUMP_FRAME: r_dump_next_frame(); break;
        }
    }
}

/* ---- retrieving messages ---- */

static int filter_matches(uint32_t hwnd, uint32_t msg, uint32_t fh, uint32_t lo, uint32_t hi) {
    if (fh == 0xFFFFFFFFu) { if (hwnd) return 0; }
    else if (fh && hwnd != fh && !is_descendant(fh, hwnd)) return 0;
    return (!lo && !hi) || (msg >= lo && msg <= hi);
}

static void write_msg(uint32_t m, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t time, int px, int py) {
    M32(m) = hwnd; M32(m + 4) = msg; M32(m + 8) = wp; M32(m + 12) = lp; M32(m + 16) = time;
    M32(m + 20) = (uint32_t)px; M32(m + 24) = (uint32_t)py;
    last_msg_time = time;
    last_msg_x = px;
    last_msg_y = py;
}

/* Posted and input messages first, then WM_PAINT for any window with an update region, then WM_QUIT. */
static int get_msg(CPU* c, uint32_t m, uint32_t fh, uint32_t lo, uint32_t hi, int remove) {
    for (;;) {
        int found = -1;
        for (int k = 0; k < qn; k++) {
            if (filter_matches(queue[k].hwnd, queue[k].msg, fh, lo, hi)) { found = k; break; }
        }
        if (found >= 0) {
            QMsg q = queue[found];
            if (remove) queue_remove(found);
            if (remove && q.msg == WM_TIMER) timer_delivered(q.hwnd, q.wp);
            int key = q.msg >= WM_KEYDOWN && q.msg <= 0x109;
            if (key && next_hook(WH_KEYBOARD, UINT32_MAX) && call_hooks(c, WH_KEYBOARD, remove ? 0 : 3, q.wp, q.lp)) {
                if (!remove)
                    for (int k = 0; k < qn; k++)
                        if (queue[k].msg == q.msg && queue[k].wp == q.wp && queue[k].lp == q.lp) { queue_remove(k); break; }
                continue;
            }
            if (key && remove) last_key.hwnd = q.hwnd, last_key.msg = q.msg, last_key.vk = q.wp, last_key.lp = q.lp, last_key.ch = q.ch;
            write_msg(m, q.hwnd, q.msg, q.wp, q.lp, q.time, q.px, q.py);
            return 1;
        }
        break;
    }
    for (int k = 0; k < MAX_WIN; k++) {
        Win* w = &wins[k];
        if (!w->used || !(w->inval || w->internal) || !visible_chain(w)) continue;
        uint32_t hwnd = win_handle(w);
        if (!filter_matches(hwnd, WM_PAINT, fh, lo, hi)) continue;
        write_msg(m, hwnd, WM_PAINT, 0, 0, (uint32_t)port_now_ms(), cursor_x, cursor_y);
        return 1;
    }
    if (quit_posted) {
        write_msg(m, 0, WM_QUIT, quit_code, 0, (uint32_t)port_now_ms(), cursor_x, cursor_y);
        if (remove) quit_posted = 0;
        return 1;
    }
    return 0;
}

static void after_get(CPU* c, uint32_t m, int remove) {
    if (next_hook(WH_GETMESSAGE, UINT32_MAX)) call_hooks(c, WH_GETMESSAGE, 0, remove ? 1 : 0, m);
}

static void pump_all(CPU* c) {
    pump_input(c);
    pump_timers();
    port_threads_pump(c);
}

static void u_PeekMessageA(CPU* c) {
    pump_all(c);
    int remove = ARG(4) & 1;
    int got = get_msg(c, ARG(0), ARG(1), ARG(2), ARG(3), remove);
    if (got) after_get(c, ARG(0), remove);
    RET(got);
}

/* Waits for input or the next timer; emulated threads keep running in between. */
static int wait_input(CPU* c, double deadline) {
    double now = port_now_ms(), soonest = next_timer_ms();
    double until = deadline;
    if (soonest && (until < 0 || soonest < until)) until = soonest;
    double slice = until < 0 ? 50 : until - now;
    if (slice > 50) slice = 50;
    if (slice < 0) slice = 0;
    if (!js_input_wait((int)slice)) {
        if (until < 0) return 0;
        while (port_now_ms() < now + slice) {}
    }
    pump_all(c);
    return 1;
}

static void u_GetMessageA(CPU* c) {
    for (;;) {
        pump_all(c);
        if (get_msg(c, ARG(0), ARG(1), ARG(2), ARG(3), 1)) break;
        call_hooks(c, WH_FOREGROUNDIDLE, 0, 0, 0);
        if (!wait_input(c, -1) && !next_timer_ms()) port_fatal("GetMessage with an empty queue (nothing would wake the guest)");
    }
    after_get(c, ARG(0), 1);
    RET(M32(ARG(0) + 4) != WM_QUIT);
}
static void u_WaitMessage(CPU* c) {
    for (;;) {
        pump_all(c);
        uint32_t m = guest_alloc(28, 4);
        int got = get_msg(c, m, 0, 0, 0, 0);
        guest_free(m);
        if (got) break;
        call_hooks(c, WH_FOREGROUNDIDLE, 0, 0, 0);
        if (!wait_input(c, -1) && !next_timer_ms()) break;
    }
    RET(1);
}
/* Kernel objects are not waitable here, so only the input part of the wait can complete it. */
static void u_MsgWaitForMultipleObjects(CPU* c) {
    double deadline = ARG(3) == 0xFFFFFFFFu ? -1 : port_now_ms() + ARG(3);
    for (;;) {
        pump_all(c);
        uint32_t m = guest_alloc(28, 4);
        int got = get_msg(c, m, 0, 0, 0, 0);
        guest_free(m);
        if (got) { RET(ARG(0)); return; }
        if (deadline >= 0 && port_now_ms() >= deadline) { RET(0x102); return; }
        if (!wait_input(c, deadline) && deadline < 0 && !next_timer_ms()) { RET(0x102); return; }
    }
}

static void u_DispatchMessageA(CPU* c) {
    uint32_t m = ARG(0);
    if (M32(m + 4) == WM_TIMER && M32(m + 12)) {
        RET(guest_call(c, M32(m + 12), 4, M32(m), (uint32_t)WM_TIMER, M32(m + 8), (uint32_t)port_now_ms()));
        return;
    }
    RET(send_message(c, M32(m), M32(m + 4), M32(m + 8), M32(m + 12)));
}
static void u_GetMessageTime(CPU* c) { RET(last_msg_time); }
static void u_GetMessagePos(CPU* c) { RET(((uint32_t)last_msg_y & 0xffff) << 16 | ((uint32_t)last_msg_x & 0xffff)); }

/* Tab moves the focus between WS_TABSTOP children, Enter and Escape become IDOK and IDCANCEL, unless the focused
   control asks for those keys through WM_GETDLGCODE. */
static void u_IsDialogMessageA(CPU* c) {
    uint32_t dlg = ARG(0), m = ARG(1), hwnd = M32(m), msg = M32(m + 4), wp = M32(m + 8);
    if (!win_get(dlg) || (hwnd != dlg && !is_descendant(dlg, hwnd))) { RET(0); return; }
    if (msg == WM_KEYDOWN) {
        uint32_t code = send_message(c, hwnd, WM_GETDLGCODE, wp, m);
        if (!(code & 4)) {
            if (wp == 9 && !(code & 2)) {
                static Win* list[MAX_WIN];
                uint32_t tab[MAX_WIN];
                int n = 0, at = -1;
                for (int k = 0, cnt = siblings(dlg, list); k < cnt; k++)
                    if ((list[k]->style & (WS_TABSTOP | WS_VISIBLE | WS_DISABLED)) == (WS_TABSTOP | WS_VISIBLE)) {
                        if (win_handle(list[k]) == focus_hwnd) at = n;
                        tab[n++] = win_handle(list[k]);
                    }
                if (n) set_focus(c, tab[key_down[0x10] ? (at <= 0 ? n - 1 : at - 1) : (at + 1) % n]);
                RET(1);
                return;
            }
            if ((wp == 13 || wp == 27) && !(code & 0x4)) {
                post(dlg, WM_COMMAND, wp == 13 ? 1 : 2, 0);
                RET(1);
                return;
            }
        }
    }
    guest_call(c, api("user32.dll", "TranslateMessage"), 1, m);
    guest_call(c, api("user32.dll", "DispatchMessageA"), 1, m);
    RET(1);
}

/* ---- modals: message boxes and popup menus are drawn by the page; the guest keeps running its timers meanwhile ---- */

#ifdef __EMSCRIPTEN__
EM_JS(int, js_modal_open, (const char* kind, const char* json), {
    if (typeof postMessage !== 'function' || !globalThis.inputRing) return -1;
    const reply = new Int32Array(new SharedArrayBuffer(8));
    globalThis.modals = globalThis.modals || [];
    const id = globalThis.modals.length;
    globalThis.modals.push(reply);
    postMessage({ type: 'modal', kind: UTF8ToString(kind), data: JSON.parse(UTF8ToString(json)), reply });
    return id;
});
EM_JS(int, js_modal_result, (int id), {
    const reply = globalThis.modals[id];
    return Atomics.load(reply, 0) ? Atomics.load(reply, 1) : -2147483648;
});
#else
static int js_modal_open(const char* kind, const char* json) { (void)kind; (void)json; return -1; }
static int js_modal_result(int id) { (void)id; return INT_MIN; }
#endif

size_t hle_json_str(char* out, size_t cap, const char* s) {
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '"';
    for (const uint8_t* p = (const uint8_t*)s; *p && n + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p < 0x20 || *p >= 0x7f) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *p);
        else out[n++] = (char)*p;
    }
    out[n++] = '"';
    out[n] = 0;
    return n;
}

/* Returns the page's answer, or -1 when there is no page to ask (the Node runner). */
int hle_modal(CPU* c, const char* kind, const char* json) {
    int id = js_modal_open(kind, json);
    if (id < 0) return -1;
    for (;;) {
        int r = js_modal_result(id);
        if (r != INT_MIN) return r;
        pump_timers();
        port_threads_pump(c);
        uint32_t m = guest_alloc(28, 4);
        while (get_msg(c, m, 0, WM_TIMER, WM_TIMER, 1) || get_msg(c, m, 0, WM_PAINT, WM_PAINT, 1))
            guest_call(c, api("user32.dll", "DispatchMessageA"), 1, m);
        guest_free(m);
        js_input_wait(10);
    }
}

static void u_MessageBoxA(CPU* c) {
    static const int buttons[7][3] = {{1}, {1, 2}, {3, 4, 5}, {6, 7, 2}, {6, 7}, {4, 2}, {2, 10, 11}};
    uint32_t type = ARG(3), set = type & 0xf, def = (type >> 8) & 0xf;
    if (set > 6) set = 0;
    const char* text = ARG(1) ? GSTR(ARG(1)) : "";
    const char* caption = ARG(2) ? GSTR(ARG(2)) : "Error";
    port_log("MessageBoxA \"%s\": %s", caption, text);
    size_t cap = strlen(text) * 6 + strlen(caption) * 6 + 128;
    char* json = malloc(cap);
    size_t n = (size_t)snprintf(json, cap, "{\"text\":");
    n += hle_json_str(json + n, cap - n, text);
    n += (size_t)snprintf(json + n, cap - n, ",\"caption\":");
    n += hle_json_str(json + n, cap - n, caption);
    n += (size_t)snprintf(json + n, cap - n, ",\"icon\":%u,\"default\":%u,\"buttons\":[", (type >> 4) & 0xf, def);
    int count = 0;
    for (int k = 0; k < 3 && buttons[set][k]; k++) n += (size_t)snprintf(json + n, cap - n, "%s%d", k ? "," : "", buttons[set][k]), count++;
    snprintf(json + n, cap - n, "]}");
    uint32_t owner = ARG(0) ? root_of(ARG(0)) : active_hwnd;
    Win* ow = win_get(owner);
    int disabled = ow && !(ow->style & WS_DISABLED);
    if (disabled) ow->style |= WS_DISABLED;
    int r = hle_modal(c, "msgbox", json);
    free(json);
    ow = win_get(owner);
    if (disabled && ow) ow->style &= ~(uint32_t)WS_DISABLED;
    if (r <= 0) r = buttons[set][def < (uint32_t)count ? def : 0];
    port_log("MessageBoxA -> %d", r);
    RET(r);
}

static void u_MessageBeep(CPU* c) { RET(1); }
static void u_WinHelpA(CPU* c) {
    port_log("WinHelpA %s cmd=%u: no help viewer", ARG(1) ? GSTR(ARG(1)) : "", ARG(2));
    RET(0);
}

/* ---- system metrics and parameters ---- */

static int metric(int i) {
    switch (i) {
    case 0: case 16: case 61: case 78: return SCREEN_W;
    case 1: case 79: return SCREEN_H;
    case 17: return WORK_H - 23;
    case 62: return WORK_H;
    case 2: case 3: case 9: case 10: case 20: case 21: return 17;
    case 4: return 23;
    case 5: case 6: return 1;
    case 7: case 8: return 3;
    case 11: case 12: case 13: case 14: return 32;
    case 15: return 20;
    case 19: case 75: case 80: return 1;
    case 22: case 23: case 42: case 82: return 0;
    case 28: return 112;
    case 29: return 27;
    case 30: case 31: return 18;
    case 32: case 33: return 4;
    case 34: return 136;
    case 35: return 39;
    case 36: case 37: return 4;
    case 38: return 75;
    case 39: return 75;
    case 43: return 3;
    case 45: case 46: return 2;
    case 49: case 50: return 16;
    case 51: return 17;
    case 52: case 53: return 16;
    case 54: case 55: return 19;
    case 57: return 160;
    case 58: return 28;
    case 59: return SCREEN_W + 12;
    case 60: return SCREEN_H + 12;
    case 68: case 69: return 4;
    case 71: case 72: return 13;
    case 76: case 77: return 0;
    default: return 0;
    }
}
static void u_GetSystemMetrics(CPU* c) { RET(metric((int)ARG(0))); }

/* Settings with a DWORD or BOOL value; SPI_SET* is the getter + 1 and changes the value for this process. */
typedef struct { uint32_t get, value; } Spi;
static Spi spi_values[] = {
    {0x01, 1}, {0x05, 1}, {0x0A, 31}, {0x0E, 900}, {0x10, 0}, {0x12, 0}, {0x16, 1}, {0x19, 1}, {0x1B, 0},
    {0x23, 1}, {0x26, 1}, {0x38, 0}, {0x44, 0}, {0x46, 0}, {0x4A, 1}, {0x5F, 0}, {0x62, 4}, {0x64, 4},
    {0x66, 400}, {0x68, 3}, {0x6A, 400}, {0x6C, 3}, {0x6E, 1}, {0x70, 10}, {0x72, 0},
    {0x1000, 0}, {0x1002, 1}, {0x1004, 1}, {0x1006, 1}, {0x1008, 1}, {0x100A, 0}, {0x100C, 0}, {0x100E, 1},
    {0x1012, 1}, {0x1014, 1}, {0x1016, 1}, {0x1018, 1}, {0x101A, 1}, {0x101C, 0}, {0x101E, 0}, {0x1020, 1},
    {0x1022, 1}, {0x1024, 1}, {0x1026, 0}, {0x103E, 1}, {0x1040, 0}, {0x1042, 1}, {0x1048, 1},
    {0x2000, 200000}, {0x2002, 0}, {0x2004, 3}, {0x2006, 1}, {0x2008, 1200}, {0x200A, 2}, {0x200C, 1200},
    {0x200E, 1}, {0x2010, 1}, {0x2012, 1},
};
/* Accessibility structures: only their dwFlags member (offset 4) is state. */
static Spi spi_flags[] = {{0x32, 0x7E}, {0x34, 0x1E}, {0x3A, 0x1FA}, {0x42, 0x7E}, {0x48, 1}};

static void spi_font(uint32_t lf, int bold) {
    memset(GPTR(lf), 0, 60);
    M32(lf) = (uint32_t)-12;
    M32(lf + 16) = bold ? 700 : 400;
    M8(lf + 23) = 1;
    strcpy(GSTR(lf + 28), "Tahoma");
}

static void u_SystemParametersInfoA(CPU* c) {
    uint32_t action = ARG(0), ui = ARG(1), pv = ARG(2);
    for (size_t k = 0; k < sizeof spi_values / sizeof spi_values[0]; k++) {
        if (spi_values[k].get == action) { if (pv) M32(pv) = spi_values[k].value; RET(1); return; }
        if (spi_values[k].get + 1 == action) {
            spi_values[k].value = action >= 0x1000 ? pv : ui;
            RET(1);
            return;
        }
    }
    for (size_t k = 0; k < sizeof spi_flags / sizeof spi_flags[0]; k++) {
        if (spi_flags[k].get == action) { M32(pv + 4) = spi_flags[k].value; RET(1); return; }
        if (spi_flags[k].get + 1 == action) { spi_flags[k].value = M32(pv + 4); RET(1); return; }
    }
    switch (action) {
    case 0x30:   /* SPI_GETWORKAREA */
        M32(pv) = 0; M32(pv + 4) = 0; M32(pv + 8) = SCREEN_W; M32(pv + 12) = WORK_H;
        break;
    case 0x1F: spi_font(pv, 0); break;      /* SPI_GETICONTITLELOGFONT */
    case 0x29:                               /* SPI_GETNONCLIENTMETRICS */
        M32(pv + 4) = 1; M32(pv + 8) = 17; M32(pv + 12) = 17; M32(pv + 16) = 18; M32(pv + 20) = 18;
        spi_font(pv + 24, 1);
        M32(pv + 84) = 12; M32(pv + 88) = 15; spi_font(pv + 92, 0);
        M32(pv + 152) = 18; M32(pv + 156) = 18; spi_font(pv + 160, 0);
        spi_font(pv + 220, 0);
        spi_font(pv + 280, 0);
        if (M32(pv) >= 344) M32(pv + 340) = 0;
        break;
    case 0x2B:                               /* SPI_GETMINIMIZEDMETRICS */
        M32(pv + 4) = 160; M32(pv + 8) = 0; M32(pv + 12) = 0; M32(pv + 16) = 8;
        break;
    case 0x2D:                               /* SPI_GETICONMETRICS */
        M32(pv + 4) = 75; M32(pv + 8) = 75; M32(pv + 12) = 1; spi_font(pv + 16, 0);
        break;
    case 0x59: M32(pv) = layouts[cur_layout]; break;   /* SPI_GETDEFAULTINPUTLANG */
    default:
        port_log("SystemParametersInfoA: unknown action %#x", action);
        set_last_error(87);
        RET(0);
        return;
    }
    RET(1);
}

static void u_EnumDisplayDevicesA(CPU* c) {
    uint32_t d = ARG(2);
    if (ARG(0) || ARG(1) || !d) { RET(0); return; }
    memset(GPTR(d + 4), 0, 420);
    strcpy(GSTR(d + 4), "\\\\.\\DISPLAY1");
    strcpy(GSTR(d + 36), "WebGL 2");
    M32(d + 164) = 1 | 4;
    strcpy(GSTR(d + 168), "PCI\\VEN_0000&DEV_0000");
    RET(1);
}

static void u_GetCursorPos(CPU* c) { M32(ARG(0)) = (uint32_t)cursor_x; M32(ARG(0) + 4) = (uint32_t)cursor_y; RET(1); }
/* The browser owns the pointer, so moving it is not possible; Windows refuses the same way without input desktop access. */
static void u_SetCursorPos(CPU* c) { set_last_error(5); RET(0); }
static void u_GetDoubleClickTime(CPU* c) { RET(500); }
static void u_GetCaretBlinkTime(CPU* c) { RET(530); }

/* ---- rectangles and strings ---- */

static void u_EqualRect(CPU* c) { RET(!memcmp(GPTR(ARG(0)), GPTR(ARG(1)), 16)); }
static void u_IsRectEmpty(CPU* c) {
    uint32_t r = ARG(0);
    RET(!r || (int)M32(r + 8) <= (int)M32(r) || (int)M32(r + 12) <= (int)M32(r + 4));
}
static void u_IntersectRect(CPU* c) {
    uint32_t d = ARG(0), a = ARG(1), b = ARG(2);
    int l = (int)M32(a) > (int)M32(b) ? (int)M32(a) : (int)M32(b);
    int t = (int)M32(a + 4) > (int)M32(b + 4) ? (int)M32(a + 4) : (int)M32(b + 4);
    int r = (int)M32(a + 8) < (int)M32(b + 8) ? (int)M32(a + 8) : (int)M32(b + 8);
    int bb = (int)M32(a + 12) < (int)M32(b + 12) ? (int)M32(a + 12) : (int)M32(b + 12);
    if (r <= l || bb <= t) { memset(GPTR(d), 0, 16); RET(0); return; }
    M32(d) = (uint32_t)l; M32(d + 4) = (uint32_t)t; M32(d + 8) = (uint32_t)r; M32(d + 12) = (uint32_t)bb;
    RET(1);
}
static void u_UnionRect(CPU* c) {
    uint32_t d = ARG(0), a = ARG(1), b = ARG(2);
    int ea = (int)M32(a + 8) <= (int)M32(a) || (int)M32(a + 12) <= (int)M32(a + 4);
    int eb = (int)M32(b + 8) <= (int)M32(b) || (int)M32(b + 12) <= (int)M32(b + 4);
    if (ea && eb) { memset(GPTR(d), 0, 16); RET(0); return; }
    if (ea || eb) { memmove(GPTR(d), GPTR(ea ? b : a), 16); RET(1); return; }
    int l = (int)M32(a) < (int)M32(b) ? (int)M32(a) : (int)M32(b);
    int t = (int)M32(a + 4) < (int)M32(b + 4) ? (int)M32(a + 4) : (int)M32(b + 4);
    int r = (int)M32(a + 8) > (int)M32(b + 8) ? (int)M32(a + 8) : (int)M32(b + 8);
    int bb = (int)M32(a + 12) > (int)M32(b + 12) ? (int)M32(a + 12) : (int)M32(b + 12);
    M32(d) = (uint32_t)l; M32(d + 4) = (uint32_t)t; M32(d + 8) = (uint32_t)r; M32(d + 12) = (uint32_t)bb;
    RET(1);
}

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
static void u_CharUpperBuffW(CPU* c) {
    for (uint32_t k = 0; k < ARG(1); k++) {
        uint16_t* ch = (uint16_t*)GPTR(ARG(0) + 2 * k);
        *ch = (uint16_t)towupper(*ch);
    }
    RET(ARG(1));
}
static void u_OemToCharA(CPU* c) { if (ARG(0) != ARG(1)) strcpy(GSTR(ARG(1)), GSTR(ARG(0))); RET(1); }
static void u_CharToOemA(CPU* c) { if (ARG(0) != ARG(1)) strcpy(GSTR(ARG(1)), GSTR(ARG(0))); RET(1); }
static void u_IsCharAlphaA(CPU* c) { RET(isalpha((int)(ARG(0) & 0xff)) != 0); }
static void u_IsCharAlphaNumericA(CPU* c) { RET(isalnum((int)(ARG(0) & 0xff)) != 0); }
static void u_wsprintfA(CPU* c) {
    char* out = GSTR(ARG(0));
    const char* f = GSTR(ARG(1));
    int a = 2;
    char* o = out;
    for (; *f; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        char spec[16] = "%";
        int n = 1;
        f++;
        while ((*f == '-' || *f == '0' || *f == '#' || (*f >= '1' && *f <= '9') || *f == '.') && n < 12) spec[n++] = *f++;
        if (*f == 'l' || *f == 'h') f++;
        switch (*f) {
        case 's': spec[n++] = 's'; o += sprintf(o, spec, ARG(a) ? GSTR(ARG(a)) : "(null)"); a++; break;
        case 'd': case 'i': spec[n++] = 'd'; o += sprintf(o, spec, (int32_t)ARG(a++)); break;
        case 'u': spec[n++] = 'u'; o += sprintf(o, spec, ARG(a++)); break;
        case 'x': case 'X': spec[n++] = *f; o += sprintf(o, spec, ARG(a++)); break;
        case 'c': *o++ = (char)ARG(a++); break;
        case '%': *o++ = '%'; break;
        default: *o++ = *f; break;
        }
        if (!*f) break;
    }
    *o = 0;
    RET(o - out);
}

static void g_GetDeviceCaps(CPU* c) {
    switch (ARG(1)) {
    case 0: RET(0x4000); break;              /* DRIVERVERSION */
    case 2: RET(1); break;                   /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: RET(SCREEN_W * 254 / 960); break;/* HORZSIZE, mm at 96 dpi */
    case 6: RET(SCREEN_H * 254 / 960); break;/* VERTSIZE */
    case 8: RET(SCREEN_W); break;
    case 10: RET(SCREEN_H); break;
    case 12: RET(32); break;
    case 14: RET(1); break;
    case 24: RET(-1); break;                 /* NUMCOLORS: more than 256 */
    case 40: RET(0); break;                  /* ASPECTX-family */
    case 88: case 90: RET(96); break;
    case 104: RET(0); break;                 /* SIZEPALETTE: not palette-based */
    case 38: RET(0x7E99); break;             /* RASTERCAPS without RC_PALETTE */
    case 116: RET(60); break;                /* VREFRESH */
    case 117: RET(SCREEN_W); break;          /* DESKTOPHORZRES */
    case 118: RET(SCREEN_H); break;          /* DESKTOPVERTRES */
    default: RET(0); break;
    }
}
static void g_GdiFlush(CPU* c) { RET(1); }
static void k_MulDiv(CPU* c) {
    int64_t r = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    int32_t d = (int32_t)ARG(2);
    RET(d ? (int32_t)((r + (r >= 0 ? d / 2 : -d / 2)) / d) : -1);
}

/* ---- for the other modules ---- */

int hle_window_exists(uint32_t hwnd) { return win_get(hwnd) != 0; }
uint32_t hle_window_menu(uint32_t hwnd) { Win* w = win_get(hwnd); return w && !is_child(w) ? w->menu : 0; }
void hle_window_set_menu(CPU* c, uint32_t hwnd, uint32_t menu) {
    Win* w = win_get(hwnd);
    if (!w || is_child(w)) return;
    w->menu = menu;
    set_window_pos(c, hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void hle_window_origin(uint32_t hwnd, int* x, int* y) { origin_of(hwnd, x, y); }
void hle_window_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h) {
    Win* win = win_get(hwnd);
    int cw = 0, ch = 0;
    if (win) client_size(win, &cw, &ch);
    else if (hwnd == HWND_DESKTOP) { cw = SCREEN_W; ch = SCREEN_H; }
    *w = (uint32_t)cw;
    *h = (uint32_t)ch;
}

#define U(n, a) {"user32.dll", #n, a, u_##n}

static const Hle user32[] = {
    U(RegisterClassA, 1), U(GetClassInfoA, 3), U(UnregisterClassA, 2), U(CreateWindowExA, 12), U(DestroyWindow, 1),
    U(DefWindowProcA, 4), U(CallWindowProcA, 5), U(DefFrameProcA, 5), U(DefMDIChildProcA, 4),
    U(TranslateMDISysAccel, 2), U(SetWindowLongA, 3), U(SetWindowLongW, 3), U(GetWindowLongA, 2),
    U(SetClassLongA, 3), U(GetClassLongA, 2), U(GetClassNameA, 3), U(SendMessageA, 4), U(PostMessageA, 4),
    U(PostQuitMessage, 1), U(PeekMessageA, 5), U(GetMessageA, 4), U(TranslateMessage, 1), U(DispatchMessageA, 1),
    U(WaitMessage, 0), U(MsgWaitForMultipleObjects, 5), U(GetMessageTime, 0), U(GetMessagePos, 0),
    U(IsDialogMessageA, 2), U(ShowWindow, 2), U(ShowOwnedPopups, 2), U(IsWindowVisible, 1), U(IsWindow, 1),
    U(IsWindowEnabled, 1), U(EnableWindow, 2), U(IsIconic, 1), U(IsZoomed, 1), U(IsChild, 2), U(GetParent, 1),
    U(SetParent, 2), U(SetWindowTextA, 2), U(GetWindowTextA, 3), U(GetWindowTextLengthA, 1), U(GetWindowRect, 2),
    U(GetClientRect, 2), U(SetWindowPos, 7), U(MoveWindow, 6), U(GetWindowPlacement, 2), U(SetWindowPlacement, 2),
    U(AdjustWindowRectEx, 4), U(ClientToScreen, 2), U(ScreenToClient, 2), U(MapWindowPoints, 4),
    U(WindowFromPoint, 2), U(GetWindow, 2), U(GetTopWindow, 1), U(GetDesktopWindow, 0), U(EnumWindows, 2),
    U(EnumThreadWindows, 3), U(GetWindowThreadProcessId, 2), U(FindWindowA, 2), U(FindWindowExA, 4),
    U(GetLastActivePopup, 1), U(SetPropA, 3), U(GetPropA, 2), U(RemovePropA, 2), U(InvalidateRect, 3),
    U(ValidateRect, 2), U(GetUpdateRect, 3), U(UpdateWindow, 1), U(RedrawWindow, 4), U(SetScrollInfo, 4),
    U(GetScrollInfo, 3), U(SetScrollPos, 4), U(GetScrollPos, 2), U(SetScrollRange, 5), U(GetScrollRange, 4),
    U(ShowScrollBar, 3), U(EnableScrollBar, 3), U(ScrollWindow, 5), U(SetFocus, 1), U(GetFocus, 0),
    U(SetActiveWindow, 1), U(GetActiveWindow, 0), U(GetForegroundWindow, 0), U(SetForegroundWindow, 1),
    U(SetCapture, 1), U(GetCapture, 0), U(ReleaseCapture, 0), U(SetWindowsHookExA, 4), U(UnhookWindowsHookEx, 1),
    U(CallNextHookEx, 4), U(SetTimer, 4), U(KillTimer, 2), U(MessageBoxA, 4), U(MessageBeep, 1), U(WinHelpA, 4),
    U(RegisterWindowMessageA, 1), U(GetSystemMetrics, 1), U(SystemParametersInfoA, 4), U(EnumDisplayDevicesA, 4),
    U(GetKeyboardLayout, 1), U(GetKeyboardLayoutList, 2), U(LoadKeyboardLayoutA, 2), U(ActivateKeyboardLayout, 2),
    U(GetKeyboardState, 1), U(GetKeyState, 1), U(GetAsyncKeyState, 1), U(MapVirtualKeyA, 2),
    U(GetKeyNameTextA, 3), U(keybd_event, 4), U(GetCursorPos, 1), U(SetCursorPos, 2), U(GetDoubleClickTime, 0),
    U(GetCaretBlinkTime, 0), U(EqualRect, 2), U(IsRectEmpty, 1), U(IntersectRect, 3), U(UnionRect, 3),
    U(CharNextA, 1), U(CharPrevA, 2), U(CharLowerA, 1), U(CharUpperA, 1), U(CharLowerBuffA, 2),
    U(CharUpperBuffA, 2), U(CharUpperBuffW, 2), U(OemToCharA, 2), U(CharToOemA, 2), U(IsCharAlphaA, 1),
    U(IsCharAlphaNumericA, 1), {"user32.dll", "wsprintfA", CDECL_ARGS, u_wsprintfA},
    {"kernel32.dll", "GlobalAddAtomA", 1, k_GlobalAddAtomA}, {"kernel32.dll", "GlobalDeleteAtom", 1, k_GlobalDeleteAtom},
    {"kernel32.dll", "GlobalFindAtomA", 1, k_GlobalFindAtomA}, {"kernel32.dll", "GlobalGetAtomNameA", 3, k_GlobalGetAtomNameA},
    {"kernel32.dll", "MulDiv", 3, k_MulDiv},
    {"comctl32.dll", "InitializeFlatSB", 1, c_InitializeFlatSB}, {"comctl32.dll", "UninitializeFlatSB", 1, c_UninitializeFlatSB},
    {"comctl32.dll", "FlatSB_GetScrollInfo", 3, u_GetScrollInfo}, {"comctl32.dll", "FlatSB_SetScrollInfo", 4, u_SetScrollInfo},
    {"comctl32.dll", "FlatSB_GetScrollPos", 2, u_GetScrollPos}, {"comctl32.dll", "FlatSB_SetScrollPos", 4, u_SetScrollPos},
    {"comctl32.dll", "FlatSB_GetScrollRange", 4, u_GetScrollRange}, {"comctl32.dll", "FlatSB_SetScrollRange", 5, u_SetScrollRange},
    {"comctl32.dll", "FlatSB_ShowScrollBar", 3, u_ShowScrollBar}, {"comctl32.dll", "FlatSB_EnableScrollBar", 3, u_EnableScrollBar},
    {"comctl32.dll", "FlatSB_GetScrollProp", 3, c_FlatSB_GetScrollProp}, {"comctl32.dll", "FlatSB_SetScrollProp", 4, c_FlatSB_SetScrollProp},
    {"gdi32.dll", "GetDeviceCaps", 2, g_GetDeviceCaps}, {"gdi32.dll", "GdiFlush", 0, g_GdiFlush},
};

void hle_user32_register(void) { hle_register(user32, sizeof user32 / sizeof user32[0]); }
