# Foveating the game's own render

Issue #21 item 3. Written 2026-11-13 on the Linux laptop, for whoever takes it to the Mac ("Skyrim - M1").
Branch `immersive/foveated-render`. Nothing here has run on a Mac or the headset.

Markers: *read* = read in the source at the version named; *documented* = Apple's Metal documentation says so;
*measured* = run on the Linux laptop (NVIDIA GPU, native Vulkan, no Metal); *computed* = from
`tools/foveation_map.py`; *unverified* = neither read nor run, with the test that would settle it; *idea* =
untested design.

## Verdict

Feasible, with one small patch to MoltenVK and a real amount of work in DXVK, and three Metal behaviours still
to confirm on a Mac before anyone builds further (scissor space, fragment-position space, MSAA with a map).

- The compositor already foveates its own eye passes; that only saves compositor fill. The game's frame is the
  one that is short of pixels: 1280 across a 115.6° frustum is 7.0 px/deg at the centre against a panel of
  about 34 (`docs/handoff/stereo-quality.md` on `origin/helper/stereo-quality`).
- A rate map on the game's passes gives **2× the centre sharpness for about 1.5× the pixels** (2560×1440
  logical rendered into 1321×1077, *computed*). The issue's "2–3× for 1.3–1.6×" holds for 2×; 3× costs about
  3.5× the pixels at `LB_FOV=100` (table below).
- MoltenVK 1.4.2 has no way to pass a rate map (*read*). The patch that adds one is 113 lines in four files and
  needs MoltenVK built from source at the pinned tag. No third helper fork is strictly needed.
- The hard part is not the map but the game's frame: every later pass that reads a foveated target must be
  told where its texels went. The prototype sorts draws into "through the map" and "in warped space" and
  counts what it cannot handle yet; how well that sorting fits Skyrim is *unverified* and is the first thing
  the Mac build can answer without the patch.

## What it costs and buys (*computed*)

`tools/foveation_map.py table`. Default map: full rate inside 20° of the centre, falling to 0.3 of the centre's
angular resolution over the next 30°, 32 column bands by 18 row bands. "Physical" is from the requested rates;
Metal may round rates up, so the real size will be somewhat larger (*documented*; the probe prints it).

| LB_FOV | Game size | Centre px/deg | Rendered at | Pixels vs 1280×720 | px/deg at 20° / 35° / 50° |
|---|---|---|---|---|---|
| 100 | 1280×720 | 7.0 | 661×539 | 0.39× | 7.3 / 4.7 / 3.5 |
| 100 | 1920×1080 | 10.6 | 991×808 | 0.87× | 11.0 / 7.1 / 5.3 |
| 100 | **2560×1440** | **14.1** | 1321×1077 | **1.54×** | 14.6 / 9.5 / 7.0 |
| 100 | 3200×1800 | 17.6 | 1651×1346 | 2.41× | 18.3 / 11.8 / 8.8 |
| 100 | 3840×2160 | 21.1 | 1981×1615 | 3.47× | 22.0 / 14.2 / 10.6 |
| 80 | 1280×720 | 10.0 | 828×623 | 0.56× | 10.1 / 6.7 / - |
| 80 | 1920×1080 | 15.0 | 1241×935 | 1.26× | 15.1 / 10.0 / - |
| 80 | 2560×1440 | 20.0 | 1655×1246 | 2.24× | 20.2 / 13.3 / - |

For comparison, the uniform 1280×720 frame at FOV 100 has 7.0 px/deg at the centre and 17 at 50° off-axis: a
planar projection spends 1/cos² more pixels per degree toward the edge, which is the wrong way round for an
eye. Removing only that oversampling (`--inner 90 --floor 1`, an equi-angular map) already gives 2560 logical
for 2.21× the pixels; the foveal falloff takes it to 1.54×.

Two limits on what this buys:

- It is pixels, not CPU. On the M1 the frame is limited by the game's main thread (79% lifted code, *measured*
  in `docs/perf/LOG.md`), and the map does not change vertex or draw-call work. It helps where the GPU is the
  limit or has headroom to spend on sharpness, which is the headset case the issue describes.
- The map is fixed at the frame centre. Apps do not get gaze on visionOS, so this is the design, not a test
  shortcut. Looking 20° off-centre with the eyes shows the falloff zone; the `--inner` angle is the tuning
  knob for that, and it needs the headset to judge.

## Findings

### 1. MoltenVK

- The project uses the **official prebuilt MoltenVK 1.4.2 release**, pinned by digest: the macOS dylib through
  `macos/graphics/fetch_dependencies.py`, the device slices through `ipad/prepare_moltenvk.py` (*read*). The
  iOS Simulator slice is already built from source at a pinned commit by `simulator/bootstrap.py` (*read*), so
  a source build has a precedent in the repo.
- 1.4.2 does not advertise `VK_EXT_fragment_density_map`, and nothing in it references
  `rasterizationRateMap` (*read*, grep of the v1.4.2 tree). `VK_EXT_metal_objects` exports Metal objects but
  cannot attach anything to a pass.
- The Metal pass is made in one place: `MVKCommandEncoder::beginMetalRenderPass`
  (`MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm:768`), which builds the `MTLRenderPassDescriptor` and calls
  `renderCommandEncoderWithDescriptor:`. It is reached from `beginRenderpass` → `setSubpass` and again for
  every mid-pass restart (`restartMetalRenderPassIfNeeded`), so a map stored on the encoder for the life of
  the Vulkan render pass survives restarts (*read*).
- `vkCmdBeginRenderPass` lands in `MVKCmdBeginRenderPassBase::setContent`
  (`Commands/MVKCmdRendering.mm:33`), which is where a private `pNext` struct can be picked up (*read*).
- Scissors are clamped to the render area (`clipToRenderArea`, `MVKCommandBuffer.mm:914`, used at
  `MVKCommandEncoderState.mm:1405`); viewports are passed through unclamped (*read*). With a map the render
  area is the physical size while the app's scissor is in screen space, so the clamp has to change.
- Deployment targets are macOS 12 / iOS 15 (*read*), and rate maps exist from macOS 10.15.4 / iOS 13
  (*documented*), so the patch needs no availability checks beyond excluding tvOS.

**The patch** is at the end of this note: a private struct (`sType` 1000999001) chained to
`VkRenderPassBeginInfo` carries an `id<MTLRasterizationRateMap>`; MoltenVK sets it on the pass descriptor and
clamps scissors to the map's screen size. `git apply --check` passes against v1.4.2; it has **never been
compiled**. It does not need a fork: a script like `simulator/bootstrap.py` can clone the tag, apply the diff
and build. A fork only becomes worth it if the patch grows (the scissor conversion, MSAA).

### 2. What Metal says about rate maps

*Documented* (Apple's `MTLRasterizationRateMap`, `MTLRasterizationRateMapDescriptor`,
`MTLRenderPassDescriptor.rasterizationRateMap` and "Rendering at different rasterization rates" pages):

- A layer has one rate per column band and one per row band (separable). Metal "guarantees that the actual
  rasterization rates are at least as high as the rates you specified", and "may split it into more detailed
  cells or choose higher rates". So the real map is only known by asking it.
- `screenSize` is the logical size; `physicalSize(layer:)` is what the pass really rasterizes. Render targets
  "should be at least as large as the physical size".
- With a map set, "Metal uses the rate map to convert between viewport coordinates and physical coordinates";
  with none, "viewport coordinates are in the same coordinate system as the physical coordinates in the
  render target". So the **viewport stays logical** in a mapped pass.
- The content lands in the top-left physical-size region of the target, warped. To read it, a later pass
  converts screen coordinates to physical ones: on the CPU with `physicalCoordinates(screenCoordinates:layer:)`
  and its inverse, in a shader with `rasterization_rate_map_decoder` fed from `copyParameterData`.

*Unverified*, and what `tools/ratemap_probe.swift` reports for each:

- whether the **scissor rectangle** is in screen or physical space (decides the clamp in the patch);
- whether a fragment's `[[position]]` is physical or screen (matters for any shader that uses `VPOS`);
- whether a mapped pass can be **multisampled** (`--msaa 4`; a crash or validation error is the answer);
- how far the real physical size and table are from the requested ones for these maps.

### 3. The game's frame

From `docs/perf/LOG.md` (*measured* there, Riverwood, low preset, 1280×720): 2,013 draws in about 27 passes.

| Pass | Target | Back-buffer-sized |
|---|---|---|
| two small targets (17 draws) | 512×512 | no |
| shadow map (268) | 512×1024 | no |
| reflection (197) | 960×540 | no |
| main scene (620) | 1280×720 RGBA16F | yes |
| scene depth again (638) | 1280×720 R32F colour | yes |
| effects, first person | 1280×720 | yes |
| luminance chain | small, halving | no |
| final image | back buffer | yes |

Which of these are full-screen quads, and which scene draws sample a back-buffer-sized texture by screen
position (water depth and refraction, heat shimmer, the depth-of-field and blur passes), is **not in the log**
and I did not have a trace to read: *unverified*. The prototype counts them instead (below), and
`LB_D3DTRACE` on the Mac can list them.

How DXVK 1.9.2 makes these (*read*, `android/app/jni/dxvk/src/d3d9`):

- Every render-target or depth-stencil surface or texture is a `D3D9CommonTexture`; its Vulkan image is made in
  `CreatePrimaryImage` with the extent from the D3D description. The back buffers are made the same way in
  `D3D9SwapChainEx::CreateBackBuffers`, the automatic depth buffer right after.
- The viewport and scissor come straight from the game's `SetViewport` / `SetScissorRect` in
  `BindViewportAndScissor`; `SetRenderTarget` resets the viewport to the new target's D3D size.
- A framebuffer's size is the smallest of its attachments.
- The immersive present tap exports the back buffer copy and the R32F depth at the image's own extent, so it
  hands the compositor the physical size without being told.
- Hazard tracking (`config::HazardTrackingEnabled`) is compiled **off** in this tree, so
  `m_activeRTTextures` is always zero. The first version of the prototype used it and never saw a warped
  source; the smoke test caught that (*measured*). The prototype now keeps its own mask.

### 4. The trap

It is real (*documented* semantics, section 2): after a mapped pass the picture occupies only the physical
region and is warped. A later full-screen pass that reads it with uniform UVs over the game's size reads the
wrong texels, and if the target is still the game's size most of it is never written.

What follows, and what the design below does about each:

| Later pass | Problem | Handling |
|---|---|---|
| Full-screen quad reading a foveated texture (tone map, blur, final copy) | uniform UV ≠ where the texel went | run it **without** the map, viewport shrunk to the physical size: texel-for-texel in warped space |
| Same, but its shader rebuilds a view ray or position from UV and depth | the UV it sees is warped, so the ray is wrong off-centre | *not handled*; needs the shader list (request 6) |
| Blur or downsample taps at fixed texel offsets | a fixed offset covers more of the view where the rate is low: blur widens toward the edge | accepted; visible only as slightly wider bloom/DoF off-centre |
| Quad writing a foveated picture into a smaller ordinary target (bloom, luminance) | the small target now holds a warped picture | marked "tainted" so quads that read it back into a foveated target stay in warped space |
| Scene geometry sampling a foveated texture by projected position (water, refraction) | wrong texels | *not handled*, counted as "hazards"; the fix is a lazily resolved unwarped copy |
| Shaders using `VPOS` | position space under a map is unverified | probe, then count the shaders (request 5) |
| A foveated depth buffer bound with an ordinary colour target (the 960×540 reflection pass, if it shares the main depth buffer) | framebuffer shrinks to the smaller of the two | *not handled*, counted as "mixed targets"; the fix is a uniform twin of the depth buffer |
| `Clear` or `StretchRect` of part of a foveated target | the rectangle is in screen space | scaled by plain proportion (exact for whole targets and for the emulation), counted |
| HUD and menus drawn into the back buffer | drawn through the map, so text off-centre is at reduced rate | accepted for now; the plan already moves the HUD to its own panel |
| MSAA targets | unverified with a map | the prototype switches itself off for a multisampled back buffer |

### 5. The compositor (`apple/Sources/App/ImmersiveScreen.swift`)

Today it draws a 321×181 grid, uniform in the frame's UV, displaced by the depth it samples per vertex, and it
already sets the drawable's own rate maps on its eye passes (lines 214, 451, *read*). Those are unrelated to
the game's map and stay.

To undo the game's map it needs one thing: every place that samples the game's colour or depth at a frame UV
must sample at the **physical** position instead. The smallest change (*idea*, nothing edited here because
PRs #13 and #14 both touch this file):

- get the map with `skyrim_foveation_current()` (same process, same device), copy its parameters to a buffer
  once per swap-chain change (`parameterDataSizeAndAlign`, `copyParameterData`);
- in the shaders, a helper `toPhysical(uv)` using `rasterization_rate_map_decoder::
  map_screen_to_physical_coordinates`, divided by the physical size, wrapped round each colour and depth read.
  The grid, the displacement maths and the frustum stay in screen UV exactly as now.

Bilinear filtering stays correct because the map is continuous: neighbours in physical space are neighbours on
screen. Later the grid could be laid out uniformly in physical space so its vertex density follows the pixel
density.

- **PR #14 (per-pixel depth search):** each step's depth read goes through `toPhysical`. The step lengths are
  computed in screen space and do not change.
- **PR #13 (MetalFX spatial):** the scaler takes the warped image and returns a larger warped image; normalised
  physical UVs are unchanged, so it composes. The two overlap in purpose, though: with a map it is cheaper to
  raise the game's logical size than to upscale.
- The Mac window build has no compositor. With the emulation switch DXVK's own present blit scales the source
  rectangle and the window looks normal; with a real map the window shows the warped picture stretched. An
  unwarping present blit in DXVK is not written; the Mac tests use `LB_SHOT` captures and
  `tools/foveation_map.py unwarp --lut`.

### 6. Alternatives that avoid the MoltenVK patch

| Alternative | Cost | Verdict |
|---|---|---|
| **Narrower game FOV** (`LB_FOV=80`, already possible) | none to build; 10.0 px/deg at 1280 for 1.0× pixels, but the picture covers 96° instead of 116° and the edge of the world is in view sooner when the head turns | not foveation, but the cheapest 1.4×; combine it with the map (FOV 80 at 1920 logical: 15 px/deg for 1.26×) |
| Swizzle `renderCommandEncoderWithDescriptor:` from the app | no MoltenVK build, but the app must guess which Metal pass belongs to which DXVK pass, and MoltenVK's scissor clamp to the physical render area cannot be fixed from outside | fragile; no |
| Vertex-shader warp in DXVK's shader translator | no Metal feature needed; straight edges of large triangles bend (walls, ground, water), and every vertex shader has to be rewritten | wrong picture; no |
| Multi-resolution shading in DXVK (several viewports per pass, each its own scale) | 4–9× vertex work or geometry amplification Metal lacks; seams; touches every pass | far more work than the patch |
| Two passes: full frame low, inset high | doubles the draw calls on the thread that is already the limit | no |
| Unwarp once in DXVK at the last post pass (hidden twin back buffer) | still needs the map on the scene passes | a later refinement, not an alternative |

I would build the MoltenVK patch: it is the only route where the scene passes cost fewer pixels with no extra
CPU and a correct picture, and the patch is small. I would turn `LB_FOV` down to about 80 with it.

## Recommended design: the whole frame in warped space

1. The game is told its usual (logical) size. Every render target and depth buffer of exactly that size is
   created at the map's physical size.
2. **Scene draws** into such a target go through the map: the render pass carries the map, viewport and
   scissor stay logical.
3. **Full-screen quads** that read a warped texture (at most two triangles, depth test off) run without the
   map with viewport and scissor scaled to the physical size: warped in, warped out.
4. Ordinary targets filled by such a quad are marked as holding a warped picture, so the chain
   scene → bloom → back into the scene stays consistent.
5. DXVK ends a mapped pass before any `Clear`, because MoltenVK clears with a quad sized in physical pixels.
6. The compositor undoes the map once, in the shaders that already resample the frame (section 5).

The game never sees any of it: `GetDesc`, viewports and readbacks keep the logical size.

## What is prototyped

All of it is off unless `LB_GAME_FOVEATION=1`. With the switch off, what remains on the hot paths is one
predicted-false branch per draw and one mask update per `SetTexture`.

**In vendored DXVK** (`android/app/jni/dxvk`, on this branch). It is in the tree rather than a patch file
because it can be compiled and run here. PR #8 moves DXVK to a submodule: when that lands, this diff has to be
carried to `twcclegg/dxvk-native` (`skyrim-apple` branch) instead.

- `include/native/skyrim_foveation.h`: the C interface. The app registers a provider that returns the physical
  size and the `MTLRasterizationRateMap` for a screen size; `skyrim_foveation_current()` is for the
  compositor; `skyrim_foveation_last_frame()` returns the counts below.
- `src/d3d9/d3d9_foveation.h`, `d3d9_swapchain.cpp`: configuration when the back buffers are made, the
  per-frame counts, the scaled source rectangle for DXVK's own present blit.
- `src/d3d9/d3d9_common_texture.*`: back-buffer-sized targets get the physical extent; the "holds a warped
  picture" flag.
- `src/d3d9/d3d9_device.*`: `UpdateFoveationForDraw` sorts each draw (steps 2–4), viewport and scissor
  scaling, `Clear`, `StretchRect` and `GetRenderTargetData` on foveated surfaces.
- `src/dxvk/dxvk_context.*`, `dxvk_context_state.h`: `setFoveationRateMap` and the private struct chained to
  `VkRenderPassBeginInfo` (the other half of the MoltenVK patch).

Switches:

| Variable | Meaning |
|---|---|
| `LB_GAME_FOVEATION=1` | on (default off) |
| `LB_GAME_FOVEATION_EMULATE=<0.25–1>` | no map: shrink foveated targets by this factor. Runs every code path except the map itself, on stock MoltenVK and on any platform |
| `LB_GAME_FOVEATION_LOG=1` | log the counts every 60 frames |
| `LB_GAME_FOVEATION_MAP=<rates.json>` | (provider) build a real map from `foveation_map.py rates` |
| `LB_GAME_FOVEATION_LUT=<lut.json>` | (provider) write the map Metal really made, for `unwarp --lut` |

The counts, per frame: draws through the map; quads run in warped space; **hazards**; **tainted** targets;
**mixed targets**; partial clears; partial copies; mode switches (each restarts a render pass, which on a
tile-based GPU is a store and a load of the target, so this number is also the prototype's overhead).

**Outside DXVK:**

- `macos/graphics/foveation_smoke.cpp` (+ `CMakeLists.txt`): a D3D9 program that checks the plumbing under the
  emulation, and that two geometry draws and one textured quad are sorted as 2 mapped / 1 unmapped.
- `runtime/foveation_provider.mm`: the provider for a fixed map from a JSON file. **In no build**; the header
  comment says how to wire it into the Mac target.
- `tools/foveation_map.py`: the table, the rates, and warp/unwarp of images (including real captures with the
  real map's table).
- `tools/ratemap_probe.swift`: the stand-alone Metal probe for section 2.
- The MoltenVK patch: below, and privately at `~/skyrim-work/patches/moltenvk-rate-map/` on the Linux laptop.
  It is in the note, not the tree, because nothing in the repo builds MoltenVK for macOS or the device from
  source yet, and the Mac session cannot read the laptop's private directory.

### What was checked here, and what was not

- DXVK with the changes: all 120 objects of the `desktop/` CMake build compile with clang with no warnings and
  `libdxvk_d3d9.so` links (*measured*, Linux).
- `foveation_smoke`: passes with `LB_GAME_FOVEATION_EMULATE=0.5` (256×128 rendered at 128×64, eight pixel
  checks, counts 2 mapped / 1 unmapped / 1 mode switch) and with `--off` (*measured*, Linux, NVIDIA GPU).
- `foveation_map.py`: warp then unwarp of a synthetic 2560×1440 image returns it; the LUT path agrees with the
  rates path (*measured*).
- `ratemap_probe.swift`: `swiftc -parse` only (Linux has no Metal). **Never compiled or run.**
- `foveation_provider.mm`, the MoltenVK patch, the `macos/graphics/CMakeLists.txt` addition: **never compiled.**
- The game has not been run with any of this.

## Risks

1. **The quad test is a guess.** "At most two primitives and depth test off" has not been checked against
   Skyrim. A full-screen pass drawn as one large triangle is caught; one drawn with the depth test left on, or
   a small quad that is not full-screen but reads a warped texture (a lens flare occlusion test, say), is
   sorted wrongly. The counts and a trace settle it.
2. **Hazards** (water, refraction, shimmer) need the unwarped-copy fix before the picture is right near water.
   If there are many per frame the copies cost a full-screen pass each.
3. **Mixed targets**: if the reflection pass uses the main depth buffer, reflections are clipped as soon as the
   physical size is smaller than 960×540 in either direction. At `EMULATE=0.75` they are equal by coincidence
   and it will look fine; use 0.5 to see it.
4. **MSAA.** Presets above low use it, and "highest settings" is the goal. If Metal refuses MSAA with a map,
   foveation means FXAA only.
5. **Shaders that rebuild position from UV and depth in a post pass**, or use `VPOS`, are wrong off-centre
   (section 4).
6. **Metal may quantise the map** so that the real saving is smaller than the table says.
7. `LockRect` on a foveated target is not handled; game-created multisampled targets of back-buffer size are
   left alone and would no longer match their resolve target; half-pixel and texel-size constants the game
   computes from the logical size are slightly off in warped space.
8. The provider makes the map on the system default device, which must be the one MoltenVK draws on. True on
   Apple silicon Macs and the headset.
9. Each mapped ↔ unmapped switch restarts a render pass. A frame that alternates often loses some of what the
   map saves.

## Test plan

### Mac build, no headset

In this order; each step is useful if the next one fails.

1. **Probe Metal** (10 minutes, no game):
   `tools/foveation_map.py rates --width 2560 > /tmp/rates.json`,
   `xcrun swiftc -O tools/ratemap_probe.swift -o /tmp/ratemap_probe`,
   `/tmp/ratemap_probe /tmp/rates.json /tmp/lut.json`, and again with `--msaa 4`. Wanted: the whole output.
   It answers scissor space, fragment-position space, MSAA, and the real physical size.
2. **Smoke test through MoltenVK**: build `foveation_smoke` in `macos/graphics`, run it and `--off`. Wanted:
   the JSON line and exit status of each.
3. **The game with the emulation, stock MoltenVK**:
   `LB_GAME_FOVEATION=1 LB_GAME_FOVEATION_EMULATE=0.5 LB_GAME_FOVEATION_LOG=1` in Riverwood, then by water,
   then indoors, then in a menu. The picture should be the normal one at half resolution. Wanted: the
   "Foveation frame" log lines for each place, an `LB_SHOT` capture of each, and anything that looks wrong
   (missing reflections = risk 3, broken bloom or blur = risk 1, wrong water = risk 2).
4. **Trace**: with `LB_D3DTRACE`, which depth buffer the reflection and shadow passes bind, and whether the
   full-screen passes are ≤ 2 primitives with `D3DRS_ZENABLE` off.
5. **Shaders**: how many of the game's pixel shaders declare `vPos`, and which post shaders take depth as an
   input.
6. **The real map**: build MoltenVK v1.4.2 from source with the patch (adjusting the scissor clamp to what the
   probe said), add `runtime/foveation_provider.mm` to the Mac target, run the game at 2560×1440 with
   `LB_GAME_FOVEATION_MAP=/tmp/rates.json LB_GAME_FOVEATION_LUT=/tmp/lut.json`. Capture with `LB_SHOT`,
   unwarp with `tools/foveation_map.py unwarp shot.bmp out.png --lut /tmp/lut.json`, and compare with a
   uniform 2560×1440 capture of the same save. Wanted: both pictures, GPU frame time for uniform 1280×720,
   uniform 2560×1440 and mapped 2560×1440, and the counts.

Pass for moving to the headset: the unwarped capture matches the uniform one at the centre and has no
geometric errors elsewhere in a scene without water; GPU time for mapped 2560 is clearly below uniform 2560.

### Headset, one short session

Needs the compositor change (section 5) on top of #13/#14. One build, switches in the launch environment:

1. Uniform 1280×720 (today), 2 minutes: the reference for sharpness and frame rate.
2. Mapped 2560×1440, default map: is the centre visibly sharper, is the falloff visible when the eyes move
   20–30° off-centre while the head stays still, does the frame rate hold?
3. The same with `--inner 30`, and with `LB_FOV=80` at 1920×1080: pick one.
4. Turn the head quickly: does the low-rate edge of the previous frame show before the next frame arrives?

Record frame rate and the compositor's GPU time for each; write down which map was preferred.

## Appendix: the MoltenVK patch (against v1.4.2, never compiled)

MoltenVK is Apache-2.0. The struct layout and `sType` match `DxvkFoveationRateMapInfo` in
`android/app/jni/dxvk/src/dxvk/dxvk_context_state.h`.

```diff
diff --git a/MoltenVK/MoltenVK/Commands/MVKCmdRendering.h b/MoltenVK/MoltenVK/Commands/MVKCmdRendering.h
index 55132e2..e5cb867 100644
--- a/MoltenVK/MoltenVK/Commands/MVKCmdRendering.h
+++ b/MoltenVK/MoltenVK/Commands/MVKCmdRendering.h
@@ -51,8 +51,20 @@ protected:
 	MVKFramebuffer* _framebuffer;
 	VkRect2D _renderArea;
 	VkSubpassContents _contents;
+	id _skyrimRateMap = nil;	// id<MTLRasterizationRateMap>, not retained: the app keeps it alive
 };
 
+/**
+ * Private to skyrim-apple (docs/handoff/foveated-render.md): chained to VkRenderPassBeginInfo to
+ * rasterize this render pass through a Metal rasterization rate map. Not a Vulkan extension.
+ */
+#define SKYRIM_STRUCTURE_TYPE_RATE_MAP_INFO  ((VkStructureType)1000999001)
+typedef struct SkyrimRateMapInfo {
+	VkStructureType sType;
+	const void* pNext;
+	void* mtlRasterizationRateMap;	// id<MTLRasterizationRateMap>
+} SkyrimRateMapInfo;
+
 
 #pragma mark -
 #pragma mark MVKCmdBeginRenderPass
diff --git a/MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm b/MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm
index df38d4b..a83d01b 100644
--- a/MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm
+++ b/MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm
@@ -37,6 +37,13 @@ VkResult MVKCmdBeginRenderPassBase::setContent(MVKCommandBuffer* cmdBuff,
 	_framebuffer = (MVKFramebuffer*)pRenderPassBegin->framebuffer;
 	_renderArea = pRenderPassBegin->renderArea;
 
+	_skyrimRateMap = nil;
+	for (const auto* next = (VkBaseInStructure*)pRenderPassBegin->pNext; next; next = next->pNext) {
+		if (next->sType == SKYRIM_STRUCTURE_TYPE_RATE_MAP_INFO) {
+			_skyrimRateMap = (id)((const SkyrimRateMapInfo*)next)->mtlRasterizationRateMap;
+		}
+	}
+
 	cmdBuff->_currentSubpassInfo.beginRenderpass(_renderPass);
 
 	return VK_SUCCESS;
@@ -62,6 +69,7 @@ VkResult MVKCmdBeginRenderPass<N_CV, N_A>::setContent(MVKCommandBuffer* cmdBuff,
 
 template <size_t N_CV, size_t N_A>
 void MVKCmdBeginRenderPass<N_CV, N_A>::encode(MVKCommandEncoder* cmdEncoder) {
+	cmdEncoder->_skyrimRateMap = _skyrimRateMap;	// before beginRenderpass(): that starts the Metal pass
 	cmdEncoder->beginRenderpass(this,
 								_contents,
 								_renderPass,
diff --git a/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h b/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h
index 76ca788..c62f259 100644
--- a/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h
+++ b/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h
@@ -261,6 +261,12 @@ public:
 	/** Encode commands from the specified secondary command buffer onto the Metal command buffer. */
 	void encodeSecondary(MVKCommandBuffer* secondaryCmdBuffer);
 
+	/**
+	 * skyrim-apple: the id<MTLRasterizationRateMap> of the current render pass, or nil.
+	 * Set by MVKCmdBeginRenderPass before beginRenderpass(), dropped by endRenderpass().
+	 */
+	id _skyrimRateMap = nil;
+
 	/** Begins a render pass and establishes initial draw state. */
 	void beginRenderpass(MVKCommand* passCmd,
 						 VkSubpassContents subpassContents,
diff --git a/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm b/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm
index 8c24deb..7337901 100644
--- a/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm
+++ b/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm
@@ -835,6 +835,11 @@ void MVKCommandEncoder::beginMetalRenderPass(MVKCommandUse cmdUse) {
 		[mtlRPDesc setSamplePositions: sampPosns.data() count: sampPosns.size()];
 	}
 
+#if !MVK_TVOS
+	// skyrim-apple: the attachments are the map's physical size; viewports stay in the map's screen space.
+	if (_skyrimRateMap) { mtlRPDesc.rasterizationRateMap = (id<MTLRasterizationRateMap>)_skyrimRateMap; }
+#endif
+
     _mtlRenderEncoder = [_mtlCmdBuffer renderCommandEncoderWithDescriptor: mtlRPDesc];
 	retainIfImmediatelyEncoding(_mtlRenderEncoder);
 	_cmdBuffer->setMetalObjectLabel(_mtlRenderEncoder, getMTLRenderCommandEncoderName(cmdUse));
@@ -918,6 +923,20 @@ VkRect2D MVKCommandEncoder::clipToRenderArea(VkRect2D rect) {
 	uint32_t raBottom = max(_renderArea.offset.y, 0);
 	uint32_t raTop = raBottom + _renderArea.extent.height;
 
+#if !MVK_TVOS
+	// skyrim-apple: the render area is the physical size, but the app's scissors (like its viewports) are in
+	// the map's screen space, so clip to that. ASSUMES Metal takes scissors in screen space when a map is set;
+	// tools/ratemap_probe.swift checks. If it wants physical pixels, convert here instead with
+	// [map mapScreenToPhysicalCoordinates:forLayer:] on the two corners.
+	if (_skyrimRateMap) {
+		MTLSize screen = [(id<MTLRasterizationRateMap>)_skyrimRateMap screenSize];
+		raLeft = 0;
+		raBottom = 0;
+		raRight = (uint32_t)screen.width;
+		raTop = (uint32_t)screen.height;
+	}
+#endif
+
 	rect.offset.x      = mvkClamp<uint32_t>(rect.offset.x, raLeft, max(raRight - 1, raLeft));
 	rect.offset.y      = mvkClamp<uint32_t>(rect.offset.y, raBottom, max(raTop - 1, raBottom));
 	rect.extent.width  = min<uint32_t>(rect.extent.width, raRight - rect.offset.x);
@@ -1041,6 +1060,7 @@ void MVKCommandEncoder::endRenderpass() {
 	}
 	_attachments.clear();
 	_renderSubpassIndex = 0;
+	_skyrimRateMap = nil;
 }
 
 void MVKCommandEncoder::endMetalRenderEncoding() {
```

To apply: clone `KhronosGroup/MoltenVK` at tag `v1.4.2`, save the block above as a file (tabs matter),
`git apply` it, and build the macOS dylib the way `simulator/bootstrap.py` builds the simulator slice
(`fetchDependencies --macos`, then the `MoltenVK Package (macOS only)` scheme). Point
`SKYRIM_MOLTENVK_ROOT` at the result.
