# Brief: stereo and head tracking by depth reprojection

For the "Skyrim - M1" session, which owns the Vision Pro work. Written 2026-11-10 by the performance session
(the one that took `FABLE-RECOMPILER-BRIEF.md`). Thomas asked for this to be handed to you. Everything is
observed on the M1 unless marked *untested*; nothing here has run on the headset.

## Goal

Thomas's pass/fail for the project: the game is comfortably playable on the M2 Vision Pro in 3D immersive with
head tracking. This brief covers the stereo and head-tracking part. You decide the order and the details.

## The approach, and why

The game keeps rendering one mono frame. The compositor draws both eyes from that frame's colour **and depth**,
every displayed frame (90 Hz), for the newest head pose. Compared with the plan's alternate-eye hook (M3) it
doesn't halve each eye's frame rate and doesn't put a second frame on the game's main thread, which is the
limit (79% of its on-CPU time is lifted code; DXVK's front end is about 3%). It also gives head translation,
not only rotation. `docs/perf/IMMERSIVE_OPTIONS.md` compares it with the two alternatives.

## What is already known

- **Skyrim renders readable depth itself.** One Riverwood frame is 2,013 draws in about 27 passes; 638 of them
  render the scene's depth a second time into a back-buffer-sized **R32F colour target** (1280×720,
  `D3DFMT_R32F`), after the main scene pass and before effects, with a short first-person pass added later in
  the frame. No INTZ/RESZ work is needed. `runtime/hud_screen.cpp` keeps the last one bound:
  `IDirect3DSurface9 *d3d_scene_depth_target(void)` (not referenced; valid within the frame).
- **Its values** are D3D depth, 0 near to 1 far (0.92–1.0 in the test frame, sky = 1). With the near plane at
  15 units and a far plane much larger, view distance ≈ `15 / (1 - d)` units; about 70 units per metre.
  The near plane and FOV used here are assumptions (INI defaults); the NiCamera in the plan holds the real
  near/far and frustum, which is what the shader should use.
- **It works on real data.** `LB_SHOT=<frame>` on the Mac build now also writes `depth-<frame>.f32`.
  `tools/stereo_proto.py shot.bmp depth.f32 out` synthesizes left and right views the way a fragment shader
  would (per output pixel, a few fixed-point steps along the row). At 64 mm eye separation: largest shift 8.9 px
  per eye, 1.9% of pixels unresolved, all thin slivers at foliage and rock edges. A sample pair is in
  `bench-results/mac-saves/` (private, not in git).
- **The HUD moves with the scene behind it** (the compass visibly shifts). It needs its own panel, which the
  plan already wants through the `hud_screen.cpp` redirect.
- `LB_D3DTRACE=<from>:<to>` now prints size, format and usage for `SetRenderTarget` and
  `SetDepthStencilSurface`, and the flags of `Clear`, so a frame's pass structure can be read from the log.

## What needs building (all *untested*)

1. **Depth through the present tap.** The tap copies the back buffer into one of three slot images inside
   DXVK's command list. Give each slot a second image and copy the R32F target into it in the same place. The
   bridge knows the D3D9 surface; the tap needs its `DxvkImage`
   (`D3D9Surface::GetCommonTexture()->GetImage()`). Add the image and its format to
   `dxvk_present_tap_frame`, and export it in `ipad/immersive_bridge.mm` like the colour image.
2. **Camera values with each frame:** near, far, the frustum, and the head pose the frame was rendered for.
   The plan's NiCamera notes say where they are; the frame ring in the plan already carries the pose.
3. **The compositor pass** (`apple/Sources/App/ImmersiveScreen.swift`): instead of one world-locked quad, draw
   the frame per eye with depth. Two ways: a fragment-shader search as in `stereo_proto.py`, or a
   depth-displaced grid mesh drawn with each eye's real view and projection, which also covers head movement
   between game frames. Write real depth to the drawable so visionOS's own reprojection keeps working.
4. **Head rotation into the game camera** (the plan's M3a hook), with a game FOV wider than the headset shows,
   so there is picture to reproject into.
5. **HUD and menus on their own panel**, without the scene's depth.

A sensible first target: steps 1 and 3 with the existing world-locked screen, i.e. the cinema screen gains
depth. That proves the depth path on the device before any camera hook.

## Things to watch

- Particles, water and other blended effects have the depth of what is behind them.
- The first-person pass writes its depth into the same target after a clear; check hands and weapons.
- The depth target is rendered at the back-buffer size; if you add a render-scale knob it scales with it.
- Don't make the unresolved slivers worse than they need to be: put the mono camera between the eyes, and
  consider a smaller separation than 64 mm for near objects.
- The mono immersive screen (`Play Immersive`) hasn't run on the device yet. Verify that first.

## State of the working tree (please read before editing)

- The checkout is now on local branch **`perf/lifted-arm64`**, created from `visionos/port-v2` with your
  uncommitted changes carried along untouched. It has five commits of mine (lifter x87 cache, constant guest
  base, a native function, tests, tracing, the prototype, docs). Nothing is pushed.
- **I edited files that also hold your uncommitted work**, and left those edits uncommitted so your changes and
  mine aren't mixed in a commit:
  - `runtime/platform_sdl.c`: frame pacing and the benchmark hooks also on the Mac build; a per-frame work time
    as a fourth `ft` column.
  - `runtime/d3d9_bridge.cpp`, `.h`: `platform_present_slept`; the COM object table is lock-free for readers
    (`com_host`); `shot_depth`.
  - `ipad/CMakeLists.txt`: `LB_CONST_MEM` option (on by default) for the generated code.
  - `tools/bench/bench.py` and a `riverwood-walk-mac` scenario: a `--mac` target; summaries include work time.
- **The lifter changed**, so the build's manifest check requires regenerated code. It is at
  `~/SkyrimAVP/gen-x87` on the M1 (same 96,653 functions). `~/SkyrimAVP/gen` is the original. Game files:
  `~/SkyrimAVP/game/Skyrim`.
- `ipad/build/visionos/out/libSkyrimNative.a` is now my build from `gen-x87` (sha256 e4849f52…); your
  previous engine and app receipt are in `~/SkyrimAVP/engines/`.
- Measurements and method: `docs/perf/LOG.md`. The Mac build runs the game on the M1
  (`tools/bench/bench.py run riverwood-walk-mac --mac macos/build/perf-x87 --game ~/SkyrimAVP/game/Skyrim`),
  which is a quick way to try bridge and DXVK changes without the headset.

## Rules (unchanged)

Private data stays on Thomas's machines; commit only locally; ask Thomas before installing on the headset, one
change per headset test; the recompiler has no license, so only DXVK, MoltenVK and SDL patches and our own code
can go to other projects.
