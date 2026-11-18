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

## Driving the game through its own functions

`recomp/web/control.js` lets a test or an agent play without aiming the mouse. The page sends one JSON request at a
time through a shared buffer (`window.clientControl.request({...})`); the game thread answers it between window
messages, so a client function called there runs where the client's own input handlers would. Nothing builds or
sends a packet: an action is the call a click or a key ends in, and the client sends what it would have sent for it.

The functions and globals are found by byte patterns in the client's code, so they follow the executable rather than
a fixed address. `{op: 'api'}` lists what this build resolved (0 = not found).

| Request | What it does |
|---|---|
| `{op:'state', radius, vnums, kinds}` | Character (id, name, map, cell, level, HP/MP, sitting), target, monsters, NPCs, players, ground items, nearest first with their `distance`; the optional filters keep entities within `radius` cells, with those `vnums`, or only some lists |
| `{op:'skills'}`, `{op:'inventory'}`, `{op:'grid'}` | Learned skills, inventory, the map's walkability grid |
| `{op:'walk', x, y}` | A floor click on that cell |
| `{op:'select', id}` | Selects the entity as the target |
| `{op:'interact', id}` | A click on the entity: select it, walk up, then attack with the armed skill or talk |
| `{op:'skill', castId, target}` | Selects the target, arms the skill from the skill bar and uses it; refused while sitting, on cooldown or out of range |
| `{op:'pickup', id}`, `{op:'collect', id}`, `{op:'rest'}` | Picks up, collects from a map object, sits or stands |
| `{op:'use_item', vnum}` or `{op:'use_item', pocket, slot}` | A double click on the inventory item |
| `{op:'dialogs'}` | Open yes/no boxes (revive, teleport offers, confirmations): their text and what Yes and No answer |
| `{op:'answer', yes, text, index}` | Presses the Yes or No button of the box whose text or answers contain `text` (or the one at `index`) |
| `{op:'read', address, length}`, `{op:'scan', signature}`, `{op:'call', address, eax, edx, ecx, stack}` | For finding new structures and functions |

`recomp/web/mcp.mjs` wraps these, plus screenshots, clicks and keys for the windows that have no call yet, as an MCP
server over stdio. It opens its own Chromium on the game host:

```bash
claude mcp add clientlab -- node /path/to/NosCore.ClientLab/recomp/web/mcp.mjs --url http://127.0.0.1:8090/ --params PORT_SCREEN=1280x720
```

`enter_world` picks the first server and channel and the character slot; the other tools then work on the character
in the world. On another server's character select, pass `slot` (or start the MCP with `--slot x,y`) as the
character's position on a screenshot. `screenshot` returns its size, and `click` and `slot` take that screenshot's
pixel coordinates, whatever the window size.

To drive the desktop app instead, start it with `CLIENTLAB_DEVTOOLS_PORT=9222` and attach to it; `client_close` then
only disconnects and leaves the app running:

```bash
claude mcp add clientlab -- node /path/to/NosCore.ClientLab/recomp/web/mcp.mjs --attach http://127.0.0.1:9222 --log C:/dev/clientlab-actions.jsonl
```

`--log` appends every action (walk, select, answer_dialog, ...) with its arguments and result as one JSON line, so a run
can be replayed or reviewed next to the packet log. Reads such as `client_state` and `screenshot` are left out.

With the packet log on (`PORT_PACKET_LOG=1`, which the MCP adds to the pages it opens, or the launcher's packet logger
setting), the page keeps the last 2000 cleartext packets. `packets` lists them and `wait_for` waits for one by header,
direction or text, counted from just before the last action so a fast reply is not missed. `wait_for` also takes
`dialog` to wait for a yes/no box.

`guard` keeps a character alive while an agent thinks: below `hp_below` or `mp_below` percent it uses `hp_item` or
`mp_item` from the inventory, and it presses Yes on any box whose text or answers contain one of `accept` (a teleport
offer, `#revival^0`). It runs before each tool, inside `walk` and `wait_for`, and every second while the MCP is idle;
`guard` with no arguments shows what it did. `client_state` lists entities nearest first with their distance in cells,
so an agent can pull the closest group instead of a whole room.

`walk` refuses a cell on or next to a portal of the current map (the client's own portal list, shown by `portals`)
unless `through_portal` is set, so a character is not sent through one by accident. Around portals it plans the route
itself, in legs short enough that no route the client could pick between them passes one, and a `walk` with `wait`
off is refused when it could. A walk that ends on another map returns the character there with `changedMap`.

`client_state` leaves out monsters the server has reported dead (an `su` whose target is no longer alive, or `die`)
until they appear again, since the client keeps drawing them; this needs the packet log. `use_item` refuses when the
client would name another entity than the character as the item's user.

`interact` and `pick_up` return once the walk to the target is over and, for `pick_up`, once the item has left the ground. `rest` returns once
the server has sat or stood the character, and `use_skill` waits up to `wait` seconds for the skill's cooldown.

The client gives up on a long detour without moving. When a `walk` stalls, the MCP finds a path on the map grid and
walks it in short legs; if that fails too, `walk` returns an error with where the character stopped. The guard's
`food_item` is eaten only when no monster is within `safe_radius` cells, since eating sits the character down.

On a Linux machine without a GPU, headless Chromium stops updating the canvas after the login screen, although the
game keeps running. Run it headed under Xvfb with ANGLE on Mesa's GL instead:

```bash
sudo apt-get install -y xvfb libegl-mesa0 libegl1 libgles2
CHROME_ARGS="--use-angle=gl --ignore-gpu-blocklist" xvfb-run -a -s "-screen 0 1920x1200x24" \
  node recomp/web/mcp.mjs --headed --params PORT_SCREEN=1280x720
```

The control requests do not depend on the display; only `screenshot` does.
