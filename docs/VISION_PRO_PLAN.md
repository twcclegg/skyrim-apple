# Vision Pro plan

Native Apple Vision Pro support for the AOT-recompiled 2011 Skyrim runtime, developed in
`twcclegg/skyrim-apple` (DXVK in `twcclegg/dxvk-native`, branch `skyrim-apple`). Upstream
(`zschwendi/skyrim-apple`) is inactive. This document says what the project is now and what comes next, in
order. Measurements and their history are in [perf/LOG.md](perf/LOG.md) and issue #42. Rewritten 2026-11-20;
the earlier milestone plan is summarised under [Superseded](#superseded).

## Status (2026-11-20)

- **Target:** the game at 45 fps on the 90 Hz display, each game frame shown for two refreshes: **22.2 ms per
  game frame** for CPU, GPU and the compositor's share.
- **Works on the M2 Vision Pro:** live per-eye stereo (DXVK multiview), head tracking, and the
  depth-reprojecting immersive compositor, at the full field of view of both eyes. In Riverwood the game holds
  45 fps and every refresh is presented (headset, 19 Nov; #42).
- **Not known:** whether 45 fps holds in combat at the current config. Every timed headset run at this config is
  in Riverwood, the quiet scene. No stereo boss-fight run has been recorded on the headset at all; the 18 Nov
  boss-fight runs at 1280×720 and `LB_FOV=65` (549–587 pipelines compiled at draw) were Mac dry runs.
- **Windowed play** (Shared Space window) also works: about 60 fps at 1280×720 on the Low preset, with audio,
  keyboard and saves. The trackpad can't keep the pointer in the window, so mouse look stops at its edge.
- **Visible defects:** hitches from pipelines compiled at draw time; the reticle doubles on fast pans (fix in
  #66); shadows break with eye views (eye views stay off).

## How it works

### Runtime (from the iPad port)

- `gen/` (private, per user) is the x86 game lifted to C and compiled for arm64 with the runtime, with pinned
  guest registers (`LB_PINNED_REGS`, which also needs three LLVM passes off: `macos/CMakeLists.txt`).
- `runtime/` emulates Win32, D3D9, XAudio and DInput. D3D9 goes through `runtime/d3d9_bridge*.cpp` into DXVK
  (D3D9 → Vulkan) → MoltenVK → Metal.
- `runtime/platform_sdl.c` owns the SDL2 window, event pump, pacing and suspend; `ipad/native_bridge.mm` is the
  C API the SwiftUI launcher calls. The game runs on the main thread inside the launcher.
- `ipad/build.py --platform visionos` builds one static `libSkyrimNative.a` (gen, runtime, SDL with the
  visionOS overlay, DXVK, MoltenVK, FFmpeg) that `xcodebuild` links into the `SkyrimVision` app. The 4 GiB guest
  reservation uses `extended-virtual-addressing` and `increased-memory-limit`.

### Stereo: DXVK multiview

- `LB_STEREO_MULTIVIEW=1` gives two layers to colour targets of the back buffer's size and to their
  depth-stencil, and draws those passes once with two views (main scene, effects, first person, full-size post,
  interface). Each view gets its own clip matrices. Shadow maps, the downsampled post chain and the 960×540 pass
  (`refl` class; what it is: see [perf/LOG.md, Pass classes](perf/LOG.md#pass-classes)) stay mono, and the
  two-view passes sample them in both eyes.
- `LB_STEREO_EYE_UNITS=2.24` (half of a 64 mm separation at 70 units per metre) is required; without it live
  stereo stays off.
- `LB_DEPTH_COPY` (on with multiview) replaces the game's second scene pass into an R32F depth target with a GPU
  copy of the hardware depth: −12% GPU in stereo on the M1.
- `LB_STEREO_PRESENT=1` hands both eyes' final pictures and depth layers to the compositor through the present
  tap in `d3d9_swapchain.cpp` (`VK_EXT_metal_objects`). The interface is drawn at 2 m (`LB_STEREO_HUD_DEPTH=2`);
  #66 with dxvk-native #7 draws it as its own flat layer instead.
- First-person draws write depth through their own near plane; the bridge rescales it to the world's
  (`LB_FP_DEPTH_SCALE`), so hands and weapons are reprojected at the right distance.
- On a device without layered rendering (the visionOS simulator) #63 with dxvk-native #6 falls back to mono.

### Immersive compositor (`apple/Sources/App/ImmersiveScreen.swift`)

- A `CompositorLayer` frame loop at 90 Hz shows the newest finished game frame; at 45 fps each is held for two
  refreshes (`immersive cadence: … held 2`).
- Each eye's picture is drawn as a mesh from its depth layer, at the pose it was rendered with, and viewed from
  the current head pose, so head motion between game frames is corrected with parallax. `LB_IMMERSIVE_REFINE=N`
  adds a per-pixel depth search that removes the mesh's shards at foliage edges (#64 makes it the default).
- Gaps the mesh leaves at depth edges get a blurred fill (`LB_IMMERSIVE_HOLES`, on by default; about 2.3 ms of
  compositor GPU on the headset, 16 Nov).
- Cost on the headset: 3.4–4.2 ms per refresh, twice per game frame (19 Nov).
- Implemented, not in the best config: system foveation of the compositor pass (`LB_FOVEATION=1`, never run on
  the headset) and following the player between game frames (`LB_IMMERSIVE_MOTION=1`).

### Head steering (`LB_IMMERSIVE_HEAD=1`, [HEAD_TRACKING.md](HEAD_TRACKING.md))

The compositor publishes the head's yaw and pitch; each game frame the runtime turns the game's camera toward
them with synthetic mouse look, closed-loop on the camera it reads at Present. Aim follows the head; the right
stick adds a body yaw offset. The compositor corrects what steering hasn't caught up with. On the headset,
steering trailed by up to 12.5° in pitch and its vertical-inversion detector flipped; #65 fixes both (Mac
sweep: lag 5.4°/8.8° → 2.2°/3.8° mean/max).

### Field of view and eye views

- Each eye sees tangents 1.732 outward, 1.0 inward, 1.0 up, 1.192 down, eyes 66 mm apart (about 105° × 95°).
  A symmetric game frustum covering both eyes plus the compositor's 2° margin is ±1.881 × ±1.280: `LB_FOV=119.8`
  at the 4:3 reference. Per eye only about 62% of the rendered pixels are ever shown.
- Eye views (`LB_EYE_VIEWS=1`, with `LB_EYE_LATE=1` turning the eyes toward the head before the main pass)
  render each layer through that eye's own frustum. On the headset they put the scene's shadows on a tilted plane
  in one eye and make them swim in the other, so they are off until items 3 and 4 below.

## Current best headset config

| Key | Value |
|---|---|
| `LB_IMMERSIVE_HEAD` | `1` |
| `LB_STEREO_EYE_UNITS` | `2.24` |
| `LB_STEREO_MULTIVIEW` | `1` (turns on `LB_DEPTH_COPY`) |
| `LB_STEREO_PRESENT` | `1` |
| `LB_STEREO_HUD_DEPTH` | `2` |
| `LB_WINDOW` | `1600x1100` (the eyes' aspect) |
| `LB_FOV` | `119.8` |
| `LB_IMMERSIVE_REFINE` | `4` |
| `LB_MAX_FPS` | `45` (the visionOS default) |
| eye views | off |

Riverwood at this config (19 Nov, capped): DXVK GPU busy 57–65%, 12.7–14.4 ms per frame; game GPU latency mean
15 ms; compositor 3.4–4.2 ms per refresh. Capped busy time on Apple GPUs is not a cost measure (the GPU clocks
down with slack; #42 correction), so the headroom is uncertain both ways. A measured win not yet in it:
`LB_LOCK_STAGING=1` (−3 to −10% game work by phase on the M5).

## Work list, in order

Numbers are the items of the 2026-11-20 plan review; the order is by expected value per effort. S ≤ 1 day,
M 2–4 days, L a week or more.

| Rank | Item | What | Size / where | Depends on |
|---|---|---|---|---|
| 1 | 1. Headset frame budget | The timed session never run: stereo 1600×1100, stereo 1280×880, mono 1280×880 (`LB_FOV=119.8 LB_DEPTH_COPY=1`), interleaved, two rounds, `boss-fight-avp` and `whiterun-market-avp`, two warm-ups; then one uncapped stereo 1600×1100 run. Decides GPU work vs CPU work next, and whether 1600×1100 holds in combat | S / headset | — |
| 2 | 2. Compositor cost | A/B `LB_FOVEATION=1` against off and `LB_IMMERSIVE_REFINE=2` against 4; make foveation the default if clean. Expect 2–4 ms of GPU per game frame back | S / headset | #64 |
| 3 | 3. The 960×540 pass | **Done 2026-11-20:** it is the deferred sun-shadow mask (back buffer × `iShadowMaskQuarter`/4), drawn mono with view 0's matrices and before the late eye turn. With the measured eye tangents the right eye's ground shadow error is 17.0 against 2.2 for plain multiview (M1, 0–255 scale). The mask draws geometry; it does not rebuild position from depth | S / Mac | — |
| 4 | 5. Pipelines compiled at draw | A warm state cache still compiles at draw in stereo (160 compiles, longest 201 ms, in one headset session). Log why entries miss on the next launch; check per-draw state that multiplies variants and whether replay finishes before play; fix; ship a seed cache if entries are portable | M / Mac, then headset | — |
| 5 | 9. Comfort session | One headset session for #65 (steering), #66 (flat HUD) and `LB_IMMERSIVE_MOTION=1` against 0 while moving; merge what passes | S / headset | #63, #64 |
| 6 | 10. CPU wins onto the headset | (a) `LB_LOCK_STAGING=1` by default after one headset pair; (b) **measured 2026-11-21:** the three LLVM passes pinned registers turn off cost 0.3–0.5% of game work (Riverwood, the M5), so a compiler fix is not worth carrying for speed; (c) `LB_STRICT_TRAP` only if the CPU binds | S / headset, M5 | (a) rides with 1 |
| 7 | 6. GPU attribution | **Measured 2026-11-21 (the M5, log):** GPU 6.7 ms per frame in Riverwood, 9.1–9.8 in the boss fight; main scene 2.7–3.4, effects 1.3–2.4, shadows 0.9–2.3 (both grow in combat), depth pass 0.45. `bFloatPointRenderTarget=0` and `d3d9.floatEmulation` save nothing; a direct depth copy could save at most 0.45 ms. Candidate: the main scene runs as 10–12 passes in a row and shadows as up to 27, each loading and storing the full two-layer colour and depth even without GPU timers (the timers roughly double the splits in combat) | S+ / M5 | — |
| 8 | 4. Per-eye shadow mask, earlier late eyes | **Draft #71 with dxvk-native #9 (2026-11-20):** right-eye error 17.0 → 3.3 on the M1, about +0.45 ms per frame there. The remaining 3.3 was the bloom and distance-blur targets (quarter size, drawn mono): draft #72 draws them per eye, bringing the right eye to run-to-run noise for about +0.3 ms. Both await the headset check. Two layers for the mask target and its depth-stencil, flagged by the runtime (not by size or aspect: at quarter 1 the mask matches the luminance targets' size), so the pass runs in both views; late eyes at the first draw into the mask, not the first main-pass draw. No per-view inverse needed. Zero-code headset check first: `iShadowMaskQuarter=4` makes the mask back-buffer-sized and already two-view. Makes eye views usable, so each eye uses all its pixels | L / M5, headset | 3 (done); 7 for device captures |
| 9 | 7. Right-eye captures on the device | `LB_SHOT` returns layer 0 for both eyes on the headset; fix the read-back or dump the compositor's input | S–M / Mac, headset | — |
| 10 | 11. Budget report in the repo | Session driver and summariser into `tools/bench`; `summary.json` gets cadence, GPU latency, compositor GPU, at-draw compiles, hitches, thermal state | S | — |
| 11 | 12. Plan and log | This document and the repaired log; fix the `refl` label once 3 answers | S | 3 for the label |
| 12 | 13. Soak | 30 min of scripted Whiterun at the chosen config, on AC then battery | S / headset | 1 (ideally 2, 5) |
| 13 | 8. Skip unseen pixels | Only if 4 stalls: near depth in the strip each eye can't see, per view, so early depth rejects it there (about 23% of each eye's width on scene passes) | M / M5, headset | 6; excludes 4 |
| 14 | 14. Alternate-eye compositor | Only if 1 shows the GPU binding in combat: show `LB_STEREO_ALTERNATE=1` frames (rendering is on main, #31) by keeping one frame slot per eye and moving each from its own render time, rays from each eye's origin. Near-zero extra GPU and no mono-pass shadow fault; each eye updates at half the game rate and the eyes are one frame apart, so self-moving objects may shimmer (comfort to judge on the headset). Alternative to 8 | M / headset | 1; after #66 and 2 in `ImmersiveScreen.swift` |

**In parallel now:** 3, 5, 6, 11, 12 and 10b. One headset session covers 1, 2 and 10a; a second, shorter one
covers 9; 13 runs unattended afterwards. Serial: 3 → 4; 6 → 8; 4 and 8 both edit `d3d9_device.cpp`;
#64 → 2 → #66 in `ImmersiveScreen.swift`; #63, #66, 4 and 7 all edit `d3d9_bridge.cpp`, so land #63 first.

**Not on the list, with reasons**
- More lifter work (#22 items 2–5, #57 stack slots, PGO, order file): each measured as nothing on top of pinned
  registers. Reopen only if item 1 shows game work p95 over about 18 ms at FOV 119.8.
- Havok threading, guest CPU count, QoS hints, half-rate shadows, single-threaded device: closed with numbers
  (#42).
- Foveated game render through a MoltenVK patch: broke the same screen-position shadow lookups and its GPU gain
  was inconclusive (#21). Revisit after items 4 and 6.
- Temporal upscaling: MetalFX's temporal scaler is unavailable in the visionOS SDK, and neither it nor a
  hand-written resolve gained in moving foliage (#21).

## Constraints

- **Hardware:** the original M2 Vision Pro (8-core CPU with 4 performance cores, 10-core GPU, 16 GB),
  visionOS 27.3. The M5 MacBook Air is the A/B machine for GPU and CPU questions; the 8 GB M1 swaps and is used
  for counts, captures and builds.
- **Thermals and power:** after about 2 h on battery, runs were 7% slower, then *serious*; the headset powered
  off twice on battery, never on AC. Drop runs at *serious* from comparisons.
- **Memory:** a 19 Nov JetsamEvent report lists the game as the largest process (1.76 GB resident, peak
  2.37 GB); whether it ended a session is not established.

## Validation conventions

- Label every claim measured / read / inferred; a Mac or the simulator is not the headset.
- Per run: device, OS, build and DXVK commits, thermal state, frame counts, logs.
- **Measure GPU cost uncapped** (`LB_MAX_FPS=0`, `DXVK_CONFIG=d3d9.presentInterval = 0`). Capped busy time
  misstates differences because Apple GPUs clock down with slack.
- Interleave arms, rotate the order, one warm-up per arm, six or more rounds for the boss fight; pin the weather.
- On a Mac the OS's Metal cache hides compile time: count compiles there, time them on the headset.
- `LB_SHOT`'s per-layer capture is wrong on the device (item 7); verify stereo by capture on a Mac.
- `LB_*` switches with an A/B for each risky setting. Never commit `gen/`, game files, provisioning, team or
  bundle IDs, or captures of the game.

## Superseded

The 2026-11-10 plan ordered the work as windowed play → mono "cinema" screen → alternate-eye stereo through
hooks on the game's camera → PSVR2 Sense. Windowed play and the cinema screen were built (the bring-up blockers:
SDL visionOS overlay, xros MoltenVK and FFmpeg, build tooling, memory entitlements, deployment target, launcher
gating). The cinema screen grew into the depth-reprojecting compositor. Alternate-eye stereo and the camera hooks
were replaced by DXVK multiview (both eyes every frame) and by head steering through mouse look, which needs no
regenerated code. (Alternate-eye rendering itself survives as `LB_STEREO_ALTERNATE`, rendered through DXVK's clip
shift rather than camera hooks; its compositor side is work item 14.) PSVR2 Sense input is deferred: no controllers to test with. The full text is in git history.

### Game camera addresses (from the old M3 notes; used by `game_camera_read` in `runtime/d3d9_bridge.cpp`)

Static reads of the 1.9.32.0.8 executable; only the camera read itself is verified at runtime.
- Main world NiCamera W: `*(*(0x1B2E8BC)+0xB8)`. World rotation +0x54..+0x74 (3×3, row-major), translation
  +0x78..+0x80, world → clip matrix +0xA8..+0xE4, frustum tangents l r t b +0xE8..+0xF4, near/far +0xF8/+0xFC,
  ortho +0x100. 0xAB8640 builds the matrix from the transform and the frustum.
- PlayerCamera singleton 0x12E7288: worldFOV +0xAC, firstPersonFOV +0xB0. FOV → frustum at 0xB17960.
- Frame order in 0x69F950: PlayerCamera update (0x73F240) → world render 0x69BDF0 (frustum via 0x6975D0). The
  first-person camera (0x697FC0 / 0x698B30) re-runs 0xB17960 on W with firstPersonFOV and restores W's frustum.
