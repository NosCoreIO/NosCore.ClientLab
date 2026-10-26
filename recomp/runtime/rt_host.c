#include "rt.h"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <psapi.h>

#define HOST_STACK_SIZE (64u << 20)
#define GUEST_BASE 0x00400000u

typedef struct HostTls {
    uint8_t guest[0x100];
    CPU* cpu;
} HostTls;

static __declspec(thread) HostTls host_tls;
extern unsigned long _tls_index;

static char guest_path[MAX_PATH];
static char guest_cmdline[4096];
static uint32_t guest_tls_template_lo, guest_tls_template_hi;
static FILE* log_file;

static_assert(offsetof(CPU, host_esp) == CPU_OFF_HOST_ESP, "asm offsets");
static_assert(offsetof(CPU, r) == 0, "asm offsets");

void rt_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_file) { fprintf(log_file, "%s\n", buf); fflush(log_file); }
}

void rt_fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    CPU* c = host_tls.cpu;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    if (c) {
        rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
               c->r[0].d, c->r[1].d, c->r[2].d, c->r[3].d, c->r[4].d, c->r[5].d, c->r[6].d, c->r[7].d);
        for (int k = 0; k < 16; k++) rt_log("  [esp+%02x] %08x", k * 4, ((uint32_t*)(uintptr_t)c->r[4].d)[k]);
    }
    MessageBoxA(0, buf, "NosCore.ClientLab recomp", MB_ICONERROR);
    ExitProcess(3);
}

/* ---- per-thread state ---- */

CPU* __cdecl rt_tls_cpu(void) {
    CPU* c = host_tls.cpu;
    if (!c) {
        DWORD err = GetLastError();
        uint8_t* stack;
        c = (CPU*)calloc(1, sizeof(CPU));
        stack = (uint8_t*)VirtualAlloc(0, HOST_STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!c || !stack) rt_fatal("out of memory creating thread state");
        c->host_stack_top = (uint32_t)(uintptr_t)(stack + HOST_STACK_SIZE - 64);
        c->host_esp = c->host_stack_top;
        c->fcw = 0x37f;
        if (guest_tls_template_hi > guest_tls_template_lo)
            memcpy(host_tls.guest, (void*)(uintptr_t)guest_tls_template_lo, guest_tls_template_hi - guest_tls_template_lo);
        c->tls_slots[0] = (uint32_t)(uintptr_t)host_tls.guest;
        host_tls.cpu = c;
        SetLastError(err);
    }
    return c;
}

/* ---- guest <-> host transitions ---- */

__declspec(naked) int rt_setjmp(uint32_t* jb) {
    __asm {
        mov edx, [esp + 4]
        mov [edx], ebx
        mov [edx + 4], esi
        mov [edx + 8], edi
        mov [edx + 12], ebp
        lea ecx, [esp + 4]
        mov [edx + 16], ecx
        mov ecx, [esp]
        mov [edx + 20], ecx
        xor eax, eax
        ret
    }
}

__declspec(naked) void rt_longjmp(uint32_t* jb) {
    __asm {
        mov edx, [esp + 4]
        mov ebx, [edx]
        mov esi, [edx + 4]
        mov edi, [edx + 8]
        mov ebp, [edx + 12]
        mov esp, [edx + 16]
        mov eax, 1
        jmp dword ptr [edx + 20]
    }
}

/* Calls a native function with the guest stack as the real stack. [guest esp] holds the
   guest return address; it is swapped for our return label while the callee runs. */
__declspec(naked) void __cdecl rt_call_host(CPU* c, uint32_t target) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ecx, [esp + 20]
        mov eax, [esp + 24]
        mov edx, [ecx + 16]
        push dword ptr [edx]
        push ecx
        mov [ecx + CPU_OFF_HOST_ESP], esp
        mov dword ptr [edx], offset back
        mov ebx, [ecx + 12]
        mov ebp, [ecx + 20]
        mov esi, [ecx + 24]
        mov edi, [ecx + 28]
        mov edx, [ecx + 8]
        mov esp, [ecx + 16]
        push eax
        mov eax, [ecx]
        mov ecx, [ecx + 4]
        ret
    back:
        push eax
        push edx
        push ecx
        call rt_tls_cpu
        pop dword ptr [eax + 4]
        pop dword ptr [eax + 8]
        pop dword ptr [eax]
        mov [eax + 12], ebx
        mov [eax + 20], ebp
        mov [eax + 24], esi
        mov [eax + 28], edi
        mov [eax + 16], esp
        mov esp, [eax + CPU_OFF_HOST_ESP]
        pop ecx
        pop edx
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

static RtAct* find_act(CPU* c, uint32_t target) {
    RtFn f = rt_lookup(target, 0);
    for (RtAct* a = c->acts; a; a = a->prev)
        if (a->fn == f && a->guest_esp >= c->r[4].d) return a;
    return 0;
}

uint32_t __cdecl rt_bridge_run(CPU* c, uint32_t target) {
    uint32_t saved_host = c->host_esp;
    uint32_t ret_host = M32(c->r[4].d);
    rt_call(c, target, ret_host);
    while (c->pend) {
        uint32_t t = c->pend_target;
        RtAct* a;
        c->pend = 0;
        if (t < rt_code_lo || t >= rt_code_hi) { ret_host = t; break; }
        a = find_act(c, t);
        if (!a) rt_fatal("non-local jump to %08x has no active frame", t);
        c->acts = a->prev;
        c->host_esp = a->host_esp;
        c->lj_target = t;
        rt_longjmp(a->jb);
    }
    c->host_esp = saved_host;
    return ret_host;
}

/* Entered (via the VEH) when native code executes a guest address. [esp] = guest target,
   all other registers are the native caller's. */
__declspec(naked) void rt_bridge_stub(void) {
    __asm {
        push eax
        push ecx
        push edx
        call rt_tls_cpu
        pop dword ptr [eax + 8]
        pop dword ptr [eax + 4]
        pop dword ptr [eax]
        mov [eax + 12], ebx
        mov [eax + 20], ebp
        mov [eax + 24], esi
        mov [eax + 28], edi
        pop edx
        mov [eax + 16], esp
        mov esi, eax
        mov esp, [eax + CPU_OFF_HOST_ESP]
        and esp, 0xFFFFFFF0
        push edx
        push esi
        call rt_bridge_run
        mov ecx, esi
        mov esp, [ecx + 16]
        push eax
        mov eax, [ecx]
        mov edx, [ecx + 8]
        mov ebx, [ecx + 12]
        mov ebp, [ecx + 20]
        mov esi, [ecx + 24]
        mov edi, [ecx + 28]
        mov ecx, [ecx + 4]
        ret
    }
}

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    PCONTEXT ctx = ep->ContextRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
        er->ExceptionInformation[0] == 8 && ctx->Eip >= rt_code_lo && ctx->Eip < rt_code_hi) {
        ctx->Esp -= 4;
        *(uint32_t*)(uintptr_t)ctx->Esp = ctx->Eip;
        ctx->Eip = (DWORD)(uintptr_t)rt_bridge_stub;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- import overrides: make the guest see itself as the main module ---- */

static HMODULE WINAPI ov_GetModuleHandleA(LPCSTR name) {
    if (!name) return (HMODULE)(uintptr_t)GUEST_BASE;
    return GetModuleHandleA(name);
}

static DWORD WINAPI ov_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD size) {
    if (!m || m == (HMODULE)(uintptr_t)GUEST_BASE) {
        DWORD n = (DWORD)strlen(guest_path);
        if (size == 0) return 0;
        if (n >= size) n = size - 1;
        memcpy(buf, guest_path, n);
        buf[n] = 0;
        return n;
    }
    return GetModuleFileNameA(m, buf, size);
}

static LPSTR WINAPI ov_GetCommandLineA(void) { return guest_cmdline; }

static VOID WINAPI ov_ExitProcess(UINT code) {
    rt_log("guest ExitProcess(%u)", code);
    ExitProcess(code);
}

typedef struct { const char* dll; const char* name; void* fn; } Override;
static const Override overrides[] = {
    {"kernel32.dll", "GetModuleHandleA", (void*)ov_GetModuleHandleA},
    {"kernel32.dll", "GetModuleFileNameA", (void*)ov_GetModuleFileNameA},
    {"kernel32.dll", "GetCommandLineA", (void*)ov_GetCommandLineA},
    {"kernel32.dll", "ExitProcess", (void*)ov_ExitProcess},
};

static void* find_override(const char* dll, const char* name) {
    for (size_t k = 0; k < sizeof overrides / sizeof overrides[0]; k++)
        if (!_stricmp(overrides[k].dll, dll) && !strcmp(overrides[k].name, name)) return overrides[k].fn;
    return 0;
}

/* ---- loader ---- */

static void resolve_imports(uint8_t* base) {
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    int missing = 0;
    for (; d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        HMODULE h = LoadLibraryA(dll);
        uint32_t* names = (uint32_t*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t* iat = (uint32_t*)(base + d->FirstThunk);
        if (!h) { rt_log("cannot load %s (error %lu)", dll, GetLastError()); missing++; }
        for (; *names; names++, iat++) {
            const char* name;
            void* fn;
            char ordname[16];
            if (*names & 0x80000000u) {
                sprintf_s(ordname, sizeof ordname, "#%u", *names & 0xffff);
                name = ordname;
                fn = h ? (void*)GetProcAddress(h, (LPCSTR)(uintptr_t)(*names & 0xffff)) : 0;
            } else {
                name = (const char*)(base + *names + 2);
                fn = find_override(dll, name);
                if (!fn && h) fn = (void*)GetProcAddress(h, name);
            }
            if (!fn) { rt_log("unresolved import %s!%s", dll, name); missing++; }
            *iat = (uint32_t)(uintptr_t)fn;
        }
    }
    if (missing) rt_log("%d imports unresolved", missing);
}

static void setup_tls(uint8_t* base) {
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    IMAGE_TLS_DIRECTORY32* t;
    if (!dir.VirtualAddress) return;
    t = (IMAGE_TLS_DIRECTORY32*)(base + dir.VirtualAddress);
    if (t->EndAddressOfRawData - t->StartAddressOfRawData > sizeof host_tls.guest)
        rt_fatal("guest TLS template too large");
    guest_tls_template_lo = t->StartAddressOfRawData;
    guest_tls_template_hi = t->EndAddressOfRawData;
    memcpy(host_tls.guest, (void*)(uintptr_t)guest_tls_template_lo, guest_tls_template_hi - guest_tls_template_lo);
    M32(t->AddressOfIndex) = 0;
    if (t->AddressOfCallBacks && M32(t->AddressOfCallBacks))
        rt_log("guest has TLS callbacks; not called yet");
}

static void protect_code(uint8_t* base) {
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)rt_code_lo, rt_code_hi - rt_code_lo, PAGE_READWRITE, &old))
        rt_fatal("VirtualProtect on guest code failed (%lu)", GetLastError());
}

static void make_writable(uint8_t* base) {
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD old;
    VirtualProtect(base, nt->OptionalHeader.SizeOfImage, PAGE_READWRITE, &old);
}

__declspec(naked) static void guest_returned(void) {
    __asm {
        push 0
        call ExitProcess
    }
}

static void __cdecl run_guest_entry(CPU* c, uint32_t entry) {
    rt_call(c, entry, (uint32_t)(uintptr_t)guest_returned);
    rt_fatal("guest entry point returned");
}

__declspec(naked) static void __cdecl run_on_host_stack(CPU* c, uint32_t entry, void (__cdecl* fn)(CPU*, uint32_t)) {
    __asm {
        mov ecx, [esp + 4]
        mov edx, [esp + 8]
        mov eax, [esp + 12]
        mov esp, [ecx + CPU_OFF_HOST_ESP]
        push edx
        push ecx
        call eax
        int 3
    }
}

static uint32_t guest_entry;

static DWORD WINAPI guest_main_thread(LPVOID unused) {
    CPU* c = rt_tls_cpu();
    uint8_t* guest_stack = (uint8_t*)_alloca(64);
    c->r[4].d = ((uint32_t)(uintptr_t)guest_stack - 0x1000) & ~0xFu;
    c->r[4].d -= 4;
    M32(c->r[4].d) = (uint32_t)(uintptr_t)guest_returned;
    rt_log("entering guest at %08x", guest_entry);
    run_on_host_stack(c, guest_entry, run_guest_entry);
    return 0;
}

/* Windows maps NLS tables and heaps around 0x400000 during process start, before main().
   Relaunch ourselves suspended and reserve the guest range in the child before its loader runs. */
static int relaunch_with_reservation(void) {
    char self[MAX_PATH];
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    GetModuleFileNameA(0, self, sizeof self);
    SetEnvironmentVariableA("NOSRECOMP_CHILD", "1");
    if (!CreateProcessA(self, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) {
        fprintf(stderr, "relaunch failed (%lu)\n", GetLastError());
        return 1;
    }
    if (!VirtualAllocEx(pi.hProcess, (void*)(uintptr_t)GUEST_BASE, 0x00800000, MEM_RESERVE, PAGE_NOACCESS))
        fprintf(stderr, "could not reserve guest range in child (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

int main(int argc, char** argv) {
    char dir[MAX_PATH], *slash;
    uint8_t* base;
    IMAGE_NT_HEADERS32* nt;
    int k;

    if (!GetEnvironmentVariableA("NOSRECOMP_CHILD", dir, sizeof dir))
        return relaunch_with_reservation();

    if (argc < 2) {
        fprintf(stderr, "usage: %s <path to NostaleClientX.exe> [client args...]\n", argv[0]);
        return 2;
    }
    if (!GetFullPathNameA(argv[1], sizeof guest_path, guest_path, 0)) return 2;
    strcpy_s(dir, sizeof dir, guest_path);
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    {
        char logp[MAX_PATH], *s;
        GetModuleFileNameA(0, logp, sizeof logp);
        s = strrchr(logp, '\\');
        strcpy_s(s ? s + 1 : logp, sizeof logp - (s ? (size_t)(s + 1 - logp) : 0), "recomp.log");
        fopen_s(&log_file, logp, "w");
    }
    SetCurrentDirectoryA(dir);
    SetDllDirectoryA(dir);
    rt_log("guest: %s", guest_path);

    sprintf_s(guest_cmdline, sizeof guest_cmdline, "\"%s\"", guest_path);
    for (k = 2; k < argc; k++) {
        strcat_s(guest_cmdline, sizeof guest_cmdline, " ");
        strcat_s(guest_cmdline, sizeof guest_cmdline, argv[k]);
    }

    if (!VirtualFree((void*)(uintptr_t)GUEST_BASE, 0, MEM_RELEASE))
        rt_log("no guest address reservation to release (%lu)", GetLastError());
    base = (uint8_t*)LoadLibraryExA(guest_path, 0, DONT_RESOLVE_DLL_REFERENCES);
    if (!base) rt_fatal("cannot map %s (%lu)", guest_path, GetLastError());
    if ((uint32_t)(uintptr_t)base != GUEST_BASE) rt_fatal("guest mapped at %p, expected %08x", base, GUEST_BASE);
    nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);

    make_writable(base);
    resolve_imports(base);
    setup_tls(base);
    protect_code(base);
    AddVectoredExceptionHandler(1, veh);

    guest_entry = GUEST_BASE + nt->OptionalHeader.AddressOfEntryPoint;
    {
        HANDLE th = CreateThread(0, 16u << 20, guest_main_thread, 0, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
        if (!th) rt_fatal("CreateThread failed (%lu)", GetLastError());
        WaitForSingleObject(th, INFINITE);
    }
    return 0;
}
