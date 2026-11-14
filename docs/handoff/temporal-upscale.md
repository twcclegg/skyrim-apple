# MetalFX temporal upscaling for the immersive picture (branch `immersive/metalfx-temporal`)

Item 4 of issue #21. Builds on `helper/metalfx-upscale` (PR #13) and is off unless asked for.

The idea: render the game small, shift the camera by a different fraction of a pixel each frame, and let
MetalFX's temporal scaler build a picture of the full size out of the last several frames. The game half
(the shift, and the frame carrying what it was shifted by) was built and measured on Linux. The compositor
half (the motion vectors and the scaler) was written on Linux **without a Swift or Metal compiler for
visionOS and has never been compiled or run**. `tools/metalfx_temporal_probe.swift` is there so a Mac can
settle what is open before anyone puts a headset on.

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
- In the simulator (no MetalFX in that SDK) it logs once and draws the image as it is.

Tools:

- `tools/taa_sim.py`: the measurements below, from frames the desktop build dumps.
- `tools/metalfx_temporal_probe.swift`: the real scaler on a Mac; see "For the Mac".

## Settings

| Setting | Where | What |
| --- | --- | --- |
| `LB_IMMERSIVE_TEMPORAL=1` | compositor | temporal instead of spatial; needs `LB_IMMERSIVE_UPSCALE` |
| `LB_IMMERSIVE_UPSCALE=<factor>` | compositor | as in PR #13; the output is this times the game's size |
| `LB_IMMERSIVE_TEMPORAL_SIGNS=jx,jy,mx,my` | compositor | signs for MetalFX's jitter offset and motion vectors; default `-1,-1,1,1`, a guess |
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

## What was not tested

Everything on the Apple side:

- `ImmersiveScreen.swift` and the probe passed only `swiftc -parse` on Linux (syntax). They have not been
  type-checked against Metal, MetalFX or CompositorServices, and the Metal shader source has not been
  compiled.
- Whether MetalFX's temporal scaler is in the visionOS device SDK at all, and whether it takes
  RGBA16Float colour, Depth32Float depth and RG16Float motion.
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
   (`swiftc -typecheck`, `-D SKYRIM_NATIVE_RUNTIME`). Fix what the blind writing got wrong.
2. Build and run the probe (macOS has MetalFX, so this needs no headset):

       xcrun swiftc -O tools/metalfx_temporal_probe.swift -o /tmp/temporal_probe
       /tmp/temporal_probe apple/Sources/App/ImmersiveScreen.swift

   It compiles the compositor's shaders, then renders a made-up scene small with the game's jitter, runs it
   through the compositor's own motion pass and the scaler under each choice of signs, and compares with the
   same view rendered at the full size. It prints the best jitter signs (camera still), the best motion signs
   (camera moving), baselines without history and without motion vectors, GPU ms, and which formats the
   scaler accepts. If the best signs are not `-1 -1` / `+1 +1`, change the defaults in
   `readTemporalConventions`. "NOT CLEAR" means the test did not separate the choices; say so rather than
   picking one.
3. With game data on the Mac, the real picture: dump frames from the desktop build at half size, standing
   still and then walking,

       LB_WINDOW=640x360 LB_JITTER=32 LB_TEXTURE_BIAS=-1 LB_CAMLOG=1 LB_SHOT=600-680 ...

   (stderr to `log.txt` in the dump directory), then

       /tmp/temporal_probe apple/Sources/App/ImmersiveScreen.swift frames <dump dir> 600 680 <out dir> 2 <signs>

   and compare `temporal-00680.bmp` with `spatial-00680.bmp` and with a 1280x720 render of the same place.
   Look at the compass while walking, at grass and water, and at the hands with a weapon drawn.
4. Report the probe's output and what step 3 looked like on issue #21.

## Headset test (Thomas)

Only after the Mac steps. One change at a time in `launch-env.json`:

1. Baseline: `LB_WINDOW=1280x720`, `LB_IMMERSIVE_UPSCALE=2` (PR #13's spatial scaler). Note `compositor gpu`
   and the game's fps lines.
2. Add `LB_IMMERSIVE_TEMPORAL=1`. The log should say `immersive: image: upscale x2.0 temporal` and
   `immersive: temporal: MetalFX temporal 1280x720 -> 2560x1440, 32 jitter positions, texture bias -1.0, ...`,
   then every few seconds `temporal: N frames upscaled, M fresh starts`. A line ending
   `using the spatial upscale` means it fell back.
3. Standing still: distant detail, thin edges (branches, railings), texture detail on the ground. It should
   be clearly sharper than step 1 and should not shimmer.
4. Walking and turning: smearing behind edges, the compass and crosshair, people walking past, grass, water.
5. If it holds, the point of it: lower the render size (`LB_WINDOW=960x540`, same upscale target with
   `LB_IMMERSIVE_UPSCALE=2.67`) and compare fps and sharpness with step 1.

## Merging

- On top of PR #13; that goes first.
- `immersive/locomotion-extrapolation` and `immersive/foveated-render` change `ImmersiveScreen.swift` too
  and will conflict in `draw()` and the uniforms. With foveation the motion pass needs the foveated frustum
  mapping (it assumes pixels are evenly spaced in tangent); until that is written, don't turn both on.
