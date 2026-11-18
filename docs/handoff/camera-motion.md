# Following the player's movement between game frames

Issue #21, item 2. Written 2026-11-13 on the Linux machine, for the "Skyrim - M1" session to build and try.

**State:** the runtime part compiles and is tested on Linux. The Swift and Metal part has never been compiled.
Nothing here has run on a Mac, in the simulator or on the headset. It is off unless `LB_IMMERSIVE_MOTION=1`, and
with it off the compositor draws exactly what it drew before.

## The problem

In head-tracking mode (`docs/HEAD_TRACKING.md`) the compositor redraws at 90 Hz and places each game frame where
its camera pointed, so turning or moving the head is smooth at any game frame rate. The player's own movement is
not: `framePose` used the frame camera's direction and ignored its position, so walking, strafing and turning
with the right stick all moved the world once per game frame. At 45 game fps every other display frame showed
no movement at all. That is the main thing standing between a 45 fps game and a picture that looks like 90.

## What it does

1. **The runtime remembers where the camera was and when** (`runtime/motion_path.c`, portable C). At each
   Present it records the world camera's position; after head steering it records the steering offset (the
   angle the right stick has turned the view away from the head). Both go into a short path of the last 8
   samples with their times.
2. **The compositor asks where the camera is for the display frame it is drawing**
   (`skyrim_immersive_motion`), and gets back how far the camera has moved since the frame it is showing, and
   the offset at that moment.
3. **It moves the frame by that much the other way** (`framePoses` in `ImmersiveScreen.swift`): a turn through
   the offset, and a shift in metres. Depth is already at true scale in this mode, so near things pass by
   faster than far ones, as they should.
4. **The player's own hands and weapon stay put.** They travel with the camera, so shifting them with the world
   would make them shake. Anything nearer the eye than `LB_IMMERSIVE_MOTION_NEAR_M` (1 m by default) is drawn
   where the frame had it; the change is blended over the next half metre.

### Between frames, not ahead of them

By default the path is read **one game frame back from the moment of drawing**. The answer then lies between
two positions the game really produced. Nothing overshoots when the player stops, and the view never enters a
wall the game kept it out of. Guessing ahead from the last two frames would do both.

It costs almost nothing in delay. Holding the newest frame is already half a game frame old on average, plus
the time the GPU took to finish it; reading the path one frame back comes to about the same (table below).

If the game is late, the path is continued in a straight line for at most 20 ms and then held.
A change faster than 4,000 units/s (57 m/s), a gap over a quarter second, or a frame with no world camera
(menus, loading) starts a new path, so fast travel and doors don't sweep the view across the map.

### Settings (launch-env.json)

| Setting | Default | Meaning |
|---|---|---|
| `LB_IMMERSIVE_MOTION` | off | `1` turns it on. Needs `LB_IMMERSIVE_HEAD=1` and depth. |
| `LB_IMMERSIVE_MOTION_LAG_MS` | one game frame | How far back to read the path. `0` reads it at the moment of drawing, which runs ahead of the newest frame most of the time. |
| `LB_IMMERSIVE_MOTION_AHEAD_MS` | 20 | The longest the path is continued past the newest frame. |
| `LB_IMMERSIVE_MOTION_NEAR_M` | 1.0 | Nearer than this travels with the camera. `0` moves everything with the world. |

Every 5 s the log gets one line, enough to tell from a short session whether it behaved:

    immersive: camera motion read 22 ms back: 96% between frames, 4% continued, 0% held; mean shift 4.1 units, 0 cuts, 0 frames off the path

## What was checked without a headset

**Measured on Linux, 2026-11-13.**

- `python3 tests/motion/run.py` (26 checks, address and undefined-behaviour sanitizers on, exit status 0):
  interpolation, the 20 ms cap, teleports, menus, the offset crossing ±180°, display-time stepping, and that a
  metre forward, right and up in the game comes out as a metre forward, right and up in the compositor's axes.
- `tools/motion_sim.py --synthetic` plays a made-up run (run, strafe, stop dead, turn at 115°/s, fast travel)
  as a slower game on a 90 Hz display. "Step error" is how far each display frame's movement is from the
  player's real movement over the same 11 ms: the judder. A display frame of running is 5.9 cm.

| Game fps | Step error, path (mean) | Step error, held frame (mean) | Trails the player, path | Trails, held |
|---|---|---|---|---|
| 30 | 0.29 cm | 7.46 cm | 31.3 cm | 27.9 cm |
| 45 | 0.24 cm | 5.63 cm | 25.7 cm | 25.1 cm |
| 60 | 0.21 cm | 3.79 cm | 22.9 cm | 23.8 cm |

  Heading at 45 fps: 0.015° against 0.21° per display frame. "Trails" includes 30 ms assumed between drawing
  and showing, the same for both columns. With `--lag-ms 0` at 45 fps the path trails by 14.6 cm instead.

The made-up run has instant starts and stops and no noise in its timing beyond what the tool adds. A recording
of real play says more: run any build with `LB_CAMLOG=1 LB_FRAMETIMES=1`, keep stderr, and give the log to
`tools/motion_sim.py`.

## What it does not fix

- **Things that move on their own** (people, animals, arrows) still move at the game's rate.
- **The player's body in third person** travels with the camera but is further than 1 m away, so it is shifted
  with the world and will shake while the world is smooth. Not handled.
- **Depth edges.** A shifted frame shows nothing where it was hidden. The existing rule (a triangle across a
  depth edge is laid flat at the far side) covers it the same way it does for head movement. At a run, one
  45 fps frame is 12 cm of movement.
- **Hands, if their depth is wrong.** The first-person pass writes depth after a clear, and the shader turns
  depth into distance with the scene's near plane (15 units). If that pass uses another near plane the hands
  come out at the wrong distance and the 1 m test misses them. Unknown; see the first Mac check.

## For the Mac session

Build this branch (`immersive/locomotion-extrapolation`) for visionOS and the Mac. `runtime/motion_path.c` is
picked up by the existing glob.

1. **Does it compile?** `ImmersiveScreen.swift` (uniforms, shader string, `framePose`, `framePoses`) and the
   new declarations in `SkyrimNativeBridge.h` were written blind. The two uniform structs must stay the same
   layout: `heldModel` after `model`, `heldWithin` last.
2. **`python3 tests/motion/run.py`** on the Mac (uses `CLOCK_UPTIME_RAW` there).
3. **Clock.** The path's clock on Apple systems is `CLOCK_UPTIME_RAW`, assumed to be the clock of
   `drawable.frameTiming.presentationTime` (the seconds already passed to `queryDeviceAnchor`). If they differ
   the runtime logs `the display time ... isn't on the runtime's clock` once and falls back to the time of
   drawing, which still works but steps less evenly. Check the simulator's log for that line.
4. **Simulator, `LB_IMMERSIVE_HEAD=1 LB_IMMERSIVE_MOTION=1 LB_FOV=100`, with `LB_MAX_FPS=30`** so the effect is
   large: walk and strafe past something near (a fence, a doorway), turn with the right stick, then draw a
   weapon and do it again. Wanted: the world slides instead of stepping; hands and weapon steady; no jump when
   stopping; the 5 s log line mostly "between frames". A screen recording at 30 fps game rate with the setting
   on and off would let Thomas judge it without the headset.
5. **Hands.** If the weapon shakes with the setting on, the 1 m test is missing it: try
   `LB_IMMERSIVE_MOTION_NEAR_M=2`, and dump one first-person frame's depth (`LB_SHOT`) to see what distance the
   hands come out at.
6. A real log for `tools/motion_sim.py`: a minute of walking, running, strafing and turning on the Mac build
   with `LB_CAMLOG=1 LB_FRAMETIMES=1`.

## For the headset, one short session

Only after 1–4 pass. Ten minutes:

1. `LB_IMMERSIVE_MOTION=1`, game capped at 45 (`LB_MAX_FPS=45`). Walk, run, strafe, stick-turn. Comfortable?
2. The same with `LB_IMMERSIVE_MOTION=0`, to feel the difference.
3. If it feels slow to respond: `LB_IMMERSIVE_MOTION_LAG_MS=0`. If that overshoots on stops, try `11`.
4. Keep the log: the 5 s `camera motion` lines and the `Hz presented, fps from the game` lines.

## Merging

`ImmersiveScreen.swift` is also changed by #13, #14 and #20; `SkyrimNativeBridge.h` and `d3d9_bridge.cpp` by
#20. The changes here are small and local (`framePose`, the uniforms, one line at Present, one after
`head_steer`), so whichever lands second needs a hand merge, not a redesign. With #20's frame pacing the two
belong together: pace the game at 45, and this makes 45 look like 90.
