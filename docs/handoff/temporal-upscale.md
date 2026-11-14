# MetalFX temporal upscaling for the immersive picture (branch `immersive/metalfx-temporal`)

Item 4 of issue #21. Builds on `helper/metalfx-upscale` (PR #13) and is off unless asked for.

The idea: render the game small, shift the camera by a different fraction of a pixel each frame, and let
MetalFX's temporal scaler build a picture of the full size out of the last several frames. The game half
(the shift, and the frame carrying what it was shifted by) was built and measured on Linux. The compositor
half (the motion vectors and the scaler) was written on Linux **without a Swift or Metal compiler for
visionOS and has never been compiled or run**. `tools/metalfx_temporal_probe.swift` is there so a Mac can
settle what is open before anyone puts a headset on.

## Status, 2026-11-13: the visionOS SDK has no temporal scaler

The M1 type-checked f3dde77 (issue #21): the simulator SDK passes, the device SDK does not. Xcode's visionOS
SDK 27.2 marks `MTLFXTemporalScaler`, `MTLFXTemporalDenoisedScaler`, `MTLFXFrameInterpolator` and their
`MTL4FX` variants `API_UNAVAILABLE(visionos)`. Only the spatial scaler (PR #13) is available.

Whether the headset's system has the class anyway is **not known**. A third-party port reports the temporal
scaler working there when looked up by name, and branch `immersive/metalfx-probe` (c3c3206, logging only)
prints what the device has when immersive play starts. Until a headset log says so, treat this branch as an
experiment that may end at "the system does not have it".

What changed here because of it:

- The compositor no longer mentions MetalFX's temporal types. `ipad/metalfx_temporal.mm` looks the
  descriptor class up by name, with the few properties it uses declared again so the calls are typed, and
  hands Swift a plain C interface (`skyrim_temporal_scaler_make`, `_encode`, `_free`). If the class is
  absent, does not support the device, makes no scaler or throws, the reason is logged once
  (`immersive: temporal: ...; using the spatial upscale`) and the spatial scaler is used. So one headset
  session answers both "is it there" and "what does it look like".
- That file was checked on Linux with clang against stand-in Metal headers (syntax, ARC, the casts) and
  `ImmersiveScreen.swift` with `swiftc -parse`. **Neither has met the real SDK.** "For the Mac" below has a
  self-test that runs the by-name path on macOS.
- Calling an API the SDK marks unavailable is fine for a build installed from Xcode. It would not pass App
  Store review.
- A hand-written temporal resolve was sized as the alternative and is not worth porting; see "A hand-written
  resolve instead".

## What was built

Game side (`runtime/`):

- `jitter.c`, `jitter.h`: the sequence of shifts (Halton 2,3, each within half a pixel) and the bookkeeping
  that puts one into a frustum and takes it out again. Plain C; `tests/temporal/run.py` checks it anywhere.
- `d3d9_bridge.cpp`: the world camera's update (NiCamera vtable slot 45, `0xAB8F30`) goes through
  `camera_update`, which shifts the camera's frustum before the game rebuilds its matrices from it. One
  update a frame, before anything is drawn, so the world, the first-person pass and the scene depth all move
  by the shift and the interface does not. Nothing is patched until jitter is asked for; no shift while a
  menu is up.
- `skyrim_camera_jitter(phases)` turns it on and off; `skyrim_frame_jitter(out)` and the camera in
  `skyrim_frame_camera` describe the frame just presented, the camera with the shift taken out.
- `skyrim_texture_bias(levels)`: a mip bias on every sampler the game sets (samplers 0 to 15). A game
  rendered at half size picks textures one level too blurry for the upscaled picture; the temporal scaler
  wants `log2(render size / shown size)`, -1 at 2x.
- `LB_SHOT` takes ranges (`A-B`) and writes the scene depth next to each shot; `LB_CAMLOG` lines end with the
  frame's jitter. Those are what the tools below read.

Bridge (`apple/Sources/Native/SkyrimNativeBridge.h`, `ipad/immersive_bridge.mm`): `skyrim_immersive_frame`
carries `jitter[2]` and `has_jitter` with the camera it already had.

Compositor (`apple/Sources/App/ImmersiveScreen.swift`), with `LB_IMMERSIVE_TEMPORAL=1` and
`LB_IMMERSIVE_UPSCALE=<factor>`:

- When the first frame arrives it makes the scaler and its textures, then asks the game for jitter
  (`8 x factor squared` positions) and the texture bias.
- For each new game frame, one full-screen pass (`temporal_fragment`) reads the game's colour and depth and
  writes linear colour (RGBA16Float), depth (Depth32Float) and a motion vector per pixel (RG16Float): where
  that pixel's surface was in the frame before, worked out from depth and the two cameras. Then the scaler
  runs, and the screen mesh is textured from its output. A repeated frame reuses the last output.
- History starts afresh when there was no camera the frame before (a menu, a loading screen), or the camera
  jumped (over 1000 units, or turned more than 60 degrees).
- Frames with no camera (menus) and anything that fails go through the spatial scaler of PR #13 instead. If
  the scaler, its textures or the pass can't be made, one `immersive: temporal: ...; using the spatial upscale`
  line says why, and jitter and bias are turned off again.
- In the simulator (no MetalFX there) the lookup by name finds nothing: it logs once and the image is drawn
  as it is.

Tools:

- `tools/taa_sim.py`: the measurements below, from frames the desktop build dumps.
- `tools/taa_resolve.py`: a hand-written temporal upscale in numpy, scored against full-size frames.
- `tools/metalfx_temporal_probe.swift`: the real scaler on a Mac (macOS has it); see "For the Mac".

## Settings

| Setting | Where | What |
| --- | --- | --- |
| `LB_IMMERSIVE_TEMPORAL=1` | compositor | temporal instead of spatial; needs `LB_IMMERSIVE_UPSCALE` |
| `LB_IMMERSIVE_UPSCALE=<factor>` | compositor | as in PR #13; the output is this times the game's size |
| `LB_IMMERSIVE_TEMPORAL_SIGNS=jx,jy,mx,my` | compositor | signs for MetalFX's jitter offset and motion vectors; default `1,1,1,1` (the probe on an M5 Mac; was `-1,-1,1,1`, a guess) |
| `LB_IMMERSIVE_TEMPORAL_AUTOEXPOSURE=1` | compositor | MetalFX's auto exposure (off by default: the picture is already tone-mapped) |
| `LB_JITTER=<phases>[x<scale>][@<frame>]` | game, for tests | jitter without the compositor; the scale makes the shifts whole pixels |
| `LB_JITTER=<x>,<y>[@<frame>]` | game, for tests | the same shift every frame |
| `LB_JITTER_LOG=1` | game | a line per frame: the shift, camera updates, shifts applied |
| `LB_TEXTURE_BIAS=<levels>` | game, for tests | the mip bias without the compositor |
| `LB_SHOT=A-B`, `LB_CAMLOG=1` | game | dump frames A to B with depth, and a camera line per frame |

## What was measured (Linux, Riverwood, 1280x720 and 640x360)

Dumps are game imagery and are not in the repository.

- **The shift is exact.** With whole-pixel shifts (`LB_JITTER=8x16`) every frame's picture and scene depth
  moved by exactly the logged amount; the compass and crosshair did not move. `taa_sim.py shifts`.
- **One camera update a frame**, and the frustum the game keeps never collects a second shift, including
  around the first-person pass, a change of field of view and a menu (unit test, 349 checks).
- **The frames hold the detail.** 32 jittered 640x360 frames, added up on a 1280x720 grid at the places
  their jitter says, against the game rendered at 1280x720 (texture bias -1): one frame upscaled bilinearly
  22.87 dB with 56% of the reference's fine detail; the sum 23.54 dB with 73%; the same sum with the jitter's
  sign flipped 21.91 dB. Two runs of the reference agree to 31.71 dB (leaves and water differ), so the PSNR
  ceiling is low; the detail figure is the one to read. `taa_sim.py accumulate`. This is a plain sum over a
  still scene, not MetalFX.
- **Depth and camera find the previous frame.** While walking, the mean difference between a frame and the
  one before fell from 14.64 to 5.18 (of 255) when the one before was fetched where the arithmetic says each
  pixel was; with jitter on, 14.70 to 5.18. `taa_sim.py reproject`. The compositor's motion pass is the same
  arithmetic.
- **Menus.** A pausing menu (the console) stops the camera's update altogether, so the scene behind it keeps
  the last shift and stands still. For menus that don't pause, the bridge applies no shift.
- The desktop build still builds and runs with all of this off (the default).

## A hand-written resolve instead (Linux, measured)

If the headset has no temporal scaler, the alternative is the compositor's own shader. `tools/taa_resolve.py`
tries the usual one on real frames before anyone writes it in Metal. 200 frames at 1280x720 are the truth;
each gives a 640x360 frame by keeping one pixel of every 2x2 block, a different one each frame, which is what
a half-size render with a quarter-pixel jitter and texture bias -1 samples. The resolve sees those, their
depth and the cameras. Riverwood, low preset, weather pinned: 25 frames standing, 71 walking forward through
tall grass, 45 strafing into a pine at close range.

World PSNR in dB (compass and crosshair left out) / compass PSNR / fine detail as a share of the truth's:

| Variant | Standing | Walking | Strafing |
| --- | --- | --- | --- |
| one frame upscaled bilinearly, no history | 24.95 / 20.09 / 70% | 24.61 / 21.51 / 72% | 26.28 / 22.52 / 77% |
| history from the same pixel (no motion vectors) | 30.98 / 24.12 / 76% | 19.98 / 18.78 / 79% | 20.20 / 18.34 / 88% |
| motion vectors, bicubic history, neighbourhood clamp | 30.98 / 24.12 / 76% | 22.34 / 18.47 / 70% | 21.87 / 18.23 / 78% |
| ... without the clamp | 33.18 / 26.50 / 83% | 20.22 / 17.58 / 89% | 17.86 / 15.02 / 119% |
| ... with depth rejection | 27.41 / 21.87 / 76% | 24.17 / 19.94 / 75% | 25.65 / 20.64 / 81% |
| ... depth rejection, faster blend when moving | 27.41 / 21.87 / 76% | 24.50 / 20.32 / 71% | 26.11 / 21.96 / 77% |
| ... tolerant depth rejection | 30.75 / 23.91 / 76% | 22.79 / 18.73 / 72% | 24.18 / 19.71 / 83% |
| ... tolerant depth rejection, faster blend when moving | 30.75 / 23.91 / 76% | 23.54 / 19.11 / 68% | 25.32 / 21.21 / 77% |

- **Standing still it works:** about 6 dB over a single upscaled frame, and more detail.
- **Moving through foliage it does not:** every variant is at or below a single frame. The reprojection is
  right on solid surfaces (checked by warping full-size frames); the loss is in thin alpha-tested grass and
  needles that uncover each other and sway. Rejecting history there gets back to par, no further, and the
  strict rejection that does it costs 3.5 dB of the standing gain because it also fires at foliage edges
  when nothing moves.
- **The compass is worse in every moving case**, as the known limits below say it would be.
- This route is a worst case on purpose; an open road was not measured. The prototype had only a
  little tuning and is not the best such a shader can do. It is enough to say a hand-written resolve is not a quick win.

Run it (dumps are game imagery; keep them out of the repository):

    tools/taa_resolve.py <dump dir> --frames 1560-1759 --span standing=1575-1599 --span walking=1620-1690 \
        --span strafing=1715-1759

## What was not tested

Everything on the Apple side:

- `ImmersiveScreen.swift` and the probe passed only `swiftc -parse` on Linux (syntax). f3dde77 was
  type-checked by the M1 (simulator SDK clean; device SDK failed on the temporal types). The by-name version
  after it has not been type-checked, and the Metal shader source has not been compiled.
- Whether the headset's system has MetalFX's temporal scaler (the SDK says no), and whether it takes
  RGBA16Float colour, Depth32Float depth and RG16Float motion there. On a Mac the M1's probe made one with
  those formats.
- **The sign conventions.** Which way MetalFX wants `jitterOffset` and the motion vectors is a reading of
  the documentation. A wrong jitter sign gives a picture softer than the spatial scaler; a wrong motion sign
  smears everything when the camera moves. The probe measures both.
- GPU cost, and what it looks like.

Known limits, none of them looked at on a headset:

- **The interface is in the same picture.** The compass, crosshair and subtitles are not jittered but get the
  motion vectors of the world behind them, so they may smear or wobble while the camera moves. The fix is to
  give the interface its own layer after the upscale (`runtime/hud_screen.cpp` already separates the menu
  render); not done here.
- **Only the camera's motion is known.** People, animals, foliage, water and particles move without the
  scaler being told, and may ghost. MetalFX's reactive mask is the usual remedy and is not fed.
- **First-person hands and weapon** are drawn with their own field of view; they get the shift (unit test)
  but their motion vectors are computed as if they were world geometry at their depth. Not looked at.
- `GetSamplerState` hands the game back the biased value.
- The colour goes through one extra copy (to RGBA16Float). If the probe's format table says the scaler takes
  the tap's own format, that copy can go.

## For the Mac (Skyrim - M1)

1. Type-check `apple/Sources` for the visionOS device SDK and the simulator SDK, as for PR #13
   (`swiftc -typecheck`, `-D SKYRIM_NATIVE_RUNTIME`). Fix what the blind writing got wrong. The device SDK
   should now pass: nothing names the temporal types any more.
2. Run the by-name path on macOS. This is the code the headset will run, less the compositor around it:

       xcrun clang++ -std=c++17 -fobjc-arc -DSKYRIM_TEMPORAL_SCALER_SELFTEST ipad/metalfx_temporal.mm \
           -framework Metal -framework Foundation -framework MetalFX -o /tmp/temporal_scaler && /tmp/temporal_scaler

   It should print the scaler's class and `two frames encoded and completed`, exit 0. Also compile the file
   with the visionOS engine's own flags, as `immersive_bridge.mm` is (`ipad/CMakeLists.txt` now lists it).
3. Build and run the probe (macOS has MetalFX, so this needs no headset). It uses the typed API, which is
   fine on macOS:

       xcrun swiftc -O tools/metalfx_temporal_probe.swift -o /tmp/temporal_probe
       /tmp/temporal_probe apple/Sources/App/ImmersiveScreen.swift

   It compiles the compositor's shaders, then renders a made-up scene small with the game's jitter, runs it
   through the compositor's own motion pass and the scaler under each choice of signs, and compares with the
   same view rendered at the full size. It prints the best jitter signs (camera still), the best motion signs
   (camera moving), baselines without history and without motion vectors, GPU ms, and which formats the
   scaler accepts. If the best signs are not `+1 +1` / `+1 +1`, change the defaults in
   `readTemporalConventions`. "NOT CLEAR" means the test did not separate the choices; say so rather than
   picking one.
4. With game data on the Mac, the real picture: dump frames from the desktop build at half size, standing
   still and then walking,

       LB_WINDOW=640x360 LB_JITTER=32 LB_TEXTURE_BIAS=-1 LB_CAMLOG=1 LB_SHOT=600-680 ...

   (stderr to `log.txt` in the dump directory), then

       /tmp/temporal_probe apple/Sources/App/ImmersiveScreen.swift frames <dump dir> 600 680 <out dir> 2 <signs>

   and compare `temporal-00680.bmp` with `spatial-00680.bmp` and with a 1280x720 render of the same place.
   Look at the compass while walking, at grass and water, and at the hands with a weapon drawn. Walking
   through tall grass is where the hand-written resolve above gained nothing, so that is the case that says
   whether MetalFX is worth a headset session.
5. Report the outputs and what step 4 looked like on issue #21.

## Headset test (Thomas)

Only after the Mac steps. One change at a time in `launch-env.json`:

0. With `immersive/metalfx-probe` merged into the build, the log's `metalfx probe:` lines say whether the
   headset has the temporal scaler at all. If `MTLFXTemporalScalerDescriptor` is `absent`, stop here: step 2
   will fall back to the spatial scaler and there is nothing to look at.
1. Baseline: `LB_WINDOW=1280x720`, `LB_IMMERSIVE_UPSCALE=2` (PR #13's spatial scaler). Note `compositor gpu`
   and the game's fps lines.
2. Add `LB_IMMERSIVE_TEMPORAL=1`. The log should say `immersive: image: upscale x2.0 temporal` and
   `immersive: temporal: MetalFX temporal 1280x720 -> 2560x1440, 32 jitter positions, texture bias -1.0, ...`,
   then every few seconds `temporal: N frames upscaled, M fresh starts`. A line ending
   `using the spatial upscale` means it fell back, and says why (for one,
   `this system has no MetalFX temporal scaler`).
3. Standing still: distant detail, thin edges (branches, railings), texture detail on the ground. It should
   be clearly sharper than step 1 and should not shimmer.
4. Walking and turning: smearing behind edges, the compass and crosshair, people walking past, grass, water.
5. If it holds, the point of it: lower the render size (`LB_WINDOW=960x540`, same upscale target with
   `LB_IMMERSIVE_UPSCALE=2.67`) and compare fps and sharpness with step 1.

## Merging

- On top of PR #13; that goes first.
- Not before a headset log shows the temporal scaler exists there. If it does not, the game side (jitter,
  frame jitter, texture bias, the dump tools) is still what any other temporal method would need; the
  compositor's temporal path and `ipad/metalfx_temporal.mm` would come out.
- `immersive/metalfx-probe` adds a block to `ipad/immersive_bridge.mm`; this branch does not touch that
  place.
- `immersive/locomotion-extrapolation` and `immersive/foveated-render` change `ImmersiveScreen.swift` too
  and will conflict in `draw()` and the uniforms. With foveation the motion pass needs the foveated frustum
  mapping (it assumes pixels are evenly spaced in tangent); until that is written, don't turn both on.
