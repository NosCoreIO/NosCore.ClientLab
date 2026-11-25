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

Against a local NosCore stack, in a Chromium-based browser (Chrome, Edge, or the launcher's embedded Chromium). The
game runs in a Web Worker that blocks on `Atomics.wait` between frames and shares memory with the page, so the page
must be cross-origin isolated: the game host sends `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: credentialless`. Browsers without `credentialless` cross-origin isolation, such as
Safari, cannot start it. No WebAssembly JavaScript Promise Integration (JSPI) is needed.

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
            ──Emscripten──▶ nostale.wasm, run in a Web Worker; the C# game host serves the page and relays sockets
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
- **Game host** (`desktop/ClientLab.Launcher/Services/GameHost.cs`): the one loopback server for every way of
  running the game (desktop window, browser launcher, headless `--serve` for Playwright). It serves the page, the
  build and your game folder (lazily, with HTTP Range), the Gameforge sign-in endpoints, and a TCP relay for the worker.
- **Native host** (`recomp/runtime`, MSVC): the same generated C as a Windows executable, useful for debugging.

The full story, every gotcha found so far, and how to continue are in [docs/STATUS.md](docs/STATUS.md).

## Quick start

### Desktop launcher (experimental)

The native Avalonia launcher embeds the game in Chromium. Installation, patch downloads, the x86-to-C recompiler,
compilation orchestration and the game host are written in C#; Emscripten still uses a private Python runtime
internally. Requires .NET 10 for development.

The game module is compiled by Emscripten by default. A released launcher can instead compile it with clang and
wasm-ld built as WebAssembly, run in-process by Wasmtime (no emsdk, no Python; about 3x slower to compile). It does
that automatically when Emscripten cannot be set up, or always with `"compiler": "wasm"` in `settings.json`
(`--compiler wasm` for `ClientLab --compile`). See [docs/WASM-BACKEND.md](docs/WASM-BACKEND.md).

```powershell
dotnet run --project desktop/ClientLab.Launcher -- --data-dir C:/dev/ClientLab-data
```

**Updates and preparation**

- Opening the launcher checks the latest official patch manifest. Both sign-in modes play the current official
  client; files an installation chosen in Settings already has are copied rather than downloaded, and only network
  transfers are labelled downloads.
- Verified file sizes and timestamps are recorded against the manifest. A patch hard-links every unchanged file
  from the previous version (no copy, no re-hash, no extra disk) and downloads only changed files, six at a time,
  checksummed while they stream. Unreferenced versions and abandoned build attempts are removed afterwards.
- An open launcher checks for client patches every 20 minutes while idle. A new client whose analysis is not
  published yet keeps the previous build playable and is retried every 5 minutes.
- The game is recompiled only when the executable changes, a complete runtime is missing, or a runtime compatibility
  revision requires a one-time rebuild. UI releases and asset-only patches reuse the compiled runtime. Build tools are
  set up only when compilation is needed.
- Function discovery combines the exact build's Ghidra function ranges (`symbols/<md5>/`, bundled or fetched from
  this repository, where a scheduled workflow commits them for each client patch) with the C# recompiler's
  relocation, call-graph and Delphi initializer discovery. Players need neither Ghidra nor Java. An executable without
  published analysis keeps the previous runtime instead of building from incomplete discovery.
- Each build attempt has its own output directory, so retries never overwrite generated C another process has
  mapped. Compiled objects are cached by source, headers, flags and compiler identity, so cancelled attempts reuse
  their finished work. Compilation uses up to eight workers depending on CPU and memory, largest files first, with
  file counts and elapsed time shown. Very large functions use structured dispatch to keep WebAssembly compilation
  practical.

**Playing**

- Sign-in stays available while preparation runs; Play launches the prepared runtime in a separate NosTale window
  with an aspect-correct game surface. Browser login controls and debug panes are hidden there, and startup errors
  are forwarded to Activity.
- Game audio and display preferences use the client's own `Config.ini` format and persist across desktop and browser
  launches and client updates; existing settings are imported from the selected installation on first use. Settings
  has Music, Sound effects, 3D sound and Full screen controls ([Game preferences](docs/GAME-SETTINGS.md)).
- Settings also offers an opt-in packet logger window with timestamped, live-readable JSONL files, or a packet
  logger plugin of your choice in its place ([Packet logger](docs/PACKET-LOGGER.md)).

**Gameforge sign-in**

- Sign-in runs Gameforge's official live identity script in the embedded browser, with one collector shared by
  sign-in and launch verification. Collection uses a properly sized verification view; native feature encoding and
  fixed-length JSON framing match the official launcher. Live Windows validation covers proof-of-work, sign-in,
  account lookup, iovation verification and launch-code issuance (HTTP 201).
- The installation ID comes from Settings, Windows, or an existing identity file. On Windows, when no identity file is
  configured and no device identifier has been saved yet, the launcher reads the device identity (`x-game`/`x-vec`)
  the official Gameforge client registered on this machine from its Local Storage. A freshly minted identifier passes
  sign-in and iovation but is refused at launch-code creation with HTTP 403 "Not allowed to create code", because
  Gameforge never registered it for the installation. A saved identifier that does not match the registered one is
  replaced on the next sign-in; one that matches keeps its live vector rotation. The device identifier and vector are
  preserved across changing loopback ports. Without the official client, configure `--gf-identity-file` (below).
- Image CAPTCHA and MFA answers are entered in the launcher. An image-drop CAPTCHA is answered by dragging one of the
  four icons onto the target or clicking its numbered button; a rejected answer refreshes the whole image set, and old
  drags cannot submit against a refreshed challenge. Other challenge types are identified and reported explicitly.
  Proof-of-work runs through Gameforge's official hosted widget in the embedded browser, then sign-in resumes with
  fresh identity data; failed or cancelled work never advances sign-in, and repeated challenges have a bounded retry
  count.
- Remember me stores credentials in Windows Credential Manager, the macOS Keychain, or a Linux Secret Service keyring
  (`secret-tool` from libsecret-tools); signing out or disabling it removes the entry. The Windows vault is tested;
  macOS and Linux vaults need platform validation.

**Artwork and news**: NosCore branding, NosTale artwork fetched directly from the CDN linked by the official landing
page, and Gameforge's English product-news API (444×250 card images, official fallback artwork, four cards per page,
loaded as pages are viewed). Both are cached locally, network failures never block updates or sign-in, and the NosTale
image is not included in the repository or releases.

Recompiled-game compatibility and macOS/Linux end-to-end execution remain experimental.

### Release executable

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
the UI. On macOS the script also wraps the executable in `ClientLab.app` (ad-hoc signed) and a `.dmg`.

#### Releases and updates

- **Every merge to `main`** that touches `desktop/`, `recomp/` or `global.json` builds Windows, Linux and both macOS
  targets, runs the launcher checks on each, and publishes a release `v0.2.<run>` marked latest. `launcher-v*` tags
  still create a draft release; a manual run only builds the binaries.
- **The launcher updates itself.** A released launcher downloads the latest release's executable in the background
  (checked against `release-manifest.json`'s SHA-256), and the bootstrap swaps it in on the next start or when
  the player clicks *Restart* next to *Check for updates*. `dotnet run` and other development builds never update.
- **Game patches need no launcher release.** The launcher follows Gameforge's patch manifest and fetches the
  analysis that the *Client analysis* workflow commits under `symbols/` for each new client build.
- **Released launchers carry the runtime module prebuilt** (`ClientLab --build-host`, built once per release on
  Linux), so a player's machine compiles only the game module. Development builds, and launchers whose runtime
  sources or Emscripten version differ from the bundled module's stamp, build the runtime module locally as before.
- Releases are unsigned: Windows SmartScreen and macOS Gatekeeper ask once (*More info → Run anyway*,
  *System Settings → Privacy & Security → Open Anyway*). The workflow signs Windows executables and signs and
  notarizes the macOS app once the `WINDOWS_CERT_*` / `APPLE_*` secrets exist. Linux and macOS game execution
  still need platform testing.

### Browser and Playwright

The same executable runs without its window:

- `ClientLab --serve` starts only the game server for Playwright, `drive.mjs` or a normal browser, and prints
  `CLIENTLAB_URL=<url>`;
- `ClientLab --browser` serves the whole launcher (preparation, sign-in, Play) as a web page.

Options, the local API and CAPTCHA automation are in [docs/BROWSER-AUTOMATION.md](docs/BROWSER-AUTOMATION.md).

### Development pipeline (Windows)

Requirements: Windows, .NET 10, Python 3 (`pip install capstone pefile`), [Emscripten](https://emscripten.org) in
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
dotnet run --project desktop/ClientLab.Launcher -- --serve --game "C:\Program Files (x86)\Nostale" `
    --build C:\tools\re\work\wasm\web --user <account> --password <password> --source-root .
```

`--serve --user/--password` mints the launcher's auth code from the NosCore WebApi for that account on every page
load; `--auth-code <code>` or `?auth=<code>` passes one instead.

### Linux

The same pipeline runs on Linux (.NET 10, Python 3 with `capstone` and `pefile`, Emscripten's `emcc` on `PATH`,
Ghidra 12 with its JDK, Node with Playwright). Generated code and builds go to `$NOSRECOMP_WORK` (default `~/re/work`).

```bash
GHIDRA_HOME=/opt/ghidra tools/decompile.sh ~/Nostale/NosCore.exe        # Ghidra only: IDR needs Windows
W=${NOSRECOMP_WORK:-~/re/work}
python3 recomp/lift.py $W/input/NosCore.exe $W/functions.txt $W/recomp/gen --seeds $W/recomp/seeds.txt
python3 recomp/wasm/compile_all.py --web
dotnet run --project desktop/ClientLab.Launcher -- --serve --game ~/Nostale --user <account> --password <password> --source-root .
python3 recomp/web/iterate.py      # seeds each function the client stops on, re-lifts and rebuilds
```

Without IDR's seeds the function list is shorter; the lifter takes unit initialization and finalization routines
from the client's init table, and `iterate.py` is the browser counterpart of `iterate.ps1` for whatever is still
only reached indirectly.

### Gameforge sign-in from the browser

With the `gameforge` provider (the default without `--user`), open the printed URL with `?login=gameforge` and sign in
with your Gameforge email and password; a login with several linked game accounts shows an account selector:

```sh
dotnet run --project desktop/ClientLab.Launcher -- --serve --game /path/to/nostale --build ./work/wasm/web
```

- The page collects Gameforge's `game1.js` blackbox and sends it with the credentials to the loopback game host, which
  signs in to Gameforge over HTTPS, lists the linked accounts, submits a play-now iovation event and a session-bound
  encrypted blackbox to the thin-code endpoint, and passes the returned short-lived code to the worker as
  `_NC_AUTH_CODE`. Credentials and pending challenges are held only in server memory.
- The browser build does not run the Gameforge Windows launcher's named-pipe IPC: it feeds the Gameforge-issued
  ticket through the client's `noscore_gf.dll` session-ticket API. Although that DLL's exports are named `Steam_*`,
  this flow does not authenticate with Steam. The registered Gameforge installation ID is passed to the worker so the
  client's later `NoS0577` login packet uses the same ID as the Gameforge requests.
- MFA and image-CAPTCHA challenges are completed in the page when Gameforge asks for them. Gameforge can require other
  verification or launcher-specific headers; those responses appear in the sign-in form for diagnosis.
- **Installation ID.** On Windows the host prefers the ID the official client actually uses (its virtualized
  machine-wide value, then the machine-wide, then the per-user one): a stale per-user value can allow sign-in and
  iovation while code retrieval returns HTTP 403. Elsewhere set `TNT_INSTALLATION_ID` (or `--gf-installation-id`) to
  the UUID of an existing registered Gameforge installation. Do not generate a new UUID: Gameforge can accept the
  credentials and still reject launch-code creation with HTTP 403 when the installation is unregistered.
  `GF_USER_AGENT` or `--gf-user-agent` overrides the sign-in User-Agent.
- **Identity file.** If browser-collected identity data is rejected at code retrieval, use `--gf-identity-file
  /path/to/identity.json` (or `GF_IDENTITY_FILE`): an identity collected by your official launcher, refreshed and
  bound to each launch session. Its installation ID must match the one above. It contains device fingerprint data;
  keep it local and out of version control. To build one from a `tra:` blackbox taken from your own launcher's
  iovation request (for example with [GflessClient's BlackboxExtractor](https://github.com/hatz2/GflessClient)):

```sh
python3 recomp/web/gameforge_identity.py --blackbox-file blackbox.txt --installation-id YOUR_INSTALLATION_UUID --output official.gameforge-identity.json
dotnet run --project desktop/ClientLab.Launcher -- --serve --game /path/to/nostale --build /path/to/web --gf-installation-id YOUR_INSTALLATION_UUID --gf-identity-file official.gameforge-identity.json
```

The output format is ClientLab's version 12 fingerprint array, rather than GflessClient's named-field identity JSON.

### Scripted runs

With `--serve` running, `node recomp/web/drive.mjs [--maps 2,20,145] [--trace] [--dump-frame]` (Playwright with the
installed Chrome) logs in, enters the world, walks, chats, opens the game windows and teleports between maps, then
saves screenshots and the client log to `C:\tools\re\work\drive\`. It is the starting point for NosCore end-to-end
tests.

## Layout

| Path | What |
|---|---|
| `recomp/lift.py` | x86 → C lifter |
| `recomp/runtime/` | runtime shared by the native and browser builds |
| `recomp/port/` | portable Windows host (HLE of the client's DLLs) and the WebGL renderer |
| `recomp/web/` | page, game and render workers, `drive.mjs`, `iterate.py`, Gameforge identity importer |
| `desktop/` | C# launcher, game host (`--serve`), bootstrapper, checks and plugin API |
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
