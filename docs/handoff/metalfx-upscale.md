# MetalFX upscaling for the immersive picture (branch `helper/metalfx-upscale`)

## What was built

In `apple/Sources/App/ImmersiveScreen.swift`, both off by default and read once the game is presenting (so
they work from `launch-env.json`):

- `LB_IMMERSIVE_UPSCALE=<factor>` (1.25 to 3, for example `2`): each new game frame from the present tap goes
  through a MetalFX spatial scaler to factor times its size, and the screen mesh is textured from the result.
  The scale is encoded in the compositor's own command buffer, before the eye passes, only when the game
  frame is new; a repeated frame reuses the last result. Depth, the mesh and head tracking are unchanged
  (they use the game's depth texture at its own size). Unset, `0` or `1` is off.
- `LB_IMMERSIVE_SHARPEN=<0 to 1>`: an unsharp mask in the screen's fragment shader, limited to the range of
  the four neighbouring texels so edges don't ring. It works with or without the upscale; with it, it sharpens
  the upscaled image.
- If the device does not support MetalFX spatial scaling, or the scaler or its output texture can't be made
  for the tapped texture's format, one `immersive: upscale: ...` line says so and the game image is drawn as
  before.
- `tools/metalfx_probe.swift`: a macOS command-line probe that times the same scaler on the Mac's GPU and
  compiles the compositor's shader source.

To use it, render small and upscale, for example `LB_WINDOW=1280x720` with `LB_IMMERSIVE_UPSCALE=2`.

## Simulator

MetalFX is not in the visionOS Simulator SDK (Xcode's XRSimulator 27.2 SDK has no MetalFX framework; an
`import MetalFX` there fails with "no such module"). The import and the scaler are behind
`#if canImport(MetalFX)`; in the simulator the switch logs `upscale: MetalFX is not in this SDK` and the
picture is unchanged. So the upscale cannot be seen or tested in the simulator at all.

## What was checked

- All of `apple/Sources` type-checks with this change for the visionOS device SDK and for the simulator SDK
  (`swiftc -typecheck`, target visionOS 26.0, `-D SKYRIM_NATIVE_RUNTIME`).
- The compositor's Metal shader source, with the sharpening added, compiles on the Mac (`metalfx_probe`).
- A MetalFX spatial scaler can be made on the M1 for BGRA8 at the sizes below.

## GPU cost

There is no Mac path for the immersive compositor, so the cost in the compositor is **unmeasured**. As a
stand-in, `metalfx_probe` timed the bare scaler on the M1's GPU (BGRA8, perceptual mode, 300 frames per
case, three passes). The Mac was heavily loaded by other work at the time (load average over 100), and the
passes disagree, so read these as rough:

| Scale | GPU ms per frame, median of each pass | p95 of each pass |
| --- | --- | --- |
| 1280x720 -> 1920x1080 | 1.1, 2.7, 2.4 | 2.6, 6.8, 6.4 |
| 1280x720 -> 2560x1440 | 1.9, 3.0, 3.0 | 3.7, 6.1, 6.4 |
| 1280x720 -> 3840x2160 | 5.1, 4.1, 5.0 | 7.3, 6.1, 7.1 |
| 960x540 -> 1920x1080 | 1.5, 2.3, 2.8 | 3.4, 5.8, 6.6 |
| 1920x1080 -> 3840x2160 | 9.2, 4.4, 5.4 | 11.3, 6.4, 7.6 |

These say nothing reliable about the M2 headset, where the compositor shares the GPU with the game. The
compositor already logs `compositor gpu N ms per frame`; that line now includes the upscale and is the number
to compare on the headset, with and without the switch.

## What was not tested

- No visionOS app was built and nothing ran on a headset or in the simulator. The first build is untested.
- Whether MetalFX accepts the tapped texture is unknown: the scaler is created with the texture's own pixel
  format for input and output, with perceptual colour processing when the shader decodes sRGB itself
  (`needs_srgb_decode`) and linear otherwise. If the format is refused the log line names it.
- Picture quality: nobody has looked at it. Whether a 720p game image upscaled to 1440p looks better in the
  headset than the same image sampled directly is the open question; the mesh already magnifies the image
  with bilinear filtering, so the gain may be small.
- Sharpening strength values are a guess; nothing was tuned.
- The upscale costs GPU, not CPU. It only helps the CPU-bound game if the game is then run at a lower
  `LB_WINDOW` size, and render resolution costs the game mostly GPU time, so the CPU saving may be small.

## Headset test (Thomas)

One change at a time, in `launch-env.json`:

1. Baseline at the current `LB_WINDOW`, note `compositor gpu` and the game's fps lines.
2. `LB_IMMERSIVE_UPSCALE=2`. Check the log for `immersive: image: upscale x2.0` and
   `immersive: upscale: MetalFX spatial WxH -> ...`. A line ending `drawing the game image as it is` means it
   fell back. Look at distant detail, text in menus and thin edges (branches, railings); compare
   `compositor gpu` and fps with the baseline.
3. If it holds, lower the render size (`LB_WINDOW=1280x720`) with the upscale on and compare sharpness and
   fps against the baseline at the original size.
4. Separately, `LB_IMMERSIVE_SHARPEN=0.3` (then `0.6`) with and without the upscale: look for crisper texture
   detail, and for shimmer or halos on high-contrast edges.
