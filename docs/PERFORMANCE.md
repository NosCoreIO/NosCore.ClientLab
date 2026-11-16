# Performance investigation

## Measurement protocol

Use a stationary character in the same crowded NosVille location, at the same
resolution and with the same packet-logging setting. Warm the scene before
measuring. Wait for background compilation to finish. Count frames actually shown
by the page, rather than treating the game loop or display refresh count as FPS.

Launch the desktop app with `CLIENTLAB_DEVTOOLS_PORT` set to an unused loopback
port. Once in-world:

```powershell
node tools/profile-live.mjs --port 9238 --seconds 20 --frames-only --out C:/dev/ClientLab-perf/sample
node tools/profile-live.mjs --port 9238 --seconds 12 --out C:/dev/ClientLab-perf/cpu
```

Use `--uncapped` to temporarily remove the presentation cap when measuring engine
headroom; the previous cap is restored afterward. Normal presentation targets
60 FPS, with `fps=0` in the game URL available for uncapped diagnostics.

The tool attaches to the existing game, without moving the character or closing
it. It records displayed FPS, refresh rate, display-interval percentiles, profile
logs and optional CPU samples for the page. Frame URL query
strings are removed from saved CPU profiles. New builds emit
`nostale.js.symbols` alongside the runtime, so the tool can recover C function
names without adding profiler instrumentation to the game. `--symbols` can also
specify the exact build's symbol map. Do not reuse a map for another build.

`--detail` temporarily enables per-draw/per-API clocks and restores them afterward.
This is for attribution, not FPS benchmarking: in the measured crowded scene
it reduced 40–42 FPS to 8–9 FPS. Normal samples leave this expensive split off.
Do not enable the worker's full source debugger just to sample CPU: enabling it
on this large module stalled.

On the later CEF session, enabling the game worker's sampler after startup also
timed out and left the inspector unresponsive. A later direct renderer-worker
update returned successfully but diagnostics became unresponsive afterward,
so that update has no valid after-FPS result. Avoid worker inspection on CEF 120.
The tool now never auto-attaches workers during frame-only or default page
profiling, rejects explicit worker sampling on CEF 120, and saves the frame
sample before attempting CPU profiling. Restarting cleared the stalled state.

## Baseline (17 November 2026)

Official client build 6089, runtime revision 20, desktop CEF, 1920×1080,
crowded NosVille, stationary character, packet capture enabled:

- Two normal displayed-frame samples: 40.7 FPS over 12 seconds and 42.1 FPS
  over 10 seconds. The page refreshed at approximately 118–119 Hz.
- Normal renderer logs: approximately 7–9 ms/frame, 3,300–4,300 WebGL draws.
  Game frames generally took approximately 22–25 ms, with larger outliers.
- Game-worker CPU samples attributed approximately 12% to Canvas text
  measurement and 6% to renderer-state `memcmp` calls. The latter were chiefly
  batch/state checks, often comparing inactive light slots.
- The baseline runtime was re-linked with `--emit-symbol-map`; all non-custom
  WebAssembly sections matched the running runtime byte for byte before its map
  was used to interpret the samples.

These are baseline observations, not evidence that the 60 FPS target has been
achieved. Keep live candidate measurements separate from component benchmarks.

## Candidates

Revision 22 preserves the earlier revision-21 guarded client routine replacements
and short-leaf inlining, and adds:

- A bounded GDI text-metrics cache, keyed by the actual resolved font and text.
  It preserves the width/ascent/descent returned to the guest, handles invalid
  font fallback, bypasses cache storage while fonts load, and limits retention
  to 4,096 entries with no text longer than 2,048 code units retained.
- Exact word comparisons for aligned renderer state, and comparisons of only
  active light entries. Unused light slots are neither serialized nor read by
  the shader, so they no longer need per-draw clearing/comparison.
- CPU world/view/projection multiplication deferred until a software vertex
  path actually reads it. GPU draws retain their original matrices.

Revision 23 is a separate candidate, adding allocation-free finite matrix-stack
operations (the original path handles non-finite inputs), and host-only
`-O3 -msimd128` compilation/linking. Lifted game objects keep their `-O2` compiler
identity; preparing this candidate reused 114 cached objects and compiled 17
portable-host objects. It is not a live FPS result.

Revision 24 adds a bounded, three-slot shared command pool (24 MB total). The
sender copies into a free slot and the renderer releases it only after decoding
and issuing its uploads. A busy pool, oversized command stream, disabled pool
(`PORT_GL_COMMAND_POOL=0`), or unsupported shared WebGL upload retains the
original transferable-buffer path. It never waits for a free slot. Capability
checks cover buffer and texture uploads before enabling the pool. Tests exercise
the actual sender and receiver, including ownership, ordering, nonzero offsets,
exception release, fallback, and exact pixels through a real WebGL worker.

Revision 24 was prepared locally and tested in the later crowded scene below.
The component checks do not establish a 60 FPS result.

### Follow-up crowded-world samples

The later session was much heavier than the original baseline, so these samples
are not directly comparable to the original 40–42 FPS scene. Its running package
had been replaced with revision 21. At 1920×1080 it showed 10.6–11.3 displayed FPS,
roughly 18,000 guest draws and 14,500 WebGL draws per frame. After explicitly
restoring the verified revision 24 package/runtime, a 20-second sample measured
17.9 displayed FPS, with roughly 17,000 guest draws and 12,400 WebGL draws. The
crowd and animation load differed between runs. A subsequent 12-second sample
measured 15.6 FPS; none establishes the 60 FPS target.

Revision 25 avoids repeated native Canvas font parsing/getters
for an unchanged font, invalidates cached text metrics when private fonts change,
and removes per-command renderer clock reads outside detailed profiling. It also
honors the separate D3D minification/mip-filter modes and builds mipmaps lazily:
non-mipmapped text updates no longer rebuild the entire texture pyramid.
GPU checks cover all six min/mip combinations, 500 non-mipmapped updates, deferred
generation, partial/full invalidation and nonzero LOD with actual pixel readback.
The attempted decoder return-value rewrite did not benchmark faster and was
discarded.

Revision 25 preparation reused 130 objects and compiled one changed host object.

The clean revision 25 run after reboot measured 23.9 displayed FPS over 20
seconds while assets were still loading, followed by 30.9 FPS over a warmed-up
30-second sample. The warmed sample had a 33.3 ms median display interval and
50 ms p95. An uncapped sample measured 27.5 FPS as the crowd/draw load increased;
removing the cap did not demonstrate a gain. Normal renderer work was roughly
13–16 ms/frame at 11,000–13,000 draws; total game frames remained roughly
30–40 ms. No compiler jobs or worker inspection ran during those measurements.

Revision 26 adds scalar draw-state caching with explicit invalidation for
render/sampler/stage state, viewport/scissor, target, depth, reset and state-block
restoration. Texture validity and uploads remain checked every draw. State
snapshots copy only active light storage. Specialized finite scale/translation
operations preserve the generic multiply's float bits and use its fallback for
exceptional inputs. Checks compare 100,000 draw-state transitions and 200,000
affine cases, including signed zero, overflow, NaNs, subnormals and full CPU state.
The additional sampled-API diagnostics experiment was removed; no profiling
feature is added to the launcher. A guarded-getter inlining microbenchmark was
slower, so that compiler change was not adopted.

Revision 26 is installed at `dist/ClientLab-win/ClientLab.exe` with its prepared
normal runtime. Preparation reused 114 game objects and compiled 17 host objects
in one second; the final optimizer used about 1 GB of memory. Its first
20-second crowded sample measured 14.4 FPS at about 20,000 WebGL draws and
303,000 primitives per frame, a substantially heavier load than the warmed
revision 25 sample. This does not establish a comparable before/after gain or
the 60 FPS target.

The separate whole-program LTO experiment compiled its objects, but its link
ran for about 15 CPU minutes and reached approximately 10 GB of working memory.
The user reported system lag and rebooted. The experiment produced no finished
runtime and must not restart automatically or run alongside a live benchmark.
Its object files were retained only as local diagnostic artifacts. LTO is not
enabled by the product compiler and has no demonstrated FPS benefit here.

The newer page also presents on a 60 FPS schedule at 60/120/144 Hz, retaining
the two-frame buffer and counting only frames actually shown. Slow producers
still report their actual output. The packet viewer appends/prunes arriving rows
instead of rewriting every retained row's attributes; a one-packet update in a
full 10,000-row view now makes two DOM changes.

## Focused checks

```powershell
node desktop/ClientLab.Checks/text-metrics-cache.mjs
node desktop/ClientLab.Checks/frame-pacing.mjs
node desktop/ClientLab.Checks/packet-logger-updates.mjs
node desktop/ClientLab.Checks/render-command-pool.mjs
node desktop/ClientLab.Checks/render-timing.mjs
node desktop/ClientLab.Checks/texture-mipmaps.mjs
emcc -O3 -msimd128 -Irecomp/runtime -Irecomp/port desktop/ClientLab.Checks/render-state-cache.c -sENVIRONMENT=node -o /local/work/render-state-cache.js
node /local/work/render-state-cache.js
emcc -O2 -Irecomp/port desktop/ClientLab.Checks/render-compare.c -sENVIRONMENT=node -o /local/work/render-compare.js
node /local/work/render-compare.js
emcc -O3 -msimd128 -Irecomp/runtime -Irecomp/port desktop/ClientLab.Checks/matrix-stack.c -sENVIRONMENT=node -o /local/work/matrix-stack.js
node /local/work/matrix-stack.js
```

The draw test compares 100,000 randomized states with the previous effective
state logic, including float bit patterns. The matrix test compares actual HLE
implementations and guest CPU state over 100,000 cases, including non-finite,
overflow, subnormal and signed-zero inputs. Microbenchmark numbers describe only
those kernels; optimization can hoist work in synthetic loops, so they do not
establish end-to-end frame-rate gains.
