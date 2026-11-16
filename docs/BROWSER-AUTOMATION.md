# Browser launcher and Playwright

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
