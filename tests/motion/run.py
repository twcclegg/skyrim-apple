#!/usr/bin/env python3
"""Build and run the checks for runtime/motion_path.c (the camera's path between rendered frames).

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
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/motion-test")
    arguments = parser.parse_args()
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    binary = arguments.output_dir / "motion_test"
    build = [arguments.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-I", str(ROOT / "runtime"), str(ROOT / "runtime/motion_path.c"), str(ROOT / "tests/motion/test.c"),
             "-o", str(binary), "-lm", "-lpthread"]
    built = subprocess.run(build)
    if built.returncode:
        return built.returncode
    return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    sys.exit(main())
