# Build and install the native iOS / iPadOS app

This builds the game runtime **and** the launcher. The tested device is an iPad Pro M5. iPhone is included in the target but has not passed physical gameplay acceptance. There is no downloadable game IPA in this source repository.

## Mac prerequisites

Use an Apple Silicon Mac, Xcode with the iOS SDK, Python 3.12 or later, CMake 3.24 or later, Ninja, XcodeGen, and `cabextract`. The accepted device build used Xcode 27.2. For example, with Homebrew already installed:

```sh
brew install python cmake ninja xcodegen cabextract
git clone --recurse-submodules https://github.com/twcclegg/skyrim-apple.git
cd skyrim-apple
python3 -m venv macos/.venv
macos/.venv/bin/python -m pip install -r macos/requirements.txt
```

Sign Xcode into your own Apple Developer Program team. You need an App ID and provisioning that grants both [Extended Virtual Addressing](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.extended-virtual-addressing) and [Increased Memory Limit](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit). The game reserves 4 GiB of data address space. These normal Apple capabilities do not enable a guest CPU JIT. A free provisioning profile may not grant the capabilities this app requires.

Allow plenty of build space: the game is about 6.1 GB, private generated C is hundreds of megabytes, and dependencies, staging, and compiler products need additional space. Generation and the first build are substantial jobs; cached local timings are not a clean-clone completion estimate. Compiler parallelism is capped at two jobs.

## Obtain your own original Windows game

You need the original **Steam app 72850**, Skyrim version **1.9.32.0.8**, including its complete `Data` folder. Special Edition, Anniversary Edition and Skyrim VR are not compatible.

If Steam on your Mac does not offer the Windows installation, sign into your owning Steam account, open `steam://open/console`, and run these commands separately, waiting for each to finish:

```text
download_depot 72850 72851
download_depot 72850 72853
download_depot 72850 72852
```

They provide the base game, English content and the executable depot. Use the actual completed depot paths Steam prints. They are typically below `steamapps/content/app_72850/` within the Steam installation. Ownership and Steam availability are required; this project does not download or distribute the game for you.

An existing complete original Windows installation can be used instead. The input checker requires the original PE32 executables and core game data, not a folder from Skyrim SE/AE/VR.

## Prepare private inputs and generated code

The preparation tools preserve the supplied files. They create a private staging view, obtain the required 32-bit DirectX libraries from Microsoft's pinned redistributable, prepare the supported executable, and generate native-buildable C bound to your exact files.

If you do not already have the supported `TESV_unpacked.exe`, the executable preparation needs the pinned Steamless source and .NET SDK described in [Executable preparation](../macos/executable/README.md):

```sh
git clone --depth 1 --branch v3.1.0.5 https://github.com/atom0s/Steamless.git macos/deps/steamless/source
```

Install the required ARM64 .NET SDK before continuing. The prerequisite check reports missing tools rather than silently installing them. See the linked executable guide for the pinned SDK and build details.

For completed Steam depots, substitute their actual paths:

```sh
python3 ipad/build.py prepare --depots \
  /path/to/depot_72851 /path/to/depot_72853 /path/to/depot_72852
```

Or use an existing complete original installation:

```sh
python3 ipad/build.py prepare --game-dir /path/to/Skyrim
```

Preparation requires `TESV.exe`, `binkw32.dll`, and the complete `Data` files. It prepares the supported `TESV_unpacked.exe`, `d3dx9_42.dll`, and `X3DAudio1_7.dll` as needed. The private `gen/input-manifest.json` records all module/data hashes, generated source hashes and recompiler hashes. Keep the original inputs available: later builds verify that binding again.

Do not commit, upload, or share `gen/`, `analysis/`, staged Windows modules, or game assets. `--game-dir` on `check`, `engine`, and `export` must point to a **complete prepared folder** containing all five modules and the complete `Data` folder, such as an earlier exported folder. It cannot point directly to an unprepared depot or ordinary installation. If the original installation or depots moved, move the old `gen/` **out of the checkout** (or into ignored `macos/build/gen-previous`) and run `prepare` again. `--gen-dir` or `SKYRIM_GEN_DIR` selects an existing generated-source directory for verification/build/export; `prepare` writes the new `gen/` in the checkout. Existing generated output is preserved rather than silently regenerated.

## Build the device runtime and app

```sh
python3 ipad/build.py check
python3 ipad/build.py engine --jobs 2
python3 ipad/build.py app --signed \
  --team YOURTEAMID --bundle-id com.yourname.skyrim --jobs 2
```

`engine` obtains checksum-pinned MoltenVK 1.4.2 and FFmpeg 7.1.2 source/dependencies from their official URLs. Existing verified archives can be supplied with `--moltenvk-archive` and `--ffmpeg-archive`; nothing is read from the original author's Mac. Every linked archive must be a real iPhoneOS device slice. An iOS Simulator archive is refused.

The default app build uses the **local** configuration with the memory entitlements and no iCloud requirement. Automatic signing can request provisioning updates for your App ID. The app build checks the actual signed entitlements. Verify that the embedded provisioning profile grants those capabilities too; if capabilities are missing, configure them in your developer account and build again. Signing success alone is not physical gameplay acceptance.

The local signed app is written to:

```text
ipad/build/DerivedData/Build/Products/Release-iphoneos/Skyrim.app
```

Install it on your trusted, Developer Mode-enabled iPad using Xcode's Device Hub, or use its exact device identifier:

```sh
xcrun devicectl device install app --device YOUR_IPAD_UDID \
  ipad/build/DerivedData/Build/Products/Release-iphoneos/Skyrim.app
```

Use your own team and app identifier consistently for subsequent updates. Changing the identifier creates another app with a separate container. Do not uninstall an existing copy to update it: back up its saves and install the new signed build in place.

## Import the game files and play

```sh
python3 ipad/build.py export
```

This makes a regular, link-free copy of the verified game folder at `ipad/build/Skyrim`. The app enables file sharing: the expected manual import is to copy that whole **Skyrim** folder through Finder → your iPad → Files → Skyrim, or Files → On My iPad → Skyrim. These manual import steps have not yet been exercised from a fresh install; the accepted test installation used Apple's developer file-transfer tools. The required result is the app's `Documents/Skyrim/TESV.exe`, `Documents/Skyrim/Data/…`, and the other modules shown in the root README. Keep at least several gigabytes free beyond the files being imported.

Open the app, connect DualSense or keyboard and mouse, let the file check finish, and select Play. The native build uses its in-app folder directly. A folder bookmark selected elsewhere does not replace the import.

## Optional iCloud configuration

Local play and local saves need no iCloud account. For opt-in sync, create and provision your own iCloud container, then add `--cloud --icloud-container iCloud.com.yourname.skyrim` to the signed app command. The Cloud product is under `Cloud-iphoneos` instead of `Release-iphoneos`. The build uses your container in the app metadata and signed entitlements.

Cross-device sync of actual native iPad saves is still unverified. Keep a separate backup and test a save transfer on your own two provisioned devices before relying on it. Do not use the original author's private container or share signing keys and provisioning profiles.

## Diagnostics and limits

`Documents/SkyrimNative/status.json` distinguishes missing runtime, file validation, and input-gate problems. Each Play creates a log in `Documents/SkyrimLogs/`. Save data lives in Application Support under `Skyrim Jitless/Saves`. The runtime records its build/input identity and a scan of guest-memory protections.

Camera movement with the DualSense right stick can stutter in the accepted baseline. Smooth 60 fps, concurrent keyboard/controller input, DualSense haptics and selectable button prompts are active development work, not accepted features in this source snapshot. Vision Pro windowed, immersive and Sense work has separate acceptance gates.

The public setup checks can be run without game files or a device:

```sh
python3 ipad/tests/test_public_build.py
```

These checks do not replace your device's import, launch, input, audio, save and lifecycle testing.
