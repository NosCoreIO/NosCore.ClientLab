<p align="center">
  <img width="200" alt="NosCore" src="https://raw.githubusercontent.com/NosCoreIO/NosCore.Packets/master/icon.png"/>
</p>

<h1 align="center">NosCore.ClientLab</h1>

<p align="center">
  <a href="https://discord.gg/Eu3ETSw"><img alt="Discord" src="https://img.shields.io/badge/Discord-join-5865F2?style=flat-square&logo=discord&logoColor=white"/></a>
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-blue?style=flat-square"/></a>
</p>

A static recompilation of the NosTale client (a Delphi 7, 32-bit x86 Windows program) into portable C, built to
WebAssembly so the client runs **in a browser tab** against a [NosCore](https://github.com/NosCoreIO/NosCore) server.
The goal is automated end-to-end testing of NosCore with the real client: no Windows machine, no emulator, scriptable.

**No client content lives here.** You bring your own client install. The tools read it from disk; everything they
produce (generated C, Ghidra/IDR databases, builds, screenshots) stays in a local work folder outside git, and the dev
server streams your game files to your own browser only.

## What works

Against a local NosCore stack, in Chromium (Chrome or Edge 137 or later): the browser build needs WebAssembly
JavaScript Promise Integration (`-sJSPI` in `recomp/wasm/compile_all.py`), which is how the game yields to the browser
once per frame. Browsers without it, such as Safari 26 and earlier, cannot start it.

- login, server and channel selection, character selection, entering the world;
- walking, chat, the game windows (inventory, skills, ...), GM teleports between maps;
- Direct3D 9 rendering on WebGL2 (fixed-function texture stages, lighting, fog, texture-coordinate generation),
  GDI text through the browser's font engine, Miles audio through WebAudio;
- about 60 frames per second in earlier NosCore town tests at 1080p on an integrated GPU;
  crowded official-server scenes are being profiled separately (see [Performance](docs/PERFORMANCE.md)).

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
  semantics, on the client's own instruction shapes or, without the client, on a synthetic set of forms
  (`--synthetic`, runs on Linux). A few hot client functions (the height ray and the path search) have hand-written versions in
  `recomp/runtime/rt_game.c`, used only for the client build they were written against and checked against their lifted
  bodies by `recomp/difftest/overrides.c`.
- **Portable host** (`recomp/port`): the Windows the client expects, implemented on browser primitives: WebGL2 for
  Direct3D 9, OffscreenCanvas for GDI text, WebAudio for Miles, SharedArrayBuffer rings for input and sockets (the
  game loop yields to the browser only at Present, once per frame, when it draws straight into the page's canvas).
- **Native host** (`recomp/runtime`, MSVC): the same generated C as a Windows executable, useful for debugging.

The full story, every gotcha found so far, and how to continue are in [docs/STATUS.md](docs/STATUS.md).

## Quick start

### C# desktop launcher (experimental)

The native Avalonia launcher embeds the game in Chromium. Installation, patch downloads,
the x86-to-C recompiler, compilation orchestration, and the local game host are written
in C#. Emscripten still uses a private Python runtime internally. The existing browser
startup below remains available.

```powershell
dotnet run --project desktop/ClientLab.Launcher -- --data-dir C:/dev/ClientLab-data
```

Requires .NET 10 for development. Opening the launcher checks for game updates. It
downloads or imports changed files and recompiles only when the executable changes
or a complete runtime is missing. Missing build tools are set up only when compilation
is needed. UI releases and asset-only patches reuse the compiled runtime.
Sign-in remains available while preparation runs; Play launches the prepared runtime.
Play opens a separate NosTale window with an aspect-correct game surface. Browser login
controls and debug panes are hidden there; startup errors are forwarded to Activity.
Game audio/display preferences use the client's original `Config.ini` format and persist
across desktop/browser launches and client updates. Existing settings are imported from
the selected installation on first use. Settings includes Music, Sound effects,
3D sound and Full screen controls; see [Game preferences](docs/GAME-SETTINGS.md).
Settings also offers an opt-in packet logger window with timestamped, live-readable
JSONL files; see [Packet logger](docs/PACKET-LOGGER.md).
Runtime compatibility revisions trigger a one-time rebuild when recompiler fixes require
it, even if the official executable is unchanged. UI-only updates still reuse that build.
Gameforge mode uses the latest official patch manifest and reuses matching files from
an installation chosen in Settings. NosCore mode keeps the existing local-install flow.
Verified file sizes and timestamps are recorded against the official manifest, so an
unchanged patch skips repeated checksums and copies. Only network transfers are labelled downloads.
Unchanged complete builds are reused; each new build attempt has its own output directory
so retries never overwrite generated C files that another process has mapped.
Completed compilation objects are cached by source, headers, flags and compiler identity,
so cancelled attempts can reuse their finished work. Larger source chunks start first;
compilation uses up to eight workers according to CPU and memory availability, with
file counts and elapsed time shown during preparation.
Very large functions use structured dispatch to keep WebAssembly compilation practical.
Function discovery combines exact-build Ghidra-generated function ranges with the C#
recompiler's relocation, call-graph, and Delphi initializer discovery. The maps are
bundled as metadata; players do not need Ghidra or Java installed. An unknown executable
requires a compatibility update containing its Ghidra analysis; the launcher preserves
the previous runtime instead of building from incomplete discovery. Patching and
recompilation run in C# using the matching metadata. New builds are experimental.
Desktop Gameforge sign-in runs the official live
identity script in the embedded browser, sharing one collector through sign-in and
launch verification. It uses a registered installation ID from Settings, Windows, or
an existing identity file. Live Windows validation covers proof-of-work, sign-in,
account lookup, iovation verification, and successful launch-code issuance (HTTP 201).
Recompiled-game compatibility and macOS/Linux end-to-end execution remain experimental.
The official script's device identifier and vector are preserved across changing loopback
ports, with existing identifiers migrated from the configured identity file. Collection
uses a properly sized verification view; native feature encoding and fixed-length JSON
framing match the official launcher. Image CAPTCHA and MFA answers are entered in the
launcher. Proof-of-work challenges run through Gameforge's official hosted widget in
the embedded browser, then resume sign-in with fresh identity data. Failed or cancelled
work never advances sign-in, and repeated challenges have a bounded retry count.
The sign-in page offers Remember me: credentials use Windows Credential Manager,
macOS Keychain, or a Linux Secret Service keyring (`secret-tool` from libsecret-tools).
They are restored on startup; signing out or disabling Remember me removes the saved
entry. Windows vault operations are tested; macOS/Linux vaults need platform validation.

To create a single executable, run PowerShell 7 **on the target operating system**:

```powershell
./desktop/publish.ps1 -Runtime win-x64 -Version 0.1.0
# Other targets: linux-x64, osx-x64, osx-arm64
# A directory containing just ClientLab.exe (build intermediates stay under desktop/obj):
./desktop/publish.ps1 -Runtime win-x64 -OutputDirectory ./dist/ClientLab-win -ExecutableName ClientLab.exe -SkipChecksum
```

The executable bundles the launcher, .NET and Chromium, and extracts them into the
user's local application-data directory on first use. It imports or downloads game
files locally; releases contain no game executable, assets, generated client C/WASM,
credentials or device identities. `--verify-payload` checks extraction without opening
the UI. The desktop release workflow builds each target on its own OS and creates a
draft GitHub release for `launcher-v*` tags. No release is published automatically.
Linux/macOS game execution and unsigned macOS distribution still need platform testing.
Image-drop CAPTCHA verification supports dragging one of the four supplied icons onto
the target, or clicking its numbered button. Rejected answers refresh the complete image
set; old drags cannot submit against a refreshed challenge. Other challenge types are
identified before image requests and reported explicitly.
The launcher uses NosCore branding and fetches NosTale artwork directly from the CDN
linked by the official landing page. News and event cards use Gameforge's own English
product-news API, its 444×250 card images and its official fallback artwork. Left/right
controls slide through the full feed, four cards per page; images load as pages are viewed. Artwork
and news are cached locally; network failures do not
block updates or sign-in. The NosTale image is not included in the repository or releases.

### Existing browser workflow

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

### Gameforge sign-in

To sign in directly with Gameforge and mint a launch code, use the `gameforge` provider. Open the page to enter your
email and password. If the login has multiple linked game accounts, the page shows an account selector after sign-in:

```sh
python3 recomp/web/serve.py --game /path/to/nostale --auth-provider gameforge --build ./work/wasm/web
```

The page collects Gameforge's `game1.js` blackbox and sends it with the credentials to the loopback server, which
forwards the login to Gameforge over HTTPS and discards the password after the request. The server calls Gameforge's
session and account-list endpoints, then submits a play-now iovation event and a session-bound encrypted blackbox to
the thin-code endpoint before passing the returned short-lived code to the worker as `_NC_AUTH_CODE`.
The browser build does not run the Gameforge Windows launcher's named-pipe IPC: it feeds the Gameforge-issued ticket
through the client's `noscore_gf.dll` session-ticket API. Although that DLL's exports are named `Steam_*`, this flow
does not authenticate with Steam. ClientLab also passes the registered Gameforge installation ID into the worker so
the client's later `NoS0577` login packet uses the same ID as the Gameforge requests.
`TNT_INSTALLATION_ID` or `--gf-installation-id` sets the registered installation ID;
`GF_USER_AGENT` or `--gf-user-agent` sets the request User-Agent. Gameforge can require additional verification or
launcher-specific request headers; those responses appear in the sign-in form for diagnosis. MFA/captcha challenge
completion stays in the page: enter the authenticator OTP when requested, or solve the displayed image-drop captcha
and submit its answer number. The challenge and password are held only in server memory while sign-in is pending.
On Windows, ClientLab prefers the machine-wide Gameforge installation ID, falling back to the per-user registry
value when the machine-wide value is absent. A stale per-user value can allow sign-in and iovation while code
retrieval returns HTTP 403. An explicit `TNT_INSTALLATION_ID` or `--gf-installation-id` still overrides detection.
On Linux, set
`TNT_INSTALLATION_ID` to the UUID from an existing registered Gameforge installation before starting the server.
Do not generate a new UUID: Gameforge can accept the credentials session and still reject launch-code creation
with HTTP 403 when the installation ID is unregistered. Gameforge credentials sign-in without a challenge is
supported; MFA or captcha is only needed when Gameforge requests it.
For a code obtained elsewhere, `CLIENTLAB_AUTH_CODE` or `--auth-code` remains available.

If browser-collected identity data is rejected at code retrieval, use `--gf-identity-file /path/to/identity.json`
(or `GF_IDENTITY_FILE`). This uses an identity collected by your official launcher for the play-now and code
requests, refreshing the timestamps/vector and binding it to each new launch session. Credentials sign-in and
MFA/captcha continue through the page. The captured installation ID must match `TNT_INSTALLATION_ID` on Linux/macOS.
The identity file contains device fingerprint data; keep it local and out of version control.

To import a `tra:` blackbox obtained from your own launcher's iovation request (for example with
[GflessClient's BlackboxExtractor](https://github.com/hatz2/GflessClient)), save the value to a local text file and run:

```sh
python3 recomp/web/gameforge_identity.py --blackbox-file blackbox.txt --installation-id YOUR_INSTALLATION_UUID --output official.gameforge-identity.json
python3 recomp/web/serve.py --game /path/to/nostale --build /path/to/web --gf-installation-id YOUR_INSTALLATION_UUID --gf-identity-file official.gameforge-identity.json
```

The output format is ClientLab's version 12 fingerprint array, rather than GflessClient's named-field identity JSON.

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
| `recomp/difftest/` | differential testers: lifted instructions against Unicorn, hand-written functions against their lifted bodies |
| `tools/` | decompilation pipeline (IDR, Ghidra scripts, cross-reference queries) |
| `symbols/<md5>/` | names, packet handler tables and recompilation seeds for one client build |
| `docs/STATUS.md` | current state, lessons learned, how to continue |

## Contributing

Questions, ideas and pull requests are welcome; most of the discussion happens on [Discord](https://discord.gg/Eu3ETSw).
[Quick start](#quick-start) and [Linux](#linux) above give the setup and build steps; [docs/STATUS.md](docs/STATUS.md) has the lifter correctness check (`recomp/difftest/difftest.py`).

## Supporting NosCore

NosCore is a spare-time open source project. Sponsoring buys time spent on the emulator and the packages around it,
and gives the **Supporter** role on Discord.

<p>
  <a href="https://github.com/sponsors/0Lucifer0"><img alt="Sponsor on GitHub" src="https://img.shields.io/badge/GitHub-sponsor-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white"/></a>
  <a href="https://ko-fi.com/A3562BQV"><img alt="Support on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?style=for-the-badge&logo=kofi&logoColor=white"/></a>
  <a href="https://www.patreon.com/bePatron?u=6503887"><img alt="Become a patron" src="https://img.shields.io/badge/Patreon-become_a_patron-F96854?style=for-the-badge&logo=patreon&logoColor=white"/></a>
</p>

Thanks to [Navicat](https://www.navicat.com) for supporting the project.

<a href="https://www.navicat.com"><img height="60" alt="Navicat" src="https://user-images.githubusercontent.com/35202750/207230064-dcf23adc-9e96-4481-9a53-cd212f5bd60e.png"/></a>

## Legal

NosCore is an independent, unofficial project for educational use. It is not affiliated with, authorized, maintained,
sponsored or endorsed by Gameforge or any of its affiliates; NosTale is a trademark of Gameforge. Using it may be
against the terms of service of NosTale servers.

Released under the [MIT License](LICENSE).
