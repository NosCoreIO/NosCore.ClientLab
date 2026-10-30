/* Control channel for automated runs of the native client: NOSRECOMP_CONTROL=<port> listens on 127.0.0.1:<port>
   for one line-based client at a time (recomp/native_drive.py). Input is posted to the game window and mirrored
   in GetCursorPos / GetKeyState / GetKeyboardState, so a run does not need the window in the foreground or the
   real cursor. Frames come from the backbuffer at the next Present, so a covered or minimised RDP desktop still
   gives screenshots.

   Commands (coordinates are backbuffer pixels, keys are Windows virtual-key codes):
     ping                          ok
     size                          ok <width> <height>          (backbuffer, 0 0 before the device exists)
     move <x> <y>                  ok
     click <x> <y> [left|right]    ok                           (move, press, release)
     down|up <x> <y> [left|right]  ok
     key <vk>                      ok                           (press and release)
     keydown|keyup <vk>            ok
     type <text>                   ok                           (WM_CHAR per character)
     shot <path>                   ok <width> <height> | err <reason>   (PNG of the next presented frame)
     mark                          ok <offset>                  (end of the log so far)
     log <offset>                  ok <next offset> <bytes>\n<bytes>
     wait <offset> <ms> <text>     ok <offset after the match> | timeout <offset>
   The log is everything rt_log writes, kept in memory; offsets count bytes from the start of the run. */
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt_control.h"

static int enabled;
static HWND game_window;
static volatile LONG mouse_x = -1, mouse_y = -1;
static volatile uint8_t keys_down[256];

/* ---- log mirror ---- */

#define LOG_CAP (16u << 20)
static SRWLOCK log_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE log_grew = CONDITION_VARIABLE_INIT;
static char* log_buf;
static uint64_t log_base, log_end; /* log_buf holds bytes [log_base, log_end) */

void ctl_log(const char* line) {
    size_t n;
    if (!enabled) return;
    n = strlen(line);
    AcquireSRWLockExclusive(&log_lock);
    if (log_end - log_base + n + 1 > LOG_CAP) {
        /* Drop the older half; offsets keep counting from the start of the run. */
        uint64_t drop = (log_end - log_base) / 2;
        memmove(log_buf, log_buf + drop, (size_t)(log_end - log_base - drop));
        log_base += drop;
    }
    if (n + 1 <= LOG_CAP) {
        memcpy(log_buf + (log_end - log_base), line, n);
        log_buf[log_end - log_base + n] = '\n';
        log_end += n + 1;
    }
    ReleaseSRWLockExclusive(&log_lock);
    WakeAllConditionVariable(&log_grew);
}

/* ---- frame capture as PNG (stored deflate blocks: no zlib needed, files are just bigger) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t* p, size_t n) {
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t b[4];
    uint32_t c;
    put32(b, n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    c = crc32_update(0xffffffffu, (const uint8_t*)type, 4);
    c = crc32_update(c, data, n) ^ 0xffffffffu;
    put32(b, c);
    fwrite(b, 1, 4, f);
}

static int write_png(const char* path, const uint8_t* bgra, UINT w, UINT h, int pitch) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 3), blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4, pos = 0, done = 0;
    uint8_t *raw = malloc(raw_n), *z = malloc(z_n), hdr[13];
    uint32_t a = 1, b = 0;
    FILE* f;
    if (!raw || !z || fopen_s(&f, path, "wb")) { free(raw); free(z); return 0; }
    for (UINT y = 0; y < h; y++) {
        uint8_t* o = raw + (size_t)y * (1 + (size_t)w * 3);
        const uint8_t* s = bgra + (size_t)y * pitch;
        *o++ = 0;
        for (UINT x = 0; x < w; x++, s += 4) { *o++ = s[2]; *o++ = s[1]; *o++ = s[0]; }
    }
    for (size_t k = 0; k < raw_n; k++) { a = (a + raw[k]) % 65521; b = (b + a) % 65521; }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    while (done < raw_n) {
        size_t n = raw_n - done > 65535 ? 65535 : raw_n - done;
        z[pos++] = done + n == raw_n;
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + done, n);
        pos += n;
        done += n;
    }
    put32(z + pos, (b << 16) | a);
    pos += 4;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put32(hdr, w);
    put32(hdr + 4, h);
    hdr[8] = 8; hdr[9] = 2; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
    chunk(f, "IHDR", hdr, 13);
    chunk(f, "IDAT", z, (uint32_t)pos);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

static char shot_path[MAX_PATH];
static volatile LONG shot_pending;
static HANDLE shot_done;
static UINT shot_w, shot_h;
static int shot_ok;
static volatile LONG bb_w, bb_h;

static void shot_sink(void* unused, const uint8_t* bgra, UINT w, UINT h, int pitch) {
    (void)unused;
    shot_w = w;
    shot_h = h;
    shot_ok = write_png(shot_path, bgra, w, h, pitch);
}

void ctl_present(void* dev) {
    if (!enabled) return;
    if (!bb_w) {
        UINT w, h;
        if (rt_backbuffer_size(dev, &w, &h)) { bb_w = (LONG)w; bb_h = (LONG)h; }
    }
    if (InterlockedCompareExchange(&shot_pending, 0, 1) == 1) {
        shot_ok = 0;
        rt_read_backbuffer(dev, shot_sink, 0);
        SetEvent(shot_done);
    }
}

void ctl_set_window(HWND w) {
    if (w) game_window = w;
    bb_w = bb_h = 0;
}

/* ---- input ---- */

static void post_mouse(UINT msg, int x, int y, WPARAM extra) {
    WPARAM mk = extra;
    if (keys_down[VK_LBUTTON]) mk |= MK_LBUTTON;
    if (keys_down[VK_RBUTTON]) mk |= MK_RBUTTON;
    if (keys_down[VK_SHIFT]) mk |= MK_SHIFT;
    if (keys_down[VK_CONTROL]) mk |= MK_CONTROL;
    InterlockedExchange(&mouse_x, x);
    InterlockedExchange(&mouse_y, y);
    PostMessageA(game_window, msg, mk, MAKELPARAM(x, y));
}

static void mouse_button(int x, int y, int right, int down) {
    int vk = right ? VK_RBUTTON : VK_LBUTTON;
    post_mouse(WM_MOUSEMOVE, x, y, 0);
    keys_down[vk] = (uint8_t)down;
    post_mouse(right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP), x, y, 0);
}

static void key_event(int vk, int down) {
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | ((LPARAM)scan << 16) | (down ? 0 : (LPARAM)0xC0000000);
    keys_down[vk & 0xff] = (uint8_t)down;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) keys_down[VK_SHIFT] = (uint8_t)down;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) keys_down[VK_CONTROL] = (uint8_t)down;
    PostMessageA(game_window, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp);
    /* TranslateMessage would make WM_CHAR from the real keyboard state, which never saw this key. */
    if (down) {
        BYTE ks[256] = {0};
        WORD ch;
        for (int k = 0; k < 256; k++) if (keys_down[k]) ks[k] = 0x80;
        if (ToAscii((UINT)vk, scan, ks, &ch, 0) == 1 && vk != VK_RETURN && vk != VK_ESCAPE && vk != VK_BACK && vk != VK_TAB)
            PostMessageA(game_window, WM_CHAR, ch & 0xff, lp);
        else if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_BACK || vk == VK_TAB)
            PostMessageA(game_window, WM_CHAR, vk == VK_ESCAPE ? 27 : vk == VK_RETURN ? 13 : vk, lp);
    }
}

static BOOL WINAPI ov_GetCursorPos(LPPOINT p) {
    if (mouse_x < 0 || !game_window) return GetCursorPos(p);
    p->x = mouse_x;
    p->y = mouse_y;
    return ClientToScreen(game_window, p);
}

static SHORT WINAPI ov_GetKeyState(int vk) {
    SHORT real = GetKeyState(vk);
    return keys_down[vk & 0xff] ? (SHORT)(real | 0x8000) : real;
}

static BOOL WINAPI ov_GetKeyboardState(PBYTE ks) {
    BOOL r = GetKeyboardState(ks);
    for (int k = 0; k < 256; k++) if (keys_down[k]) ks[k] |= 0x80;
    return r;
}

/* ---- wsock32: the same connect / send / recv / closesocket lines as the browser build's log ---- */

typedef int (WINAPI* ConnectFn)(SOCKET, const struct sockaddr*, int);
typedef int (WINAPI* SendRecvFn)(SOCKET, char*, int, int);
typedef int (WINAPI* CloseFn)(SOCKET);
static ConnectFn real_connect;
static SendRecvFn real_send, real_recv;
static CloseFn real_closesocket;

static int WINAPI ov_connect(SOCKET s, const struct sockaddr* a, int n) {
    if (a && a->sa_family == AF_INET) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)a;
        const uint8_t* ip = (const uint8_t*)&in->sin_addr;
        rt_log("connect %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(in->sin_port));
    }
    return real_connect(s, a, n);
}

static int WINAPI ov_send(SOCKET s, char* buf, int n, int flags) {
    int r = real_send(s, buf, n, flags);
    rt_log("send %08x: %d bytes", (unsigned)s, r);
    return r;
}

static int WINAPI ov_recv(SOCKET s, char* buf, int n, int flags) {
    int r = real_recv(s, buf, n, flags);
    if (r > 0) rt_log("recv %08x: %d bytes", (unsigned)s, r);
    return r;
}

static int WINAPI ov_closesocket(SOCKET s) {
    rt_log("closesocket %08x", (unsigned)s);
    return real_closesocket(s);
}

void* ctl_override(const char* dll, const char* name) {
    if (!enabled) return 0;
    if (!_stricmp(dll, "user32.dll")) {
        if (!strcmp(name, "GetCursorPos")) return (void*)ov_GetCursorPos;
        if (!strcmp(name, "GetKeyState")) return (void*)ov_GetKeyState;
        if (!strcmp(name, "GetKeyboardState")) return (void*)ov_GetKeyboardState;
    }
    if (!_stricmp(dll, "wsock32.dll")) {
        HMODULE ws = LoadLibraryA("wsock32.dll");
        if (!ws) return 0;
        if (!strcmp(name, "connect")) { real_connect = (ConnectFn)GetProcAddress(ws, "connect"); return (void*)ov_connect; }
        if (!strcmp(name, "send")) { real_send = (SendRecvFn)GetProcAddress(ws, "send"); return (void*)ov_send; }
        if (!strcmp(name, "recv")) { real_recv = (SendRecvFn)GetProcAddress(ws, "recv"); return (void*)ov_recv; }
        if (!strcmp(name, "closesocket")) { real_closesocket = (CloseFn)GetProcAddress(ws, "closesocket"); return (void*)ov_closesocket; }
    }
    return 0;
}

/* ---- server ---- */

static int send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int r = send(s, p, n > 0x10000 ? 0x10000 : (int)n, 0);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int reply(SOCKET s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    strcat_s(buf, sizeof buf, "\n");
    return send_all(s, buf, strlen(buf));
}

static int is_right(const char* arg) { return arg && !strcmp(arg, "right"); }

static int command(SOCKET s, char* line) {
    char *cmd, *rest = 0, *a1, *a2, *a3;
    cmd = strtok_s(line, " ", &rest);
    if (!cmd) return reply(s, "err empty");
    if (!strcmp(cmd, "type")) {
        for (const char* p = rest; p && *p; p++) {
            PostMessageA(game_window, WM_CHAR, (uint8_t)*p, 1);
            Sleep(60);
        }
        return reply(s, "ok");
    }
    if (!strcmp(cmd, "wait")) {
        char *after_off, *text;
        uint64_t off = _strtoui64(rest ? rest : "0", &after_off, 10), found = 0;
        DWORD ms = strtoul(after_off, &text, 10), start = GetTickCount();
        while (*text == ' ') text++;
        AcquireSRWLockExclusive(&log_lock);
        for (;;) {
            uint64_t from = off < log_base ? log_base : off;
            size_t tn = strlen(text);
            if (tn && log_end - from >= tn) {
                const char* hay = log_buf + (from - log_base);
                for (size_t k = 0; k + tn <= log_end - from; k++)
                    if (!memcmp(hay + k, text, tn)) { found = from + k + tn; break; }
            }
            if (found) break;
            {
                DWORD spent = GetTickCount() - start;
                if (spent >= ms) break;
                SleepConditionVariableSRW(&log_grew, &log_lock, ms - spent, 0);
            }
        }
        off = log_end;
        ReleaseSRWLockExclusive(&log_lock);
        return found ? reply(s, "ok %llu", found) : reply(s, "timeout %llu", off);
    }
    a1 = strtok_s(0, " ", &rest);
    a2 = strtok_s(0, " ", &rest);
    a3 = strtok_s(0, " ", &rest);
    if (!strcmp(cmd, "ping")) return reply(s, "ok");
    if (!strcmp(cmd, "size")) return reply(s, "ok %ld %ld", bb_w, bb_h);
    if (!game_window && strcmp(cmd, "mark") && strcmp(cmd, "log") && strcmp(cmd, "shot")) return reply(s, "err no window yet");
    if (!strcmp(cmd, "move") && a2) { post_mouse(WM_MOUSEMOVE, atoi(a1), atoi(a2), 0); return reply(s, "ok"); }
    if (!strcmp(cmd, "click") && a2) {
        mouse_button(atoi(a1), atoi(a2), is_right(a3), 1);
        Sleep(120);
        mouse_button(atoi(a1), atoi(a2), is_right(a3), 0);
        return reply(s, "ok");
    }
    if ((!strcmp(cmd, "down") || !strcmp(cmd, "up")) && a2) { mouse_button(atoi(a1), atoi(a2), is_right(a3), cmd[0] == 'd'); return reply(s, "ok"); }
    if (!strcmp(cmd, "key") && a1) { key_event(atoi(a1), 1); Sleep(80); key_event(atoi(a1), 0); return reply(s, "ok"); }
    if ((!strcmp(cmd, "keydown") || !strcmp(cmd, "keyup")) && a1) { key_event(atoi(a1), cmd[3] == 'd'); return reply(s, "ok"); }
    if (!strcmp(cmd, "shot") && a1) {
        /* The path may contain spaces: take everything after "shot ". */
        if (a2) a1[strlen(a1)] = ' ';
        if (a3) a2[strlen(a2)] = ' ';
        strcpy_s(shot_path, sizeof shot_path, a1);
        ResetEvent(shot_done);
        InterlockedExchange(&shot_pending, 1);
        if (WaitForSingleObject(shot_done, 10000) != WAIT_OBJECT_0) {
            InterlockedExchange(&shot_pending, 0);
            return reply(s, "err no frame presented in 10 s");
        }
        return shot_ok ? reply(s, "ok %u %u", shot_w, shot_h) : reply(s, "err could not read or write the frame");
    }
    if (!strcmp(cmd, "mark")) {
        uint64_t end;
        AcquireSRWLockShared(&log_lock);
        end = log_end;
        ReleaseSRWLockShared(&log_lock);
        return reply(s, "ok %llu", end);
    }
    if (!strcmp(cmd, "log")) {
        uint64_t off = a1 ? _strtoui64(a1, 0, 10) : 0, end;
        char* copy;
        size_t n;
        AcquireSRWLockShared(&log_lock);
        if (off < log_base) off = log_base;
        if (off > log_end) off = log_end;
        end = log_end;
        n = (size_t)(end - off);
        copy = malloc(n + 1);
        if (copy) memcpy(copy, log_buf + (off - log_base), n);
        ReleaseSRWLockShared(&log_lock);
        if (!copy) return reply(s, "err out of memory");
        reply(s, "ok %llu %zu", end, n);
        send_all(s, copy, n);
        free(copy);
        return 1;
    }
    return reply(s, "err unknown command or missing arguments");
}

static DWORD WINAPI server_thread(LPVOID port_arg) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)(uintptr_t)port_arg);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr*)&a, sizeof a) || listen(ls, 1)) {
        rt_log("control: cannot listen on 127.0.0.1:%u (%d)", (unsigned)(uintptr_t)port_arg, WSAGetLastError());
        return 1;
    }
    rt_log("control: listening on 127.0.0.1:%u", (unsigned)(uintptr_t)port_arg);
    for (;;) {
        SOCKET s = accept(ls, 0, 0);
        char line[4096];
        size_t used = 0;
        if (s == INVALID_SOCKET) continue;
        for (;;) {
            int r = recv(s, line + used, (int)(sizeof line - 1 - used), 0);
            char* nl;
            if (r <= 0) break;
            used += (size_t)r;
            line[used] = 0;
            while ((nl = strchr(line, '\n'))) {
                size_t len = (size_t)(nl - line);
                if (len && line[len - 1] == '\r') line[len - 1] = 0;
                *nl = 0;
                if (!command(s, line)) { used = 0; break; }
                memmove(line, nl + 1, used - len - 1 + 1);
                used -= len + 1;
            }
            if (used == sizeof line - 1) used = 0;
        }
        closesocket(s);
    }
}

void ctl_start(void) {
    char v[16];
    WSADATA wd;
    unsigned port;
    if (!GetEnvironmentVariableA("NOSRECOMP_CONTROL", v, sizeof v)) return;
    /* CEF helpers inherit the environment; only the main client process listens. */
    if (strstr(GetCommandLineA(), "--type=")) return;
    port = (unsigned)atoi(v);
    if (!port || !(log_buf = malloc(LOG_CAP))) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    shot_done = CreateEventA(0, TRUE, FALSE, 0);
    WSAStartup(MAKEWORD(2, 2), &wd);
    enabled = 1;
    CreateThread(0, 0, server_thread, (LPVOID)(uintptr_t)port, 0, 0);
}
