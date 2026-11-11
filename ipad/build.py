#!/usr/bin/env python3
"""Build the native Skyrim engine for physical iPad (iphoneos arm64) and the launcher app that runs it.

You need your own original Skyrim (Steam app 72850, version 1.9.32.0.8), an Apple Silicon Mac with
Xcode, and a paid Apple Developer Program team to sign. Only an iPad Pro (M5) has been tested.
From the checkout root:

  python3 ipad/build.py prepare --depots DEPOT [DEPOT ...]
                         your Steam depots -> private gen/ (or --game-dir for an installation)
  python3 ipad/build.py check              tools and the generated code; nothing is built
  python3 ipad/build.py engine             pinned device dependencies, then the engine:
                                           ipad/build/out/libSkyrimNative.a with a receipt
  python3 ipad/build.py app --signed --team TEAM --bundle-id ID [--cloud]
                                           the launcher (apple/, SkyrimIOS) linked with it
  python3 ipad/build.py export             a link-free copy of your verified game files
                                           (ipad/build/Skyrim) for the app's Documents/Skyrim

engine and app take --platform visionos for Apple Vision Pro (xros arm64, SkyrimVision): outputs go
to ipad/build/visionos/ instead. Vision Pro support is in development and not a device result yet.

check, engine and export read --gen-dir (SKYRIM_GEN_DIR; default gen/) and accept it only if
macos/inputs.py verify-generated binds it to exact original inputs that are still present: the
folder its manifest records, or --game-dir (SKYRIM_GAME_DIR). They never regenerate it. At most
two compiler jobs. Outputs stay in ignored folders (gen/, ipad/build/, ipad/deps/, macos/build/).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
IPAD = ROOT / "ipad"
BUILD = IPAD / "build/engine"
OUT = IPAD / "build/out"
# Per device platform: SDK, Mach-O LC_BUILD_VERSION platform, build and output folders, toolchain,
# dependency prefixes and the launcher scheme. iOS keeps the original paths.
PLATFORMS = {
    "ios": {"sdk": "iphoneos", "macho": "2", "vtool": "platform IOS", "label": "physical iPad", "build": BUILD,
            "out": OUT, "toolchain": IPAD / "toolchain.cmake", "moltenvk": IPAD / "deps/MoltenVK-ios-v1.4.2",
            "ffmpeg": IPAD / "deps/ffmpeg-iphoneos", "scheme": "SkyrimIOS", "destination": "generic/platform=iOS",
            "entitlements": "Native-iOS-{variant}.entitlements", "log_prefix": ""},
    "visionos": {"sdk": "xros", "macho": "11", "vtool": "platform VISIONOS", "label": "Apple Vision Pro",
                 "build": IPAD / "build/visionos/engine", "out": IPAD / "build/visionos/out",
                 "toolchain": IPAD / "visionos/toolchain.cmake", "moltenvk": IPAD / "deps/MoltenVK-xros-v1.4.2",
                 "ffmpeg": IPAD / "deps/ffmpeg-xros", "scheme": "SkyrimVision",
                 "destination": "generic/platform=visionOS", "entitlements": "Native-visionOS-{variant}.entitlements",
                 "log_prefix": "visionos-",
                 # MoltenVK v1.4.2's xros slice is built for visionOS 26.5, so the native app needs it too.
                 "app_settings": ["XROS_DEPLOYMENT_TARGET=26.5"]},
    # Pre-headset checks only (window, SDL scene, MoltenVK surface); the Simulator runs on the Mac.
    "visionos-simulator": {"sdk": "xrsimulator", "macho": "12", "vtool": "platform VISIONOSSIMULATOR",
                           "simulator": True, "label": "visionOS Simulator",
                           "build": IPAD / "build/visionos-simulator/engine",
                           "out": IPAD / "build/visionos-simulator/out",
                           "toolchain": IPAD / "visionos/toolchain-simulator.cmake",
                           "moltenvk": IPAD / "deps/MoltenVK-xrsimulator-v1.4.2", "ffmpeg": IPAD / "deps/ffmpeg-xrsimulator",
                           "scheme": "SkyrimVision", "destination": "generic/platform=visionOS Simulator",
                           "entitlements": "Native-visionOS-{variant}.entitlements",
                           "log_prefix": "visionos-simulator-",
                           # The engine is arm64 only, as is every Apple silicon Mac that runs the Simulator.
                           "app_settings": ["XROS_DEPLOYMENT_TARGET=26.5", "ARCHS=arm64"]},
}
DEPOT_VIEW = ROOT / "macos/build/steam-72850-view"
VENV_PYTHON = ROOT / "macos/.venv/bin/python"
sys.path.insert(0, str(ROOT / "macos"))
import inputs  # noqa: E402

FRAMEWORKS = ["Metal", "QuartzCore", "IOSurface", "CoreGraphics", "UIKit", "Foundation", "AVFoundation",
              "AudioToolbox", "CoreAudio", "CoreHaptics", "CoreMotion", "GameController"]
MEMORY = ("com.apple.developer.kernel.extended-virtual-addressing", "com.apple.developer.kernel.increased-memory-limit")
TOOLS = {"xcrun": "Xcode with the iOS SDK (then: sudo xcode-select -s /Applications/Xcode.app)",
         "cmake": "CMake 3.24 or newer (brew install cmake)", "ninja": "Ninja (brew install ninja)",
         "xcodegen": "XcodeGen (brew install xcodegen)"}
# SHA-256 of the exact supported inputs, from the verified generation (input manifest a40988a8...): Steam's
# original 1.9.32.0.8 files, the pinned Steamless v3.1.0.5 unpacked TESV.exe and Microsoft's DirectX June 2010
# DLLs (the same pins as macos/directx/fetch.py). The ESMs are the English depot 72853's; other languages are
# untested and refused.
SUPPORTED = {"TESV.exe": "69362f77bc56bf17570a33106d020b6c67fd8f32a294b82f8b687b9cf71efc72",
             "TESV_unpacked.exe": "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605",
             "d3dx9_42.dll": "f8b9cfab7fffbc8f98e41aa439d72921dc180634a1febca2a9d41a0df35d3472",
             "binkw32.dll": "13a57401c477be421af3679e3c1f0e851b31cd54e52d6ae53981a090c5723d2f",
             "X3DAudio1_7.dll": "288c701bdedf1d45c63dd0b7d424a752f8819f90feb5088c582f76bc98970ba3",
             "Skyrim.esm": "bc84992ac392672ae38dfa400307baef1e5c84d7c19aea3a7caf552916a5b6ed",
             "Update.esm": "b033d2820f129af34b134b79b925318cbc48dd5510a58aaee9d5de2e0b0ba0f4"}
# Upstream's own unpacked copy, which macos/inputs.py also accepts, is published only as a SHA-1 prefix.
UPSTREAM_UNPACKED_SHA1 = "fed96638"
REGENERATE = ("move gen/ out of this checkout (for example to macos/build/gen-previous) and run "
              "python3 ipad/build.py prepare again")
OBTAIN = """This build needs your own original Skyrim, version 1.9.32.0.8 (Steam app 72850).
Special Edition, Anniversary Edition and VR are different games and are refused.
  1. With Skyrim in your Steam library, open the Steam console (steam://open/console) and
     enter each line separately (72851 base game, 72853 English content, 72852 TESV.exe):
       download_depot 72850 72851
       download_depot 72850 72853
       download_depot 72850 72852
  2. Prepare the private generated code (a long job; your files are never changed):
       python3 ipad/build.py prepare --depots CONTENT/depot_72851 CONTENT/depot_72853 CONTENT/depot_72852
     where CONTENT is Steam's steamapps/content/app_72850 folder, or, for an existing original
     installation: python3 ipad/build.py prepare --game-dir /path/to/Skyrim
  3. python3 ipad/build.py engine"""
SIGNING = """Signing needs your own paid Apple Developer Program team and an App ID you own:
  python3 ipad/build.py app --signed --team TEAMID --bundle-id com.example.skyrim [--cloud]
TEAMID is the 10-character Team ID (Xcode > Settings > Accounts, or developer.apple.com > Membership);
sign Xcode into that account first. Automatic signing (-allowProvisioningUpdates) requests the
Extended Virtual Addressing and Increased Memory Limit capabilities for the App ID; if it can't, enable
them for the App ID at developer.apple.com. SKYRIM_DEVELOPMENT_TEAM and SKYRIM_BUNDLE_ID can be set
instead of the options."""


def fail(message):
    raise SystemExit(message)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*arguments):
    try:
        return subprocess.check_output(["git", "-C", str(ROOT), *arguments], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return None


def run(command, name, cwd=ROOT):
    name = LOG_PREFIX + name
    log = IPAD / "build/logs" / f"{name}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log.open("w") as output:
        result = subprocess.run([str(c) for c in command], cwd=cwd, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text()[-8000:])
        fail(f"{name} failed; log: {log}")
    print(f"{name} passed in {time.monotonic() - start:.0f} s; log: {log}", flush=True)


LOG_PREFIX = ""


def require_host(*tools, sdk="iphoneos"):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        fail("An Apple Silicon Mac (macOS on arm64) is required.")
    missing = [f"  {tool}: {TOOLS[tool]}" for tool in tools if not shutil.which(tool)]
    if missing:
        fail("Missing build tools:\n" + "\n".join(missing))
    if "xcrun" in tools and subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-path"],
                                           capture_output=True).returncode:
        fail(f"Xcode's {sdk} SDK was not found. Install {TOOLS['xcrun']}.")


def verified_generation(gen, game=None):
    """The user's own gen/, verified against exact inputs: (manifest SHA-256, manifest)."""
    if not (gen / "input-manifest.json").is_file():
        fail(f"No generated code at {gen} (its input-manifest.json is missing).\n\n{OBTAIN}")
    try:
        receipt = inputs.verify_generated(gen, game)
    except (OSError, ValueError, KeyError, TypeError) as error:
        fail(f"The generated code at {gen} can't be used: {error}\n"
             "It must have been generated by this checkout's tools/ from your own original Skyrim 1.9.32.0.8 "
             "files, and those files must still be present and unchanged. --game-dir must be a complete prepared "
             "game folder (TESV.exe, TESV_unpacked.exe, binkw32.dll, d3dx9_42.dll, X3DAudio1_7.dll and Data), "
             f"such as the one export writes. Otherwise {REGENERATE}.")
    # The manifest only binds gen/ to whatever it was generated from; these must be the exact supported files.
    recorded = {**receipt["modules"], **receipt["essential_data"]}
    foreign = sorted(name for name, sha in SUPPORTED.items() if name not in recorded or (
        recorded[name]["sha256"] != sha and not (name == "TESV_unpacked.exe"
                                                 and recorded[name].get("sha1", "").startswith(UPSTREAM_UNPACKED_SHA1))))
    if foreign:
        fail(f"{', '.join(foreign)}: not the exact supported original Skyrim 1.9.32.0.8 (English) input. It is "
             "modified, modded or from another release or language. Restore the original files (Steam: Verify "
             "integrity of game files, or download the depots again), remove any added or replacement DLLs from "
             f"the game folder, then {REGENERATE}.")
    return sha256(gen / "input-manifest.json"), receipt


def platforms(archive):
    """LC_BUILD_VERSION platforms of every member (2 = iphoneos, 7 = iOS Simulator, 11 = xros)."""
    text = subprocess.run(["otool", "-l", str(archive)], capture_output=True, text=True).stdout
    return sorted({line.split()[1] for line in text.splitlines() if line.strip().startswith("platform ")})


def prepare(args):
    if not VENV_PYTHON.is_file() or subprocess.run(
            [VENV_PYTHON, "-c", "import sys, capstone, pefile; sys.exit(sys.version_info < (3, 10))"],
            capture_output=True).returncode:
        fail("Generation needs Python 3.10+ with the lifter's pinned packages in macos/.venv:\n"
             "  python3 -m venv macos/.venv\n  macos/.venv/bin/python -m pip install -r macos/requirements.txt")
    game = args.game_dir
    if args.depots and game:
        fail("Use either --depots or --game-dir, not both.")
    if args.depots:
        import merge_depots
        try:
            game = merge_depots.merge_depots(args.depots, DEPOT_VIEW)
        except (OSError, ValueError) as error:
            fail(f"The depots could not be merged: {error}\nAn existing merged view is reused with: "
                 f"python3 ipad/build.py prepare --game-dir {DEPOT_VIEW}")
    if game is None:
        fail(OBTAIN)
    sys.path.insert(0, str(ROOT / "macos/launcher"))
    import bridge
    try:
        bridge.inspect_original(game)
    except (OSError, ValueError) as error:
        fail(f"{game} can't be used: {error}\n\n{OBTAIN}")
    missing = []
    directx = ROOT / "macos/build/directx/inputs"
    if not shutil.which("cabextract") and any(bridge.optional_file(game, name) is None and not (directx / name).is_file()
                                              for name in ("d3dx9_42.dll", "X3DAudio1_7.dll")):
        missing.append("cabextract, to take d3dx9_42.dll and X3DAudio1_7.dll from Microsoft's pinned "
                       "DirectX June 2010 redistributable (brew install cabextract)")
    unpacked = bridge.optional_file(game, "TESV_unpacked.exe") or bridge.optional_file(game, "TESV.exe.unpacked.exe")
    if unpacked is None and not bridge.converter_ready():
        if not (ROOT / "macos/deps/steamless/source").is_dir():
            missing.append("the pinned Steamless source, to unpack a private copy of TESV.exe: git clone --depth 1 "
                           "--branch v3.1.0.5 https://github.com/atom0s/Steamless.git macos/deps/steamless/source")
        if not (shutil.which("dotnet") or Path("/opt/homebrew/bin/dotnet").is_file()):
            missing.append(".NET SDK 10.0.402 for arm64 on PATH, to build Steamless locally (see macos/executable/README.md)")
    if missing:
        fail("Generation also needs:\n" + "\n".join(f"  - {item}" for item in missing))
    log_path = IPAD / "build/logs/prepare.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Preparing; generation is a long job. Log: {log_path}", flush=True)
    try:
        with log_path.open("w") as log:
            staged = bridge.prepare_generated(Path(game), log)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        fail(f"Preparation stopped: {error}\nYour game files were not changed. Log: {log_path}")
    manifest_sha, _ = verified_generation(ROOT / "gen", staged)
    print(json.dumps({"generated_code": str(ROOT / "gen"), "generated_manifest_sha256": manifest_sha,
                      "game_directory": str(staged)}, indent=2))
    print("Next: python3 ipad/build.py engine. Keep the depots or installation: the build verifies them again.")


def check(args):
    gen = args.gen_dir.resolve()
    manifest_sha, receipt = verified_generation(gen, args.game_dir)
    require_host(*TOOLS)
    print(json.dumps({"generated_code": str(gen), "generated_manifest_sha256": manifest_sha,
                      "game_directory": str(args.game_dir or receipt["game_directory"]), "scope": receipt["scope"],
                      "modules": sorted(receipt["modules"]), "generated_files": len(receipt["generated"])}, indent=2))


def export(args):
    """A real (link-free) copy of the verified game folder, for the app's Documents/Skyrim."""
    gen = args.gen_dir.resolve()
    _, receipt = verified_generation(gen, args.game_dir)
    output = args.output.absolute()
    stage = output.with_name(f".{output.name}.partial")
    for path in (output, stage):
        if path.exists() or path.is_symlink():
            fail(f"{path} already exists and was preserved; choose another --output or remove it")
    stage.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(Path(args.game_dir or receipt["game_directory"]), stage,
                    ignore=shutil.ignore_patterns("input-view.json", "depot-view.json"))
    try:
        inputs.verify_generated(gen, stage)
    except (OSError, ValueError, KeyError) as error:
        fail(f"The copy at {stage} doesn't match the generated code ({error}); it was preserved for inspection")
    stage.rename(output)
    print(f"{output} holds your verified game files. After installing the app, copy this folder as Skyrim into "
          "the app's Documents (Finder > your iPad > Files > Skyrim, or the Files app: On My iPad > Skyrim).")


def engine(args):
    target = PLATFORMS[args.platform]
    BUILD, OUT, sdk = target["build"], target["out"], target["sdk"]
    gen = args.gen_dir.resolve()
    manifest_sha, receipt = verified_generation(gen, args.game_dir)
    game = Path(args.game_dir or receipt["game_directory"]).resolve()
    require_host("xcrun", "cmake", "ninja", sdk=sdk)
    if not (ROOT / "third_party/dxvk/CMakeLists.txt").is_file():
        fail("DXVK is a git submodule that isn't checked out: git submodule update --init third_party/dxvk")
    jobs = str(args.jobs)
    run([sys.executable, IPAD / "build_ffmpeg.py", "--jobs", jobs, *(["--sdk", sdk] if sdk != "iphoneos" else []),
         *(["--source-archive", args.ffmpeg_archive] if args.ffmpeg_archive else [])], f"ffmpeg-{sdk}")
    run([sys.executable, IPAD / "prepare_moltenvk.py", *(["--sdk", sdk] if sdk != "iphoneos" else []),
         *(["--archive", args.moltenvk_archive] if args.moltenvk_archive else [])], f"moltenvk-{sdk}")
    run(["cmake", "-S", IPAD, "-B", BUILD, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={target['toolchain']}",
         "-DCMAKE_BUILD_TYPE=Release", f"-DSKYRIM_PYTHON={sys.executable}", f"-DSKYRIM_GEN_DIR={gen}",
         f"-DSKYRIM_GAME_DIR={game}"], "engine-configure")
    commands = json.loads((BUILD / "compile_commands.json").read_text())
    sources = {Path(c["file"]).resolve() for c in commands}
    sources = {p for p in sources if not p.is_relative_to(gen)}
    sources |= set((ROOT / "runtime").glob("*.h")) | {p for p in IPAD.iterdir() if p.is_file()}
    source_hashes = {str(p): sha256(p) for p in sorted(sources)}
    run(["cmake", "--build", BUILD, "--parallel", jobs], "engine-build")
    if any(sha256(p) != h for p, h in source_hashes.items()):
        fail("A source changed during the build; rebuild before using the engine")
    if verified_generation(gen, game)[0] != manifest_sha:
        fail("The generated code changed during the build; rebuild before using the engine")
    parts = {"engine": BUILD / "libSkyrimNativeEngine.a", "sdl2": BUILD / "sdl2/libSDL2.a",
             "dxvk_d3d9": BUILD / "dxvk/libdxvk_d3d9.a",
             "moltenvk": target["moltenvk"] / "lib/libMoltenVK.a",
             "avcodec": target["ffmpeg"] / "lib/libavcodec.a",
             "avutil": target["ffmpeg"] / "lib/libavutil.a"}
    for name, path in parts.items():
        found = platforms(path)
        if found != [target["macho"]]:
            fail(f"{name} ({path}) is not purely an {sdk} archive: platforms {found}")
    OUT.mkdir(parents=True, exist_ok=True)
    merged = OUT / "libSkyrimNative.a"
    merged.unlink(missing_ok=True)
    run(["libtool", "-static", "-no_warning_for_no_symbols", "-o", merged, *parts.values()], "engine-merge")
    status = git("status", "--short")
    receipt = {"schema": 1, "scope": f"{target['label']} native Skyrim engine ({sdk} arm64); not a device result",
               "built_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "generated_manifest_sha256": manifest_sha, "game_directory": str(game),
               "source_commit": (git("rev-parse", "HEAD") or "unknown (not a git checkout)").strip(),
               "worktree_changes": status.splitlines() if status is not None else None,
               "sources": source_hashes,
               "archives": {name: {"path": str(path), "sha256": sha256(path), "platforms": platforms(path)}
                            for name, path in parts.items()},
               "library": {"path": str(merged), "sha256": sha256(merged), "bytes": merged.stat().st_size,
                           "platforms": platforms(merged)},
               "link": {"frameworks": FRAMEWORKS, "libraries": ["c++", "iconv", "z"]},
               "identity_header": (BUILD / "generated/skyrim_native_identity.h").read_text()}
    (OUT / "engine-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"library": receipt["library"]}, indent=2))


def signing_settings(args):
    """xcodebuild overrides for the user's own team, App ID and iCloud container; no per-user project edits."""
    if args.signed and not (args.team and args.bundle_id):
        fail(SIGNING)
    if args.team and not re.fullmatch(r"[A-Z0-9]{10}", args.team):
        fail(f"{args.team!r} is not a 10-character Apple Team ID.\n\n{SIGNING}")
    if args.bundle_id and not re.fullmatch(r"[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+", args.bundle_id):
        fail(f"{args.bundle_id!r} is not a reverse-DNS bundle identifier such as com.example.skyrim.")
    settings = [f"DEVELOPMENT_TEAM={args.team}"] if args.team else []
    settings += [f"PRODUCT_BUNDLE_IDENTIFIER={args.bundle_id}"] if args.bundle_id else []
    container = None
    if args.cloud:
        container = args.icloud_container or (f"iCloud.{args.bundle_id}" if args.bundle_id else None)
        if not container:
            fail("--cloud needs your own iCloud container: pass --bundle-id (the container is then "
                 "iCloud.BUNDLE_ID) or --icloud-container iCloud.your.container.")
        if not re.fullmatch(r"iCloud\.[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*", container):
            fail(f"{container!r} is not an iCloud container identifier (iCloud.com.example.skyrim).")
        settings.append(f"SKYRIM_ICLOUD_CONTAINER={container}")
    return settings, container


def app(args):
    target = PLATFORMS[args.platform]
    OUT, sdk = target["out"], target["sdk"]
    settings, container = signing_settings(args)
    require_host("xcrun", "xcodegen", sdk=sdk)
    try:
        receipt = json.loads((OUT / "engine-receipt.json").read_text())
    except (OSError, ValueError):
        fail("No engine receipt; run python3 ipad/build.py engine first.")
    library = Path(receipt["library"]["path"])
    if not library.is_file():
        library = OUT / library.name  # an engine built on another Mac and copied into OUT with its receipt
    if not library.is_file() or sha256(library) != receipt["library"]["sha256"]:
        fail("libSkyrimNative.a changed since its receipt; rebuild the engine")
    apple = ROOT / "apple"
    # As apple/build.sh: the Mac target's (ignored) checkout pointer must exist for XcodeGen.
    (apple / "Resources/checkout-path.txt").write_text(f"{ROOT}\n")
    run(["xcodegen", "generate", "--spec", apple / "project.yml", "--project", apple], "app-xcodegen")
    # Local is the default: no iCloud entitlement at all. Cloud is optional and needs your own container.
    configuration, variant = ("Cloud", "Cloud") if args.cloud else ("Release", "Local")
    flags = ["-ObjC", str(library), "-lc++", "-liconv", "-lz"] + [f for name in FRAMEWORKS for f in ("-framework", name)]
    derived = IPAD / "build/DerivedData" if args.platform == "ios" else OUT.parent / "DerivedData"
    command = ["xcodebuild", "-project", apple / "SkyrimApple.xcodeproj", "-scheme", target["scheme"],
               "-configuration", configuration, "-sdk", sdk, "-destination", target["destination"],
               "-derivedDataPath", derived, "-jobs", str(args.jobs), "COMPILER_INDEX_STORE_ENABLE=NO",
               f"SKYRIM_NATIVE_LDFLAGS={' '.join(flags)}", "SKYRIM_NATIVE_SWIFT_CONDITIONS=SKYRIM_NATIVE_RUNTIME",
               f"CODE_SIGN_ENTITLEMENTS={apple / target['entitlements'].format(variant=variant)}",
               *target.get("app_settings", []), *settings]
    if args.skip_metal:  # no Metal Toolchain on this Mac: the launcher's smoke background is left out
        command.append("EXCLUDED_SOURCE_FILE_NAMES=SmokeBackground.metal")
    command += ["-allowProvisioningUpdates"] if args.signed else ["CODE_SIGNING_ALLOWED=NO"]
    run(command + ["build"], "app-build-signed" if args.signed else "app-build-unsigned")
    product = derived / f"Build/Products/{configuration}-{sdk}/Skyrim.app"
    binary = product / "Skyrim"
    build = subprocess.check_output(["xcrun", "vtool", "-show-build", str(binary)], text=True)
    if target["vtool"] not in build or ("SIMULATOR" in build) != target.get("simulator", False):
        fail(f"The app binary is not an {sdk} binary:\n" + build)
    exports = subprocess.run(["nm", "-gU", str(binary)], capture_output=True, text=True).stdout
    info = json.loads(subprocess.check_output(["plutil", "-convert", "json", "-o", "-", str(product / "Info.plist")]))
    result = {"schema": 1, "scope": "native-enabled launcher candidate; not a device result",
              "built_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "signed": args.signed,
              "configuration": f"{configuration} ({variant})", "icloud_container": container,
              "app": str(product), "bundle_id": info.get("CFBundleIdentifier"),
              "version": f"{info.get('CFBundleShortVersionString')} ({info.get('CFBundleVersion')})",
              "binary_sha256": sha256(binary), "binary_bytes": binary.stat().st_size, "vtool": build.strip(),
              "engine_library_sha256": receipt["library"]["sha256"],
              "exports_vkGetInstanceProcAddr": "_vkGetInstanceProcAddr" in exports}
    entitlements = signed_entitlements(product) if args.signed else None
    check_containers(info, entitlements, container)
    result["info_icloud_container"] = info.get("SkyrimICloudContainerIdentifier")
    if args.signed:
        result["codesign_verify"] = subprocess.run(["codesign", "--verify", "--deep", "--strict", "-v", str(product)],
                                                   capture_output=True, text=True).stderr.strip()
        result["entitlements"] = entitlements
    name = f"app-receipt-{'signed' if args.signed else 'unsigned'}-{variant.lower()}.json"
    (OUT / name).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "entitlements"}, indent=2))


def signed_entitlements(product):
    """The signed app's entitlements; it must carry both memory capabilities and nothing JIT-like."""
    output = subprocess.run(["codesign", "-d", "--xml", "--entitlements", "-", str(product)], capture_output=True).stdout
    try:
        values = plistlib.loads(output)
    except Exception:
        values = {}
    missing = [key for key in MEMORY if values.get(key) is not True]
    if missing:
        fail(f"The signed app lacks {', '.join(missing)}. Enable Extended Virtual Addressing and Increased Memory "
             "Limit for your App ID (a paid Apple Developer Program team is required) and build again.")
    if any("dynamic-codesigning" in key or "allow-jit" in key for key in values):
        fail("The signed app carries a JIT or dynamic-codesigning entitlement; this runtime must not.")
    return values


CONTAINER_ENTITLEMENTS = ("com.apple.developer.icloud-container-identifiers",
                          "com.apple.developer.ubiquity-container-identifiers")


def check_containers(info, entitlements, container):
    """The built Info.plist and signed entitlements name exactly the requested iCloud container (Cloud) or
    carry no iCloud container entitlement (Local), with every build setting expanded."""
    if "$(" in json.dumps([info, entitlements], default=str):
        fail("The built app still contains an unexpanded $(...) build setting in its Info.plist or entitlements.")
    found = {"Info.plist SkyrimICloudContainerIdentifier": [info.get("SkyrimICloudContainerIdentifier")]}
    if entitlements is not None:
        found.update({key: entitlements.get(key) for key in CONTAINER_ENTITLEMENTS})
    if container:
        wrong = [key for key, value in found.items() if value != [container]]
        if wrong:
            fail(f"{', '.join(wrong)} doesn't name the requested iCloud container {container}.")
    elif any(found.get(key) for key in CONTAINER_ENTITLEMENTS):
        fail("This Local build is signed with iCloud container entitlements; it must have none.")


def main(argv=None):
    environ = os.environ.get
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest="step", required=True)

    def generated_options(step):
        step.add_argument("--gen-dir", type=Path, default=Path(environ("SKYRIM_GEN_DIR") or ROOT / "gen"),
                          help="your verified generated code (default: SKYRIM_GEN_DIR or gen/)")
        step.add_argument("--game-dir", type=Path, default=environ("SKYRIM_GAME_DIR") or None,
                          help="a complete prepared game folder for gen/ (TESV.exe, TESV_unpacked.exe, the three "
                               "DLLs and Data), such as export writes (default: SKYRIM_GAME_DIR or the folder "
                               "recorded in gen/input-manifest.json). An original installation or depot folder is "
                               "not one: move gen/ out of this checkout and run prepare again instead")

    step = steps.add_parser("prepare", help="generate your private gen/ from your own Steam depots or installation")
    step.add_argument("--depots", type=Path, nargs="+", metavar="DEPOT",
                      help=f"Steam depot_72851, depot_72853 and depot_72852 folders, merged into {DEPOT_VIEW}")
    step.add_argument("--game-dir", type=Path, help="an original installation or an existing merged depot view")
    generated_options(steps.add_parser("check", help="verify tools and the generated code without building"))
    step = steps.add_parser("export", help="copy the verified game files (links followed) for the app's Documents/Skyrim")
    generated_options(step)
    step.add_argument("--output", type=Path, default=IPAD / "build/Skyrim", help="a new folder (default: ipad/build/Skyrim)")
    step = steps.add_parser("engine", help="device dependencies, then ipad/build/out/libSkyrimNative.a")
    generated_options(step)
    step.add_argument("--platform", choices=sorted(PLATFORMS), default="ios",
                      help="ios (default), visionos or visionos-simulator (outputs in ipad/build/<platform>/)")
    step.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    step.add_argument("--ffmpeg-archive", type=Path, help="local ffmpeg-7.1.2.tar.xz instead of downloading it")
    step.add_argument("--moltenvk-archive", type=Path, help="local MoltenVK-ios.tar (v1.4.2) instead of downloading it")
    step = steps.add_parser("app", help="the launcher app linked with the engine")
    step.add_argument("--platform", choices=sorted(PLATFORMS), default="ios",
                      help="ios (SkyrimIOS, default), visionos or visionos-simulator (SkyrimVision)")
    step.add_argument("--skip-metal", action="store_true",
                      help="leave out the launcher's Metal shader, for a Mac without Xcode's Metal Toolchain")
    step.add_argument("--signed", action="store_true", help="sign with your team's development identity")
    step.add_argument("--team", default=environ("SKYRIM_DEVELOPMENT_TEAM"), help="your Apple Team ID")
    step.add_argument("--bundle-id", default=environ("SKYRIM_BUNDLE_ID"), help="an App ID you own")
    step.add_argument("--cloud", action="store_true", help="optional iCloud save mirroring (unverified across devices)")
    step.add_argument("--icloud-container", default=environ("SKYRIM_ICLOUD_CONTAINER"),
                      help="your iCloud container with --cloud (default: iCloud.BUNDLE_ID)")
    step.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    args = parser.parse_args(argv)
    global LOG_PREFIX
    LOG_PREFIX = PLATFORMS[getattr(args, "platform", "ios")]["log_prefix"]
    if sys.version_info < (3, 12):
        fail("Python 3.12 or newer is required (for example Homebrew's python3); macOS's /usr/bin/python3 is too old.")
    {"prepare": prepare, "check": check, "export": export, "engine": engine, "app": app}[args.step](args)


if __name__ == "__main__":
    sys.exit(main())
