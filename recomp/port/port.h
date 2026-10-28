/* Portable host: runs the recompiled client with no Windows underneath (WebAssembly first, via Emscripten).
   Guest addresses are linear-memory addresses; the image sits at 0x00400000 and Emscripten's own memory lives
   above GLOBAL_BASE (0x20000000). Imports point at trampoline addresses that dispatch to C implementations. */
#pragma once
#include "rt.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GUEST_BASE 0x00400000u
#define GUEST_STACK_TOP 0x0f000000u
#define GUEST_STACK_SIZE 0x00800000u
#define GUEST_TEB 0x0fe00000u
#define GUEST_PEB 0x0fe10000u
#define GUEST_SCRATCH 0x0fe20000u
#define TRAMP_BASE 0x0ff00000u
#define TRAMP_LIMIT 0x10000000u
/* Return addresses for host-to-guest calls. Never dereferenced; they sit above 0x70000000 like system DLL code,
   because the client walks its call stack and treats any lower return address outside its own image as tampering
   (and then ignores mouse clicks). */
#define SENTINEL_BASE 0x7fdf0000u

#define ARG(n) M32(c->esp + 4 + 4 * (n))
#define GSTR(p) ((char*)(uintptr_t)(p))
#define GPTR(p) ((void*)(uintptr_t)(p))
#define CDECL_ARGS 0xff

typedef void (*HleFn)(CPU*);
typedef struct {
    const char* dll;
    const char* name;
    uint8_t argc;
    HleFn fn;
} Hle;

typedef struct {
    const char* name;
    uint8_t argc;
} StdcallArgc;
extern const StdcallArgc stdcall_argc[];
extern const int stdcall_argc_count;

void hle_register(const Hle* table, int count);
uint32_t hle_trampoline(const char* dll, const char* name);
uint32_t hle_module_handle(const char* dll);
const char* hle_module_name(uint32_t handle);

int port_is_web(void);
void port_host_path(const char* guest, char* out, size_t n);
void port_log(const char* fmt, ...);
void port_fatal(const char* fmt, ...);

uint32_t guest_alloc(uint32_t size, uint32_t align);
void guest_free(uint32_t p);
uint32_t guest_call(CPU* c, uint32_t fn, int n, ...);
uint32_t guest_strdup(const char* s);

int port_file_fd(uint32_t handle);
extern uint32_t port_frames;
extern int port_trace_budget;
void port_fn_trace_start(CPU* c);
void port_threads_pump(CPU* c);
uint32_t port_reg_known_key(uint32_t subkey);

void set_last_error(uint32_t e);
uint32_t get_last_error(void);

/* Resources of the guest image. */
uint32_t res_find(uint32_t type, const char* type_name, uint32_t id, const char* name, uint32_t* size);

extern char port_guest_path[260];
extern char port_guest_cmdline[1024];
extern uint32_t port_guest_cmdline_addr;

void hle_kernel32_register(void);
void hle_user32_register(void);
void hle_window_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
void hle_misc_register(void);
void hle_gdi_register(void);
void hle_text_register(void);
void hle_audio_register(void);

typedef struct { int32_t w, h; int top_down; uint32_t bpp, stride, bits; } GdiBitmap;
int gdi_bitmap(uint32_t h, GdiBitmap* out);
int gdi_delete_bitmap(uint32_t h);
void hle_d3d9_register(void);
void web_present(uint32_t surface);
