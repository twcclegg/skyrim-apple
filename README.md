# Skyrim for iOS and iPadOS

The original **2011 Windows version of Skyrim**, running locally on Apple Silicon through an ahead-of-time ARM64 recompilation. **No guest CPU JIT, debugger attachment, Windows installation, or PC streaming is needed to play.**

**Working on a physical iPad Pro M5.** The November 2, 2026 build reached the PC main menu and the opening cart ride, with rendered 3D scenery, audio, and physical controls. This repository publishes the source and build tools; it does not include a ready-to-install game or any Skyrim files.

![Skyrim running natively on iPad Pro](docs/media/ipad-opening-cart.png)

*An actual GPU frame from the physical iPad build, not a simulator or streamed PC session.*

## Platform status

| Platform | Current status |
| --- | --- |
| **iPadOS** | **Physical iPad Pro M5 gameplay verified**, iPadOS 27.0.1, app 1.2 (3). This is the tested build described here. |
| **iOS / iPhone** | The same ARM64 iOS target includes iPhone. Native gameplay on a physical iPhone has **not** been verified. |
| **visionOS / Apple Vision Pro** | Native windowed and immersive implementations are in development. The existing launcher alone cannot play the game. Neither mode is a verified Vision Pro release yet. |
| macOS / iPad Simulator | Earlier native prototypes have rendered the game and exercised save/load. They are development references; this publication focuses on the physical iOS/iPadOS target. |

The iOS target requests iOS/iPadOS 17 or later. That deployment setting is not a claim that older OS versions, older iPads, or low-memory phones work. The tested device is the M5 iPad Pro.

## What you need

- Your own **The Elder Scrolls V: Skyrim (2011)** Windows installation, final Steam version **1.9.32.0.8**. [Original Skyrim on Steam](https://store.steampowered.com/app/72850/).
- **Special Edition, Anniversary Edition, and Skyrim VR are different executables and are not supported by this runtime.**
- An Apple Silicon Mac with Xcode and the build tools described in [Building and installing](docs/BUILDING.md).
- Your own Apple development signing setup. The native app needs provisioning that grants Apple's extended virtual addressing and increased memory limit capabilities. These are memory capabilities, not JIT permissions.
- A **DualSense controller, or a physical keyboard and mouse**. The launcher checks for supported controls before Play. It pauses the game when the required controls disconnect; reconnecting does not resume automatically. There are no touch gameplay controls.
- Approximately **6.1 GB for the tested base game files**, plus space for the app, caches, and saves. Keep additional free space for installation and copying.

## Setup

1. Follow [Building and installing](docs/BUILDING.md) to prepare your owned Windows files, generate the private native code, build the **runtime-enabled iOS app**, and sign it for your device. Building the SwiftUI launcher by itself does not install a game runtime.
2. Copy the prepared **`Skyrim` folder** into the installed app's Documents using Finder file sharing or Files. The app expects the layout below; selecting a folder elsewhere does not replace this import.
3. Connect DualSense, or both keyboard and mouse. Open Skyrim and let it verify the files.
4. When it says **Ready to play**, select **Play**.

```text
Skyrim app / Documents /
└── Skyrim /
    ├── TESV.exe
    ├── TESV_unpacked.exe
    ├── binkw32.dll
    ├── d3dx9_42.dll
    ├── X3DAudio1_7.dll
    └── Data /
        ├── Skyrim.esm
        ├── Update.esm
        └── …the complete original game's Data files
```

Executables, libraries, and the essential master files must match the inputs used for your local recompilation. The build tools verify this binding. The DirectX libraries and executable preparation are covered in the build guide; they are not included here.

## Launcher, saves, and settings

The single-title launcher uses original smoke-gray artwork with white and gold accents, separate settings, and an input gate, inspired by [Dark Eco](https://github.com/zschwendi/dark-eco). The app icon is original artwork; it does not use Skyrim's logo or extracted game art.

Native saves are kept in the app's Application Support directory under `Skyrim Jitless/Saves`. The launcher's Saved Games view uses the same location. **Back up your saves before uninstalling the app.**

Save sync is opt-in and requires your own correctly provisioned iCloud container. Local saves do not require an account. **Cross-device iCloud save sync has not been accepted on the native iPad build.** See the build guide before choosing a Cloud configuration.

## What has been verified

On the physical iPad Pro M5, the runtime-enabled app launched normally without a debugger, verified the owned game files, rendered the menu and opening cart ride through at least **8,700 frames**, and produced nonzero audio output. DualSense, keyboard, and mouse were detected, and pointer capture was active. A launcher pause and resume were observed. Subsequent play identified camera-movement stutter with the DualSense right stick; that remains under investigation.

The full **4 GiB guest address range** was inspected: 64 KiB inaccessible, the rest read/write, and **zero executable guest-memory bytes**. The app has no JIT entitlement. Graphics shader compilation still occurs normally through DXVK, MoltenVK, and Metal.

This is initial gameplay acceptance, not a full-game or performance certification. The inspected binary was version **1.2 (3)**, SHA-256 `c301eb579d6a745d154b88fce78e58e73e87b4a6eba4d998e8b3760ab35f8d46`. Public setup tooling can evolve separately from that tested binary.

## Known limits

- **Camera turns can stutter with the DualSense right stick**, even when action otherwise appears smooth. Frame pacing and input delivery are being investigated. Smooth **60 fps is the target, not a verified guarantee**.
- Simultaneous keyboard and controller gameplay is requested and under investigation; detecting both devices does not yet establish mixed-input support.
- DualSense haptics and a PlayStation/Xbox button-prompt setting are in development and are not yet accepted in this baseline.
- In-game save/reload, cross-device sync, a full controller-disconnect/background test matrix, and prolonged thermal/performance testing remain to be completed on the physical iPad.
- The observed game rendered at **1210×834**, rather than the display's full pixel resolution. First-run shader compilation can affect smoothness; no steady-state frame-rate benchmark is claimed.
- DLC, mods, Skyrim Script Extender, and alternative executables are untested.
- iPhone and native Vision Pro gameplay remain unverified. The Vision Pro work will establish windowed gameplay before immersive acceptance, with separate stereo/head-tracking and input checks.

## Troubleshooting

- **“The Skyrim runtime isn't installed”** means you built the launcher-only target. Use the native iOS build workflow in the build guide.
- **Files do not match:** use the exact original Windows files you generated the native code from, and keep the complete `Data` folder together.
- **Controls required:** connect DualSense or both a physical keyboard and mouse. Other gamepads are not accepted by the current launcher policy.
- **The 4 GiB reservation is refused:** inspect the app's actual signed entitlements and embedded profile. Adding a key to an entitlements file does not grant the capability.
- Diagnostics are in `Documents/SkyrimNative/status.json` and `Documents/SkyrimLogs/`. Remove personal paths and account/device identifiers before attaching logs to an issue. Do not upload game files, generated source, saves, or signing credentials.

Please [open an issue](https://github.com/twcclegg/skyrim-apple/issues) with the device, OS, source revision, and steps to reproduce. Feature requests and reports from legitimate owned installations are welcome.

## Credits and source

The original recompiler and Windows runtime come from **[ttom795/Skyrim_Android](https://github.com/ttom795/Skyrim_Android)**. This is static recompilation, not a Skyrim decompilation. Apple work builds on that project with Darwin support, graphics/audio adaptations, native platform integration, and the shared launcher. Required upstream SDL2 and DXVK sources retain their notices under `android/app/jni/`; that directory name is historical.

Apple launcher and presentation references include [Dark Eco](https://github.com/zschwendi/dark-eco), [117 / Halo CE](https://github.com/zschwendi/117-vision), [Titanfall 2 Apple](https://github.com/zschwendi/titanfall2-apple), and [AVPcraft](https://github.com/zschwendi/avp-craft). See [Credits and licensing](NOTICE.md).

No game executables, DLLs, master files, archives, private generated game code, saves, provisioning profiles, or compiled game apps are distributed in this repository. Build from your own legally obtained copy. Bethesda and the other respective owners retain their names and game content; this project is unofficial and not endorsed by them.
