#!/usr/bin/env python3
"""Exercise actual host helpers, guest release semantics and profiler on macOS."""
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
if platform.system() != "Darwin":
    raise SystemExit("This test requires macOS host APIs.")
with tempfile.TemporaryDirectory(prefix="skyrim-host-runtime-") as temporary:
    work = Path(temporary)
    binary = work / "host-runtime-test"
    command = [
        "clang", "-arch", "arm64", "-DGAME_SKYRIM", "-std=gnu11", "-O1",
        "-g", "-fno-strict-aliasing", "-fwrapv", "-Werror=implicit-function-declaration",
        "-I", str(ROOT / "runtime"),
        str(ROOT / "tests/macos_runtime/host_runtime_test.c"),
        str(ROOT / "runtime/heap.c"), str(ROOT / "runtime/prof.c"),
        "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary), str(work)], text=True, capture_output=True, check=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    samples = re.search(r"profile(?:, all threads)?: (\d+) samples", result.stderr)
    cpu = re.search(r"cpu: (\d+)% of one core", result.stderr)
    if samples is None or int(samples[1]) == 0 or cpu is None or int(cpu[1]) == 0:
        raise SystemExit("Actual Darwin profiler did not report CPU samples and thread time.")
    print(json.dumps({"passed": True, "cpu_samples": int(samples[1]), "cpu_percent": int(cpu[1])}))
