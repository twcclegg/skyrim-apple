#!/bin/sh
# Builds the multiview probe against the pinned MoltenVK package (the same release the game uses), linking
# libMoltenVK.dylib directly as the Vulkan driver, with no loader, like macos/graphics/CMakeLists.txt does.
#
#   tools/multiview_probe/build.sh [output]        # default output: <repo>/build/multiview_probe
#
# MoltenVK package root (the directory that holds include/ and dynamic/), first match wins:
#   $SKYRIM_MOLTENVK_ROOT
#   <repo>/macos/deps/MoltenVK/MoltenVK
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

if [ -n "${SKYRIM_MOLTENVK_ROOT:-}" ]; then
    MVK="$SKYRIM_MOLTENVK_ROOT"
elif [ -f "$REPO/macos/deps/MoltenVK/MoltenVK/include/vulkan/vulkan.h" ]; then
    MVK="$REPO/macos/deps/MoltenVK/MoltenVK"
else
    echo "MoltenVK package not found; set SKYRIM_MOLTENVK_ROOT to the directory holding include/ and dynamic/" >&2
    exit 1
fi
LIBDIR="$MVK/dynamic/dylib/macOS"
for f in "$MVK/include/vulkan/vulkan.h" "$MVK/include/MoltenVK/mvk_deprecated_api.h" "$LIBDIR/libMoltenVK.dylib"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done

OUT="${1:-$REPO/build/multiview_probe}"
mkdir -p "$(dirname "$OUT")"

# .mm => Objective-C++ (needed only because MoltenVK's MVKPhysicalDeviceMetalFeatures uses Metal enum types).
# No ARC: the probe creates no Objective-C objects.
# The rpath lets the binary find @rpath/libMoltenVK.dylib (its install name) without DYLD_LIBRARY_PATH.
set -x
xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Wno-missing-field-initializers \
    -I"$MVK/include" \
    "$HERE/multiview_probe.mm" \
    -L"$LIBDIR" -lMoltenVK -Wl,-rpath,"$LIBDIR" \
    -o "$OUT"
