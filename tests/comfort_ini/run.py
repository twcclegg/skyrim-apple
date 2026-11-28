#!/usr/bin/env python3
"""Build and run the checks for runtime/comfort_ini.c (the comfort defaults for immersive play, merged into LB_INI).

Needs only a C compiler: no game files, no generated code, any platform. The exit status is that of the test.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/comfort-ini-test")
    arguments = parser.parse_args()
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    binary = arguments.output_dir / "comfort_ini_test"
    build = [arguments.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-I", str(ROOT / "runtime"), str(ROOT / "runtime/comfort_ini.c"), str(ROOT / "tests/comfort_ini/test.c"),
             "-o", str(binary), "-lm", "-lpthread"]
    built = subprocess.run(build)
    if built.returncode:
        return built.returncode
    return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    sys.exit(main())
