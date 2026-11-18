#!/usr/bin/env python3
"""Validate the synthetic native JITless boundary with copied hardened binaries.

This is not a game boot test and does not establish completeness of lifted code.
No source binary or dependency is modified; signing affects copies in ignored build output.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

AOT_FIXTURES = 21  # tests/aot/harness.c cases; raise it with the harness

ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN_ENTITLEMENTS = {
    "com.apple.security.cs.allow-jit",
    "com.apple.security.cs.allow-unsigned-executable-memory",
    "com.apple.security.cs.disable-executable-page-protection",
    "com.apple.security.get-task-allow",
    "get-task-allow",
}
LIBRARY_VALIDATION_EXCEPTION = "com.apple.security.cs.disable-library-validation"


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def command(arguments, *, timeout=30, check=True):
    environment = dict(os.environ)
    for name in ("DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        environment.pop(name, None)
    result = subprocess.run([str(a) for a in arguments], capture_output=True, text=True,
                            env=environment, timeout=timeout)
    record = {"command": [str(a) for a in arguments], "returncode": result.returncode,
              "stdout": result.stdout, "stderr": result.stderr}
    if check and result.returncode:
        raise RuntimeError(json.dumps(record, indent=2))
    return record


def last_json(output):
    for line in reversed(output.splitlines()):
        if line.startswith("{"):
            return json.loads(line)
    raise ValueError("Expected a structured test result")


def copy_and_sign(source, destination, entitlements=None):
    source = Path(source).resolve(strict=True)
    before = digest(source)
    shutil.copy2(source, destination)
    architecture = command(["lipo", "-archs", destination])["stdout"].strip()
    if architecture != "arm64":
        raise ValueError(f"Expected ARM64-only executable: {architecture}")
    arguments = ["codesign", "--force", "--sign", "-", "--options", "runtime", "--timestamp=none"]
    if entitlements:
        plist = destination.with_suffix(".entitlements.plist")
        plist.write_bytes(plistlib.dumps(entitlements))
        arguments += ["--entitlements", plist]
    arguments += [destination]
    sign = command(arguments)
    verification = command(["codesign", "--verify", "--strict", destination])
    details = command(["codesign", "-dvv", destination])
    flags = re.search(r"flags=0x([0-9a-fA-F]+)", details["stderr"])
    if not flags or not (int(flags.group(1), 16) & 0x10000):
        raise ValueError("Hardened runtime flag is absent")
    extracted = command(["codesign", "-d", "--entitlements", "-", "--xml", destination])
    observed = plistlib.loads(extracted["stdout"].encode()) if extracted["stdout"].strip() else {}
    if observed != (entitlements or {}):
        raise ValueError(f"Unexpected executable entitlements: {observed}")
    if any(observed.get(name) for name in FORBIDDEN_ENTITLEMENTS):
        raise ValueError("JIT/debugger/executable-memory entitlement was unexpectedly enabled")
    if before != digest(source):
        raise ValueError("Source binary changed during copy/sign; rerun after the build is stable")
    return {"source": str(source), "source_sha256": before,
            "signed_copy": str(destination), "copy_sha256": digest(destination),
            "architecture": architecture, "entitlements": observed,
            "hardened_runtime": True, "sign": sign, "verify": verification,
            "signature": details, "libraries": command(["otool", "-L", destination])["stdout"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-bin", type=Path, default=ROOT / "macos/build/native/runtime_smoke")
    parser.add_argument("--aot-receipt", type=Path, default=ROOT / "macos/build/native/aot-fixtures-strict/receipt.json")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "macos/build/jitless-proof")
    parser.add_argument("--build-mapping-probe", action="store_true",
                        help="Compile a small real-heap mapping probe; coordinate the shared compiler slot first")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ["DXVK_LOG_PATH"] = str(output)
    receipt = {"scope": "synthetic-native-JITless-boundary", "gameplay_tested": False,
               "limitations": ["Synthetic instruction/runtime tests do not establish Skyrim function or import coverage.",
                               "The full-runtime copy permits third-party library loading only; no JIT, unsigned executable memory, executable-page-protection exception or debugger entitlement is enabled.",
                               "GPU shader compilation by the graphics driver is outside guest CPU translation."]}

    aot_receipt = json.loads(args.aot_receipt.read_text())
    if aot_receipt.get("game_code") is not False or aot_receipt.get("architecture") != "arm64":
        raise ValueError("Expected an authored synthetic ARM64 AOT receipt")
    aot_source = Path(aot_receipt["executable"])
    if digest(aot_source) != aot_receipt["executable_sha256"]:
        raise ValueError("AOT source executable does not match its build receipt")
    for name, sha in aot_receipt["inputs"].items():
        if digest(ROOT / name) != sha:
            raise ValueError(f"AOT fixture source changed since build: {name}")
    aot_copy = output / "aot-fixtures-hardened"
    aot = copy_and_sign(aot_source, aot_copy)
    aot["run"] = command([aot_copy])
    aot["observed"] = last_json(aot["run"]["stdout"])
    if aot["observed"] != {"passed": AOT_FIXTURES, "total": AOT_FIXTURES}:
        raise ValueError("Unexpected AOT fixture coverage/result")
    aot["negative"] = {}
    for name, expected in aot_receipt["negative_tests"].items():
        test = command([aot_copy, "--" + name], check=False)
        if test["returncode"] != expected["exit_code"] or expected["expected_error"] not in test["stderr"]:
            raise ValueError(f"Hardened AOT negative test failed: {name}")
        aot["negative"][name] = test
    receipt["aot_hardened_zero_entitlements"] = aot

    runtime_copy = output / "runtime-smoke-hardened"
    runtime = copy_and_sign(args.runtime_bin, runtime_copy, {LIBRARY_VALIDATION_EXCEPTION: True})
    runtime["run"] = command([runtime_copy])
    runtime["observed"] = last_json(runtime["run"]["stdout"])
    if runtime["observed"].get("guest_memory_executable") is not False or runtime["observed"].get("unknown_code_aborts") is not True:
        raise ValueError("Full runtime did not verify the expected data-only memory/fatal dispatch boundary")
    receipt["full_runtime_hardened_no_jit_entitlements"] = runtime

    if args.build_mapping_probe:
        executable = output / "mapping-probe"
        compile_command = ["clang", "-arch", "arm64", "-std=gnu11", "-O2", "-DGAME_SKYRIM",
                           "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", "-I", ROOT / "runtime",
                           ROOT / "tests/jitless/mapping_probe.c", ROOT / "runtime/heap.c", "-o", executable]
        receipt["mapping_compile"] = command(compile_command)
        mapping = copy_and_sign(executable, output / "mapping-probe-hardened")
        mapping["run"] = command([output / "mapping-probe-hardened"])
        mapping["observed"] = last_json(mapping["run"]["stdout"])
        if mapping["observed"].get("executable_guest_regions") != 0 or mapping["observed"].get("scans") != 4:
            raise ValueError("Full guest mapping protection coverage failed")
        receipt["full_guest_mapping_hardened_zero_entitlements"] = mapping

    for record in (aot, runtime):
        if digest(record["source"]) != record["source_sha256"]:
            raise ValueError("A source binary changed during the test; rerun once builds settle")
    receipt["pass"] = True
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"pass": True, "receipt": str(output / "receipt.json"),
                      "aot_assertions": aot["observed"]["passed"],
                      "runtime_checks": runtime["observed"]["checks"],
                      "mapping_probe": args.build_mapping_probe}))


if __name__ == "__main__":
    main()
