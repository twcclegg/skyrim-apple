# Options for immersive performance beyond the lifted code

Written 2026-11-10 alongside `docs/perf/LOG.md`. Nothing here is implemented. Items marked *read* come from
reading the code, *measured* from the M1 benchmark, *idea* is untested.

## What the measurements say first

On the M1 at 1280×720 the frame is limited by the game's main thread running lifted code (79% of its on-CPU
time); DXVK's front end is about 3% of it, and DXVK's own threads use under a quarter of a core (*measured*).
The M2 headset adds a compositor and a weaker GPU than the iPad, so the GPU may matter more there, but any
stereo design that makes the game simulate or submit the scene twice per displayed frame pays on the thread that
is already the limit.

## The goal, and the path I'd take to it

Pass/fail for the project (Thomas, 2026-11-10): the game is comfortably playable on the M2 Vision Pro in 3D
immersive with head tracking. "Comfortable" in a headset mostly means three things: the world stays still when
the head moves (the compositor redraws at the display's 90 Hz with the newest head pose, whatever the game's
frame rate), both eyes get a new image every displayed frame, and the game itself stays near 60 fps.

Three ways to get the second eye, cheapest for the game first:

| | Game CPU | GPU | Per-eye rate | Engineering | Main artefact |
|---|---|---|---|---|---|
| **A. Depth reprojection** (section 0) | one mono frame | mono frame + a small warp pass | display rate | moderate, mostly in our own compositor code | stretching at depth edges; HUD and transparent effects need care |
| B. Per-eye draws in DXVK (section 1) | one mono frame | about 2× scene | game rate | large, inside vendored DXVK | screen-space effects slightly off |
| C. Alternate-eye camera hook (plan M3) | one frame per eye | one frame per eye | half the game rate | moderate, in guest hooks | eye-to-eye lag in motion; 40 fps becomes 20 per eye |

I'd build **A first**. It is the only one whose cost doesn't land on the game's main thread or double the GPU
scene, it gives head *translation* as well as rotation for free, and its pieces are ones the plan already needs
(the present tap, the 90 Hz compositor loop, the head-rotation camera hook). B is the upgrade if A's artefacts
bother; C stays as the fallback that needs no depth.

## 0. Depth reprojection: one mono frame, two eyes drawn in the compositor (*idea*, pieces *read*)

The game renders one frame as now, with a field of view wider than the headset shows and the camera turned by
the head's rotation (the plan's M3a hook). The frame's colour **and depth** go to the compositor through the
present tap. Each compositor frame (90 Hz) draws that frame as a depth-displaced grid mesh, once per eye, from
each eye's real view and projection for the newest head pose, and writes its depth so visionOS's own
reprojection keeps working. Head rotation and translation between game frames, and the offset between the
eyes, all come out of the same draw.

- **Depth source:** the plan notes the game creates a readable INTZ depth texture; the D3D9 bridge can find the
  depth buffer bound for the scene, and the tap exports it the way it exports colour. Needs the camera's near
  and far planes and field of view, which sit in the NiCamera the plan already located.
- **Cost:** a grid of a few tens of thousands of triangles per eye, textured, at 90 Hz. No game CPU.
- **Artefacts and their usual fixes:** surfaces revealed at depth edges are stretched (keep the eye separation
  modest, render the mono camera between the eyes); the HUD and menus have no useful depth (draw them to their
  own panel, which the plan already wants via `hud_screen.cpp`); particles, water and other blended effects sit
  at the depth of what is behind them.
- **Can be judged without the headset:** dump colour and depth for a few frames on the Mac build, then render
  the two eye views offline and look at them side by side. That is the next step here.

## 1. Stereo without a second game pass: per-eye draws inside DXVK

The plan's M3 renders alternate eyes through a camera hook, which halves each eye's frame rate. The alternative
is what NVIDIA 3D Vision Automatic did for D3D9 games: the game renders one mono frame, and the graphics layer
issues each scene draw once per eye with the vertex shader shifting clip-space x by
`eye * separation * (w - convergence)`. The game thread's cost doesn't change; the replay runs on DXVK's
command thread and the GPU.

What our DXVK 1.9.2 offers (*read*, by a read-only study of what is now `third_party/dxvk`):

- **Where the shift goes:** `src/dxso/dxso_compiler.cpp` `emitVsFinalize()` (line 3914), between
  `emitLinkerOutputSetup()` and `emitVsClipping()`; the fixed-function vertex shader
  (`src/d3d9/d3d9_fixed_function.cpp`) needs the same patch.
- **How the eye reaches the shader:** the push-constant block `D3D9RenderStateInfo` (`src/d3d9/d3d9_state.h:33`)
  has room for eye sign, separation and convergence, and can change per draw inside a command-stream lambda.
  Spec constants would recompile pipelines per eye.
- **Mechanism:** duplicate each scene draw in the five Draw lambdas of `d3d9_device.cpp`, the second time bound
  to a twin of each back-buffer-sized render target and depth buffer. Twins rather than a double-wide target,
  because post passes compute UVs over the whole texture. Vulkan multiview isn't used anywhere in this DXVK and
  would need layered targets that the translated `sampler2D` shaders can't read.
- **Present:** `dxvk_present_tap_frame` carries one image; it would carry two.

Risks: effects that rebuild position from depth with the mono projection (water, fog, imagespace) will be
slightly off; the HUD and full-screen passes must be recognised and left unshifted; the game culls for one
camera, so the field of view has to be widened and cropped; GPU scene cost roughly doubles. Not verified:
Skyrim's real render-target sizes and pass order, and MoltenVK's feature set on visionOS.

This keeps the plan's head-rotation camera hook and pose-tagged reprojection, and replaces only the per-eye
frustum hooks. It is a large change to vendored DXVK (seven areas), so it wants its own milestone and a
prototype on the Mac first: a side-by-side window is enough to judge the artefacts.

## 2. Thread scheduling on the headset (*idea*, cheap to try)

Guest threads are plain pthreads with no QoS class, and `SetThreadPriority` is a no-op
(`runtime/kernel32.c:455`). The M2 has four performance cores; the game's main thread, two busy workers and
DXVK's two threads already fill them, and nothing tells the scheduler which matter. Give the presenting thread
and DXVK's threads `QOS_CLASS_USER_INTERACTIVE` and map the game's own `SetThreadPriority` calls onto QoS
classes. Measure on the headset; the M1 result won't transfer.

## 3. Loading-time spin (*measured*, not yet addressed)

For the first 30 s of a run most CPU time is `swtch_pri`, i.e. `Sleep(0)`/`sched_yield` loops (the brief's
spin-wait). It doesn't appear once in the world on the M1, so it costs loading time, heat and battery rather
than frame rate. A short real sleep after a few consecutive yields is the usual fix.

## 4. Render scale and upscaling (*idea*)

If the headset turns out GPU-bound in immersive mode: render below the panel's angular resolution and upscale
in the compositor pass (MetalFX spatial needs no motion vectors), and use the compositor's foveation with a
rasterization rate map, as the plan already notes for M2.

## 5. Compiler-level work still open (*idea*)

ThinLTO (inlining across the 140 generated files removes spill/reload pairs at direct calls), profile-guided
optimisation, and a linker order file built from a profile to keep the hot few megabytes of the 110 MB of code
together. Each needs a 20-minute build per try on the M1.
