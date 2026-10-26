# NosCore.ClientLab status

Last updated 2026-10-26.

**Goal:** the NosTale client running in a browser, for automated NosCore testing. Readable decompiled source is not a goal.
**Ground rule:** no client content is ever committed. Users bring their own client, and all generated code, Ghidra and IDR databases, and builds stay in `C:\tools\re\work`.

## Where we are

The client (`NosCore.exe`, Delphi 7, md5 `51bbff3ec09d67c8425f88d74c6d5df5`) is **statically recompiled to C** and runs natively on Windows, against real Windows DLLs:

- 17,008 functions are lifted, covering 87% of the code section's bytes. The remainder is mostly Delphi data embedded in the code section (VMTs, RTTI, string literals, jump tables).
- The client boots through Delphi RTL and VCL initialisation, its own startup checks, and Chromium Embedded (CEF) setup. CEF's `--type=gpu-process` helper also runs as a recompiled process.
- It stays up for more than 45 s (about 120 MB) but does not show a game window yet. Each run still stops now and then on a function nobody discovered. `recomp/iterate.ps1` seeds these automatically (25 so far, in `symbols/<md5>/recomp_seeds.txt`).
- The differential tester passes 398 of 409 instruction shapes against Unicorn. The 11 remaining are junk decodes that never execute (`in`, `sldt`, `das`) and Unicorn's own x87 transcendental imprecision.

## Pipeline

1. `tools/decompile.ps1`: IDR (Delphi 7 knowledge base) → Dhrake → Ghidra (`-cspec borlanddelphi`). This produces names and the function list.
2. `tools/ghidra/CreateFunctions.java` (seeded from the IDR map) + `ExportFunctions.java` → `C:\tools\re\work\functions.txt`.
3. `recomp/lift.py <exe> functions.txt <gen> --seeds seeds.txt` lifts x86 to C (about 1 minute).
4. `recomp/build.ps1` does an incremental MSVC x86 build (`/Od` by default; a full build takes under a minute) and produces `C:\tools\re\work\recomp\out\NosTaleRecomp.exe`.
5. Run: `NosTaleRecomp.exe "C:\Program Files (x86)\Nostale\NosCore.exe" gf 0`. The log goes to `out\recomp.log`, plus `recomp-<pid>.log` for each CEF helper.
6. `recomp/iterate.ps1` loops: run → missing function → add a seed → re-lift → rebuild.
7. `recomp/difftest/difftest.py <exe> functions.txt <workdir>` runs the lifter correctness check (about 30 s).

### How the native runtime works (`recomp/runtime`)

- **Memory and stacks.** Guest addresses are host addresses (32-bit). The client is mapped at `0x400000` with `LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)`, which gives us its data and resources but never runs its code. The guest stack is the real thread stack, so SEH and stack probes work natively. Recompiled C runs on a separate 64 MB host stack.
- **Calls into Windows.** Guest calls to imports and COM go through `rt_call_host_raw`, which switches to the guest stack and calls the real function.
- **Callbacks.** When Windows calls guest code (WndProc, threads, SEH handlers), it lands in guest code pages that are mapped non-executable. The vectored exception handler catches the DEP fault and redirects to `rt_bridge_stub`.
- **Non-local jumps.** Delphi's `except` blocks jump across frames. They become a pending jump, which `rt_bridge_run` resolves by longjmp to the right activation record.
- **Hardware faults** inside recompiled code are re-raised as guest exceptions (`rt_raise_guest_fault`).
- **Debug switches.** `NOSRECOMP_TRACE_API=1` logs every Windows call, with string arguments and the return value. Fault reports map the host instruction pointer to its generated C line, which shows the original x86 instruction.

## What we learned (non-obvious; read before changing things)

**Ghidra and IDR**
- Ghidra's `analyzeHeadless.bat` breaks on paths containing `(x86)` and on `;`-joined `-scriptPath`. Copy the exe and keep scripts in one folder.
- Ghidra scripts must be saved without a BOM. PowerShell's `Out-File` adds one.
- IDR has no CLI: `tools/idr_export.py` drives it with pywinauto. The knowledge bases live in a Google Drive folder linked from old IDR README revisions.

**Address space and processes**
- `0x400000` is taken by NLS tables before `main()` runs. The host relaunches itself suspended and reserves the range in the child first.
- CEF re-launches "itself" with `--type=...`. The guest path is passed down in `NOSRECOMP_GUEST`, and the child-mode flag is cleared so grandchildren reserve `0x400000` too.

**Host code must stay off SEH**
- Never call SEH-using APIs (for example `OutputDebugString`) from host C code. Its handler frame sits on the host stack, outside the thread's stack bounds, so Windows rejects it and the process dies.
- TLS: the guest's single `fs:[0x2C]` read is redirected to a per-thread slot array the runtime owns.

**Code that Ghidra and IDR miss (the lifter recovers each of these)**
- Delphi try/finally epilogues (`push label; ... ret`). The label is pushed right after `mov fs:[reg], reg`.
- SEH handler pairs (`jmp @HandleFinally; jmp @finally`).
- Interface thunks (`add [esp+4], -N; jmp`) and dispatch stubs that do `ret` to a computed method. A `ret` to a guest function entry is a tail call.
- Jump-table cases, read through relocation slots, plus cross-function jumps and mid-function call targets.
- FastMM's register-convention assembly routines, which have no prologue.

**Semantics bugs found so far**
- x87 registers are doubles, but Delphi's `Move` copies 8 bytes with `fild`/`fistp qword`. Slots now carry the exact int64. A full 80-bit x87 model (SoftFloat, as in `x86recomp`) is the real fix.
- Flag liveness: `repe cmpsb` with a zero count, and shifts by `cl` with a zero count, leave flags untouched. They count as flag readers.
- The differential tester also caught: `cmpxchg` flags, PF on `lock inc`/`dec`/`xadd`, `#DE` on divide overflow, `fxch` operand order, and unnormal 80-bit loads.

**Process gotchas**
- An antivirus scan locks the freshly linked exe, so `build.ps1` renames the old exe before linking.
- Never let two loops or builds share one output directory.

## How to continue: browser build

The native build is the correctness oracle. The browser build uses the same generated C, compiled with Emscripten to wasm32. Pointers stay 32-bit, guest addresses are linear-memory offsets, and the image goes at `0x400000`. It links against a host layer that *implements* Windows instead of calling it:

1. **Finish native boot to the login screen.** Keep running `iterate.ps1`, and add a function-coverage counter.
2. **Use the existing kits as the Win32 layer.** The user is fine reusing them without a license.
   - Coverage of our 501 imports: `recomp-kit` covers 380, `x86recomp` covers 232, and together they cover 403.
   - `recomp-kit` has Direct3D 9 → WebGPU (its shader generator), a browser worker host and OPFS file storage. That's what NFS Most Wanted runs on in Chrome.
   - `x86recomp` has Miles (`mss32`), SoftFloat x87, SEH and threads.
   - The 98 imports neither kit covers are in `docs/imports_not_in_kits.txt`: `steam_wrapper`, `noscore_gf`, `ewdl.ews`, `shlwapi` and parts of `imm32`/`ole32`. Most are stubs for us.
   - The kits are cloned in `C:\tools\re\kits`.
3. **In the browser there's no VEH or DEP trick.** Callbacks are direct `rt_call` from the host layer into guest functions; WndProc and thread starts are known guest addresses.
4. **Stub CEF (the NosMall browser).**
5. **Winsock → WebSocket.** Put a small WebSocket↔TCP bridge in front of the NosCore login and world servers. The client's socket layer is the `TClientCore` class (`symbols/<md5>/core.txt`).
6. **Extend the differential tester to whole basic blocks**, so cross-instruction bugs like the flag-liveness one are caught automatically.

References: [pcrecomp](https://github.com/sp00nznet/pcrecomp), [recomp-kit](https://github.com/veritr1x/recomp-kit), [NFS Most Wanted web](https://github.com/austynoconnor/most-wanted-web), [x86recomp](https://github.com/JTM-rootstorm/x86recomp), [momo5502 on agent decompilation](https://momo5502.com/posts/2026-10-09-game-decompilation/).
