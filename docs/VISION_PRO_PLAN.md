# Vision Pro plan (working draft)

Native Apple Vision Pro support for the AOT-recompiled 2011 Skyrim runtime, developed in
`twcclegg/skyrim-apple`. Upstream (`zschwendi/skyrim-apple`) is inactive, and nothing further is expected
from it. The order is **windowed play → immersive (stereo + head tracking) → PSVR2 Sense**, each gated
on its own device evidence.

**Status:** windowed play (M1) works on the M2 Vision Pro (visionOS 27.3): about 60 fps at 1280×720
on the Low preset, with audio, keyboard and saves. The trackpad can't keep the pointer in
a Shared Space window, so mouse look stops at the window's edge.

3D immersive with head tracking (M2 and the M3a path: the game's depth on a world-locked screen, the head
turning the game camera) mostly works and is borderline playable on the M2 at the Low preset and 1280×720
in quiet scenes; busy scenes such as combat drop frames. Open problems: the trackpad still loses the pointer,
and the headset powered off twice on battery (never on AC). See [HEAD_TRACKING.md](HEAD_TRACKING.md).

## How the existing iPad runtime works (what we inherit)

- `gen/` (private, per-user) is the x86 game lifted to C; compiled for arm64 with the runtime.
  Nothing in it is iPad-specific, so the same `gen/` should compile for `xros` unchanged.
- `runtime/` emulates Win32/D3D9/XAudio/DInput. D3D9 calls go through
  `runtime/d3d9_bridge*.cpp` into DXVK (d3d9 → Vulkan) → MoltenVK → Metal.
- `runtime/platform_sdl.c` owns the SDL2 window (Vulkan surface for DXVK), event pump,
  pause/suspend/quit (`LB_IOS_DEVICE`), and `platform_present_hook()` runs once per Present.
- `ipad/native_bridge.mm` is the C API the SwiftUI launcher calls (`skyrim_native_run` etc.).
  The game runs **on the main thread** inside the launcher; SDL attaches its `UIWindow` to the
  launcher's scene (`simulator/prepare_sdl.py` overlay on pinned SDL 2.30.9).
- `ipad/build.py` + `ipad/CMakeLists.txt` build one merged static `libSkyrimNative.a`
  (gen + runtime + SDL + DXVK + MoltenVK + FFmpeg), then `xcodebuild` links it into the
  `SkyrimIOS` target via `SKYRIM_NATIVE_LDFLAGS` / `SKYRIM_NATIVE_RUNTIME`.
- The 4 GiB guest reservation needs `extended-virtual-addressing` + `increased-memory-limit`.
- `apple/project.yml` already has a `SkyrimVision` launcher target (launcher only, no runtime).
- Useful precedents in-tree:
  - `runtime/hud_screen.cpp` redirects `IMenu::Render` vtable slots and swaps render targets to
    draw HUD/menus into a separate image — the exact tool for floating menus in immersive.
  - DXVK's `wsi_native_surface.{h,cpp}` lets us hand DXVK an arbitrary native surface by HWND.
  - `d3d9_bridge.cpp` `B_IDirect3DDevice9_Present` is the single choke point for frame handoff.

## Blockers found

| # | Blocker | Plan |
|---|---|---|
| B1 | Vendored **SDL 2.30.9 has no visionOS support** (uses `UIScreen`, unavailable on xros). | Extend the `prepare_sdl.py` overlay with a visionOS adaptation (backport SDL3's `SDL_PLATFORM_VISIONOS` handling: no `UIScreen`, scene-sized window, fixed display mode, no orientation). Keeps runtime input code identical. |
| B2 | MoltenVK fetcher uses `MoltenVK-ios.tar` (no xros slice). | `MoltenVK-all.tar` v1.4.2 **does** contain `static/MoltenVK.xcframework/xros-arm64` (+ xros simulator). Add a pinned xros extraction. |
| B3 | FFmpeg built for `iphoneos` only. | Same script, `arm64-apple-xros` triple + `xros` SDK. |
| B4 | Build tooling hard-codes iphoneos (`CMakeLists.txt` SDK check, `platforms()` LC_BUILD_VERSION check expects 2; xros device is 11). | Parameterise `ipad/build.py` by platform (`--platform visionos`), new `vision/toolchain.cmake`. |
| B5 | **Memory entitlements on visionOS** are the #1 risk: without the 4 GiB reservation nothing runs. Apple's docs don't list visionOS for EVA, but avp-craft, mc-avp-stereo and the maintainer's Titanfall work all report it granted and working on the headset. | Day-1 on-device probe (signed `mmap(4 GiB)` app). Do **not** add `increased-debugging-memory-limit` (iOS-only; one bad key fails signing and drops the memory keys with it). |
| B7 | visionOS watchdog kills apps whose main thread blocks (avp-craft). The iPad build runs the game on the main thread and relies on SDL turning the run loop. | Keep that for M1 (it's what works on iPad) but measure; fallback is Halo's model: engine on its own high-QoS pthread, UI/SDL events on main. |
| B8 | Deployment target is visionOS 2.0; Sense, `AccessoryTrackingProvider`, `UIHostingSceneDelegate` immersive need 26. | Target visionOS 26 with the Xcode 27 SDK, like 117-vision and avp-craft. |
| B6 | Launcher gates runtime to `os(iOS)` (`NativeRuntime.swift`, `LauncherModel.Runtime`). | Add `os(visionOS)` paths, `Native-visionOS-*.entitlements`, link flags on `SkyrimVision`. |

## Target hardware: Vision Pro M2

Our headset is the original M2 Vision Pro (8-core CPU, 10-core GPU, 16 GB). The only verified device
is an iPad Pro M5, which rendered at 1210×834 with no steady 60 fps claim — so the M2 headset will
have noticeably less GPU, and the system compositor and passthrough also take part of it.

- **Measure before designing stereo.** M1 must record steady-state frame times (in town, in the open,
  in combat) at a few resolutions and presets. Those numbers decide M3.
- **Get an early estimate off the headset:** run the existing macOS build on an M2 Mac at the same
  resolution and settings before the visionOS port is up.
- **Default settings:** start windowed at about 1280×720 on the Low/Medium preset with reduced
  shadows, and expose a render-scale knob (`LB_*`).
- **Shader compilation:** first-run DXVK shader compilation stutters more on fewer cores. Keep the
  state cache, and consider a warm-up pass.
- **Immersive GPU budget:** the compositor pass must be cheap (one textured quad, foveated). Halo
  dropped to 45 Hz when compositor GPU time went over 11.1 ms.
- **Memory:** record `os_proc_available_memory()` at launch. The headset's limit may be below the
  iPad's even with `increased-memory-limit`.
- **Thermals:** run 30-minute soak tests. Sustained clocks on the headset are the real limit.

## Milestones

### M0 — Bring-up (no game yet)
- Workflow: `twcclegg/skyrim-apple`, a branch and PR per milestone, merged there.
- Build our own `gen/` from an owned 1.9.32.0.8 install; build + run the iPad target once as a
  baseline so we know the toolchain is healthy before touching it.
- Entitlement probe on the headset (B5). Record device model (M2 vs M5 AVP) and visionOS version.

### M1 — Windowed play
1. SDL visionOS overlay (B1); compile-only first, then window creation in a SwiftUI WindowGroup.
2. Deps for xros (B2, B3); `ipad/build.py --platform visionos engine/app`.
3. Launcher: `SkyrimVision` links the engine; `NativeRuntime` on visionOS; status.json/logs same.
4. Game window: separate 16:9 window (resizable), back buffer sized from window points × scale;
   `view_rect()` letterboxing already handles aspect.
5. Input: DualSense via GameController (same XInput path), GCKeyboard/GCMouse.
   visionOS turns controller input into pinches unless the game view opts in: add a
   `GCEventInteraction` (`handledEventTypes = .gamepad`) to SDL's view in the SDL overlay (117-vision
   does this on its MTKView); SDL's MFi joystick path then gets input exclusively. SDL 2.30.9's
   GCKeyboard/GCMouse path compiles for xros (the xros 27.2 SDK defines
   `__IPHONE_OS_VERSION_MAX_ALLOWED` = 17.0, observed). Device unknowns: pointer lock in a Shared Space
   window, BT mouse as GCMouse. No-controller fallback: pinch → click for menus, and the existing
   virtual-pad overlay (`runtime/touch.cpp`, `LB_TOUCH=1`) for play.
6. Lifecycle: scene phase / occlusion → `platform_native_set_suspended`; pause → launcher.
7. Acceptance: menu + cart ride + 10 min play, audio, controls, pause/resume, save/load, on device,
   with frame-time logs. Check thermals/perf on the AVP GPU (M2 is much weaker than iPad M5).

### M2 — Immersive "cinema" (mono, low risk)
- `ImmersiveSpace { CompositorLayer }` opened from the running game ("Expand"); closing returns
  to the same running game. Copy 117-vision's single-owner state machine: one runtime, one
  serialized presentation owner; Expand = open space → wait for first compositor frame → dismiss
  window; Return = open window → wait for `.foregroundActive` → dismiss space; scene generations
  reject stale `onDisappear`.
- Compositor thread at display rate (90 Hz), independent of the game's ≤60 fps; it re-presents
  the newest finished game image. Frame loop per avp-craft/117: `queryNextFrame` →
  `predictTiming` → `wait(optimalInputTime)` → `queryDrawables` → `startSubmission` → encode →
  `encodePresent` → `endSubmission` (never end an abandoned frame). Head pose at
  `presentationTime`; `drawable.deviceAnchor` = display pose.
- Handoff at `Present` (static review of dxvk-native 1.9.2 + MoltenVK 1.4.2, untested):
  there is no `ID3D9VkInteropDevice` in 1.9.2, but we build our own DXVK (the fork
  `twcclegg/dxvk-native`, branch `skyrim-apple`), so tap it directly there. Add an
  `extern "C"` present-tap in `d3d9_swapchain.cpp` `D3D9SwapChainEx::PresentImage`, inside the
  `SubmitPresent` CS lambda after `submitCommandList`, passing VkDevice/VkQueue and the D3D9 back
  buffer's VkImage (`m_backBuffers[0]->GetCommonTexture()->GetImage()->handle()`), extent and format.
  Enable `VK_EXT_metal_objects` in `DxvkAdapter::createDevice`'s `__APPLE__` block and get the
  `MTLTexture` via `vkExportMetalObjectsEXT` (proc address; 1.9.2's loader lacks it). Export the back
  buffer, **never** swapchain images (MoltenVK returns the current `CAMetalDrawable`, which changes and
  can block). Sync: timeline semaphore → exported `MTLSharedEvent`, compositor `encodeWait` before its
  blit into a 3–6 slot ring; publish with the pose it was rendered at (gta5-vision `sendToVR`). The
  bridge's `Present` and `platform_present_hook` run before DXVK's async present: bookkeeping only.
- Layer: `bgra8Unorm_srgb` + `depth32Float`; pipeline built against the *granted* colour format.
  **Write the quad's geometric depth** (reverse-Z, `.greater`, writes on, clear to 0): visionOS
  reprojects with drawable depth, and a quad without depth writes is invisible (avp-craft, on device).
  Foveation off for M2; turn it on only together with `rasterizationRateMap` on the pass. Validate
  config through `LayerRenderer.Properties` (throws instead of killing the scene).
- Prerequisites (design review, untested): `UIApplicationSupportsMultipleScenes = true` (an
  ImmersiveSpace is a second scene), so SDL must bind **explicitly** to the game window's scene (the
  overlay's attach loop and the shim's `SDLXR_ActiveScene` currently match any scene). Check that
  opening a Full Space doesn't fire `UIApplicationWillResignActive` (which suspends the game).
- Slot handoff: the tap callback fills a one-entry mailbox (replacing = release); the compositor takes
  it only when `skyrim_present_tap_ready` says the GPU is done, else re-presents the current slot.
  Retired slots are released from the completion handler of the first command buffer that no longer
  samples them. Sample through a `.bgra8Unorm_srgb` texture view (the tap image is UNORM holding sRGB
  values); force alpha 1; no vertical flip.
- Risk: while immersive, DXVK still presents to the (hidden) SDL window, and `nextDrawable` can stall
  ~1 s. Plan: a tap-only mode that skips WSI acquire/present while the immersive layer runs, pacing
  with the runtime's `pace_present`.
- Content: a world-locked 16:9 screen (~2.8 m wide at 3 m, placed at head yaw, recenter on request); menus/settings via `.utilityPanel` windows.

### M3 — Immersive stereo + head tracking
- **The approach depends on M2 performance.** Alternating eyes halves each eye's frame rate: a 60 fps
  game gives 30 fps per eye, which is fine, but 40 fps gives 20, which is likely uncomfortable. If M1
  shows the M2 can't hold about 50–60 fps at a usable resolution, ship **M3a, mono with head-look**
  first: the head pose turns the render camera, with rotational timewarp, and no stereo. Make
  alternate-eye stereo an opt-in setting.
- Alternate-eye rendering (gta5-vision / R.E.A.L. style): hook the main scene camera update in
  the generated code (NiCamera world→camera for the player camera), apply head pose + eye offset
  for render only, **restore after** so game logic/aim/collision never see it.
- Camera hook candidates (static reads of TESV_unpacked.exe d97eb0cc… + gen/, **not runtime-verified**;
  full notes kept privately with the RE work):
  - Main world NiCamera W: `*(*(0x1B2E8BC)+0xB8)`. NiCamera: world rotation +0x54..+0x74, translation
    +0x78..+0x80, world→**clip** matrix (view × projection) +0xA8..+0xE4, frustum l/r/t/b tangents
    +0xE8..+0xF4, near/far +0xF8/+0xFC, ortho +0x100. 0xAB8640 builds the matrix from the transform
    **and** the frustum, so write the frustum first, then call it. Projection is standard D3D Z [0,1].
  - PlayerCamera singleton 0x12E7288; worldFOV +0xAC, firstPersonFOV +0xB0. FOV→frustum at 0xB17960
    (horizontal FOV at a 4:3 reference, inferred).
  - Frame order in 0x69F950: PlayerCamera update (0x73F240) → world render 0x69BDF0 (frustum via
    0x6975D0, then world passes). The 1st-person camera (0x697FC0 / 0x698B30) re-runs 0xB17960 on W
    with firstPersonFOV, copies W's frustum and **rotation** only, takes its translation from
    PlayerCamera + a viewmodel origin offset, then restores W's frustum.
  - So a one-shot frustum write is lost. Hook instead: post-hook 0xB17960 (or 0xAB7710) to substitute
    the per-eye frustum on every call; apply head rotation + translation + eye offset to W after
    0x6975D0 returns (0x69B540); post-hook 0x697FC0/0x698B30 to add translation + eye offset to the
    1st-person camera; restore after 0x69BDF0 returns (0x69FA1D). In stereo, 1st-person FOV = headset FOV.
  - Confirm first at runtime: one-frame entry/exit log of 0x73F240, 0x6975D0, 0x697FC0, 0xB17960,
    0x69BDF0, and read W's frustum at 0x69B540 with `fDefaultWorldFOV=90` to settle the FOV axis.
  - Per-pixel depth for later (M3+): the game creates a readable INTZ depth texture (RESZ path,
    supported by DXVK 1.9.2); export it before the UI passes, linearise, mask UI.
- Tag each frame with its pose (ring buffer), draw per-eye quads at the rendered pose with a wider
  game FOV than the display → rotational reprojection covers 60→90 Hz and eye alternation.
- Skyrim is Z-up; ~70 game units per metre for head translation. Yaw-only recenter on start and on
  Crown press.
- Disable depth of field / imagespace blur in stereo (INI) to avoid eye-swap smear.
- Compositor draws each eye's image as a quad at the pose/FOV it was rendered with, viewed from the
  current head pose (avp-craft `kl_reproject.h`, rotation-only timewarp; no translation without depth).
- HUD/menus to a floating panel via the `hud_screen.cpp` render-target redirect technique; lazy
  head-follow (117-vision `EngineImmersiveHUDPose`: small deadband, ≤10° lag) or avp-craft's
  head-locked 2 m / −15° placement.
- Comfort: off / smooth / 30°·45° snap turn (no auto-repeat), recenter on request.
- Later upgrade if AER halves perceived rate too much: 117-vision's "centre-only stereo" (render
  only the forward view twice) — but Skyrim can't easily render a scene twice per frame, so AER first.

### M4 — PSVR2 Sense (deferred: no Sense controllers to test with)
- `GCProductCategorySpatialController` (visionOS 26+; launcher already detects it) →
  `Accessory(device:)` → `AccessoryTrackingProvider` on its **own** `ARKitSession`
  (a denied permission must not kill rendering); `predictAnchor(for:at:)` per frame.
  Info.plist: `NSWorldSensingUsageDescription`, `NSAccessoryTrackingUsageDescription`.
- Buttons/sticks → XInput first; tracked aiming later. Cancel system trigger-pinch while active.

## Validation conventions
- Label every claim tested / untested / observed; Mac or simulator ≠ headset.
- Per-build evidence: device, OS, build hash, frame counts, audio, logs (as the iPad README does).
- Env-var knobs (`LB_*`) with an A/B for each risky setting; first-frame checkpoint trace.
- Pure-logic fixtures for state machines (window↔immersive ownership, HUD pose, recenter).
- Never commit `gen/`, game files, provisioning, or team IDs; use `--team/--bundle-id` overrides.
