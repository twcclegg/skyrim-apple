#!/usr/bin/env python3
"""Bind a privately built native executable to its verified inputs and sources.

This is a local consistency receipt, not an authenticity signature or gameplay
certificate. CMake runs begin before any game target compiles and seal only
after the final link. Launch verifies the sealed identity again.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import tempfile

from inputs import ROOT, digest, verify_generated

SCHEMA = 1
SCOPE = "private-native-windows-skyrim-macos-build"
SOURCE_DIRECTORIES = ("runtime", "third_party/dxvk", "third_party/SDL2")
MACOS_SOURCES = ("CMakeLists.txt", "build.sh", "bootstrap.sh", "inputs.py",
                 "prepare_game.py", "build_receipt.py", "requirements.txt", "runtime.conf")
CONFIG_FILES = ("CMakeCache.txt", "compile_commands.json")
DEPENDENCIES = {"dxvk", "sdl2", "moltenvk"}


def _atomic_json(path, value):
    """Replace only this helper's local receipt, never game or source files."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    content = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if path.is_file() and path.read_text() == content:
        return
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def source_files(root):
    root = Path(root)
    files = []
    for relative in SOURCE_DIRECTORIES:
        directory = root / relative
        if not directory.is_dir():
            raise ValueError(f"Missing source directory: {relative}")
        for path in directory.rglob("*"):
            if path.is_symlink():
                raise ValueError(f"Source symlinks are not supported in the build binding: {path}")
            if path.is_file():
                files.append(path)
    for relative in MACOS_SOURCES:
        path = root / "macos" / relative
        # runtime.conf is optional before root adds its bounded shader settings.
        if path.is_file():
            files.append(path)
    graphics = root / "macos/graphics"
    if graphics.exists():
        files.extend(path for path in graphics.iterdir() if path.is_file() and
                     (path.suffix in (".c", ".cpp", ".h", ".py", ".conf") or path.name == "CMakeLists.txt"))
    return sorted(set(files))


def _source_identity(root):
    root = Path(root).resolve(strict=True)
    return {str(path.relative_to(root)): digest(path) for path in source_files(root)}


def _config_identity(build_directory):
    build_directory = Path(build_directory).resolve(strict=True)
    return {name: digest(build_directory / name) for name in CONFIG_FILES}


def _identity(generated_directory, build_directory, game_directory=None, source_root=ROOT):
    generated_directory = Path(generated_directory).resolve(strict=True)
    manifest = generated_directory / "input-manifest.json"
    before = digest(manifest)
    verify_generated(generated_directory, game_directory)
    sources = _source_identity(source_root)
    configuration = _config_identity(build_directory)
    if digest(manifest) != before:
        raise ValueError("Generated input manifest changed while checking the build identity")
    return {"manifest_sha256": before, "sources": sources, "configuration": configuration}


def _paths(binary):
    binary = Path(binary).absolute()
    return (binary.with_name(binary.name + ".build-start.json"),
            binary.with_name(binary.name + ".build-receipt.json"))


def _validate_native_executable(binary):
    binary = Path(binary)
    if not binary.is_file():
        raise ValueError(f"Native game executable is absent: {binary}")
    with binary.open("rb") as source:
        header = source.read(16)
    if len(header) != 16:
        raise ValueError("Native game executable has a truncated Mach-O header")
    magic, cpu, _subtype, filetype = struct.unpack("<IIII", header)
    if (magic, cpu, filetype) != (0xfeedfacf, 0x0100000c, 2):
        raise ValueError("Expected a thin ARM64 Mach-O executable, not a fixture, library or translated host binary")


def _dependency_identity(dependencies):
    if not isinstance(dependencies, dict) or set(dependencies) != DEPENDENCIES:
        raise ValueError("Seal exactly the dxvk, sdl2 and moltenvk runtime dependencies")
    result = {}
    for name, requested in sorted(dependencies.items()):
        requested = Path(requested).absolute()
        resolved = requested.resolve(strict=True)
        if not resolved.is_file():
            raise ValueError(f"Runtime dependency is not a regular file: {name}")
        result[name] = {"path": str(requested), "resolved_path": str(resolved), "sha256": digest(requested)}
    return result


def begin(generated_directory, binary, build_directory, game_directory=None, *, source_root=ROOT):
    identity = _identity(generated_directory, build_directory, game_directory, source_root=source_root)
    start, _receipt = _paths(binary)
    _atomic_json(start, {"schema": SCHEMA, "scope": SCOPE, "identity": identity})
    return identity


def seal(generated_directory, binary, build_directory, dependencies, game_directory=None, *, source_root=ROOT):
    start, receipt_path = _paths(binary)
    started = json.loads(start.read_text())
    if started.get("schema") != SCHEMA or started.get("scope") != SCOPE:
        raise ValueError("Missing or unsupported precompile build identity")
    current = _identity(generated_directory, build_directory, game_directory, source_root=source_root)
    if current != started["identity"]:
        raise ValueError("Build inputs, source or configuration changed during compilation; rebuild before launch")
    _validate_native_executable(binary)
    receipt = {"schema": SCHEMA, "scope": SCOPE, "identity": current,
               "binary_sha256": digest(binary), "binary_name": Path(binary).name,
               "dependencies": _dependency_identity(dependencies)}
    _atomic_json(receipt_path, receipt)
    return receipt


def verify(generated_directory, binary, build_directory, game_directory=None, *, source_root=ROOT):
    _start, receipt_path = _paths(binary)
    receipt = json.loads(receipt_path.read_text())
    if receipt.get("schema") != SCHEMA or receipt.get("scope") != SCOPE:
        raise ValueError("Missing or unsupported native game build receipt")
    _validate_native_executable(binary)
    if receipt.get("binary_name") != Path(binary).name or receipt.get("binary_sha256") != digest(binary):
        raise ValueError("Native executable changed after its build receipt was sealed")
    dependencies = receipt.get("dependencies", {})
    if set(dependencies) != DEPENDENCIES:
        raise ValueError("Native build receipt lacks the complete runtime dependency binding")
    current_dependencies = _dependency_identity({name: item["path"] for name, item in dependencies.items()})
    if current_dependencies != dependencies:
        raise ValueError("A sealed runtime dependency changed; rebuild before launch")
    current = _identity(generated_directory, build_directory, game_directory, source_root)
    if current != receipt["identity"]:
        raise ValueError("Native executable is stale for the current generated inputs, source or configuration; rebuild it")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("begin", "seal", "verify"))
    parser.add_argument("--gen-dir", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--dependency", action="append", default=[], metavar="NAME=PATH",
                        help="seal-only named runtime library; supply dxvk, sdl2 and moltenvk")
    parser.add_argument("--print-dependency", choices=sorted(DEPENDENCIES),
                        help="verify everything and print only this sealed library path")
    args = parser.parse_args()
    if args.print_dependency and args.command != "verify":
        parser.error("--print-dependency is only valid with verify")
    if args.dependency and args.command != "seal":
        parser.error("--dependency is only valid with seal")
    try:
        if args.command == "begin":
            result = begin(args.gen_dir, args.binary, args.build_dir, args.game_dir)
        elif args.command == "seal":
            dependencies = {}
            for item in args.dependency:
                name, separator, path = item.partition("=")
                if not separator or not path or name in dependencies:
                    raise ValueError("Each dependency must have one unique NAME=PATH argument")
                dependencies[name] = path
            result = seal(args.gen_dir, args.binary, args.build_dir, dependencies, args.game_dir)
        else:
            result = verify(args.gen_dir, args.binary, args.build_dir, args.game_dir)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Native build binding failed: {error}", file=sys.stderr)
        return 2
    if args.print_dependency:
        print(result["dependencies"][args.print_dependency]["path"])
    else:
        print(json.dumps({"scope": SCOPE, "command": args.command,
                          "manifest_sha256": result.get("manifest_sha256", result.get("identity", {}).get("manifest_sha256"))}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
