# NosCore.ClientLab status

Last updated 2026-10-26.

**Goal:** the NosTale client running in a browser, for automated NosCore testing. Readable decompiled source is not a goal.
**Ground rule:** no client content is ever committed. Users bring their own client, and all generated code, Ghidra and IDR databases, and builds stay in `C:\tools\re\work`.

## Where we are

The client (`NosCore.exe`, Delphi 7, md5 `51bbff3ec09d67c8425f88d74c6d5df5`) is **statically recompiled to C** and runs natively on Windows, against real Windows DLLs:

- 17,008 functions are lifted, covering 87% of the code section's bytes. The remainder is mostly Delphi data embedded in the code section (VMTs, RTTI, string literals, jump tables).
- The client boots through Delphi RTL and VCL initialisation, its own startup checks, and Chromium Embedded (CEF) setup. CEF's `--type=gpu-process` helper also runs as a recompiled process.
- **It opens the NosTale window and renders the loading/title artwork through Direct3D 9** (2026-10-26). Startup takes about 45 s at `/Od` and stays responsive at about 250 MB. Not yet tested: getting past loading to login (needs `_NC_AUTH_CODE` from the launcher), input, networking.
- **Frame capture for tests:** the runtime hooks `IDirect3DDevice9::Present`. Create `out\shot.req` and the next frame is saved as `out\shot.bmp`. The title screen renders correctly this way, after about 42 s at `/Od`. It stays on the title background because the login UI needs the launcher's `_NC_AUTH_CODE` and a running NosCore stack (none was running).
- Run it with `NOSRECOMP_NO_CEF=1`. The recompiled client's Chromium Embedded integration hits a CEF `CHECK` (int3 in `libcef.dll`) in every helper process right after it calls back into the client's Delphi CEF classes; the main process then dies with `0x80000003`. With `libcef.dll` hidden, CEF4Delphi shows "Error loading libcef.dll" (press OK) and the game continues without its embedded browser (NosMall). Root cause not found yet. The browser build stubs CEF anyway.
- Each run still stops now and then on a function nobody discovered. `recomp/iterate.ps1` seeds these automatically (28 so far, in `symbols/<md5>/recomp_seeds.txt`).
- The differential tester passes 398 of 409 instruction shapes against Unicorn. The 11 remaining are junk decodes that never execute (`in`, `sldt`, `das`) and Unicorn's own x87 transcendental imprecision.

### In the browser, as of 2026-10-28

**The browser client plays against a local NosCore stack**: login, server and channel lists, character selection, NosVille with HUD, minimap, NPCs and lighting; clicking the ground walks, the keyboard types and sends chat, the game windows open, GM teleports change maps (NosVille, Meadows, Krem, Port Alveus, Cylloan checked), and sound effects play. Capped at 60 frames/s; NosVille runs at about 50-55 frames/s even in headless Chrome.

**Recompilation coverage:** 19,436 functions; 90.4% of the code section's bytes are lifted, and the remaining 9.6% classifies as pointer/jump tables, strings, RTTI and four code fragments (374 bytes). `lift.py` prints the coverage and writes the gaps to `gen/gaps.txt`.

```powershell
python recomp\wasm\compile_all.py --web          # worker build into C:\tools\re\work\wasm\web
python recomp\web\serve.py --game "C:\Program Files (x86)\Nostale" --user <account> --password <password>   # http://127.0.0.1:8077/
```

- **Worker:** the client runs in a Web Worker and its loop never yields, so everything it talks to is synchronous: input arrives through a SharedArrayBuffer ring, network through sync XHR, frames leave as ImageBitmaps.
- **Rendering:** vertices are transformed on the CPU (`render.c` `fetch`); WebGL2 on an OffscreenCanvas (`render_gl.inc`) rasterises, evaluates two texture stages, alpha-tests, blends and depth-tests. Textures upload once per Lock (object +56 is the dirty flag) and are mipmapped. Under Node the software rasteriser in `render.c` is still used. D3D9 samples pixels at integer coordinates: the GL path offsets by half a pixel, the software path samples at x, y.
- **Text:** `hle_text.c` tracks DCs, fonts and selected bitmaps; glyphs are rasterised by the worker's OffscreenCanvas 2D context. The client builds its own glyph atlas from `TextOutA` per character.
- **Game files:** served by `serve.py` with HTTP Range support and mounted lazily, in lower case, at `/c:/nostale`.
- **Network:** Winsock is relayed through `serve.py` (`/net/connect|send|recv|close`). Sockets the page stops polling are closed after 20 s; otherwise a reloaded page keeps the account logged in on the world server (`failc` AlreadyConnected). `--remap FROM=TO` redirects a port when :4000 is taken locally.
- **Login packet:** the GF InstallationId is read from `HKLM\...\Gameforge4d\GameforgeClient\MainApp` through `TRegistry.OpenKey(..., True)`, i.e. RegCreateKeyExA; the registry HLE answers it (`NC_INSTALLATION_ID` overrides). Without it the client sends `NONE_CII` and the server rejects the GUID.
- **Auth code:** `?auth=<code>`, or `serve.py --user/--password` mints one per page load from the NosCore WebApi.
- **Input:** canvas events -> ring -> WM_* for the largest visible window (VCL forms are owned by the TApplication window, so "no parent" does not find it). One button/key state change per Present. SetTimer posts real WM_TIMER.
- **Anti-tamper stack check:** before acting on a click the client walks its EBP chain and treats any return address below 0x70000000 outside its own image as tampering (`FUN_004ccf8c`), silently dropping the input. Host-to-guest call return addresses (`SENTINEL_BASE`) therefore live at 0x7fdf0000, where system DLL code would be.
- **Audio:** `hle_audio.c` plays Miles samples (WAV/MP3 file images; IMA ADPCM converted to PCM) through WebAudio on the page; finished sounds are reported through a SharedArrayBuffer for `AIL_sample_status`. Audio starts after the first click or key press (browser autoplay rule).
- **Threads:** the only thread is a `TCkThread` raising an event every 10 minutes through Synchronize; the message pump raises it on the same schedule. `CreateThread` logs the Delphi class of any other thread.
- **Performance lessons:** direct calls between recompiled functions are plain C calls (a per-call label search had cost about 70% of the game code's time); indirect ones use a flat table. Recompiled functions take `CPU* RT_RESTRICT` so guest stores do not force register reloads, and a call kills flag liveness instead of keeping all flags alive. Draws with identical state are batched and only changed GL state is re-sent. `Present` sleeps to hold 60 frames/s; unthrottled, the worker starved the page.
- **Rendering lessons:** keep the real (negative) w when rebuilding clip coordinates or triangles crossing the camera become screen-filling polygons; terrain uses `D3DTSS_TCI_CAMERASPACEPOSITION` texgen for projected shadows; a single terrain draw can exceed 64k vertices; fog is per vertex from camera depth. `F9` in the page (or `drive.mjs --dump-frame`) logs every draw of the next frame.
- **Automated runs:** `node recomp/web/drive.mjs [--trace] [--headed] [--charselect-input] [--maps 2,20,145] [--dump-frame]` logs in, enters the world, clicks the ground, types a chat line, opens the game windows by hotkey, and saves screenshots and logs to `C:\tools\re\work\drive\`; it always closes the browser. Headless Chrome renders WebGL in software, so its fps is lower than a real browser's.
- **Debugging:** `?trace=1` logs the API calls and the guest functions (from the RT_TRACE ring) that follow each mouse-down. The page keeps 1000 log lines; `window.allLog` has everything.
- **Memory:** the build reserves 1 GB and may grow to 2 GB; several reloads in one Playwright browser reached 4 GB. Reload one tab and close the browser after testing.

**Open (2026-10-28):**
1. Gameplay not exercised yet: combat and skills, NPC dialogs and shops, trading, every UI window. Extend `drive.mjs`; each new feature may hit a missing API or rendering feature (the per-setup logs and the F9 frame dump show which).
2. Speed is fine for testing; the next step would be vertex transform and lighting in a vertex shader (7-8 ms CPU per busy frame).
3. CEF (NosMall, the in-game browser) is not loaded.
4. Android: Chrome on Android should run it once memory is reserved lazily and `serve.py` is reachable over HTTPS.

### WebAssembly (portable host), as of 2026-10-27

The same generated C, built with Emscripten and linked with `recomp/port/` (a host that implements Windows instead of calling it), **runs the client's main game loop headless under Node.** It goes through Delphi RTL and VCL init, the Gameforge stub (`Steam_*`, same answers as `NosCore.DeveloperTools.GfStub`), Direct3D 9 device creation on a null device, and `ShowWindow` of the main form, then presents frames continuously.

- Build and run: `recomp/wasm/run_node.ps1` (incremental compile with `emcc`, link `nostale.js` + `nostale.wasm` of about 37 MB, run under Node). Logs go to `C:\tools\re\work\wasm\stderr.txt`; `PORT_TRACE=1` logs every API call.
- Layout: the image is mapped at `0x400000` in linear memory, Emscripten's own memory sits above `GLOBAL_BASE` (`0x20000000`), and the guest stack and TEB sit at fixed guest addresses. Imports become trampolines at `0x0ff00000+` dispatching to `hle_*.c`.
- Every import of the current client has an implementation. `GetProcAddress` returns NULL for a name without one, as Windows does for a missing export. An import a newer client adds before it is implemented becomes a soft stub (logged once, returns 0) when its stdcall argument count is known. Counts come from the SDK import libraries plus `EWDL.EWS`, which is actually D3DX9. `argc_table.c` is generated.
- No `setjmp` in portable mode: Delphi's non-local except jumps propagate outward by returning (`guest_call` / `rt_call_host_raw`). Emscripten's setjmp transform takes forever on the huge generated functions.
- VCL's runtime-written window thunks (`call` / `pop ecx` / `jmp StdWndProc`) are emulated.
- Under Node the software rasteriser renders and audio is silent; networking is relayed only in the browser build, so under Node `connect` is refused.

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
- **Debug switches.** `NOSRECOMP_TRACE_API=1` logs every Windows call, with string arguments and the return value (about 1M lines per minute, and it slows the client a lot). `NOSRECOMP_NO_CEF=1` hides `libcef.dll`. Unexpected exceptions (breakpoint, stack overflow, `/GS`, access violation) are logged with the faulting module and the trace ring before Windows handles them. Fault reports map the host instruction pointer to its generated C line, which shows the original x86 instruction.

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
- Ghidra ends a function where it cannot follow control flow, typically right before an unresolved `case` dispatch, so the dispatch and every case body were missing. `extend_functions` re-walks every function (branches, jump tables, try-handler pushes) into code no other function owns; 2,458 functions grew.
- `@TryFinallyExit` enters a try handler at handler + 5 (the second `jmp` of the pair), computed at run time, so both halves are labels wherever they end up. Every relocated code pointer that lands inside a function is a label there.
- Delphi aligns routines to 4 bytes: an aligned relocated pointer into code is an entry point whatever its first instruction (empty virtual methods are a bare `ret`).
- Unowned runs between functions that decode as clean code ending in `ret`/`jmp` join the preceding function (`absorb_gaps`).
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

1. **Finish native boot to the login screen.** Keep running `iterate.ps1`, launch with the launcher's `_NC_AUTH_CODE`, and add a function-coverage counter. Also: the WebAssembly build of the generated code already compiles (`recomp/wasm/compile_all.py`, Emscripten in `C:\tools\emsdk`). The kit route is blocked on installing Ghidra 12.1.3 and the kit's Python venv (see `kit-game/game.toml`).
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
