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

- CI builds the host once per launcher release as an Emscripten **main module**
  (`-sMAIN_MODULE=2`, explicit exports): runtime, port, JS glue. It ships inside the launcher.
- The device builds only the game as a **side module** that imports memory, the indirect-call table and
  the `rt_*` / port symbols it uses. The worker loads it with `loadDynamicLibrary` before `callMain`.
- This immediately cuts local compile work, and makes host fixes (renderer, audio, input) ship as plain
  launcher updates with no rebuild on the device.
- Things to measure: the cost of PIC (`-fPIC` global access through the GOT) on the hot game loop. Memory
  accesses go through `M32(addr)` on a fixed base, so this cost should be small. Use
  `docs/PERFORMANCE.md` scenes, with a gate of no more than 3% frame-time regression.

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
- **To verify first:** peak memory of the largest generated file at `-O2` (phones), and whether
  the side module needs any Binaryen post-processing that `emcc` used to do.

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
