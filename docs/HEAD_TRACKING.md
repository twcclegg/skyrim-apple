# Head tracking on Vision Pro (M3a: "full immersive")

Goal (the project's pass/fail): the game is comfortably playable on the M2 Vision Pro in 3D immersive with
head tracking. The depth-reprojected screen (`apple/Sources/App/ImmersiveScreen.swift`) gives 3D; this adds
head tracking without hooking the game's render code.

## Approach

1. **Every frame says which camera rendered it.** At Present the runtime reads the game's main world camera
   (NiCamera W: rotation, position, frustum; see `game_camera_read` in `runtime/d3d9_bridge.cpp`) and passes it
   through the present tap with the frame's colour and depth.
2. **The head steers the game camera.** The compositor publishes the head's yaw and pitch each display frame.
   Each game frame, the runtime compares them with the camera W it last rendered and turns the view toward them
   with synthetic mouse look (closed loop on the game's real camera, so there's no drift). The right stick
   still turns the body: it adds a yaw offset between the head and the game's view.
3. **The compositor draws each frame where it was rendered.** The frame is shown at its camera's orientation and
   true angular size (its frustum), with depth at true scale (game units per metre) instead of compressed onto a
   screen. Between game frames and while the camera catches up with the head, the picture stays fixed in the
   world, and depth gives the eyes their parallax and covers small head movements.
4. **A wider game FOV than the headset shows**, so turning the head finds picture already rendered.
5. **HUD and menus**: menus already go flat; with head tracking they become a head-following panel.
6. **The frame follows the player between game frames** (`LB_IMMERSIVE_MOTION=1`,
   `docs/handoff/camera-motion.md`). Step 3 covers the head. Walking, strafing and turning with the stick moved
   the world only once per game frame; the runtime now keeps the camera's recent positions and the steering
   offset, and the compositor shifts and turns the frame it is showing to where they are for each display frame.

## Why not hook the camera

The plan's hook route (post-hook 0x6975D0, restore after 0x69BDF0, first-person camera at 0x697FC0) gives
zero-latency head rotation but needs runtime verification of each address, a lifter change and regenerated
code, and must keep game logic, aim and culling consistent. Steering through mouse look keeps the game's own
state consistent (aim follows the head, as in most head-look ports) and needs no regeneration. Its latency (the
game turns a frame or two after the head) is hidden by step 3. The hook stays the upgrade path.

## Steps and checks

- [x] Mac: `LB_CAMLOG=N` logs W at Present (`tools/bench/scenarios/camera-probe-mac.json`). Confirmed
      2026-11-10: the rotation follows a scripted turn (L) and the position follows running (W). Layout, as
      rot[9] row-major: forward = column 0 (r0 r3 r6), up = column 1, right = column 2; at the main menu it is
      the identity-like (forward +Y, up +Z, right +X). Frustum at 16:9: l/r ±0.849, t/b ±0.478, i.e. 80.7° x
      51.1° with PlayerCamera worldFOV 65 (a 4:3 reference); near 15, far 353,840 units outdoors.
- [x] Tap carries the camera: `skyrim_frame_camera()` is filled at Present and read by the tap callback, which
      runs inside Present; `skyrim_immersive_frame.camera`. (Builds; not yet run on the headset.)
- [x] Head steering (`head_steer` in `runtime/platform_sdl.c`), Mac with `LB_HEAD_TEST=sweep`
      (`tools/bench/scenarios/head-steer-mac.json`): with a pad connected the game is in gamepad mode and ignores
      mouse look, so it deflects the right stick (proportional, past a 0.25 dead zone). At gain 7, following a
      ±34° yaw / ±14° pitch sweep: yaw error mean 2.3°, max 3.6°; pitch mean 0.6°, max 1.7°. Off in menus;
      the real right stick takes over and re-anchors. The mouse path (no pad) is written but unverified: the
      bench always has its virtual pad.
- [x] Compositor head-tracking mode (`LB_IMMERSIVE_HEAD=1`): publishes the head's yaw/pitch to the runtime each
      display frame (relative to its starting heading); draws each frame on a plane facing back along its game
      camera's direction (heading minus the steering offset), sized to the frame's own frustum, with each pixel
      at its true view distance (`LB_IMMERSIVE_HEAD_SCALE`, 1 = true scale, capped at 300 m). Builds; untested.
- [x] Wider game FOV: `LB_FOV=<deg>` writes PlayerCamera worldFOV/firstPersonFOV at Present (the INI keys had
      no effect). Mac: `LB_FOV=100` gives frustum ±1.589 x ±0.894, i.e. 115° x 83°.

Headset test settings (Documents/SkyrimNative/launch-env.json): `LB_IMMERSIVE_HEAD=1`, `LB_FOV=100`, and
`LB_ANY_PAD=1` for the Xbox controller. Then Play Immersive.
- [x] Player movement between game frames (`runtime/motion_path.c`, `tests/motion/run.py`,
      `tools/motion_sim.py`). Linux, 2026-11-13: at 45 game fps on a 90 Hz display the error in each display
      frame's movement falls from 5.6 cm to 0.24 cm on a made-up run. The compositor side is written but has
      never been compiled.
- [ ] Mac and simulator: build it, check the clock, hands and weapon (`docs/handoff/camera-motion.md`).
- [ ] Headset: comfort, latency, frame rate.
