/* kernel32 odds and ends, private profile (.ini) files, shlwapi paths, the IME and the remaining Gameforge calls. */
#include "port.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RET(v) (c->eax = (uint32_t)(v))

static uint32_t copy_out(uint32_t dst, uint32_t size, const char* s) {
    if (!dst || !size) return 0;
    uint32_t n = (uint32_t)strlen(s);
    if (n >= size) n = size - 1;
    memcpy(GPTR(dst), s, n);
    M8(dst + n) = 0;
    return n;
}

static void host_path(uint32_t guest, char* out, size_t n) { port_host_path(guest ? GSTR(guest) : "", out, n); }

/* ---- private profile files ---- */

typedef struct { char section[128], key[128], value[512]; } IniLine;

/* Reads an .ini file into section/key/value triples; a section header is a line with an empty key. */
static int ini_read(const char* path, IniLine** out) {
    FILE* f = fopen(path, "rb");
    int n = 0, cap = 0;
    IniLine* lines = 0;
    char buf[1024], section[128] = "";
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            char* s = buf;
            while (*s == ' ' || *s == '\t') s++;
            size_t len = strlen(s);
            while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) s[--len] = 0;
            if (!len || *s == ';') continue;
            if (n == cap) { cap = cap ? cap * 2 : 32; lines = realloc(lines, sizeof *lines * (size_t)cap); }
            IniLine* l = &lines[n];
            memset(l, 0, sizeof *l);
            if (*s == '[') {
                char* e = strchr(s, ']');
                if (e) *e = 0;
                snprintf(section, sizeof section, "%s", s + 1);
                snprintf(l->section, sizeof l->section, "%s", section);
                n++;
                continue;
            }
            char* eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char* k = s + strlen(s);
            while (k > s && (k[-1] == ' ' || k[-1] == '\t')) *--k = 0;
            char* v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            snprintf(l->section, sizeof l->section, "%s", section);
            snprintf(l->key, sizeof l->key, "%s", s);
            snprintf(l->value, sizeof l->value, "%s", v);
            n++;
        }
        fclose(f);
    }
    *out = lines;
    return n;
}

static void ini_write(const char* path, IniLine* lines, int n) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (int k = 0; k < n; k++) {
        if (!lines[k].key[0]) fprintf(f, "%s[%s]\r\n", k ? "\r\n" : "", lines[k].section);
        else fprintf(f, "%s=%s\r\n", lines[k].key, lines[k].value);
    }
    fclose(f);
}

/* A relative profile name lives in the Windows directory; here that is the game folder. */
static void profile_path(uint32_t file, char* out, size_t n) {
    const char* name = file ? GSTR(file) : "win.ini";
    if (strchr(name, '\\') || strchr(name, '/')) port_host_path(name, out, n);
    else {
        char guest[300];
        snprintf(guest, sizeof guest, "C:\\Nostale\\%s", name);
        port_host_path(guest, out, n);
    }
}

/* Lists (NUL-separated, double-NUL terminated) or looks up one value, with GetPrivateProfileString's truncation rules. */
static void k_GetPrivateProfileStringA(CPU* c) {
    uint32_t app = ARG(0), key = ARG(1), def = ARG(2), buf = ARG(3), size = ARG(4);
    char path[600];
    profile_path(ARG(5), path, sizeof path);
    IniLine* lines;
    int n = ini_read(path, &lines);
    if (!buf || !size) { free(lines); RET(0); return; }
    if (!app || !key) {
        uint32_t pos = 0;
        for (int k = 0; k < n; k++) {
            const char* item = 0;
            if (!app && !lines[k].key[0]) item = lines[k].section;
            if (app && lines[k].key[0] && !strcasecmp(lines[k].section, GSTR(app))) item = lines[k].key;
            if (!item) continue;
            uint32_t len = (uint32_t)strlen(item);
            if (pos + len + 2 > size) { len = size > pos + 2 ? size - pos - 2 : 0; }
            memcpy(GPTR(buf + pos), item, len);
            pos += len;
            M8(buf + pos++) = 0;
            if (pos + 1 >= size) break;
        }
        M8(buf + pos) = 0;
        free(lines);
        RET(pos ? pos - 1 : 0);
        return;
    }
    const char* value = def ? GSTR(def) : "";
    for (int k = 0; k < n; k++)
        if (lines[k].key[0] && !strcasecmp(lines[k].section, GSTR(app)) && !strcasecmp(lines[k].key, GSTR(key))) {
            value = lines[k].value;
            break;
        }
    uint32_t len = copy_out(buf, size, value);
    free(lines);
    RET(len);
}

static void k_WritePrivateProfileStringA(CPU* c) {
    uint32_t app = ARG(0), key = ARG(1), value = ARG(2);
    if (!app) { RET(0); return; }
    char path[600];
    profile_path(ARG(3), path, sizeof path);
    IniLine* lines;
    int n = ini_read(path, &lines), out = 0, section_at = -1, done = 0;
    IniLine* result = malloc(sizeof *result * (size_t)(n + 2));
    for (int k = 0; k < n; k++) {
        int in_section = !strcasecmp(lines[k].section, GSTR(app));
        if (in_section && !lines[k].key[0]) {
            section_at = out;
            if (!key) continue;   /* NULL key removes the section */
        }
        if (in_section && !key) continue;
        if (in_section && lines[k].key[0] && !strcasecmp(lines[k].key, GSTR(key))) {
            done = 1;
            if (!value) continue;   /* NULL value removes the key */
            snprintf(lines[k].value, sizeof lines[k].value, "%s", GSTR(value));
        }
        result[out++] = lines[k];
    }
    if (key && value && !done) {
        IniLine l;
        memset(&l, 0, sizeof l);
        snprintf(l.section, sizeof l.section, "%s", GSTR(app));
        if (section_at < 0) { result[out++] = l; section_at = out - 1; }
        snprintf(l.key, sizeof l.key, "%s", GSTR(key));
        snprintf(l.value, sizeof l.value, "%s", GSTR(value));
        int at = section_at + 1;
        while (at < out && result[at].key[0] && !strcasecmp(result[at].section, GSTR(app))) at++;
        memmove(&result[at + 1], &result[at], sizeof *result * (size_t)(out - at));
        result[at] = l;
        out++;
    }
    ini_write(path, result, out);
    free(result);
    free(lines);
    RET(1);
}

/* ---- dates and calendars (en-US, Gregorian) ---- */

static const char* const day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* const month_names[12] = {"January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December"};

/* Expands a Windows date picture (d dd ddd dddd M MM MMM MMMM y yy yyyy, 'quoted text'). */
static void format_date(const char* pic, int year, int month, int day, int dow, char* out, size_t n) {
    size_t o = 0;
    for (const char* p = pic; *p && o + 1 < n;) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'' && o + 1 < n) out[o++] = *p++;
            if (*p) p++;
            continue;
        }
        char ch = *p;
        int run = 0;
        while (p[run] == ch) run++;
        char piece[32] = "";
        if (ch == 'd') {
            if (run == 1) snprintf(piece, sizeof piece, "%d", day);
            else if (run == 2) snprintf(piece, sizeof piece, "%02d", day);
            else if (run == 3) snprintf(piece, sizeof piece, "%.3s", day_names[dow]);
            else snprintf(piece, sizeof piece, "%s", day_names[dow]);
        } else if (ch == 'M') {
            if (run == 1) snprintf(piece, sizeof piece, "%d", month);
            else if (run == 2) snprintf(piece, sizeof piece, "%02d", month);
            else if (run == 3) snprintf(piece, sizeof piece, "%.3s", month_names[month - 1]);
            else snprintf(piece, sizeof piece, "%s", month_names[month - 1]);
        } else if (ch == 'y') {
            if (run <= 2) snprintf(piece, sizeof piece, run == 1 ? "%d" : "%02d", year % 100);
            else snprintf(piece, sizeof piece, "%d", year);
        } else if (ch == 'g') {
            snprintf(piece, sizeof piece, "A.D.");
        } else {
            out[o++] = *p++;
            continue;
        }
        p += run;
        for (const char* q = piece; *q && o + 1 < n; q++) out[o++] = *q;
    }
    out[o] = 0;
}

static void k_GetDateFormatA(CPU* c) {
    uint32_t flags = ARG(1), st = ARG(2), pic = ARG(3), buf = ARG(4), size = ARG(5);
    int year, month, day, dow;
    if (st) { year = M16(st); month = M16(st + 2); dow = M16(st + 4); day = M16(st + 6); }
    else {
        time_t now = time(0);
        struct tm* t = localtime(&now);
        year = t->tm_year + 1900; month = t->tm_mon + 1; day = t->tm_mday; dow = t->tm_wday;
    }
    if (month < 1 || month > 12 || dow < 0 || dow > 6) { set_last_error(87); RET(0); return; }
    char out[256];
    format_date(pic ? GSTR(pic) : (flags & 2) ? "dddd, MMMM d, yyyy" : "M/d/yyyy", year, month, day, dow, out, sizeof out);
    uint32_t need = (uint32_t)strlen(out) + 1;
    if (!size) { RET(need); return; }
    if (size < need) { set_last_error(122); RET(0); return; }
    memcpy(GPTR(buf), out, need);
    RET(need);
}

/* Calls the guest's callback once with the calendar string asked for (CAL_* index). */
static void k_EnumCalendarInfoA(CPU* c) {
    uint32_t proc = ARG(0), type = ARG(3) & 0xffff;
    char value[64] = "";
    if (type == 2) snprintf(value, sizeof value, "Gregorian Calendar");
    else if (type == 4) snprintf(value, sizeof value, "A.D.");
    else if (type == 5) snprintf(value, sizeof value, "M/d/yyyy");
    else if (type == 6) snprintf(value, sizeof value, "dddd, MMMM d, yyyy");
    else if (type >= 7 && type <= 0x0d) snprintf(value, sizeof value, "%s", day_names[(type - 7 + 1) % 7]);
    else if (type >= 0x0e && type <= 0x14) snprintf(value, sizeof value, "%.3s", day_names[(type - 0x0e + 1) % 7]);
    else if (type >= 0x15 && type <= 0x20) snprintf(value, sizeof value, "%s", month_names[type - 0x15]);
    else if (type >= 0x22 && type <= 0x2d) snprintf(value, sizeof value, "%.3s", month_names[type - 0x22]);
    else if (type == 0x2f) snprintf(value, sizeof value, "MMMM yyyy");
    else { RET(1); return; }
    if (proc) {
        uint32_t s = guest_strdup(value);
        guest_call(c, proc, 1, s);
        if (c->pend) return;
        guest_free(s);
    }
    RET(1);
}

static void k_IsValidLocale(CPU* c) { RET(1); }
static void k_GetComputerNameA(CPU* c) {
    uint32_t buf = ARG(0), psize = ARG(1);
    const char* name = "NOSCORE-BROWSER";
    uint32_t need = (uint32_t)strlen(name) + 1;
    if (!psize || M32(psize) < need) { if (psize) M32(psize) = need; set_last_error(111); RET(0); return; }
    memcpy(GPTR(buf), name, need);
    M32(psize) = need - 1;
    RET(1);
}

/* ---- files ---- */

static void put_filetime(uint32_t p, time_t t) {
    if (!p) return;
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    M32(p) = (uint32_t)ft;
    M32(p + 4) = (uint32_t)(ft >> 32);
}
static void k_GetFileTime(CPU* c) {
    struct stat st;
    int fd = port_file_fd(ARG(0));
    if (fd < 0 || fstat(fd, &st)) { set_last_error(6); RET(0); return; }
    put_filetime(ARG(1), st.st_ctime);
    put_filetime(ARG(2), st.st_atime);
    put_filetime(ARG(3), st.st_mtime);
    RET(1);
}
static void k_SetEndOfFile(CPU* c) {
    int fd = port_file_fd(ARG(0));
    off_t at = fd < 0 ? -1 : lseek(fd, 0, SEEK_CUR);
    RET(at >= 0 && !ftruncate(fd, at));
}
static void k_MoveFileA(CPU* c) {
    char from[600], to[600];
    host_path(ARG(0), from, sizeof from);
    host_path(ARG(1), to, sizeof to);
    struct stat st;
    if (!stat(to, &st)) { set_last_error(183); RET(0); return; }   /* MoveFile never replaces */
    RET(!rename(from, to));
}
static void k_RemoveDirectoryA(CPU* c) {
    char path[600];
    host_path(ARG(0), path, sizeof path);
    RET(!rmdir(path));
}

/* Only the main thread runs here, so leaving it ends the process. */
static void k_ExitThread(CPU* c) {
    port_log("ExitThread(%u) on the only thread: exiting", ARG(0));
    exit((int)ARG(0));
}
static void k_SetProcessShutdownParameters(CPU* c) { RET(1); }

/* ---- shlwapi ---- */

static void sh_PathIsRelativeA(CPU* c) {
    const char* p = ARG(0) ? GSTR(ARG(0)) : "";
    RET(!(p[0] == '\\' || p[0] == '/' || (isalpha((unsigned char)p[0]) && p[1] == ':')));
}
static void sh_PathIsUNCA(CPU* c) {
    const char* p = ARG(0) ? GSTR(ARG(0)) : "";
    RET(p[0] == '\\' && p[1] == '\\' && p[2] != '?');
}
/* Removes "." segments and folds ".." into the segment before it. */
static void sh_PathCanonicalizeA(CPU* c) {
    uint32_t dst = ARG(0);
    const char* src = ARG(1) ? GSTR(ARG(1)) : "";
    char out[260];
    size_t o = 0, root = 0;
    if (isalpha((unsigned char)src[0]) && src[1] == ':') { out[o++] = src[0]; out[o++] = ':'; src += 2; }
    if (*src == '\\' || *src == '/') { out[o++] = '\\'; src++; }
    root = o;
    while (*src && o < sizeof out - 1) {
        const char* e = src;
        while (*e && *e != '\\' && *e != '/') e++;
        size_t len = (size_t)(e - src);
        if (len == 1 && src[0] == '.') {
        } else if (len == 2 && src[0] == '.' && src[1] == '.') {
            if (o > root) {
                o--;
                while (o > root && out[o - 1] != '\\') o--;
            }
        } else if (len) {
            if (o > root && out[o - 1] != '\\') out[o++] = '\\';
            for (size_t k = 0; k < len && o < sizeof out - 1; k++) out[o++] = src[k];
        }
        src = *e ? e + 1 : e;
        if (!*e && len == 0) break;
    }
    if (o > root && out[o - 1] == '\\') o--;
    if (!o) out[o++] = '\\';
    out[o] = 0;
    memcpy(GPTR(dst), out, o + 1);
    RET(1);
}

/* ---- registry ---- */

static void a_RegFlushKey(CPU* c) { RET(0); }

/* ---- IME: the browser delivers composed text as WM_CHAR, so no IME composition is ever open here ---- */

static void imm_GetOpenStatus(CPU* c) { RET(0); }
static void imm_GetCompositionString(CPU* c) { RET(0); }
static void imm_GetCandidateListW(CPU* c) { RET(0); }
static void imm_SetCandidateWindow(CPU* c) { RET(1); }

/* ---- Gameforge / Steam wrapper ---- */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, js_open_url, (const char* url), {
    if (typeof postMessage === 'function') postMessage({ type: 'open-url', url: UTF8ToString(url) });
});
#else
static void js_open_url(const char* url) { port_log("open url %s", url); }
#endif

static void st_OpenOverlayToWebPage(CPU* c) { if (ARG(0)) js_open_url(GSTR(ARG(0))); RET(0); }
/* Tickets are the auth code handed over at login; there is nothing to revoke. */
static void st_CancelAuthSessionTicket(CPU* c) { RET(0); }
/* Steam would dispatch its callbacks here; none are ever registered in the browser. */
static void st_OnFrameTick(CPU* c) { RET(0); }

/* No visual style is loaded (the classic look), so controls draw themselves without theme parts. */
static void ux_IsAppThemed(CPU* c) { RET(0); }
static void ux_IsThemeActive(CPU* c) { RET(0); }
static void ux_OpenThemeData(CPU* c) { set_last_error(0x80070490u); RET(0); }
static void ux_CloseThemeData(CPU* c) { RET(0); }
static void ux_GetThemeAppProperties(CPU* c) { RET(0); }

#define K(n, a) {"kernel32.dll", #n, a, k_##n}
static const Hle sys[] = {
    {"uxtheme.dll", "IsAppThemed", 0, ux_IsAppThemed}, {"uxtheme.dll", "IsThemeActive", 0, ux_IsThemeActive},
    {"uxtheme.dll", "OpenThemeData", 2, ux_OpenThemeData}, {"uxtheme.dll", "CloseThemeData", 1, ux_CloseThemeData},
    {"uxtheme.dll", "GetThemeAppProperties", 0, ux_GetThemeAppProperties},
    K(GetPrivateProfileStringA, 6), K(WritePrivateProfileStringA, 4), K(GetDateFormatA, 6), K(EnumCalendarInfoA, 4),
    K(IsValidLocale, 2), K(GetComputerNameA, 2), K(GetFileTime, 4), K(SetEndOfFile, 1), K(MoveFileA, 2),
    K(RemoveDirectoryA, 1), K(ExitThread, 1), K(SetProcessShutdownParameters, 2),
    {"shlwapi.dll", "PathIsRelativeA", 1, sh_PathIsRelativeA}, {"shlwapi.dll", "PathIsUNCA", 1, sh_PathIsUNCA},
    {"shlwapi.dll", "PathCanonicalizeA", 2, sh_PathCanonicalizeA},
    {"advapi32.dll", "RegFlushKey", 1, a_RegFlushKey},
    {"imm32.dll", "ImmGetOpenStatus", 1, imm_GetOpenStatus},
    {"imm32.dll", "ImmGetCompositionStringA", 4, imm_GetCompositionString},
    {"imm32.dll", "ImmGetCompositionStringW", 4, imm_GetCompositionString},
    {"imm32.dll", "ImmGetCandidateListW", 4, imm_GetCandidateListW},
    {"imm32.dll", "ImmSetCandidateWindow", 2, imm_SetCandidateWindow},
    {"steam_wrapper.dll", "Steam_OpenOverlayToWebPage", CDECL_ARGS, st_OpenOverlayToWebPage},
    {"noscore_gf.dll", "Steam_OpenOverlayToWebPage", CDECL_ARGS, st_OpenOverlayToWebPage},
    {"steam_wrapper.dll", "Steam_CancelAuthSessionTicket", CDECL_ARGS, st_CancelAuthSessionTicket},
    {"noscore_gf.dll", "Steam_CancelAuthSessionTicket", CDECL_ARGS, st_CancelAuthSessionTicket},
    {"steam_wrapper.dll", "Steam_OnFrameTick", CDECL_ARGS, st_OnFrameTick},
    {"noscore_gf.dll", "Steam_OnFrameTick", CDECL_ARGS, st_OnFrameTick},
};

void hle_system_register(void) { hle_register(sys, sizeof sys / sizeof sys[0]); }
