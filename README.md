# NosCore.ClientLab

Experiments to run a real NosTale client headless, and later in a browser, so NosCore can be tested end to end against the client's actual behaviour.

## Ground rules

- **No client content in this repo.** No binaries, no `.NOS` archives, no extracted assets, no disassembly databases. Users supply their own install and every tool reads it at runtime. `.gitignore` enforces this, and it is also a review rule.
- **Patching happens locally.** The patcher takes the user's executable and writes a patched copy next to it: server IP and port, plus whatever else a test run needs. Only the patch *descriptions* (signatures, offsets, replacement bytes) live here.
- **The goal is test automation.** Running in a browser is a means, not the target.

## Approach, cheapest first

### Phase 0: real client, headless, in a container

Run the locally patched client under Wine and Xvfb in a Linux container, then drive it with the existing in-process dev-tools hook (`/peek`, `/inject`, the `STATS` pipe) and the known memory offsets. A test scenario becomes "send input or inject packets → assert on client memory and screenshots".

Open questions:
- Does the patched client run cleanly under current Wine (renderer, fonts, input)?
- Can the hook pipe be reached from the host, or does the test runner move into the container?

### Phase 1: same unmodified binary, in the browser

Run the patched `.exe` inside an x86/Win32 emulator compiled to WebAssembly (Boxedwine-style), without decompiling anything.

- Assets: the user picks their install folder (File System Access API). Files are copied into the browser's own private storage (OPFS) and are never uploaded.
- Networking: browsers cannot open raw TCP sockets, so a small WebSocket ↔ TCP bridge sits in front of the login and world servers.
- Automation: Playwright drives the page, and the hook endpoints are bridged out of the emulator.

### Phase 2: decompilation or static recompilation (only if 0 and 1 fall short)

This is the Need for Speed route: lift the client to portable source, then build it for WASM with a WebGL or WebGPU renderer. Hurdles specific to NosTale:

- The client is built with Delphi rather than MSVC C++. It uses Delphi's register calling convention (EAX/EDX/ECX) and its own RTL and VCL, and the matching-decomp tooling (objdiff and similar) assumes MSVC.
- The code section is about 3.6 MB of x86. IDR (Interactive Delphi Reconstructor) can recover class names, VMTs and RTTI, which helps a lot, but a full rewrite is a multi-month effort.
- Decompiled output is derived from the original binary, so it stays private and the "no client content" rule applies to it too.

## Related

- `NosCore.Launcher`: already prepares a client for a NosCore server. Reuse its patching logic instead of duplicating it.
- `NosCore.DeveloperTools`: the in-process hook and driver used for automation.
