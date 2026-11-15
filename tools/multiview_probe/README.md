# multiview_probe

A cheap test that can kill the plan to render both eyes from one submission of each draw with Vulkan
multiview (`VK_KHR_multiview`, core in Vulkan 1.1) through D3D9 -> DXVK 1.9.2 -> MoltenVK 1.4.2 -> Metal.
It runs headless on a Mac against the MoltenVK package the game links. It draws into offscreen images, reads
them back, and checks them on the CPU.

**Status: every check passes on an M1 and an M5 Mac (MoltenVK 1.4.2), with the layered Metal path
(check 7).** Not yet run on the headset's GPU or against the visionOS MoltenVK slice.

## Build (macOS)

From the repository root:

```sh
tools/multiview_probe/build.sh
```

This produces `build/multiview_probe`, which is gitignored. The script uses the MoltenVK package at
`$SKYRIM_MOLTENVK_ROOT` if set (the directory holding `include/` and `dynamic/`), else
`macos/deps/MoltenVK/MoltenVK` in this checkout. In a worktree without `macos/deps`, point
`SKYRIM_MOLTENVK_ROOT` at another checkout's package.

Here is the exact command it runs, for building by hand:

```sh
MVK=macos/deps/MoltenVK/MoltenVK
xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Wno-missing-field-initializers \
    -I"$MVK/include" \
    tools/multiview_probe/multiview_probe.mm \
    -L"$MVK/dynamic/dylib/macOS" -lMoltenVK -Wl,-rpath,"$MVK/dynamic/dylib/macOS" \
    -o build/multiview_probe
```

- There is no Vulkan loader. `libMoltenVK.dylib` is linked directly as the driver, the way the game's Mac build
  links it (`macos/graphics/CMakeLists.txt`, `SKYRIM_MOLTENVK_ROOT`).
- The dylib's install name is `@rpath/libMoltenVK.dylib`, and the rpath above resolves it.
- The dylib is universal (arm64 and x86_64).
- No frameworks need linking: the Metal header is included only for enum types.

## Run

```sh
build/multiview_probe                                   # defaults: --size 1024x1024 --draws 2000 --reps 7
build/multiview_probe --size 1920x1824 --draws 5000     # closer to one Vision Pro eye
build/multiview_probe --dump /tmp/mvprobe               # also writes every read-back layer as PPM
MVK_CONFIG_LOG_LEVEL=3 build/multiview_probe            # MoltenVK info logging on stderr
```

Each check prints `PASS` or `FAIL`. The exit status is 0 when everything passes, 1 on any FAIL, and 2 on a
setup error (a Vulkan call failed, there is no device, or an argument is bad).

Before creating the instance, the probe sets two environment variables. A value already in the environment
wins.

- `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`, which matches the game. `simulator/game_app.cpp` sets it the same
  way.
- `MVK_CONFIG_SHADER_DUMP_DIR=<a new $TMPDIR/multiview_probe_msl.XXXXXX>`, used by check 7.

## What each check means for the DXVK multiview plan

| # | What it does | If it fails |
|---|---|---|
| 1 | Reads `VkPhysicalDeviceMultiviewFeatures.multiview` and prints `maxMultiviewViewCount` and `maxMultiviewInstanceIndex`. | The plan is dead on this MoltenVK. `maxMultiviewInstanceIndex` caps instance counts inside a multiview pass, so compare it with the largest instance count Skyrim uses (grass). |
| 2 | Render pass with view mask `0b11` over a 2-layer colour image and a 2-layer D32 depth image. The vertex shader adds `ubo.shift[gl_ViewIndex]` (-0.25 / +0.25 NDC) to x. The probe draws a near green triangle, then a far red one around it. It checks the shape's centroid in each layer (left in layer 0, right in layer 1, `W/4` px apart, within 1.5 px), its area, and that the green triangle survives in both layers. | This is the core of the plan: per-eye offsets from one draw, plus a separate depth buffer per eye. If the green area is wrong in only one layer, depth is shared or missing for that layer. |
| 3a | Second multiview pass. A full-screen triangle samples `sampler2DArray` at layer `gl_ViewIndex` (`textureLod`) from the image check 2 produced. Each output layer must equal the matching input layer, byte for byte (±1). | DXVK post-processing passes (Skyrim's bloom, tonemap, DOF and so on) would read their eye's input only if `gl_ViewIndex` works in fragment shaders and selects the layer. |
| 3b | Same pass, but the source is a 1-layer texture bound through a `2D_ARRAY` view. Both output layers must show layer 0. The result line says **CLAMPED** or **NOT CLAMPED**. | The plan binds ordinary D3D9 textures (1 layer) as arrays and samples layer `gl_ViewIndex` without knowing which textures are per-eye. Vulkan requires sampling instructions to clamp the layer to `[0, layers-1]`. If Metal or MoltenVK returns black instead, DXVK must emit an explicit clamp, for example `min(viewIndex, layerCount-1)` from a spec constant or push constant. This check counts as FAIL when not clamped. It says INCONCLUSIVE if 3a failed, because a broken `gl_ViewIndex` that is always 0 would look clamped. |
| 4 | `vkCmdClearAttachments` with a rect, inside the multiview pass (`baseArrayLayer` 0, `layerCount` 1, as multiview requires). Both layers must be magenta inside the rect and untouched outside it. A draw after the clear must still render in both layers. | D3D9 `Clear()` inside a render pass becomes this call in DXVK. MoltenVK implements it with its own internal draw, which must cover both layers and restore our pipeline state. |
| 5a | `vkCmdDraw` with `instanceCount = 3`. The x offset comes from `gl_InstanceIndex`. Each layer must contain exactly 3 instances, at the expected positions. | MoltenVK draws `instanceCount × viewCount` Metal instances and must give the shader `instance / 2` as the instance and `instance % 2` as the view. If this is wrong, every instanced draw breaks under multiview. |
| 5b | Same draw, but the offsets come from a `VK_VERTEX_INPUT_RATE_INSTANCE` attribute. | D3D9 instancing (`SetStreamSourceFreq`) reaches DXVK as instance-rate vertex bindings. Under multiview, MoltenVK must also scale the Metal step rate by the view count. This check was not in the original request. It is cheap and directly relevant. |
| 6 | Timing: N small draws (one `vkCmdPushConstants` plus one `vkCmdDraw` each). Three cases: (i) mono pass, 1 layer W×H; (ii) multiview pass, 2 layers; (iii) two mono passes, one into each layer, i.e. 2N draws. Reports the median CPU time for record and for `vkQueueSubmit`, and the wall time to the fence. | This is the payoff. Multiview CPU time should be close to mono and well below two passes. By default MoltenVK encodes Metal commands inside `vkQueueSubmit` (`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0`), so that column is part of the CPU cost. Wall time includes the GPU. The check fails only if a case did not run. |
| 7 | Reports which Metal path MoltenVK took. This is informational and does not affect the exit status. See below. | Not applicable. |

## Check 7: which Metal path MoltenVK used

MoltenVK has two ways to run a multiview subpass:

- **Layered.** One Metal render pass with `renderTargetArrayLength = 2` and the instance count doubled. The
  vertex shader writes `[[render_target_array_index]]`.
- **Fallback.** One Metal render pass per view, replaying the subpass, with no
  `[[render_target_array_index]]` output.

Both paths read the view base and count from a buffer named `spvViewMask`, so only
`render_target_array_index` tells them apart.

MoltenVK picks layered when the GPU supports layered rendering. The probe reports two hints:

- **(a) `layeredRendering`.** Read from `MVKPhysicalDeviceMetalFeatures` via
  `vkGetPhysicalDeviceMetalFeaturesMVK`. That function is MoltenVK's own exported query. It is deprecated, but
  1.4.2 still ships it. The probe looks it up with `dlsym`, so nothing links against it.
- **(b) The generated MSL.** MoltenVK dumps it into `MVK_CONFIG_SHADER_DUMP_DIR`, and the probe counts the
  files that mention `render_target_array_index` or `spvViewMask`.

If both hints are missing or disagree, confirm with a Metal capture. MoltenVK can capture by itself:

```sh
MTL_CAPTURE_ENABLED=1 MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE=1 \
MVK_CONFIG_AUTO_GPU_CAPTURE_OUTPUT_FILE=/tmp/mvprobe.gputrace \
build/multiview_probe --size 256x256 --draws 10 --reps 1
open /tmp/mvprobe.gputrace
```

Scope 1 means the whole VkDevice lifetime. The probe destroys its device at exit so the trace is written. In
Xcode, open the render command encoder for check 2:

- **Layered:** one encoder whose pass descriptor has `renderTargetArrayLength` 2, with draws of 2 instances
  (`instanceCount × 2`).
- **Fallback:** two encoders, each with 1 instance per draw.

Alternatively, `xcrun xctrace record --template 'Metal System Trace' --launch -- build/multiview_probe`
shows the encoder count per command buffer.

`MVK_CONFIG_DEBUG=1` also logs every converted MSL shader to stderr, which is another way to see
`render_target_array_index`.

## visionOS

The checks are platform-neutral, but this file is a macOS command-line tool. To run it on a Vision Pro:

- **Bundle it into an app.** visionOS cannot run a bare CLI. Rename `main()` and call it from the app, for
  example from a debug menu, and send `printf` to the log.
- **Link the visionOS MoltenVK.** Use the xcframework slice the visionOS build uses (see `ipad/`), not
  `dynamic/dylib/macOS`. The `#include` lines stay the same.
- **Pick another dump directory.** `$TMPDIR` works inside the app container, but `--dump` needs a path in it.
  Retrieve the files with Xcode's container download.
- **Expect different hardware results.** The device GPU (M2, or M5 on newer units) supports layered
  rendering, so expect the layered path. The visionOS *Simulator* runs Metal on the host through a restricted
  layer and may report different features, so don't treat Simulator results as headset results.
- **Use the same capture method.** `MTL_CAPTURE_ENABLED` is not needed when launching from Xcode with GPU
  frame capture enabled.
- **Note that MoltenVK does not use Compositor Services' vertex amplification.** Compositor Services renders
  with Metal vertex amplification (`[[amplification_id]]`), but MoltenVK 1.4.2's multiview does not use it as
  far as the source study found. It uses instancing plus layered rendering. The probe measures MoltenVK's
  path, not the best Metal could do.

## Why the SPIR-V is hand-assembled

Nothing can compile GLSL at build time or run time here:

- There is no glslang or spirv-tools in the repository, and the build does not assume any are installed.
- DXVK 1.9.2 generates its SPIR-V in C++ and does not vendor glslang.
- The MoltenVK package ships only headers and `libMoltenVK.dylib`, with no `MoltenVKShaderConverter`
  library. MoltenVK's private MSL-source shader-module path would bypass SPIRV-Cross, and SPIRV-Cross is
  exactly what derives `gl_ViewIndex` from the instance index, so it would defeat the test.

So the probe has a ~150-line SPIR-V builder (`SpvModule`) and three tiny modules, each with its GLSL in a
comment. Every opcode and enum value is listed once in `namespace sp` and was compared against `spirv.hpp`
(SPIR-V 1.4 headers, `third_party/dxvk/include/spirv/`). They are copied out so the build needs no
submodule.

The modules are SPIR-V 1.0 with `OpExtension "SPV_KHR_multiview"` and `OpCapability MultiView`, which is
the form DXVK 1.9.2 would emit.

## If a check fails (least sure → more sure)

1. **The hand-built SPIR-V.** If `vkCreateShaderModule` or `vkCreateGraphicsPipelines` fails, or MoltenVK
   logs a SPIRV-Cross error, this is the first suspect. Ways to check it:
   - Disassemble the words with `spirv-dis`, or validate them with `spirv-val --target-env vulkan1.1`, if you
     can install SPIRV-Tools. Add a temporary `fwrite` of the vectors to get them out.
   - Look at `MVK_CONFIG_DEBUG=1` output.

   Specific doubts:
   - `Flat` on the fragment `ViewIndex` built-in. It matches glslang, but if SPIRV-Cross dislikes it, drop
     that one `decorate`.
   - Push-constant blocks that differ between the vertex shader (2 members) and the sampling fragment shader
     (3 members) but share one 48-byte range.
2. **The MoltenVK header include.** `mvk_deprecated_api.h` uses `MTLLanguageVersion` and
   `MTLArgumentBuffersTier` without including Metal itself, which is why `<Metal/Metal.h>` comes first and
   the file is `.mm`. If this fails to compile, delete check 7(a) (the `getMetalFeatures` block) and the
   include. Everything else needs only `<vulkan/vulkan.h>`.
3. **Check 7's MSL dump.** The exact layout of `MVK_CONFIG_SHADER_DUMP_DIR` (file names, whether MSL is
   written at pipeline creation) is not documented by MoltenVK; 1.4.2 writes `shader-*.metal` files at
   pipeline creation. If it reports 0 files, set the variable in the shell before launching. If it still reports 0 files, use the capture method above. In either case this
   does not affect the exit status.
4. **3b's verdict.** Metal's behaviour for an out-of-range array index in `sample()` is exactly what is being
   measured. "NOT CLAMPED (something other than layer 0)" probably means undefined reads. Look at
   `--dump` output.
5. **Pixel tolerances.** The centroid tolerance is 1.5 px, the triangle area tolerance is 3%, and the green
   and instance area tolerances are 15%. They are derived from the geometry (comments next to
   `kVertices`). At very small `--size` values (below about 256), rasterisation edges may exceed the 15%
   tolerance. Use the default size for verdicts.
6. **Timing interpretation.**
   - The wall-time numbers include the CPU's `vkWaitForFences` wake-up latency.
   - With `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` off, the record and submit split would move.
   - The 2000 tiny triangles are CPU-bound by design. They say little about the GPU cost of layered
     rendering at full scene load. Use the game's own bench for that.
7. **Vulkan usage itself** (render pass dependencies, layouts, framebuffer `layers = 1` with 2-layer views
   for multiview). These follow the spec. MoltenVK is lenient here in any case.
