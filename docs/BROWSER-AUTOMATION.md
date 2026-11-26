# Browser launcher and Playwright

ClientLab has one game server, the C# game host (`desktop/ClientLab.Launcher/Services/GameHost.cs`). The desktop
window, the browser launcher and the headless `--serve` mode below all run it; there is no separate Python server.

## Headless game server (`--serve`)

`--serve` starts only the game host, without a launcher window, so Playwright or a normal browser can open the game:

```powershell
# The game the launcher last prepared (its data folder's active.json), NosCore code minted per page load:
dotnet run --project desktop/ClientLab.Launcher -- --serve --user <account> --password <password>
# A dev build from recomp/wasm/compile_all.py against your own install:
dotnet run --project desktop/ClientLab.Launcher -- --serve --game "C:\Program Files (x86)\Nostale" `
    --build C:\tools\re\work\wasm\web --user <account> --password <password> --source-root .
# Published executable:
./ClientLab.exe --serve --user <account> --password <password>
```

It prints one machine-readable line, `CLIENTLAB_URL=http://127.0.0.1:8077/`, and serves until Ctrl+C. Point
`drive.mjs`, `iterate.py` or Playwright's `page.goto` at that URL; a top-level navigation passes the host's
Host/Sec-Fetch-Site/Origin checks, while other sites' requests are refused.

| Option | Meaning |
|---|---|
| `--game <dir>` / `--build <dir>` | Game folder and browser build (`nostale.js`). Without `--game`, the prepared game in the data folder is used. `--build` defaults to `$NOSRECOMP_WORK/wasm/web`. |
| `--client-exe <file>` | The exact executable the build was recompiled from, mounted as `NosCore.exe`. Defaults to the game folder's `NosCore.exe`, then `NostaleClientX.exe`. |
| `--port <n>` | Loopback port, default 8077; 0 picks a free one. |
| `--user <name> --password <pw>` | NosCore account: a fresh launch code is minted from `--auth-url` (default `https://127.0.0.1:7001`) on every page load. |
| `--auth-code <code>` | A pre-issued code instead (or `CLIENTLAB_AUTH_CODE`). |
| `--provider noscore\|gameforge` | Defaults to `noscore` with `--user`/`--auth-code`, otherwise `gameforge`: open the URL with `?login=gameforge` to sign in from the page. |
| `--host <host>`, `--remap FROM=TO` | NosCore only: send every connection to this host, or a port elsewhere (repeatable). |
| `--gf-installation-id`, `--gf-identity-file`, `--gf-user-agent` | Gameforge identity overrides (or `TNT_INSTALLATION_ID`, `GF_IDENTITY_FILE`, `GF_USER_AGENT`); otherwise the launcher's saved settings and the Windows registry. |
| `--packet-log` | Write the packet log, as the launcher's packet logger does. |
| `--open` | Also open the URL in the default browser. |
| `--data-dir <dir>`, `--source-root <dir>` | Launcher data folder (settings, prepared game, saved game options); page sources from a checkout instead of the executable. |

Game options the page saves (Config.ini) go to the data folder, as in the desktop window. On Windows the executable
attaches to the terminal that started it, so its output shows there.

## Browser launcher

The browser launcher uses the desktop launcher's C# coordinator, authentication state
machine, credential vault, update preparation, recompiler, and game host.

```powershell
dotnet run --project desktop/ClientLab.Launcher -- --browser --no-open --port 8077
# Published executable:
./ClientLab.exe --browser --no-open --port 8077
```

Open the URL printed at startup, including its fragment on the first visit. The fragment
is the local API capability; the page retains it in session storage and removes it from
the address bar. `--no-prepare` skips startup preparation for isolated UI tests.
Both the desktop launcher and browser offer **Gameforge** and **Private server** modes.
Private server settings include the game folder, NosCore authentication URL, an optional
game host override, and optional `originalPort=serverPort` mappings.

## Explicit CAPTCHA and MFA answers

An image challenge shows the official instruction, four numbered items, and the target.
The user can drag an item, click its numbered button, or provide its number. When the
user says "the answer is 4", an automation agent can submit that answer:

```javascript
await page.getByLabel('Matching item number', { exact: true }).fill('4');
await page.getByRole('button', { name: 'Submit CAPTCHA answer', exact: true }).click();
// Alternatively:
await page.getByRole('button', { name: 'CAPTCHA answer 4', exact: true }).click();
```

Use the displayed challenge. Its version travels with the answer; stale answers are
rejected. Wait for the next stage rather than assuming that submission succeeded.
An automation agent should pause for an explicit user answer when an image challenge
appears. The UI does not implement an image-solving service.

For MFA, enter the code supplied by the user in `getByLabel('Authenticator code')`, then
click **Verify code**. Gameforge proof-of-work runs through its official hosted widget;
it does not require an image answer. Device collection uses a visible, full-size browser
frame and the actual browser's identity data.

## Shared local API

Requests use `X-ClientLab-Token` with the capability from the startup URL. The server binds
to loopback, validates Host/Origin, and does not expose credentials in state or Activity.
Saved credentials are restored through the OS vault, never browser local storage.

| Endpoint | Purpose |
|---|---|
| `GET /api/state` | Preparation, settings, Activity, sign-in stage and current challenge |
| `GET /api/news` | Complete official news/event feed |
| `POST /api/settings` | Save mode, game folder and server settings |
| `POST /api/task` | Start `startup`, `check`, `prepare`, or `tools` |
| `POST /api/cancel` | Cancel preparation |
| `POST /api/signin` | Start credentials sign-in |
| `POST /api/challenge` | Submit an explicit OTP or one-based CAPTCHA answer and version |
| `POST /api/account` | Select a linked game account |
| `POST /api/play` | Obtain a fresh launch code and open the prepared runtime |
| `POST /api/close-game` | Close the game's local host and socket connections |
| `POST /api/signout` | End sign-in and remove this mode's saved credentials |

## Analysis is a development step

Run Ghidra for each supported executable version, not on players' machines:

```powershell
./tools/analyze-client.ps1 -ClientExe /path/to/NostaleClientX.exe -WorkDir /path/to/local-analysis -GhidraDir /path/to/ghidra
```

The script exports Borland Delphi function ranges, VMT/dynamic-method boundaries, and
supplemental default-analysis entry points into `symbols/<exact-md5>/`. Review and test
that metadata, then commit it and bundle it with ClientLab. Proprietary executables,
Ghidra databases and generated client C remain in the local work directory.

The launcher performs patching and recompilation in C# using the matching map. Asset
patches reuse the compiled runtime. An unknown executable requires a compatibility
update; it never silently reuses a different executable's map or replaces a good build
with an incomplete analysis.
