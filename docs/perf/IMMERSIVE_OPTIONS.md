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

## Headset GPU log lines (stage 0 of #23: measure before optimising)

Two lines report GPU cost on the headset. Neither had ever been read from a headset run when they were added.

- `immersive: compositor gpu X ms per frame, p95 Y ms, max Z ms`, every 5 s, from `ImmersiveScreen.swift`. The
  compositor's own command buffers, `gpuEndTime - gpuStartTime`. Mean is exact; p95 is read from a 0.25 ms
  histogram (rounded up to the bin edge, never above the max); the max is exact.
- `immersive cadence: N game frames over 900 refreshes; held 1: …; replaced unseen: U; game gpu latency mean X ms,
  max Y ms`, every 900 refreshes, from `ipad/immersive_bridge.mm`. The latency is the time from the game handing a
  frame to the tap (inside Present) to the first compositor refresh that finds it finished. It is an upper bound
  on the game frame's GPU time: it also includes DXVK's command thread recording and submitting the frame (the
  tap fires on the game thread as soon as the frame is queued to that thread, before its commands reach the GPU),
  the GPU working through the compositor's own queued work, and rounding up to the next refresh (about 11 ms at
  90 Hz). Frames pushed out of a full mailbox before any refresh polled them are not counted. The suffix is absent
  when no frame was observed in the window.

Read them together: if the game latency is far above the compositor's GPU time, the game's frame is the slow part,
on the GPU or on DXVK's command thread; the line alone does not say which.

## Live stereo switches (headset session stereo-1)

Live single-pass stereo is what option B above became: DXVK renders each scene draw into two image layers with
Vulkan multiview, one per eye. These are the switches that turn it on and the lines they log. All are read
once, at launch, from the environment or `Documents/SkyrimNative/launch-env.json`. Nothing changes while they are unset.

| Switch | Read by | What it does |
|---|---|---|
| `LB_STEREO_EYE_UNITS=d` | runtime, DXVK | Half the eye separation in game units (2.24 is 32 mm at 70 units per metre, also the compositor's default). **Live stereo needs it**: without it the runtime never sets eye offsets or eye matrices, so both layers get the same picture and `LB_EYE_VIEWS`/`LB_EYE_LATE` do nothing. Logs `stereo: live single-pass stereo, d <d> units`. |
| `LB_STEREO_MULTIVIEW=1` | DXVK, runtime | Single-pass stereo. Render targets of the back buffer's size, and depth-stencils at least that size, get a second layer, and passes whose attachments all have two layers draw with view mask 0b11. Each frame the runtime gives view 0 the left eye and view 1 the right eye. Also turns `LB_DEPTH_COPY` on. |
| `LB_STEREO_PRESENT=1` | DXVK | With `LB_STEREO_MULTIVIEW=1`: the back buffer gets two layers too, so the final composite and the interface render both eyes, and the present tap hands both pictures and depths to the compositor (`texture_eye2`). Without it the compositor gets only layer 0 (the left eye) and makes the right eye from it, as in mono. Logs `immersive: stereo frames: each eye draws its own picture (eye units <d>)` at the first stereo frame. |
| `LB_EYE_VIEWS=1` | compositor, runtime | With `LB_IMMERSIVE_HEAD=1` and `LB_STEREO_MULTIVIEW=1`: each eye is rendered through the headset's own eye frustum and from that eye's own position (`skyrim_stereo_set_eyes`). The frusta are clipped to the game's own frustum less `LB_EYE_MARGIN` degrees per side (default 2), or to `LB_EYE_WINDOW=<x>x<y>` (tangents). Logs `immersive: eye views on: …` once, and `immersive: eyes: tangents L … R …; raw L … R …; eye x … m, … units/m` each time the eyes change. The `raw` values are the headset's real tangents; the first pair is after clipping. Menus and lost tracking go back to the game's frustum (`immersive: eyes dropped: …`). |
| `LB_EYE_LATE=1` | runtime | With `LB_EYE_VIEWS=1`: at each frame's first main-scene draw, the eyes are turned from the game camera toward where the head points now. The turn is at most `LB_EYE_LATE_MAX` degrees each in heading and pitch (default 2, the compositor's margin), and the far plane is pushed out to match. Logs `eye views: late eyes, up to 2.0 degrees toward the head` once, then every 300 frames `eye views: late eyes: head <h>/<p> deg from the camera, applied <h>/<p>`. The compositor reports what is left to correct every 5 s: `immersive: eye views: shown <mean> deg from where the picture was rendered looking (mean), max <max>`. While it is requested but doesn't apply, every 300 frames: `eye views: late eyes requested but inactive in N of 300 frames: <reasons with counts> (largest render target last frame WxH fmt F ds D; back buffer WxH; draws last frame: <class>=<n> …)`. The reasons: no main-pass draw, no live eye matrices, no eyes from the compositor, menu open, no world camera, no head pose, camera pitch too steep, no valid frustum. |
| `LB_STEREO_HUD_DEPTH=m` | runtime | Live stereo: interface draws (pass class ui: HUD and menus) at m metres, default 2, instead of at infinity. |
| `LB_DEPTH_COPY=0/1` | runtime | The game draws the whole scene a second time into an R32F colour target only to have its depth. Instead, at that pass's first draw the main pass's depth-stencil is copied into the target (every layer) and the pass's draws are skipped. First-person depth draws still run. Default: on with `LB_STEREO_MULTIVIEW=1`, off otherwise. Logs `LB_DEPTH_COPY: the R32F depth pass is a copy of the main pass's depth`. |
| `LB_PIPE_LOG=1` | DXVK | One stderr line per graphics pipeline compiled: `pipe compiled at draw: <ms> ms` (a draw waited for it on DXVK's command thread) or `pipe compiled ahead: <ms> ms` (a state-cache worker compiled it in advance). The lines land among the frame-time lines of their frame, so long frames can be matched to compiles. An at-draw line ends with why the state cache didn't have it: `cache: none (vs in <n>, fs in <n> entries; …)` (no entry with these shaders), `cache: differs of <n> [<fields>] (…)` (entries with the same shaders, the closest differing in the named state; spec constants as `sc<i> <old>-><new>`), or `cache: exact of <n> (…)` (an identical entry not compiled yet). The parenthesis ends with `shaders <ms> ms` (since the youngest of the pipeline's shaders was created; `shaders unregistered` when one of them was never registered with the state cache), `queue <n>` (pipelines waiting for the cache's compiler threads) and `busy <n>`. With the state cache off the line ends with `cache: off`. |
| `LB_PIPE_LOG=2` | DXVK | Everything `LB_PIPE_LOG=1` logs, plus a second line after each at-draw line: `pipe miss: key <vs>.<fs>.<state>.<pass> near <key> of <n> exact|dc-only|real [<fields>]; write queued|skipped; instances <n>` (or `near none (vs in <n>, fs in <n> entries)`). Keys are hash prefixes of the shaders, the state vector and the render pass format, the same in every launch, so a key compiled at draw in one launch can be looked for in the next. `near` is the closest entry read from the state cache with the same shaders; `<fields>` lists every field that differs from it as `name old->new`, with `dc` after fields that cannot change the pipeline (what `DXVK_PIPELINE_NORMALIZE` clears); `dc-only` means only such fields differ. Also logs `pipe cache wrote: <key>` for each entry written to the cache file, `pipe cache drained: …, <ms> ms after start` when the cache's compiler queue runs empty (among the frame lines, so it shows whether the replay finished before play), `dxso shader new this run: vs|ps <hash>, <n> bytes` for shaders no earlier launch created, and `ff shader created: vs|fs <hash>` for fixed-function shaders. |
| `DXVK_PIPELINE_NORMALIZE=0` | DXVK | Turns off pipeline state normalization. By default DXVK clears the pipeline state that Vulkan ignores before it looks a pipeline up, compiles it or writes it to the state cache: blend and swizzle state of colour attachments the fragment shader doesn't write or the pass doesn't have, blend factors and ops with blending off, depth state without a depth attachment, depth compare and write with the depth test off, stencil ops with the stencil test off, and the logic op with logic ops off. Draws and cache entries that differed only there (D3D9 sets one blend state for all four render targets, so changing it also changes the three a shader doesn't write) then share one pipeline. Entries written without it are normalized when they are replayed. |
| `DXVK_SHADER_BYTECODE_CACHE=0` | DXVK | Turns off the shader bytecode cache. By default DXVK keeps the bytecode of every shader the game creates in `<exe>.dxso-cache` next to the state cache (the saves folder) and creates those shaders on a background thread right after the device on the next launch, so the state cache compiles their pipelines during the menus and loading instead of at the first draw (the game creates most shaders just before it uses them). Every shader any earlier launch created is created, so the compiler threads compile every pipeline in the state cache file on every launch, not only what the session uses, and neither file is ever pruned: the startup work and the memory for compiled pipelines grow with play history. Logs `D3D9: Bytecode cache: creating <n> shaders of earlier runs`, `… created <n> shaders in <ms> ms` and, once the compiler threads go idle, `… state cache compiler threads idle <ms> ms after shader creation started, <n> graphics pipelines compiled meanwhile` (the count includes pipelines compiled at draw in that time) in the DXVK log. A damaged or truncated file is rewritten, keeping the shaders before the damage. The file holds the game's own shader code: keep it out of git (`.gitignore` covers `*.dxso-cache` and `*.dxvk-cache`) and out of anything shipped. |

Related: `LB_MAX_FPS` (visionOS default 45: every second 90 Hz refresh; logged as `present pacing: at most 45 fps
(LB_MAX_FPS)`), and `LB_WINDOW` (render size, also written into `SkyrimPrefs.ini`: `prefs: render size WxH (LB_WINDOW)
in <path>`). `LB_FOV` sets the game's field of view at its 4:3 reference; with `LB_EYE_VIEWS=1` and `LB_FOV` unset (or `LB_FOV=eyes`) it follows the eyes instead: the compositor asks for a frustum holding both eyes' whole frusta plus its margin, and the runtime sets the field of view from that and the back buffer's aspect (`eye views: game field of view <deg> degrees (4:3 reference) to cover the eyes: tangents <x> x <y>, back buffer WxH`). A number in `LB_FOV` always wins. The vertical field stays the same at any
aspect, and the horizontal grows with the window's width.
