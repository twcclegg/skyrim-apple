#!/usr/bin/env python3
"""Compile one job at a time, then call an actual lifted Microsoft DLL on ARM64."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from fetch import OUTPUT, ROOT, MODULES, private_path, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generation", type=Path, help="private generate.py output containing the selected DLL")
    parser.add_argument("--module", choices=("x3da", "d3dx9"), default="x3da")
    args = parser.parse_args()
    generation = args.generation.resolve(strict=True)
    generation.relative_to(OUTPUT)
    receipt = json.loads((generation / "generation-receipt.json").read_text())
    if receipt.get("scope") != "official-directx-x86-static-recompilation":
        parser.error("Expected a real DirectX generation receipt")
    module_names = {settings["family"]: name for name, settings in MODULES.items()}
    dll = OUTPUT / "inputs" / module_names[args.module]
    if receipt.get("modules", {}).get(dll.name, {}).get("sha256") != MODULES[dll.name]["sha256"]:
        parser.error("The selected Microsoft DLL is absent from this generation receipt")
    if sha256(dll) != MODULES[dll.name]["sha256"]:
        parser.error("Pinned official DLL changed")
    for name, expected in receipt["tools"].items():
        if sha256(ROOT / name) != expected:
            parser.error("Recompiler changed; regenerate first")
    actual = {path.name for path in (generation / "gen").iterdir() if path.is_file()}
    if actual != set(receipt["generated"]):
        parser.error("Generated Microsoft source closure changed")
    for name, expected in receipt["generated"].items():
        if sha256(generation / "gen" / name) != expected:
            parser.error("Generated Microsoft code changed")
    sys.path.insert(0, str(ROOT / "tools"))
    import peimg
    pe, base, image = peimg.load(str(dll), int(MODULES[dll.name]["base"], 16))
    exports = peimg.exports(pe, base)
    smoke = private_path(generation / ("native-smoke-" + args.module))
    smoke.mkdir(exist_ok=True)
    (smoke / "module_image.bin").write_bytes(image)
    entries = []
    for imported in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for item in imported.imports:
            name = item.name.decode() if item.name else f"#{item.ordinal}"
            entries.append(f"    {{0x{item.address:X}u, {json.dumps(imported.dll.decode())}, {json.dumps(name)}}},")
    wanted = ({"INITIALIZE": "X3DAudioInitialize", "CALCULATE": "X3DAudioCalculate"} if args.module == "x3da" else
              {"CHECK_VERSION": "D3DXCheckVersion", "TRANSLATION": "D3DXMatrixTranslation",
               "SCALING": "D3DXMatrixScaling", "MULTIPLY": "D3DXMatrixMultiply",
               "INVERSE": "D3DXMatrixInverse", "NORMALIZE": "D3DXVec3Normalize"})
    header = [f"#define MODULE_BASE 0x{base:X}u", f"#define MODULE_IMAGE_BYTES {len(image)}u",
              f"#define MODULE_ENTRY 0x{base + pe.OPTIONAL_HEADER.AddressOfEntryPoint:X}u",
              *[f"#define MODULE_{macro} 0x{exports[name]:X}u" for macro, name in wanted.items()],
              "static const struct { unsigned address; const char *dll, *name; } module_imports[] = {",
              *entries, "};"]
    (smoke / "module_metadata.h").write_text("\n".join(header) + "\n")
    native = ROOT / "macos/build/native"
    runtime = sorted((native / "CMakeFiles/skyrim_runtime.dir").rglob("*.o"))
    entry = sorted((native / "CMakeFiles/skyrim_entry.dir").rglob("*.o"))
    # every runtime source but main.c and the native replacements (skyrim_native*.c link with the game only)
    expected_runtime = (len(list((ROOT / "runtime").glob("*.c"))) + len(list((ROOT / "runtime").glob("*.cpp"))) - 1
                        - len(list((ROOT / "runtime").glob("skyrim_native*.c"))))
    if len(runtime) != expected_runtime or len(entry) != 1:
        parser.error(f"Build the real macOS runtime first; found {len(runtime)} runtime objects, {len(entry)} entry objects")
    objects = []
    compiler = ["xcrun", "clang", "-arch", "arm64", "-mmacosx-version-min=13.0", "-O2",
                "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", "-DLB_STRICT_ORDER", "-DGAME_SKYRIM",
                "-I" + str(ROOT / "runtime"), "-I" + str(generation / "gen"), "-I" + str(smoke)]
    commands = []
    sources = [*sorted((generation / "gen").glob(args.module + "_*.c")), ROOT / f"macos/directx/{args.module}_smoke.c"]
    with (smoke / "compile.log").open("w") as log:
        for source in sources:
            target = smoke / (source.stem + ".o")
            command = [*compiler, "-c", str(source), "-o", str(target)]
            commands.append(command)
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(target)
        binary = smoke / (args.module + "_smoke")
        dxvk = native / "graphics/dxvk/libdxvk_d3d9.dylib"
        sdl = native / "sdl/libSDL2-2.0.0.dylib"
        audio_libraries = [ROOT / "macos/deps/ffmpeg-macos/lib" / ("lib" + name + ".a")
                           for name in ("avcodec", "avutil")]
        audio_libraries = audio_libraries if all(path.is_file() for path in audio_libraries) else []
        command = ["xcrun", "clang++", "-arch", "arm64", "-mmacosx-version-min=13.0", "-o", str(binary),
                   *(str(path) for path in [*objects, *runtime, *entry]), str(dxvk), str(sdl),
                   *(str(path) for path in audio_libraries),
                   "-Wl,-rpath," + str(dxvk.parent), "-Wl,-rpath," + str(sdl.parent)]
        commands.append(command)
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    environment = dict(os.environ, DXVK_LOG_PATH=str(smoke))
    run = subprocess.run([str(binary), str(smoke / "module_image.bin")], capture_output=True, text=True,
                         timeout=30, env=environment)
    (smoke / "stdout.log").write_text(run.stdout)
    (smoke / "stderr.log").write_text(run.stderr)
    report = {"scope": "actual_microsoft_" + args.module + "_native_call_smoke", "returncode": run.returncode,
              "binary_sha256": sha256(binary), "dll_sha256": sha256(dll),
              "generation_receipt_sha256": sha256(generation / "generation-receipt.json"),
              "runtime_objects": {str(path.relative_to(native)): sha256(path) for path in [*runtime, *entry]},
              "commands": commands, "stdout": run.stdout, "stderr": run.stderr,
              "gameplay_tested": False}
    (smoke / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr[-1500:],
                      "receipt": str(smoke / "receipt.json")}, indent=2))
    return 0 if run.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
