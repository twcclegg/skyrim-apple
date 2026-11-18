# Native Apple Vision Pro port: plan and prior art

Status: **windowed play works on an M2 Vision Pro** (visionOS 27.3): about 60 fps at 1280×720, with audio,
keyboard and saves. Mouse look stops once the trackpad pointer leaves the window, because a Shared
Space window doesn't keep pointer lock. This page records the research behind the port and the order of work;
current status and milestones are in [VISION_PRO_PLAN.md](VISION_PRO_PLAN.md).

## Direction

The README sets the order: "The Vision Pro work will establish windowed gameplay before immersive
acceptance, with separate stereo/head-tracking and input checks." The launcher already has a
`SkyrimVision` target (apple/project.yml), a flat `WindowGroup` with no immersive space, and
detects PS VR2 Sense controllers as "planned". The same author's other Vision Pro ports (below) all
follow "start in a window, then expand into immersive play" with one running game across both.

| Phase | Goal | Device check |
|---|---|---|
| 1. Engine for xros | The iPad engine, unchanged, compiled for visionOS: `python3 ipad/build.py engine --platform visionos` | `libSkyrimNative.a` is purely platform 11 (xros) |
| 2. Windowed | `SkyrimVision` links the engine and runs it in its window (the same SDL/DXVK/MoltenVK path as iPad), with a game controller | Main menu, a new game and a save load in a window on the headset |
| 3. Theater | An `ImmersiveSpace` that shows the same mono frame on a large curved screen; closing it returns to the window | Stable at the window's frame rate, no game restart |
| 4. Stereo | Per-eye rendering inside the engine from the compositor's head pose and eye transforms (`CompositorLayer`) | Correct depth and scale; comfortable head tracking |
| 5. Sense controllers | PS VR2 Sense through GameController and ARKit accessory tracking | Tracked aim and buttons |

## Phase 1: what changed for xros

- `ipad/visionos/toolchain.cmake`: `CMAKE_SYSTEM_NAME visionOS` (CMake 3.28+), xros SDK, arm64,
  visionOS 26.5 (MoltenVK's xros slice is built for it). Pinned SDL 2.30 only knows iOS and tvOS as UIKit platforms, so the toolchain
  gives it the iOS path while the SDK stays xros.
- `simulator/prepare_sdl.py --platform visionos`: SDL 2.30 has no visionOS support (its UIKit
  backend depends on `UIScreen`, which visionOS lacks). The overlay adds
  `ipad/visionos/sdl/SDL_uikit_xros.{h,m}`: a single display whose size and scale come from the
  app's window scene, and no-op status bar and orientation calls. Only the overlay copy in the
  ignored build tree changes; the fetched SDL release (`third_party/SDL2`, from
  `tools/fetch_sdl.py`) and the iOS overlay do not.
- `ipad/prepare_moltenvk.py --sdk xros`: the same MoltenVK v1.4.2 release's `MoltenVK-all.tar`
  carries a static `xros-arm64` slice (pinned to the GitHub asset digest).
- `ipad/build_ffmpeg.py --sdk xros`: the same FFmpeg 7.1.2 WMA decoders, `arm64-apple-xros26.5`.
- `ipad/build.py engine|app --platform visionos`: outputs in `ipad/build/visionos/`, the Mach-O
  platform check is 11, the app step builds the `SkyrimVision` scheme. The iOS defaults and
  paths are unchanged.

## Prior art (researched 2026-11-07)

Statuses are what each project's own README reports, not verified here.

**The same author's Vision Pro ports**, credited in this README as launcher and presentation
references:
- [titanfall2-apple](https://github.com/zschwendi/titanfall2-apple): the closest analogue, an
  x86 to arm64 recompile on Metal through DXMT with a UIKit `CAMetalLayer` host and the same
  Extended Virtual Addressing and Increased Memory Limit entitlements. Its README reports that a
  separate native visionOS build "reserved the game's memory ranges with Extended Virtual
  Addressing and run a head-tracked immersive space at 90 Hz on the headset, but has shown no game
  frames yet". It also notes the simulator cannot run the D3D layer, so device testing is needed.
- [117-vision](https://github.com/zschwendi/117-vision) (Halo CE): window, then "Expand" into
  `ImmersiveSpace` + `CompositorLayer` on the same running game. `EngineImmersive.swift` is a
  compact reference for layout fallback, foveation and render quality.
- [avp-gta](https://github.com/zschwendi/avp-gta): `visionos/IMMERSIVE-BRIDGE.md` is the most
  detailed per-eye design. Swift owns ARKit and `LayerRenderer` and publishes head pose and
  per-eye transforms and projections to a guest camera hook. The engine renders per eye; the
  compositor reprojects with depth.
- [avp-craft](https://github.com/zschwendi/avp-craft) (Minecraft): ships the same MoltenVK
  v1.4.2 xros slice, and has Sense controllers through `AccessoryTrackingProvider`.
- [dark-eco](https://github.com/zschwendi/dark-eco) (Jak): one multiplatform target with an
  optional RealityKit stereo window.

**Other visionOS ports to learn from:**
- [SHAR VR](https://github.com/edgytoast/shar-visionos): an old PC engine as a static library,
  Vulkan through MoltenVK, multiview into a two-layer eye image, then a Metal present pass with
  depth and the device anchor. Its HOW-IT-WORKS notes the headset shows nothing without both.
- [Glovebox](https://github.com/glovebox-vr/glovebox) and
  [Klepton](https://github.com/shinyquagsire23/Klepton): MoltenVK into Compositor Services on
  device; `KleptonCompositor.swift` covers foveation and layout negotiation.
- [FNVVR](https://github.com/df4l/FNVVR): VR inside DXVK's d3d9 layer for Fallout: New Vegas
  (Gamebryo, Skyrim's ancestor). Each eye is drawn by the game and submitted to a VR backend
  interface; a Compositor Services backend would be the Apple equivalent. Our recompiled engine
  can patch the camera at source level rather than by address.
- [LambdaVision](https://github.com/illixion/halflife-visionos),
  [QuakeAVP](https://github.com/Ywach404/QuakeAVP),
  [portal2-recomp](https://github.com/kanapitsas/portal2-recomp) (an AI-written static recompile
  with a D3D9-on-Metal path through [mtld3d](https://github.com/athei/mtld3d)).

Most of these were developed with AI coding agents (Claude co-author trailers or AGENTS.md files).

## Known risks

- **Memory:** the runtime reserves 4 GiB of guest address space. Apple documents Increased Memory
  Limit on visionOS 2.5+, but not Extended Virtual Addressing; titanfall2-apple reports it works
  on the headset. Phase 2's first check is that reservation (native_bridge.mm probes it).
- **SDL on visionOS** is an overlay on SDL 2.30; SDL3 has native visionOS support if the overlay
  proves too narrow.
- **Frame pacing:** the engine caps at 60 fps; visionOS windows prefer 90 Hz, and immersive
  rendering needs low-latency pacing (phase 4).
- **Simulator:** `python3 ipad/build.py engine|app --platform visionos-simulator` (Mach-O 12,
  MoltenVK's xros Simulator slice) runs the real game in the visionOS Simulator: on 2026-11-08 it
  reached the main menu in a 16:9 window at about 60 fps through DXVK, MoltenVK and the Simulator
  GPU. Launch it with `-SkyrimSimulatorAutoPlay YES` (Simulator builds only) to press Play without
  a controller. The Simulator runs on the Mac, so it proves nothing about memory entitlements,
  frame pacing on the headset or input; device checks still decide each phase.
