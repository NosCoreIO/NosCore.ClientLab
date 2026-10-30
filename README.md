# NosCore.ClientLab

A static recompilation of the NosTale client (a Delphi 7, 32-bit x86 Windows program) into portable C, built to
WebAssembly so the client runs **in a browser tab** against a [NosCore](https://github.com/NosCoreIO/NosCore) server.
The goal is automated end-to-end testing of NosCore with the real client: no Windows machine, no emulator, scriptable.

**No client content lives here.** You bring your own client install. The tools read it from disk; everything they
produce (generated C, Ghidra/IDR databases, builds, screenshots) stays in a local work folder outside git, and the dev
server streams your game files to your own browser only.

## What works

Against a local NosCore stack, in Chromium:

- login, server and channel selection, character selection, entering the world;
- walking, chat, the game windows (inventory, skills, ...), GM teleports between maps;
- Direct3D 9 rendering on WebGL2 (fixed-function texture stages, lighting, fog, texture-coordinate generation),
  GDI text through the browser's font engine, Miles audio through WebAudio;
- about 50-60 frames per second in town.

Every one of the client's 19,436 functions is recompiled (90.4% of the code section's bytes; the rest is data the
compiler keeps there: VMTs, RTTI, jump tables, strings). What is still open is listed in
[docs/STATUS.md](docs/STATUS.md).

## How it works

```
NosCore.exe ──IDR + Ghidra──▶ function list + names (symbols/<md5>/)
            ──recomp/lift.py──▶ one C function per x86 function (registers, flags and x87 in a CPU struct)
                                 + recomp/runtime (shared helpers)
                                 + recomp/port   (Windows re-implemented: kernel32, user32, gdi32, d3d9, Winsock, Miles, ...)
            ──Emscripten──▶ nostale.wasm, run in a Web Worker; recomp/web serves the page and relays sockets
```

- **Lifter** (`recomp/lift.py`): recursive descent from the Ghidra function list, extended where Ghidra stops early
  (unresolved `case` dispatches, try/finally handlers, VMT-only methods). Direct calls become plain C calls; indirect
  ones go through a flat label table. A Unicorn-based differential tester (`recomp/difftest`) checks instruction
  semantics.
- **Portable host** (`recomp/port`): the Windows the client expects, implemented on browser primitives: WebGL2 for
  Direct3D 9, OffscreenCanvas for GDI text, WebAudio for Miles, SharedArrayBuffer rings for input and sockets (the
  game loop never yields to the browser).
- **Native host** (`recomp/runtime`, MSVC): the same generated C as a Windows executable, useful for debugging.

The full story, every gotcha found so far, and how to continue are in [docs/STATUS.md](docs/STATUS.md).

## Quick start

Requirements: Windows, Python 3 (`pip install capstone pefile`), [Emscripten](https://emscripten.org) in
`C:\tools\emsdk`, Ghidra 12 with Java 21 (for the first decompilation), a NosCore server, and the NosCore-patched
client (`NosCore.exe`; symbols here are for MD5 `51bbff3ec09d67c8425f88d74c6d5df5`).

```powershell
# 1. Recover functions and names (IDR + Ghidra), once per client build
./tools/decompile.ps1 -ClientExe "C:\Program Files (x86)\Nostale\NosCore.exe"

# 2. Lift x86 to C
python recomp\lift.py C:\tools\re\work\input\NosCore.exe C:\tools\re\work\functions.txt C:\tools\re\work\recomp\gen `
    --seeds symbols\51bbff3ec09d67c8425f88d74c6d5df5\recomp_seeds.txt

# 3. Build the browser version (about 5 minutes the first time, incremental afterwards)
python recomp\wasm\compile_all.py --web

# 4. Serve it with your game folder and a NosCore account, then open http://127.0.0.1:8077/
python recomp\web\serve.py --game "C:\Program Files (x86)\Nostale" --user <account> --password <password>
```

`serve.py` mints the launcher's auth code from the NosCore WebApi for that account, or you can pass one with
`?auth=<code>`.

### Linux

The same pipeline runs on Linux (Python 3 with `capstone` and `pefile`, Emscripten's `emcc` on `PATH`, Ghidra 12 with
its JDK, Node with Playwright). Generated code and builds go to `$NOSRECOMP_WORK` (default `~/re/work`).

```bash
GHIDRA_HOME=/opt/ghidra tools/decompile.sh ~/Nostale/NosCore.exe        # Ghidra only: IDR needs Windows
W=${NOSRECOMP_WORK:-~/re/work}
python3 recomp/lift.py $W/input/NosCore.exe $W/functions.txt $W/recomp/gen --seeds $W/recomp/seeds.txt
python3 recomp/wasm/compile_all.py --web
python3 recomp/web/serve.py --game ~/Nostale --user <account> --password <password>
python3 recomp/web/iterate.py      # seeds each function the client stops on, re-lifts and rebuilds
```

Without IDR's seeds the function list is shorter; the lifter takes unit initialization and finalization routines
from the client's init table, and `iterate.py` is the browser counterpart of `iterate.ps1` for whatever is still
only reached indirectly.

### Scripted runs

`node recomp/web/drive.mjs [--maps 2,20,145] [--trace] [--dump-frame]` (Playwright with the installed Chrome) logs in,
enters the world, walks, chats, opens the game windows and teleports between maps, then saves screenshots and the
client log to `C:\tools\re\work\drive\`. It is the starting point for NosCore end-to-end tests.

## Layout

| Path | What |
|---|---|
| `recomp/lift.py` | x86 → C lifter |
| `recomp/runtime/` | runtime shared by the native and browser builds |
| `recomp/port/` | portable Windows host (HLE of the client's DLLs) and the WebGL renderer |
| `recomp/web/` | page, worker, dev server with socket relay, `drive.mjs` |
| `recomp/wasm/` | Emscripten build scripts |
| `recomp/difftest/` | differential tester against Unicorn |
| `tools/` | decompilation pipeline (IDR, Ghidra scripts, cross-reference queries) |
| `symbols/<md5>/` | names, packet handler tables and recompilation seeds for one client build |
| `docs/STATUS.md` | current state, lessons learned, how to continue |

## Disclaimer

NosTale is a trademark of Gameforge. This project is not affiliated with or endorsed by Gameforge. It contains no
client code or assets; it is tooling for interoperability testing of NosCore with a client you already have.

## License

[MIT](LICENSE)
