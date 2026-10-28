#include "port.h"
#include <stdlib.h>

#define RET(v) (c->eax = (uint32_t)(v))

/* ---- menus ---- */

enum { MF_GRAYED = 0x1, MF_DISABLED = 0x2, MF_BITMAP = 0x4, MF_CHECKED = 0x8, MF_POPUP = 0x10, MF_OWNERDRAW = 0x100,
       MF_BYPOSITION = 0x400, MF_SEPARATOR = 0x800, MFS_DEFAULT = 0x1000, MFT_RADIOCHECK = 0x200 };
enum { MIIM_STATE = 0x1, MIIM_ID = 0x2, MIIM_SUBMENU = 0x4, MIIM_CHECKMARKS = 0x8, MIIM_TYPE = 0x10, MIIM_DATA = 0x20,
       MIIM_STRING = 0x40, MIIM_BITMAP = 0x80, MIIM_FTYPE = 0x100 };
enum { WM_INITMENU = 0x116, WM_INITMENUPOPUP = 0x117, WM_UNINITMENUPOPUP = 0x125, WM_ENTERMENULOOP = 0x211,
       WM_EXITMENULOOP = 0x212, WM_COMMAND = 0x111, WM_SYSCOMMAND = 0x112 };

typedef struct {
    uint32_t type, state, id, sub, bmp_checked, bmp_unchecked, data, bmp;
    char* text;
} Item;
typedef struct {
    int used;
    Item* items;
    int n;
} Menu;

#define MAX_MENU 512
#define HMENU_BASE 0x00090000u
static Menu menus[MAX_MENU];

static Menu* menu_get(uint32_t h) {
    uint32_t k = (h - HMENU_BASE) / 4;
    if (h < HMENU_BASE || (h & 3) || k >= MAX_MENU || !menus[k].used) return 0;
    return &menus[k];
}

static uint32_t menu_new(void) {
    for (int k = 0; k < MAX_MENU; k++)
        if (!menus[k].used) {
            memset(&menus[k], 0, sizeof menus[k]);
            menus[k].used = 1;
            return HMENU_BASE + (uint32_t)k * 4;
        }
    set_last_error(8);
    return 0;
}

static void menu_destroy(uint32_t h) {
    Menu* m = menu_get(h);
    if (!m) return;
    m->used = 0;
    for (int k = 0; k < m->n; k++) {
        if (m->items[k].sub) menu_destroy(m->items[k].sub);
        free(m->items[k].text);
    }
    free(m->items);
}

/* MF_BYCOMMAND looks through submenus too. */
static Item* item_find(uint32_t h, uint32_t pos, uint32_t flags, uint32_t* owner) {
    Menu* m = menu_get(h);
    if (!m) return 0;
    if (flags & MF_BYPOSITION) {
        if (pos >= (uint32_t)m->n) return 0;
        if (owner) *owner = h;
        return &m->items[pos];
    }
    for (int k = 0; k < m->n; k++) {
        Item* it = &m->items[k];
        if (it->sub) {
            Item* found = item_find(it->sub, pos, flags, owner);
            if (found) return found;
        }
        if (!it->sub && it->id == pos) {
            if (owner) *owner = h;
            return it;
        }
    }
    return 0;
}

static Item* item_insert(uint32_t h, uint32_t pos, int by_position) {
    Menu* m = menu_get(h);
    if (!m) return 0;
    int at = m->n;
    if (by_position) { if (pos < (uint32_t)m->n) at = (int)pos; }
    else if (pos != 0xFFFFFFFFu)
        for (int k = 0; k < m->n; k++)
            if (!m->items[k].sub && m->items[k].id == pos) { at = k; break; }
    m->items = realloc(m->items, sizeof(Item) * (size_t)(m->n + 1));
    memmove(&m->items[at + 1], &m->items[at], sizeof(Item) * (size_t)(m->n - at));
    m->n++;
    memset(&m->items[at], 0, sizeof(Item));
    return &m->items[at];
}

static void set_item_text(Item* it, uint32_t p) {
    free(it->text);
    it->text = p ? strdup(GSTR(p)) : 0;
}

/* The legacy MF_* flags of InsertMenu/ModifyMenu/AppendMenu. */
static void item_from_flags(Item* it, uint32_t flags, uint32_t id, uint32_t data) {
    it->type = flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR | MFT_RADIOCHECK | 0x20 | 0x40 | 0x4000 | 0x2000);
    it->state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | 0x80 | MFS_DEFAULT);
    it->sub = (flags & MF_POPUP) ? id : 0;
    it->id = (flags & MF_POPUP) ? 0 : id;
    free(it->text);
    it->text = 0;
    it->data = 0;
    it->bmp = 0;
    if (flags & MF_BITMAP) it->bmp = data;
    else if (flags & MF_OWNERDRAW) it->data = data;
    else if (!(flags & MF_SEPARATOR) && data) it->text = strdup(GSTR(data));
}

static void m_CreateMenu(CPU* c) { RET(menu_new()); }
static void m_CreatePopupMenu(CPU* c) { RET(menu_new()); }
static void m_DestroyMenu(CPU* c) {
    if (!menu_get(ARG(0))) { set_last_error(1401); RET(0); return; }
    menu_destroy(ARG(0));
    RET(1);
}
static void m_IsMenu(CPU* c) { RET(menu_get(ARG(0)) != 0); }
static void m_InsertMenuA(CPU* c) {
    Item* it = item_insert(ARG(0), ARG(1), (ARG(2) & MF_BYPOSITION) != 0);
    if (!it) { set_last_error(1401); RET(0); return; }
    item_from_flags(it, ARG(2), ARG(3), ARG(4));
    RET(1);
}
static void m_AppendMenuA(CPU* c) {
    Item* it = item_insert(ARG(0), 0xFFFFFFFFu, 1);
    if (!it) { set_last_error(1401); RET(0); return; }
    item_from_flags(it, ARG(1), ARG(2), ARG(3));
    RET(1);
}
static void m_ModifyMenuA(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2), 0);
    if (!it) { set_last_error(1456); RET(0); return; }
    if (it->sub && !(ARG(2) & MF_POPUP) && it->sub != ARG(3)) menu_destroy(it->sub);
    item_from_flags(it, ARG(2), ARG(3), ARG(4));
    RET(1);
}

static void apply_info(Item* it, uint32_t mii) {
    uint32_t mask = M32(mii + 4);
    if (mask & MIIM_TYPE) {
        it->type = M32(mii + 8);
        if (it->type & MF_BITMAP) { it->bmp = M32(mii + 36); free(it->text); it->text = 0; }
        else if (!(it->type & (MF_SEPARATOR | MF_OWNERDRAW))) set_item_text(it, M32(mii + 36));
    }
    if (mask & MIIM_FTYPE) it->type = M32(mii + 8);
    if (mask & MIIM_STATE) it->state = M32(mii + 12);
    if (mask & MIIM_ID) it->id = M32(mii + 16);
    if (mask & MIIM_SUBMENU) it->sub = M32(mii + 20);
    if (mask & MIIM_CHECKMARKS) { it->bmp_checked = M32(mii + 24); it->bmp_unchecked = M32(mii + 28); }
    if (mask & MIIM_DATA) it->data = M32(mii + 32);
    if (mask & MIIM_STRING) set_item_text(it, M32(mii + 36));
    if ((mask & MIIM_BITMAP) && M32(mii) >= 48) it->bmp = M32(mii + 44);
}

static void m_InsertMenuItemA(CPU* c) {
    Item* it = item_insert(ARG(0), ARG(1), ARG(2) != 0);
    if (!it) { set_last_error(1401); RET(0); return; }
    apply_info(it, ARG(3));
    RET(1);
}
static void m_SetMenuItemInfoA(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2) ? MF_BYPOSITION : 0, 0);
    if (!it) { set_last_error(1456); RET(0); return; }
    apply_info(it, ARG(3));
    RET(1);
}
static void m_GetMenuItemInfoA(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2) ? MF_BYPOSITION : 0, 0);
    uint32_t mii = ARG(3), mask = M32(mii + 4);
    if (!it) { set_last_error(1456); RET(0); return; }
    if (mask & (MIIM_TYPE | MIIM_FTYPE)) M32(mii + 8) = it->type;
    if (mask & MIIM_STATE) M32(mii + 12) = it->state;
    if (mask & MIIM_ID) M32(mii + 16) = it->id;
    if (mask & MIIM_SUBMENU) M32(mii + 20) = it->sub;
    if (mask & MIIM_CHECKMARKS) { M32(mii + 24) = it->bmp_checked; M32(mii + 28) = it->bmp_unchecked; }
    if (mask & MIIM_DATA) M32(mii + 32) = it->data;
    if ((mask & MIIM_BITMAP) && M32(mii) >= 48) M32(mii + 44) = it->bmp;
    if ((mask & MIIM_TYPE) && (it->type & MF_BITMAP)) M32(mii + 36) = it->bmp;
    else if (mask & (MIIM_TYPE | MIIM_STRING)) {
        uint32_t len = it->text ? (uint32_t)strlen(it->text) : 0;
        if (M32(mii + 36) && M32(mii + 40)) snprintf(GSTR(M32(mii + 36)), M32(mii + 40), "%s", it->text ? it->text : "");
        M32(mii + 40) = len;
    }
    RET(1);
}

static void m_GetMenuItemCount(CPU* c) {
    Menu* m = menu_get(ARG(0));
    if (!m) { set_last_error(1401); RET(-1); return; }
    RET(m->n);
}
static void m_GetMenuItemID(CPU* c) {
    Menu* m = menu_get(ARG(0));
    int k = (int)ARG(1);
    if (!m || k < 0 || k >= m->n) { RET(-1); return; }
    RET(m->items[k].sub ? 0xFFFFFFFFu : m->items[k].id);
}
static void m_GetSubMenu(CPU* c) {
    Menu* m = menu_get(ARG(0));
    int k = (int)ARG(1);
    RET(m && k >= 0 && k < m->n ? m->items[k].sub : 0);
}
static void m_GetMenuState(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2), 0);
    if (!it) { RET(-1); return; }
    uint32_t r = it->type | it->state;
    if (it->sub) {
        Menu* sm = menu_get(it->sub);
        r = (uint32_t)(sm ? sm->n : 0) << 8 | MF_POPUP | (it->state & 0xff);
    }
    RET(r);
}
static void m_GetMenuStringA(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(4), 0);
    if (!it) { RET(0); return; }
    const char* t = it->text ? it->text : "";
    if (!ARG(2)) { RET(strlen(t)); return; }
    if (!ARG(3)) { RET(0); return; }
    snprintf(GSTR(ARG(2)), ARG(3), "%s", t);
    RET(strlen(GSTR(ARG(2))));
}
static void m_CheckMenuItem(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2), 0);
    if (!it) { RET(-1); return; }
    uint32_t old = it->state & MF_CHECKED;
    it->state = (it->state & ~(uint32_t)MF_CHECKED) | (ARG(2) & MF_CHECKED);
    RET(old);
}
static void m_EnableMenuItem(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2), 0);
    if (!it) { RET(-1); return; }
    uint32_t old = it->state & (MF_GRAYED | MF_DISABLED);
    it->state = (it->state & ~(uint32_t)(MF_GRAYED | MF_DISABLED)) | (ARG(2) & (MF_GRAYED | MF_DISABLED));
    RET(old);
}
static void m_RemoveMenu(CPU* c) {
    uint32_t owner = 0;
    Item* it = item_find(ARG(0), ARG(1), ARG(2), &owner);
    Menu* m = menu_get(owner);
    if (!it || !m) { set_last_error(1456); RET(0); return; }
    free(it->text);
    int at = (int)(it - m->items);
    memmove(&m->items[at], &m->items[at + 1], sizeof(Item) * (size_t)(m->n - at - 1));
    m->n--;
    RET(1);
}
static void m_DeleteMenu(CPU* c) {
    Item* it = item_find(ARG(0), ARG(1), ARG(2), 0);
    if (it && it->sub) menu_destroy(it->sub);
    m_RemoveMenu(c);
}

static void m_GetMenu(CPU* c) { RET(hle_window_menu(ARG(0))); }
static void m_SetMenu(CPU* c) {
    if (!hle_window_exists(ARG(0)) || (ARG(1) && !menu_get(ARG(1)))) { set_last_error(1400); RET(0); return; }
    hle_window_set_menu(c, ARG(0), ARG(1));
    RET(1);
}
/* The menu bar lives in the non-client area, which nothing composites here: there are no pixels to refresh. */
static void m_DrawMenuBar(CPU* c) { RET(hle_window_exists(ARG(0))); }

static struct { uint32_t hwnd, menu; } sysmenus[64];

static uint32_t standard_system_menu(void) {
    static const struct { uint32_t id; const char* text; } items[] = {
        {0xF120, "&Restore"}, {0xF010, "&Move"}, {0xF000, "&Size"}, {0xF020, "Mi&nimize"}, {0xF030, "Ma&ximize"},
        {0, 0}, {0xF060, "&Close\tAlt+F4"},
    };
    uint32_t h = menu_new();
    for (size_t k = 0; h && k < sizeof items / sizeof items[0]; k++) {
        Item* it = item_insert(h, 0xFFFFFFFFu, 1);
        if (!items[k].text) { it->type = MF_SEPARATOR; continue; }
        it->id = items[k].id;
        it->text = strdup(items[k].text);
    }
    return h;
}
static void m_GetSystemMenu(CPU* c) {
    uint32_t hwnd = ARG(0);
    int slot = -1;
    if (!hle_window_exists(hwnd)) { RET(0); return; }
    for (int k = 0; k < 64; k++)
        if (sysmenus[k].hwnd == hwnd && menu_get(sysmenus[k].menu)) slot = k;
    if (ARG(1)) {
        if (slot >= 0) { menu_destroy(sysmenus[slot].menu); sysmenus[slot].hwnd = 0; }
        RET(0);
        return;
    }
    if (slot < 0) {
        for (int k = 0; k < 64 && slot < 0; k++)
            if (!sysmenus[k].hwnd || !hle_window_exists(sysmenus[k].hwnd) || !menu_get(sysmenus[k].menu)) slot = k;
        if (slot < 0) { RET(0); return; }
        sysmenus[slot].hwnd = hwnd;
        sysmenus[slot].menu = standard_system_menu();
    }
    RET(sysmenus[slot].menu);
}

/* Popup menus are drawn by the page; submenus are initialised up front since the page opens them on its own. */
static size_t menu_json(CPU* c, uint32_t h, uint32_t hwnd, char* out, size_t cap, int notify) {
    Menu* m = menu_get(h);
    size_t n = 0;
    if (!m || cap < 64) return 0;
    n += (size_t)snprintf(out + n, cap - n, "[");
    for (int k = 0; k < m->n && n + 64 < cap; k++) {
        Item* it = &m->items[k];
        if (it->sub && notify) hle_send(c, hwnd, WM_INITMENUPOPUP, it->sub, (uint32_t)k);
        m = menu_get(h);
        if (!m || k >= m->n) break;
        it = &m->items[k];
        n += (size_t)snprintf(out + n, cap - n, "%s{\"id\":%u,\"text\":", k ? "," : "", it->id);
        n += hle_json_str(out + n, cap - n, it->text ? it->text : "");
        n += (size_t)snprintf(out + n, cap - n, ",\"separator\":%d,\"disabled\":%d,\"checked\":%d,\"radio\":%d,\"default\":%d",
                              (it->type & MF_SEPARATOR) != 0, (it->state & (MF_GRAYED | MF_DISABLED)) != 0,
                              (it->state & MF_CHECKED) != 0, (it->type & MFT_RADIOCHECK) != 0, (it->state & MFS_DEFAULT) != 0);
        if (it->sub) {
            n += (size_t)snprintf(out + n, cap - n, ",\"sub\":");
            n += menu_json(c, it->sub, hwnd, out + n, cap - n, notify);
        }
        n += (size_t)snprintf(out + n, cap - n, "}");
    }
    n += (size_t)snprintf(out + n, cap - n, "]");
    return n;
}

static int is_system_menu(uint32_t h) {
    for (int k = 0; k < 64; k++)
        if (sysmenus[k].hwnd && sysmenus[k].menu == h) return 1;
    return 0;
}

static uint32_t track_popup(CPU* c, uint32_t h, uint32_t flags, int x, int y, uint32_t hwnd) {
    int notify = !(flags & 0x80);
    if (!menu_get(h)) { set_last_error(1401); return 0; }
    if (!hle_window_exists(hwnd)) { set_last_error(1400); return 0; }
    if (notify) {
        hle_send(c, hwnd, WM_ENTERMENULOOP, 1, 0);
        hle_send(c, hwnd, WM_INITMENU, h, 0);
        hle_send(c, hwnd, WM_INITMENUPOPUP, h, is_system_menu(h) ? 0x10000u : 0);
    }
    hle_screen_to_page(&x, &y);
    size_t cap = 1 << 16;
    char* json = malloc(cap);
    size_t n = (size_t)snprintf(json, cap, "{\"x\":%d,\"y\":%d,\"align\":%u,\"items\":", x, y, flags & 0x3C);
    n += menu_json(c, h, hwnd, json + n, cap - n, notify);
    snprintf(json + n, cap - n, "}");
    int r = hle_modal(c, "menu", json);
    free(json);
    if (r < 0) r = 0;
    if (notify) {
        hle_send(c, hwnd, WM_UNINITMENUPOPUP, h, 0);
        hle_send(c, hwnd, WM_EXITMENULOOP, 1, 0);
    }
    if (flags & 0x100) return (uint32_t)r;
    if (r && notify) hle_post(hwnd, r >= 0xF000 && is_system_menu(h) ? WM_SYSCOMMAND : WM_COMMAND, (uint32_t)r, 0);
    return r != 0;
}
static void m_TrackPopupMenu(CPU* c) { RET(track_popup(c, ARG(0), ARG(1), (int)ARG(2), (int)ARG(3), ARG(5))); }
static void m_TrackPopupMenuEx(CPU* c) { RET(track_popup(c, ARG(0), ARG(1), (int)ARG(2), (int)ARG(3), ARG(4))); }

/* ---- clipboard: a process-local store, mirrored to the browser's clipboard on close and refreshed from a paste ---- */

enum { CF_TEXT = 1, CF_OEMTEXT = 7, CF_UNICODETEXT = 13, CF_LOCALE = 16, WM_RENDERFORMAT = 0x305,
       WM_DESTROYCLIPBOARD = 0x307 };
typedef struct { uint32_t fmt, handle; int delayed; } Clip;
static Clip clips[32];
static int nclips;
static int clip_open, clip_changed;
static uint32_t clip_open_hwnd, clip_owner, clip_seq = 1;
static int paste_seq;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, js_paste_read, (uint16_t* out, int cap, int* seq), {
    const buf = globalThis.pasteBuf;
    if (!buf) return -1;
    const s = Atomics.load(buf, 0);
    if (s === HEAP32[seq >> 2]) return -1;
    HEAP32[seq >> 2] = s;
    const len = Math.min(Atomics.load(buf, 1), cap);
    const chars = new Uint16Array(buf.buffer, 8, len);
    for (let i = 0; i < len; i++) HEAPU16[(out >> 1) + i] = chars[i];
    return len;
});
EM_JS(void, js_clipboard_write, (const uint16_t* text, int n), {
    if (typeof postMessage !== 'function') return;
    let s = "";
    for (let i = 0; i < n; i++) s += String.fromCharCode(HEAPU16[(text >> 1) + i]);
    postMessage({ type: 'clipboard', text: s });
});
#else
static int js_paste_read(uint16_t* out, int cap, int* seq) { (void)out; (void)cap; (void)seq; return -1; }
static void js_clipboard_write(const uint16_t* text, int n) { (void)text; (void)n; }
#endif

static void clip_clear(void) {
    for (int k = 0; k < nclips; k++)
        if (clips[k].handle) port_global_free(clips[k].handle);
    nclips = 0;
}

static Clip* clip_find(uint32_t fmt) {
    for (int k = 0; k < nclips; k++)
        if (clips[k].fmt == fmt) return &clips[k];
    return 0;
}

static void clip_put(uint32_t fmt, uint32_t handle, int delayed) {
    Clip* k = clip_find(fmt);
    if (!k && nclips < 32) k = &clips[nclips++];
    if (!k) return;
    if (k->handle && k->handle != handle) port_global_free(k->handle);
    *k = (Clip){fmt, handle, delayed};
}

/* Text pasted in the page replaces the clipboard, as if another application had copied it. */
static void clip_sync(CPU* c) {
    static uint16_t buf[32768];
    int n = js_paste_read(buf, 32768, &paste_seq);
    if (n < 0) return;
    if (clip_owner) hle_send(c, clip_owner, WM_DESTROYCLIPBOARD, 0, 0);
    clip_clear();
    clip_owner = 0;
    clip_seq++;
    uint32_t h = port_global_alloc((uint32_t)n * 2 + 2);
    memcpy(GPTR(h), buf, (size_t)n * 2);
    M16(h + (uint32_t)n * 2) = 0;
    clip_put(CF_UNICODETEXT, h, 0);
}

/* CF_TEXT, CF_OEMTEXT and CF_UNICODETEXT are interchangeable: a missing one is converted from another. */
static uint32_t clip_synthesize(uint32_t fmt) {
    if (fmt != CF_TEXT && fmt != CF_OEMTEXT && fmt != CF_UNICODETEXT && fmt != CF_LOCALE) return 0;
    if (fmt == CF_LOCALE) {
        if (!clip_find(CF_TEXT) && !clip_find(CF_UNICODETEXT) && !clip_find(CF_OEMTEXT)) return 0;
        uint32_t h = port_global_alloc(4);
        M32(h) = 0x0409;
        clip_put(CF_LOCALE, h, 0);
        return h;
    }
    Clip* src = fmt == CF_UNICODETEXT ? (clip_find(CF_TEXT) ? clip_find(CF_TEXT) : clip_find(CF_OEMTEXT)) : clip_find(CF_UNICODETEXT);
    if (!src && fmt != CF_UNICODETEXT) src = clip_find(fmt == CF_TEXT ? CF_OEMTEXT : CF_TEXT);
    if (!src || !src->handle) return 0;
    uint32_t s = src->handle, h;
    if (fmt == CF_UNICODETEXT) {
        uint32_t n = (uint32_t)strlen(GSTR(s));
        h = port_global_alloc(n * 2 + 2);
        for (uint32_t k = 0; k <= n; k++) M16(h + 2 * k) = M8(s + k);
    } else if (src->fmt == CF_UNICODETEXT) {
        uint32_t n = 0;
        while (M16(s + 2 * n)) n++;
        h = port_global_alloc(n + 1);
        for (uint32_t k = 0; k <= n; k++) { uint16_t ch = M16(s + 2 * k); M8(h + k) = (uint8_t)(ch < 256 ? ch : '?'); }
    } else {
        uint32_t n = (uint32_t)strlen(GSTR(s));
        h = port_global_alloc(n + 1);
        memcpy(GPTR(h), GPTR(s), n + 1);
    }
    clip_put(fmt, h, 0);
    return h;
}

static void u_OpenClipboard(CPU* c) {
    if (clip_open) { set_last_error(5); RET(0); return; }
    clip_open = 1;
    clip_open_hwnd = ARG(0);
    clip_sync(c);
    RET(1);
}
static void u_CloseClipboard(CPU* c) {
    if (!clip_open) { set_last_error(1418); RET(0); return; }
    clip_open = 0;
    if (clip_changed) {
        clip_changed = 0;
        uint32_t h = clip_find(CF_UNICODETEXT) ? clip_find(CF_UNICODETEXT)->handle : clip_synthesize(CF_UNICODETEXT);
        if (h) {
            int n = 0;
            while (M16(h + 2 * (uint32_t)n)) n++;
            js_clipboard_write((const uint16_t*)GPTR(h), n);
        }
    }
    RET(1);
}
static void u_EmptyClipboard(CPU* c) {
    if (!clip_open) { set_last_error(1418); RET(0); return; }
    if (clip_owner && clip_owner != clip_open_hwnd) hle_send(c, clip_owner, WM_DESTROYCLIPBOARD, 0, 0);
    clip_clear();
    clip_owner = clip_open_hwnd;
    clip_changed = 1;
    clip_seq++;
    RET(1);
}
static void u_SetClipboardData(CPU* c) {
    if (!clip_open) { set_last_error(1418); RET(0); return; }
    clip_put(ARG(0), ARG(1), ARG(1) == 0);
    if (ARG(0) == CF_TEXT || ARG(0) == CF_UNICODETEXT || ARG(0) == CF_OEMTEXT) {
        uint32_t others[3] = {CF_TEXT, CF_UNICODETEXT, CF_OEMTEXT};
        for (int k = 0; k < 3; k++) {
            Clip* o = clip_find(others[k]);
            if (others[k] != ARG(0) && o && !o->delayed) { port_global_free(o->handle); *o = clips[--nclips]; }
        }
    }
    clip_changed = 1;
    clip_seq++;
    RET(ARG(1));
}
static void u_GetClipboardData(CPU* c) {
    if (!clip_open) { set_last_error(1418); RET(0); return; }
    Clip* k = clip_find(ARG(0));
    if (k && k->delayed && clip_owner) {
        hle_send(c, clip_owner, WM_RENDERFORMAT, ARG(0), 0);
        k = clip_find(ARG(0));
    }
    if (k && k->handle) { RET(k->handle); return; }
    RET(clip_synthesize(ARG(0)));
}
static int clip_has(uint32_t fmt) {
    if (clip_find(fmt)) return 1;
    int text = clip_find(CF_TEXT) || clip_find(CF_UNICODETEXT) || clip_find(CF_OEMTEXT);
    return text && (fmt == CF_TEXT || fmt == CF_UNICODETEXT || fmt == CF_OEMTEXT || fmt == CF_LOCALE);
}
static void u_IsClipboardFormatAvailable(CPU* c) {
    clip_sync(c);
    RET(clip_has(ARG(0)));
}
static void u_EnumClipboardFormats(CPU* c) {
    if (!clip_open) { set_last_error(1418); RET(0); return; }
    uint32_t list[40];
    int n = 0;
    for (int k = 0; k < nclips; k++) list[n++] = clips[k].fmt;
    uint32_t synth[4] = {CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE};
    for (int k = 0; k < 4; k++)
        if (!clip_find(synth[k]) && clip_has(synth[k])) list[n++] = synth[k];
    int at = 0;
    if (ARG(0))
        for (int k = 0; k < n; k++)
            if (list[k] == ARG(0)) at = k + 1;
    set_last_error(0);
    RET(at < n && (ARG(0) == 0 || at > 0) ? list[at] : 0);
}
static void u_CountClipboardFormats(CPU* c) {
    int n = 0;
    uint32_t synth[4] = {CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE};
    n = nclips;
    for (int k = 0; k < 4; k++)
        if (!clip_find(synth[k]) && clip_has(synth[k])) n++;
    RET(n);
}
static void u_RegisterClipboardFormatA(CPU* c) { RET(ARG(0) ? hle_atom_add(GSTR(ARG(0))) : 0); }
static void u_GetClipboardOwner(CPU* c) { RET(clip_owner); }
static void u_GetClipboardSequenceNumber(CPU* c) { RET(clip_seq); }

#define M(n, a) {"user32.dll", #n, a, m_##n}
#define C(n, a) {"user32.dll", #n, a, u_##n}

static const Hle menu_table[] = {
    M(CreateMenu, 0), M(CreatePopupMenu, 0), M(DestroyMenu, 1), M(IsMenu, 1), M(InsertMenuA, 5), M(AppendMenuA, 4),
    M(ModifyMenuA, 5), M(InsertMenuItemA, 4), M(SetMenuItemInfoA, 4), M(GetMenuItemInfoA, 4), M(GetMenuItemCount, 1),
    M(GetMenuItemID, 2), M(GetSubMenu, 2), M(GetMenuState, 3), M(GetMenuStringA, 5), M(CheckMenuItem, 3),
    M(EnableMenuItem, 3), M(RemoveMenu, 3), M(DeleteMenu, 3), M(GetMenu, 1), M(SetMenu, 2), M(DrawMenuBar, 1),
    M(GetSystemMenu, 2), M(TrackPopupMenu, 7), M(TrackPopupMenuEx, 6),
    C(OpenClipboard, 1), C(CloseClipboard, 0), C(EmptyClipboard, 0), C(SetClipboardData, 2), C(GetClipboardData, 1),
    C(IsClipboardFormatAvailable, 1), C(EnumClipboardFormats, 1), C(CountClipboardFormats, 0),
    C(RegisterClipboardFormatA, 1), C(GetClipboardOwner, 0), C(GetClipboardSequenceNumber, 0),
};

void hle_menu_register(void) { hle_register(menu_table, sizeof menu_table / sizeof menu_table[0]); }
