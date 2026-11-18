#!/bin/sh
# Local-only Python/tool dependency preparation; no game downloads or global installs.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test "$(uname -s)" = Darwin && test "$(uname -m)" = arm64 || {
    echo "Native Apple Silicon macOS is required." >&2; exit 2;
}
command -v cmake >/dev/null
command -v ninja >/dev/null
xcrun --find clang >/dev/null
if [ ! -f "$ROOT/third_party/dxvk/CMakeLists.txt" ]; then
    git -C "$ROOT" submodule update --init third_party/dxvk
fi
python3 "$ROOT/tools/fetch_sdl.py"
if [ ! -x "$ROOT/macos/.venv/bin/python" ]; then
    python3 -m venv "$ROOT/macos/.venv"
fi
"$ROOT/macos/.venv/bin/python" -m pip install -r "$ROOT/macos/requirements.txt"
python3 "$ROOT/macos/graphics/fetch_dependencies.py"
python3 "$ROOT/macos/audio/build_ffmpeg.py" macos
python3 "$ROOT/macos/audio/fetch_samples.py"
"$ROOT/macos/.venv/bin/python" "$ROOT/macos/directx/fetch.py"
