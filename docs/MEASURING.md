# Measuring frame rate and game speed

How to check a change to frame pacing or rendering on a real machine (the reference is the NUC: Radeon 780M, 1080p).
Headless Chrome renders WebGL in software, so every run below uses `--headed`.

## Frame pacing modes

Set as page parameters (`drive.mjs --params ...`, or `?PORT_...=1` in the address bar):

| Mode | Parameter | Client limiter (`SleepEx`) | Present waits for | Use |
|---|---|---|---|---|
| Display (default) | none | skipped while the game presents | `requestAnimationFrame` | normal play: up to the display's refresh rate |
| Client limiter | `PORT_CLIENT_LIMITER=1` | waits (16 ms by default) | `requestAnimationFrame` | the behaviour before this mode existed: about 62 frames/s |
| Uncapped | `PORT_UNCAPPED=1` | always skipped | a `MessageChannel` yield | measuring only: frames are not shown at the display's pace |

The log says which mode is on (`frame pacing: ...`). In every mode the worker yields to the page once per Present.

On a 60 Hz panel the default and the client limiter both show about 60 frames/s. To see the default go past 60 there,
let Chrome run animation frames unthrottled: `DRIVE_CHROME_ARGS="--disable-gpu-vsync --disable-frame-rate-limit"`.

## Steps

From the repository root, once per build (the `main` build is "before", the change's branch is "after"):

```powershell
git checkout <branch>
python recomp\wasm\compile_all.py --web
python recomp\web\serve.py --game "C:\Program Files (x86)\Nostale" --user <account> --password <password>
```

Then, in a second shell, with `<maps>` the map ids of NosVille and Chicken Farm Path used for earlier measurements:

```powershell
node recomp/web/drive.mjs --headed --maps <maps> --params "PORT_PROFILE=1"
node recomp/web/drive.mjs --headed --maps <maps> --params "PORT_PROFILE=1&PORT_CLIENT_LIMITER=1"
node recomp/web/drive.mjs --headed --maps <maps> --params "PORT_PROFILE=1&PORT_UNCAPPED=1"
$env:DRIVE_CHROME_ARGS="--disable-gpu-vsync --disable-frame-rate-limit"; node recomp/web/drive.mjs --headed --maps <maps> --params "PORT_PROFILE=1"; $env:DRIVE_CHROME_ARGS=""
```

The `main` build has no `PORT_CLIENT_LIMITER`: its default is the client limiter, so its first two runs are the same.

What to read after each map (`drive.mjs` prints the status line; the rest is in `full.log` in the drive folder):

- `status:` `N frames/s shown` (or `presented` when uncapped);
- `profile:` the ms/frame split (draw calls, GL submit, texture uploads, present, game, vertex fetch, network relay);
- `frames:` late frames and the worst frame;
- `draws per frame:` draws on the vertex-shader path, how many joined the previous command, draws transformed on
  the CPU and their vertices, GL commands and runs (builds with the renderer bookkeeping change).

### Results

| Map | Mode | Build | frames/s | ms/frame | game | draw calls | GL submit | present | vertex fetch | GL commands |
|---|---|---|---|---|---|---|---|---|---|---|
| NosVille | client limiter | before | | | | | | | | |
| NosVille | display | after | | | | | | | | |
| NosVille | display, unthrottled Chrome | after | | | | | | | | |
| NosVille | uncapped | before | | | | | | | | |
| NosVille | uncapped | after | | | | | | | | |
| Chicken Farm Path | client limiter | before | | | | | | | | |
| Chicken Farm Path | display | after | | | | | | | | |
| Chicken Farm Path | display, unthrottled Chrome | after | | | | | | | | |
| Chicken Farm Path | uncapped | before | | | | | | | | |
| Chicken Farm Path | uncapped | after | | | | | | | | |

Earlier reference (after #18, uncapped): NosVille about 130 frames/s, Chicken Farm Path about 246; with the client
limiter, about 62 everywhere.

## Game speed

Every clock the client can read follows real time, whatever the frame rate: `GetTickCount` and `timeGetTime` are
milliseconds and `QueryPerformanceCounter` microseconds (frequency 1 MHz) of the same monotonic clock (`hle_kernel32.c`).
So anything the client works out from elapsed time runs at the same speed at 60 or 144 frames/s. What the port cannot
show is whether some client logic moves a fixed step per game tick instead (one tick per frame:
`TNosTaleMainF.AppIdle` runs one `TNosTaleGame.Tick` and then the limiter). Such logic would run faster as the frame
rate rises. The client's own limiter option goes down to 3 ms, about 330 frames/s, which suggests it was meant to
run at any of those rates, but that has to be checked:

1. Walking: from the same position, run once per mode and compare.

   ```powershell
   node recomp/web/drive.mjs --headed --walk-check --walk-teleport "<map> <x> <y>"
   node recomp/web/drive.mjs --headed --walk-check --walk-teleport "<map> <x> <y>" --params "PORT_CLIENT_LIMITER=1"
   ```

   Each run prints when each packet the client sent while walking left (`walk check: packets sent at ...`) and saves
   `walk-1000ms.png`, `walk-2000ms.png` and `walk-3000ms.png`. The two runs should send the same number of packets at
   about the same times (within a frame and the 40 ms polling), and the screenshots taken at the same time should
   show the character at the same place. A server that checks walking speed would also complain if the client walked
   faster.
2. By eye, at the highest frame rate the screen allows: NPCs walking, the river's scrolling layers and foam, flags
   and other animated effects, and chat bubble and damage number timing should look the same as with
   `PORT_CLIENT_LIMITER=1`.

If anything runs faster, note which and keep `PORT_CLIENT_LIMITER=1` for it until the client logic is found.
