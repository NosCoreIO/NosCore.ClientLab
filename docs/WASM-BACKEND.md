# Compiling without Emscripten on the player's device

Goal: prepare a new client build on any device, Android included, without installing Emscripten
(a private Python, the ~1 GB emsdk, native clang). Constraint unchanged: nothing derived from the
client's code is distributed. The per-client translation still happens on the player's device.

## What has to be compiled where today

| Part | Source | Size | Depends on the client? |
|---|---|---|---|
| Runtime + portable host (`recomp/runtime`, `recomp/port`) | ours, MIT | ~25k lines of C | no |
| Lifted game code (`generated/*.c`) | C# recompiler output | ~19.4k functions | **yes** |
| JS glue, worker, page | ours | small | no |

Today the launcher compiles all three with Emscripten on each device. Only the second row has to be compiled there.

## Stage 1: split host and game (prerequisite for every option)

**Done** (`WasmCompiler`, `ClientLab --compile`).
- **What the build produces:**
  - The runtime and portable host form an Emscripten **main module** (`nostale.js/.wasm`, `-sMAIN_MODULE=1`,
    compiled with `-DRT_HOST_MODULE`).
  - The lifted client is a **side module** (`nostale-game.wasm`).
  - The worker loads the side module through `Module.dynamicLibraries` when the build has one; older single-binary
    builds still run unchanged.
- **The boundary is four symbols.** The host needs only `rt_labels`, `rt_label_count`, `rt_code_lo` and
  `rt_code_hi` from the game. `rt_game_link()` looks them up with `dlsym` at the start of `main`. Everything the
  game needs from the host (the `rt_*` helpers, port functions, libc) resolves as a side-module import.
- **The host module is cached by its own objects.** A client patch relinks only the game:
  - Cold build of the current client: 291 s.
  - Same build warm: 25 s (all 132 objects reused, runtime module reused, game relinked).
- **Measured on the boot scenes** (headless Chrome, 1080p, current client, `PORT_PROFILE=1`, two runs each):

  | Build | First frame | Game work per frame (boot, then idle) |
  |---|---|---|
  | Single binary | 12.3 s | 8.0 / 7.9 ms, then 0.6 ms |
  | Split, PIC | 11.1–11.3 s | 7.6 / 7.9 ms, then 0.6 ms |

  No measurable cost from position-independent code here. The busy in-world scenes of `docs/PERFORMANCE.md` still need
  the same comparison before Stage 2 relies on it.
- **Next:** CI prebuilds the host module per release and ships it, so devices only build the game module.

## Stage 2: replace the native toolchain (recommended path)

Compile the generated C with **clang and wasm-ld built as WebAssembly** (YoWASP `@yowasp/clang`, LLVM 22,
Apache-2). The same `.wasm` binaries run everywhere:
- in a Web Worker of the embedded Chromium on desktop;
- in Android System WebView, or a V8/wasmtime host on the device.

The C emitter, structured dispatch and the per-file object cache all stay as they are.
Only `WasmCompiler.cs` changes: it invokes the WASM clang (one instance per worker) instead of
`emcc`, then calls `wasm-ld --shared --experimental-pic` against the host module's import list.

- **Pros:** reuses the existing, validated C backend; same code quality (`-O2`); no emsdk, no Python.
  The toolchain is a few tens of MB, downloaded once and verified by hash.
- **Cons:** clang running in WASM is roughly 2–3x slower than native, and phones have fewer cores and
  less RAM. Expect a one-time compile of tens of minutes on a phone for each client executable patch.
  Asset-only patches never recompile.
- **Measured** (2026-11-26, Node 20 on a 16-thread Windows desktop, real generated code of the current client,
  `--target=wasm32-unknown-emscripten -O2 -c` against Emscripten 4.0.19's sysroot headers):

  | File | Native clang (emsdk) | clang as WASM | Object size (native / WASM) |
  |---|---|---|---|
  | `fn_004d0000.c` (1.2 MB, median) | 3.2 s | 10.1 s | 321,091 / 321,054 B |
  | `fn_00528000.c` (1.9 MB, largest) | 4.4 s | 16.5 s | 522,172 / 522,174 B |

  About 3.5x slower, with essentially identical code, and about 400 MB resident per compiler instance. The whole
  client (149 MB of C in 114 files) is roughly 20 CPU-minutes, so about 3 minutes with 8 workers on a desktop.
  On a phone, with 3–4 workers sized to its RAM, expect 20–40 minutes, once per client executable patch.
- The toolchain is `@yowasp/clang` (a 104 MB download) plus the Emscripten sysroot `include/` (22 MB) we ship.
- **Verified:** a side module compiled (`-fPIC`) and linked by the WASM toolchain loads, with no Binaryen pass,
  into an `emcc -sMAIN_MODULE=2` host through `dlopen`. It reads and writes a host global, calls a host function,
  and makes an indirect call through the shared table. The link line is emcc's own side-module line:
  `wasm-ld --whole-archive <objs> --no-whole-archive --import-memory --strip-debug --export-dynamic
  --export=__wasm_call_ctors --experimental-pic --unresolved-symbols=import-dynamic -shared`
  (plus emcc's `-mllvm` flags).

## Stage 3: direct WASM emitter (`WasmEmitter` next to `CEmitter`)

The recompiler writes WASM bytecode itself, so preparation becomes seconds and needs no toolchain at all.
This is the end state, and it is the larger project:
- Reuse `X86Instruction`, discovery and the label table. Lower each function to WASM structured control flow:
  the existing structured dispatch (`loop` + `br_table`) as a fallback, and a stackifier for reducible
  regions.
- Required to match clang's output: flag liveness (drop dead `ZF/CF/SF/OF/PF` updates), caching the
  `CPU` registers in locals between calls, and inlining small leaves (`IsSmallLeaf`).
- Optional: run Binaryen (`wasm-opt -O2`, available as WASM) over the module on the device.
- Validate per function with `recomp/difftest`, against the C backend's results.

Do Stage 2 first. It delivers Android and removes emsdk from desktop with a fraction of the work. Stage 3
then becomes a speed upgrade with a working baseline to diff against, not a blocker.

## Android shell

- An Android app (Avalonia Android reuses the launcher's C# services: patch sync, sign-in, recompiler).
- The game page is served to a `WebView` through `WebViewAssetLoader` (`https://appassets.androidplatform.net`
  is a secure context). This is needed for `SharedArrayBuffer`, which the input and socket rings use.
  Cross-origin isolation headers are set by the loader.
- The TCP relay that `GameHost` provides over loopback HTTP moves into the app (a `shouldInterceptRequest`
  route or a loopback listener), with the same protocol.
- Touch input maps to the existing mouse/keyboard rings, and the soft keyboard is used for chat.

Sources: [@yowasp/clang on npm](https://www.npmjs.com/package/@yowasp/clang),
[LLVM RFC: Building LLVM for WebAssembly](https://discourse.llvm.org/t/rfc-building-llvm-for-webassembly/79073/63).
