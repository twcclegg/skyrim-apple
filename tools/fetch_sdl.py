#!/usr/bin/env python3
"""Fetch the pinned SDL 2.30.9 release into third_party/ (ignored by git).

  third_party/SDL2          the release's source tree, unmodified
  third_party/sdlinc/SDL2   a copy of its headers, for code that includes <SDL2/SDL.h>

The archive is accepted by SHA-256 only (ipad/pinned.py). Platform changes are applied to a copy at build time
(simulator/prepare_sdl.py), never to this tree. Run it directly, or call ensure() from a build script.
"""
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ipad"))
import pinned  # noqa: E402

VERSION = "2.30.9"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL2-{VERSION}.tar.gz"
SHA256 = "24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4"
THIRD_PARTY = ROOT / "third_party"
SOURCE = THIRD_PARTY / "SDL2"
INCLUDE = THIRD_PARTY / "sdlinc"
STAMP = SOURCE / ".pinned-sha256"


def ensure(archive=None):
    """Return the SDL source directory, fetching and extracting it if missing or from another archive."""
    if STAMP.is_file() and STAMP.read_text().strip() == SHA256 and (INCLUDE / "SDL2/SDL.h").is_file():
        return SOURCE
    tarball = pinned.obtain(URL, SHA256, THIRD_PARTY / "downloads" / f"SDL2-{VERSION}.tar.gz", archive)
    with tempfile.TemporaryDirectory(dir=THIRD_PARTY) as scratch:
        with tarfile.open(tarball) as tar:
            for member in tar.getmembers():
                if member.name.startswith(("/", "..")) or "/../" in member.name or not (member.isfile() or member.isdir()):
                    raise ValueError(f"Unexpected entry in {tarball}: {member.name}")
            tar.extractall(scratch)
        extracted = Path(scratch) / f"SDL2-{VERSION}"
        if not (extracted / "include/SDL.h").is_file():
            raise ValueError(f"{tarball} doesn't contain SDL2-{VERSION}/include/SDL.h")
        for path in (SOURCE, INCLUDE):
            if path.exists():
                shutil.rmtree(path)
        shutil.copytree(extracted / "include", INCLUDE / "SDL2")
        extracted.rename(SOURCE)
    STAMP.write_text(SHA256 + "\n")
    return SOURCE


if __name__ == "__main__":
    print(ensure(sys.argv[1] if len(sys.argv) > 1 else None))
