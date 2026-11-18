# Stereo quality of the depth reprojection

Branch `helper/stereo-quality`, from `main` at 3d4cd97. Task 3 of the helper brief: the headset verdict was
"not as sharp, doesn't converge as well as our Minecraft port; depth could be stronger".

Nothing here has run on the headset. Everything measured is from seven Whiterun captures of the Mac build
(`LB_SHOT`, 1280×720, default field of view) on the M1.

## What was found

**1. The mesh leaves gaps beside every near object, different in each eye.** A triangle that spans a depth
edge is laid flat at its farthest corner. That disconnects it from the near surface, and from an eye that is
not at the rays' origin there is a strip between the two, as wide as the parallax between them, where nothing
is drawn (the clear colour, black). Each eye has its strips on its own side of an object, so the eyes disagree
exactly at the outlines they use to fuse. Measured with the renderer's own shaders on the Mac
(`tools/immersive_shader_check.swift`), head mode: 0.89% of the picture in one frame with the head at its
starting place, about 5 px wide beside a pillar 2 m away; with the head 15 cm to the side they surround every
near object. The numpy preview counts 0.07–0.71% over the seven frames at the starting place and 0.42–2.52%
with the head 10 cm to the side.

**2. Outlines and thin things get the wrong depth.** One vertex every 4 game pixels, each taking the nearest
depth around it, puts a rim of background at the foreground's distance around objects, and puts the object's
own edge cells at the background's distance. Against the capture's per-pixel depth, head mode: 1.0–2.6% of
pixels are more than 2 arc minutes of disparity off (a headset pixel is about 1.8), 0.4–1.0% more than 5.

**3. Sharpness is mostly resolution, not the reprojection.** The Minecraft port renders each eye natively at
the panel's resolution (about 34 pixels per degree, foveated). Here one 1280-wide frame is spread over the
whole game frustum, so at the centre of view:

| Setting | Frustum (horizontal) | Pixels per degree at the centre, 1280 wide | 1920 wide | 2560 wide |
|---|---|---|---|---|
| Window mode (2.8 m screen at 3 m) | 50° on the screen | 23.9 | 35.9 | 47.9 |
| Head mode, default FOV (65) | 80.7° | 13.2 | 19.7 | 26.3 |
| Head mode, `LB_FOV=80` | 96.4° | 10.0 | 15.0 | 20.0 |
| Head mode, `LB_FOV=100` (the headset test setting) | 115.6° | 7.0 | 10.5 | 14.1 |

At `LB_FOV=100` the centre of view has about a fifth of the panel's resolution each way. A narrower field of
view and a larger `LB_WINDOW` are the direct fixes; the MetalFX upscale (task 2) helps what is left. These
are computed from the frustum tangents in `docs/HEAD_TRACKING.md`, not measured.

**4. Depth in head mode is already true scale.** At `LB_IMMERSIVE_HEAD_SCALE=1` each pixel sits at its game
distance, so the disparity is what a native stereo render would give. The Minecraft port is also true scale
(one block is one metre; its world-scale setting defaults to 1), so it has no stronger depth setting to copy;
its scenes simply have more within arm's reach. "Stronger" therefore needs an exaggeration, see below.

## What was built

All off by default, in `apple/Sources/App/ImmersiveScreen.swift`, set in `launch-env.json` like the other
`LB_IMMERSIVE_*` switches.

- **`LB_IMMERSIVE_REFINE=<steps>`: per-pixel depth search in the fragment shader** (0 = off; 2 is enough,
  more changes nothing measurable). Starting from the mesh, each step takes the depth of the game pixel the
  eye's ray is thought to show, moves to where the ray reaches that distance and looks up the game pixel
  there. A ray that keeps jumping between a near and a far surface is looking past the near one's edge at
  something the game's camera never saw, and shows the far surface's pixel. With it on, the mesh is kept
  whole (edge triangles are no longer laid flat), so there are no gaps.
- **`LB_IMMERSIVE_IPD_SCALE=<k>`: eye separation for drawing, as a multiple of the wearer's** (default 1).
  Above 1 gives proportionally more disparity at every distance, and the world looks smaller by the same
  factor. Unlike `LB_IMMERSIVE_HEAD_SCALE` below 1 it does not also multiply the parallax from moving the
  head. Works in both modes, with or without the search.

Tools:

- `tools/immersive_preview.py` now follows the shader step for step (it used to drop torn cells onto a
  backplate, which the shader stopped doing), and has `--head`, `--refine`, `--ipd-scale`, `--grid` and
  `--metrics`. `--metrics` compares with the capture's per-pixel depth and writes an error map.
- `tools/immersive_shader_check.swift` reads the shader source out of `ImmersiveScreen.swift`, compiles it on
  the Mac, draws both eyes from a capture and times the GPU. Undrawn pixels come out magenta. Its copy of
  `QuadUniforms` has to be kept in step with the renderer's.

## Measurements

Disparity error against per-pixel depth, numpy preview, both eyes, 64 mm, range over the seven frames:

| | Mean, arcmin | Over 2' | Over 5' | Gaps in the picture |
|---|---|---|---|---|
| Head mode, now | 0.20–0.32 | 1.00–2.58% | 0.41–1.04% | 0.07–0.71% |
| Head mode, `REFINE=2` | 0.02–0.08 | 0.19–0.60% | 0.04–0.33% | 0 |
| Head mode, head 10 cm aside, now | 0.18–0.28 | 0.80–2.04% | 0.30–0.69% | 0.42–2.52% |
| Head mode, head 10 cm aside, `REFINE=2` | 0.02–0.08 | 0.18–0.55% | 0.08–0.32% | 0 |
| Window mode (scale 0.15), now | 0.08–0.24 | 0.52–1.50% | 0.22–0.76% | 0.10–0.43% |
| Window mode, `REFINE=2` | 0.06–0.22 | 0.38–1.79% | 0.18–1.00% | 0 |

- Head mode improves in every frame. A finer mesh instead (2 pixels per cell, four times the vertices) was
  worse than the search on the one frame tried: 0.71% over 2' against 0.43%.
- **Window mode: the gaps go, but the depth error barely changes**, and in one frame (2700) it is worse
  (1.48% to 1.79% over 2'). The search is aimed at head mode.
- Pixels the reference does not cover (what the game's camera did not see) are left out of the error; they
  are where the search shows background. The preview fills its own sampling gaps in the whole mesh from the
  same row, so its zero for gaps is by construction; the zero that counts is the Metal one below.

The renderer's own shaders on the Mac (`immersive_shader_check`, frame 3600, both eyes):

- Gaps: head mode 0.89% now, 0.00% with `REFINE=2`; window mode 0.25% and 0.00%. Frame 4190 with the head
  15 cm aside: 0.93% and 0.00%.
- Metal against the numpy preview, head mode with `REFINE=2`: mean difference 2.05/255 (the preview samples
  colour without filtering).
- GPU time for both eyes at 1888×1792 on the M1, median of 30 draws, three runs each: 2.85–3.02 ms now,
  3.66–4.90 ms with `REFINE=2`. Single draws ranged from 2.1 to 12.8 ms and the machine was not quiet (no
  bench lock taken), so read this as "about 1 to 2 ms more on the M1" and no more. The headset draws more
  pixels, with foveation; its cost is unmeasured.

Checked: the shader compiles on the Mac GPU; `ImmersiveScreen.swift` type-checks for visionOS
(`-target arm64-apple-xros26.0` with the bridging header). **Not done: no app or engine build, no simulator
run, nothing on the headset.** The eye-separation switch has only been drawn on the Mac harness, where it is a
plain camera offset; how it feels is a headset question.

## For the headset (Thomas)

One change per test, each with the usual head-mode settings (`LB_IMMERSIVE_HEAD=1`, `LB_ANY_PAD=1`):

1. `LB_IMMERSIVE_REFINE=2`. Look at the outlines of near things against far ones (posts, people, the well's
   crossbar) and at thin things (branches, railings). Before: a dark sliver on one side in each eye, shimmering
   as the head moves, and thin things flat against what is behind them. After: clean outlines; behind an
   object's edge the background is repeated for a few pixels instead. The log line `compositor gpu … ms per
   frame` gives the cost; compare it with the same scene without the switch.
2. `LB_FOV=80` in place of 100, same build. Sharper centre (10 against 7 pixels per degree), less picture to
   turn into before the game catches up.
3. `LB_IMMERSIVE_IPD_SCALE=1.3` if depth still reads as weak. The world will look about a quarter smaller;
   past 1.5 expect it to look like a model.

## Not addressed

- The HUD still takes the depth of the scene behind it (the compass sits at the sky's distance, the
  crosshair on whatever is under it). That is a convergence problem of its own and needs the HUD on its own
  panel, as the plan has it.
- Blended effects (smoke, fire, water) still have the depth of what is behind them.
- The depth the compositor writes for the system's reprojection is the mesh's, not the refined one.
