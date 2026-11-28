# Vision Pro plan

> **Dated snapshot, 2026-11-28.** Status and work list are as of the merged and open pull requests and the
> issue #42 thread on that date. No headset results have been posted since 19 Nov, so every headset figure here
> is from 19 Nov or earlier; later results are from Macs.

Native Apple Vision Pro support for the AOT-recompiled 2011 Skyrim runtime, developed in
`twcclegg/skyrim-apple` (DXVK in `twcclegg/dxvk-native`, branch `skyrim-apple`). Upstream
(`zschwendi/skyrim-apple`) is inactive. This document says what the project is now and what comes next, in
order. Measurements and their history are in [perf/LOG.md](perf/LOG.md) and issue #42. Rewritten 2026-11-20,
refreshed 2026-11-28; the earlier milestone plan is summarised under [Superseded](#superseded). The project's
one rule is in [AGENTS.md](../AGENTS.md).

## Status (2026-11-28)

- **Target:** the game at 45 fps on the 90 Hz display, each game frame shown for two refreshes: **22.2 ms per
  game frame** for CPU, GPU and the compositor's share.
- **Works on the M2 Vision Pro:** live per-eye stereo (DXVK multiview), head tracking, and the
  depth-reprojecting immersive compositor, at the full field of view of both eyes. In Riverwood the game holds
  45 fps and every refresh is presented (headset, 19 Nov; [#42-c37]).
- **Not known:** whether 45 fps holds in combat at the current config. Every timed headset run at this config is
  in Riverwood, the quiet scene; no stereo boss-fight run on the headset is logged.
- **Windowed play** (Shared Space window) also works: about 60 fps at 1280×720, with audio, keyboard and saves.
  The trackpad can't keep the pointer in the window, so mouse look stops at its edge.
- **Visible defects:** hitches from pipelines compiled at draw time (item 5); the reticle doubles on fast pans
  (#66, #85); shadows break with eye views, so eye views stay off (item 4).
- **Merged since 20 Nov:** #81 and #82 (the project rule, AGENTS.md), #83 and #84 (comfort: re-entry and Resume,
  comfort INI defaults), #87 (compositor cost switches). #83, #84 and #87 have not run on the headset yet.

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
  interface). Each view gets its own clip matrices. Shadow maps, the downsampled post chain and the sun-shadow
  mask pass (`refl` class, renamed `mask` in #71; the back buffer × `iShadowMaskQuarter`/4, so 960×540 at
  1280×720 and 1200×825 at 1600×1100 with the low preset's 3; see [perf/LOG.md, Pass classes](perf/LOG.md#pass-classes))
  stay mono, and the two-view passes sample them in both eyes.
- `LB_STEREO_EYE_UNITS=2.24` (half of a 64 mm separation at 70 units per metre) is required; without it live
  stereo stays off.
- `LB_DEPTH_COPY` (on with multiview) replaces the game's second scene pass into an R32F depth target with a GPU
  copy of the hardware depth: −12% GPU in stereo on the M1.
- `LB_STEREO_PRESENT=1` hands both eyes' final pictures and depth layers to the compositor through the present
  tap in `d3d9_swapchain.cpp` (`VK_EXT_metal_objects`). The interface is drawn at 2 m (`LB_STEREO_HUD_DEPTH=2`);
  #66 with dxvk-native #7 draws it as its own flat layer instead, and #85 scales and places that layer.
- First-person draws write depth through their own near plane; the bridge rescales it to the world's
  (`LB_FP_DEPTH_SCALE`), so hands and weapons are reprojected at the right distance.
- On a device without layered rendering (the visionOS simulator) #63 with dxvk-native #6 falls back to mono.

### Immersive compositor (`apple/Sources/App/ImmersiveScreen.swift`)

- A `CompositorLayer` frame loop at 90 Hz shows the newest finished game frame; at 45 fps each is held for two
  refreshes (`immersive cadence: … held 2`). With `LB_FRAME_REPEAT=1` (#87) the compositor itself runs at 45 Hz,
  each of its frames shown for two refreshes, which halves its cost per game frame; the pose it corrects to is
  then up to 22 ms old instead of 11.
- Each eye's picture is drawn as a mesh from its depth layer, at the pose it was rendered with, and viewed from
  the current head pose, so head motion between game frames is corrected with parallax. `LB_IMMERSIVE_REFINE=N`
  adds a per-pixel depth search that removes the mesh's shards at foliage edges (#64 makes it the default).
- Gaps the mesh leaves at depth edges get a blurred fill (`LB_IMMERSIVE_HOLES`, on by default; about 2.3 ms of
  compositor GPU on the headset, 16 Nov).
- Cost on the headset, without #87's switches: 3.4–4.2 ms per refresh, twice per game frame (19 Nov).
- Implemented, not in the best config and never timed on the headset: system foveation of the compositor pass
  (`LB_FOVEATION=1`), following the player between game frames (`LB_IMMERSIVE_MOTION=1`), and #87's cost
  switches (`LB_IMMERSIVE_REFINE_EXIT`, `LB_IMMERSIVE_SEED`, `LB_IMMERSIVE_GRID`, `LB_FRAME_REPEAT`, with
  `LB_IMMERSIVE_PASS_TIME` to time them; [perf/IMMERSIVE_OPTIONS.md](perf/IMMERSIVE_OPTIONS.md#compositor-cost-switches)).

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
- `LB_FOV` must be set whenever eye views are off. Only with eye views on does the field of view follow the eyes
  (#61); without them an unset `LB_FOV` stays at the game's 65, the field of view of the 18 Nov launch check, where
  each eye saw a small window floating in black.
- Eye views (`LB_EYE_VIEWS=1`, with `LB_EYE_LATE=1` turning the eyes toward the head before the main pass)
  render each layer through that eye's own frustum. On the headset they put the scene's shadows on a tilted plane
  in one eye and make them swim in the other, so they are off until item 4 below is checked on the headset.

## Current best headset config

| Key | Value |
|---|---|
| `LB_IMMERSIVE_HEAD` | `1` |
| `LB_STEREO_EYE_UNITS` | `2.24` |
| `LB_STEREO_MULTIVIEW` | `1` (turns on `LB_DEPTH_COPY`) |
| `LB_STEREO_PRESENT` | `1` |
| `LB_STEREO_HUD_DEPTH` | `2` |
| `LB_WINDOW` | `1600x1100` (1.45, close to the 1.47 aspect of the frustum that covers both eyes) |
| `LB_FOV` | `119.8` |
| `LB_IMMERSIVE_REFINE` | `4` |
| `LB_MAX_FPS` | `45` (the visionOS default) |
| eye views | off |

Riverwood at this config (19 Nov, capped; [perf/LOG.md](perf/LOG.md), 19 Nov): DXVK GPU busy 57–65%,
12.7–14.4 ms per frame; game GPU latency mean 15.2 ms; compositor 3.4–4.2 ms per refresh. Capped busy time on
Apple GPUs is not a cost measure (the GPU clocks down with slack; #42 correction), so the headroom is uncertain
both ways. Measured wins not yet in it, each waiting for a headset pair:
- `LB_LOCK_STAGING=1`: −3 to −10% game work by phase on the M5, but Whiterun pan p95 +5.3% even with
  cool-downs ([#42-c26]).
- #80's render-pass switches (`LB_RP_UPLOAD=1 LB_RP_SHADOW_DS=1`, with dxvk-native #13): about −0.4 ms of GPU
  per frame in Riverwood and −0.6 to −0.8 ms in the boss fight, uncapped on the M5 (item 6).

## Work list, in order

Item numbers are fixed labels from the 2026-11-20 rewrite (the log and pull requests cite them as "plan item N");
the rank is by expected value per effort. S ≤ 1 day, M 2–4 days, L a week or more.

| Rank | Item | What | Size / where | Depends on |
|---|---|---|---|---|
| 1 | 1. Headset frame budget | The timed session never run: stereo 1600×1100, stereo 1280×880, mono 1280×880 (`LB_DEPTH_COPY=1`). **Every arm sets `LB_FOV=119.8`** (eye views are off, so an unset `LB_FOV` stays at 65). Interleaved, two rounds, `boss-fight-avp` and `whiterun-market-avp`, two warm-ups; then one uncapped stereo 1600×1100 run (on the headset that is `LB_MAX_FPS=0` only; see [Validation conventions](#validation-conventions)). #68's session driver can run it. Decides GPU work vs CPU work next, and whether 1600×1100 holds in combat | S / headset | — |
| 2 | 2. Compositor cost | Time #87's switches with `LB_IMMERSIVE_PASS_TIME=1`: `LB_FRAME_REPEAT=1` (halves the compositor's cost per game frame; judge the older pose), `LB_IMMERSIVE_REFINE=2` with `LB_IMMERSIVE_REFINE_EXIT=0.25`, `LB_IMMERSIVE_SEED=1`, and `LB_FOVEATION=1`; `LB_IMMERSIVE_GRID=161x91` only if foliage looks the same (it fails the reference tolerance in #87). Make the clean ones the default | S / headset | #64 |
| 3 | 3. The mask pass | **Done 2026-11-20:** the `refl` pass is the deferred sun-shadow mask (back buffer × `iShadowMaskQuarter`/4), drawn mono with view 0's matrices and before the late eye turn. With the measured eye tangents the right eye's ground shadow error is 17.0 against 2.2 for plain multiview (M1, 0–255 scale). The mask draws geometry; it does not rebuild position from depth | S / Mac | — |
| 4 | 5. Pipelines compiled at draw | A warm state cache still compiled at draw in stereo (19 Nov headset play session: 160 compiles, longest 201 ms; log). Drafts: #69 with dxvk-native #8 creates cached shaders at startup, so warm launches compile 4–15× fewer pipelines at draw (M5, log 21 Nov); #74 with dxvk-native #10 logs misses field by field and normalizes state Vulkan ignores (warm at-draw compiles 225 → 192, M1); dxvk-native #11 (`LB_PIPE_STAGES`) splits the stages: on the M1, 88% of a compile over 100 ms is MSL → `MTLLibrary`, and with the OS's Metal cache warm known pipelines recompile in 1.2 ms median; dxvk-native #12 (`dxvk.drawPipelineCache`) helps only if visionOS drops that cache (M1, cache empty: at-draw total 35.9 → 19.2 s). Pipe logs on 27 Nov (M1, [#42-c38], [#42-c40]): cold, 99.4% of compile time is inside the Vulkan pipeline creation call; warm, 28 of 73 misses differ from a cached entry in a few fields (cull mode alone in 11), and dynamic cull mode, depth write and stencil would make about 16 of the 73 hits (inferred). Next: the warm-launch headset check in #69 and #74, then dynamic state or a seed cache made from the user's own game files (#74) | M / headset, then Mac | — |
| 5 | 9. Comfort session | One headset session for #65 (steering), #66 with dxvk-native #7 and #85 (the flat HUD layer, its scale and placement), and `LB_IMMERSIVE_MOTION=1` against 0 while moving; merge what passes. Check merged #83 (a way back into the space, a visible Resume) and #84 (comfort INI defaults: no kill-cam, idle orbit or radial blur) in the same session | S / headset | #63, #64 |
| 6 | 10. CPU wins onto the headset | (a) `LB_LOCK_STAGING=1` by default after one headset pair, watching Whiterun pan p95 ([#42-c26]); (b) **measured 2026-11-21:** the three LLVM passes pinned registers turn off cost 0.3–0.5% of game work (Riverwood, the M5), so a compiler fix is not worth carrying for speed; (c) `LB_STRICT_TRAP` only if the CPU binds. It exists only on the unmerged branch `perf/strict-trap`, not on main | S / headset, M5 | (a) rides with 1 |
| 7 | 6. GPU attribution | **Measured (the M5).** 21 Nov (log): GPU 6.7 ms per frame in Riverwood, 9.1–9.8 in the boss fight; main scene 2.7–3.4, effects 1.3–2.4, shadows 0.9–2.3 (both grow in combat), depth pass 0.45; `bFloatPointRenderTarget=0` and `d3d9.floatEmulation` save nothing. The pass splits now have fixes ([#42-c44], [#42-c46]): #80 with dxvk-native #13 (`LB_RP_UPLOAD`, `LB_RP_SHADOW_DS`), both together −0.39/−0.38 ms in Riverwood and −0.81/−0.62 ms in the boss fight (uncapped, eye views, 8 rounds; the boss spread is wide), boss-fight passes per frame 82 → 27; dxvk-native #16 (`LB_RP_ENDS`) finds under one same-target split per frame left with both; dxvk-native #15 with #90 (`LB_DEPTH_COPY_FB`) takes the depth class from 0.43 to 0.20 ms. A candidate headset stack, both #80 switches + `Imagespace:bDoDepthOfField=0` + `Display:fShadowDistance=1000`, saves 1.15 ms in Riverwood and 1.5–2.3 ms (about 20%) in the boss fight; its two INI keys change the picture. Capped at 45, neither the #80 switches nor the stack changes Mac frame time. Combat GPU time is 23–31% pixel-bound (39–41% calm, [#42-c45]); the storm costs nothing measurable (#79, [#42-c46]). Open diagnostics: #78 (`LB_PASS_STATS`), #88 (`LB_HIDDEN_DRAWS`), #89 with dxvk-native #14 (`LB_SKIP_SCENE_DEPTH`, not ready: loses first-person depth). Next: the #80 and stack arms on the headset with item 1 | S / headset | — |
| 8 | 4. Per-eye shadow mask, earlier late eyes | **Draft #71 with dxvk-native #9 (2026-11-20):** right-eye error 17.0 → 3.3 on the M1, about +0.45 ms per frame there. The remaining 3.3 was the bloom and distance-blur targets (quarter size, drawn mono): draft #72 draws them per eye, bringing the right eye to run-to-run noise for about +0.3 ms. On the M5 the two cost 0.4–0.6 ms of GPU uncapped and nothing in frame time capped at 45 (log 21 Nov). Drafts #75 (an image-space quad over the final image is post) and #76 (the screen blood placed as the interface) fix two more stereo placements. All await the headset check. Zero-code headset check first: `iShadowMaskQuarter=4` makes the mask back-buffer-sized and already two-view. Makes eye views usable, so each eye uses all its pixels | L / headset | 3 (done); 7 for device captures |
| 9 | 7. Right-eye captures on the device | `LB_SHOT` returns layer 0 for both eyes on the headset. Draft #70 (`LB_TAP_SHOT`) captures each eye from what the compositor is handed; the Mac build passes, the device check is to do | S / headset | — |
| 10 | 11. Budget report in the repo | Draft #68: a per-phase `budget` in `summary.json` (cadence, GPU busy and latency, compositor GPU, at-draw compiles, hitches, thermal state), the session driver `session.py` and `report.py` in `tools/bench`; stays draft until a headset session has run it | S / headset | — |
| 11 | 12. Plan and log | This document and the repaired log. The `refl` label is settled (log, Pass classes); #71 renames the class `mask` | S | — |
| 12 | 13. Soak | 30 min of scripted Whiterun at the chosen config, on AC then battery | S / headset | 1 (ideally 2, 5) |
| 13 | 8. Skip unseen pixels | Only if 4 stalls: near depth in the strip each eye can't see, per view, so early depth rejects it there (about 23% of each eye's width on scene passes). Draft #91 (`LB_SCISSOR_BOUND`) is its kill test: under 8% GPU saving at a 45% cut drops it. The pixel share measured since (23–31% in combat) makes a large saving less likely (inferred) | M / M5, headset | 6; excludes 4 |
| 14 | 14. Alternate-eye compositor | Only if 1 shows the GPU binding in combat: show `LB_STEREO_ALTERNATE=1` frames (rendering is on main, #52) by keeping one frame slot per eye and moving each from its own render time, rays from each eye's origin. Near-zero extra GPU and no mono-pass shadow fault; each eye updates at half the game rate and the eyes are one frame apart, so self-moving objects may shimmer (comfort to judge on the headset). Alternative to 8 | M / headset | 1; after #66 and 2 in `ImmersiveScreen.swift` |

**What runs where:** the headset is the bottleneck. One headset session covers 1, 2, 6's arms and 10a; a second,
shorter one covers 9, 7's device check and 4's check; 13 runs unattended afterwards. On the Macs meanwhile:
5's follow-ups once the headset has answered, 11, and 12 (this document). Serial: 4 and 8 both edit
`d3d9_device.cpp`; #64 → 2 → #66 in `ImmersiveScreen.swift`; #63, #66, 4 and 7 all edit `d3d9_bridge.cpp`, so
land #63 first.

**Not on the list, with reasons**
- More lifter work (#22 items 2–5, #57 stack slots, PGO, order file): each measured as nothing on top of pinned
  registers. Reopen only if item 1 shows game work p95 over about 18 ms at FOV 119.8.
- Havok threading, guest CPU count, QoS hints, half-rate shadows, single-threaded device: closed with numbers
  (#42).
- Foveated game render through a MoltenVK rate map: it broke the same screen-position shadow lookups (#21), and
  the rate map saved no measurable GPU time at 0.43 of the pixels (mono 1600×1100, the M5, 27 Nov,
  [#42-c45]). Closed.
- Temporal upscaling: MetalFX's temporal scaler is unavailable in the visionOS SDK, and neither it nor a
  hand-written resolve gained in moving foliage (#21).

## Constraints

- **The game runs only on the headset** (its CPU, GPU and memory); other machines are build, test and
  measurement hosts. See [AGENTS.md](../AGENTS.md).
- **Hardware:** the original M2 Vision Pro (8-core CPU with 4 performance cores, 10-core GPU, 16 GB),
  visionOS 27.3. The M5 MacBook Air is the A/B machine for GPU and CPU questions; the 8 GB M1 swaps and is used
  for counts, captures and builds.
- **Thermals and power:** after about 2 h on battery, runs were 7% slower at the same thermal state, then reached
  *serious* ([#42-c4]). Drop runs at *serious* from comparisons.
- **Memory:** a one-hour stereo soak on the M5 held the process at 980–1,137 MB resident with no growth
  ([#42-c22]); no headset memory figure is in the log yet.

## Validation conventions

- Label every claim measured / read / inferred; a Mac or the simulator is not the headset.
- Per run: device, OS, build and DXVK commits, thermal state, frame counts, logs.
- **Measure GPU cost uncapped:** `LB_MAX_FPS=0`, and `d3d9.presentInterval = 0` in the file `DXVK_CONFIG_FILE`
  names. On a Mac the bench points `DXVK_CONFIG_FILE` at `macos/runtime.conf`, which has no `presentInterval`,
  so add it there or to a copy. On the headset `ipad/native_bridge.mm` rewrites `Saves/dxvk.conf` at every
  launch and sets `DXVK_CONFIG_FILE` to it, so `presentInterval` can't be set there without a code change (a
  launch-environment hook, or a `dxvk.conf` that isn't overwritten); until then a headset run is uncapped by
  `LB_MAX_FPS=0` alone. Capped busy time misstates differences because Apple GPUs clock down with slack.
- Interleave arms, rotate the order, one warm-up per arm, six or more rounds for the boss fight; pin the weather.
- On a Mac the OS's Metal cache hides compile time: count compiles there, time them on the headset.
- `LB_SHOT`'s per-layer capture is wrong on the device (item 7); verify stereo by capture on a Mac.
- `LB_*` switches with an A/B for each risky setting. Never commit `gen/`, game files, provisioning, team or
  bundle IDs, or captures of the game.

## Superseded

The 2026-11-10 plan had milestones M0 (bring-up), M1 (windowed play), M2 (a mono "cinema" screen in the
immersive space), M3 (alternate-eye stereo through hooks on the game's camera, with M3a as the fallback: mono with
the head turning the game camera) and M4 (PSVR2 Sense). Windowed play and the cinema screen were built (the
bring-up blockers: SDL visionOS overlay, xros MoltenVK and FFmpeg, build tooling, memory entitlements, deployment
target, launcher gating). The cinema screen grew into the depth-reprojecting compositor. Alternate-eye stereo and
the camera hooks were replaced by DXVK multiview (both eyes every frame) and by head steering through mouse look,
which needs no regenerated code; M3a became head steering. (Alternate-eye rendering itself survives as
`LB_STEREO_ALTERNATE`, rendered through DXVK's clip shift rather than camera hooks; its compositor side is work
item 14.) PSVR2 Sense input is deferred: no controllers to test with. The full text is in git history.

### Game camera addresses (from the old M3 notes; used by `game_camera_read` in `runtime/d3d9_bridge.cpp`)

Static reads of the 1.9.32.0.8 executable; only the camera read itself is verified at runtime.
- Main world NiCamera W: `*(*(0x1B2E8BC)+0xB8)`. World rotation +0x54..+0x74 (3×3, row-major), translation
  +0x78..+0x80, world → clip matrix +0xA8..+0xE4, frustum tangents l r t b +0xE8..+0xF4, near/far +0xF8/+0xFC,
  ortho +0x100. 0xAB8640 builds the matrix from the transform and the frustum.
- PlayerCamera singleton 0x12E7288: worldFOV +0xAC, firstPersonFOV +0xB0. FOV → frustum at 0xB17960.
- Frame order in 0x69F950: PlayerCamera update (0x73F240) → world render 0x69BDF0 (frustum via 0x6975D0). The
  first-person camera (0x697FC0 / 0x698B30) re-runs 0xB17960 on W with firstPersonFOV and restores W's frustum.

<!-- Issue comments cited above: #42-cN is the Nth comment on issue #42, as in perf/LOG.md -->
[#42-c4]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-6912103883
[#42-c22]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-6941075032
[#42-c26]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-6942112816
[#42-c37]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-7027983120
[#42-c38]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-7219905158
[#42-c40]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-7220194302
[#42-c44]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-7225563008
[#42-c45]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-7228572508
[#42-c46]: https://github.com/twcclegg/skyrim-apple/issues/42#issuecomment-7231174154
