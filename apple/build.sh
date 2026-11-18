#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PLATFORM=macos
CONFIGURATION=Release
UNSIGNED=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --platform) PLATFORM=${2:?Supply a platform}; shift 2 ;;
        --cloud) CONFIGURATION=Cloud; shift ;;
        --debug) CONFIGURATION=Debug; shift ;;
        --unsigned) UNSIGNED=1; shift ;;
        -h|--help)
            echo 'Usage: apple/build.sh --platform macos|ios|visionos|ios-simulator|visionos-simulator|tests [--cloud] [--unsigned]'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
BUILD_JOBS=${SKYRIM_BUILD_JOBS:-2}
case "$BUILD_JOBS" in 1|2) ;; *) echo 'Use one or two build jobs.' >&2; exit 2 ;; esac
case "$PLATFORM" in
    macos) SCHEME=SkyrimMac; SDK=macosx; DESTINATION='generic/platform=macOS' ;;
    ios) SCHEME=SkyrimIOS; SDK=iphoneos; DESTINATION='generic/platform=iOS' ;;
    visionos) SCHEME=SkyrimVision; SDK=xros; DESTINATION='generic/platform=visionOS' ;;
    ios-simulator) SCHEME=SkyrimIOS; SDK=iphonesimulator; DESTINATION='generic/platform=iOS Simulator'; UNSIGNED=1 ;;
    visionos-simulator) SCHEME=SkyrimVision; SDK=xrsimulator; DESTINATION='generic/platform=visionOS Simulator'; UNSIGNED=1 ;;
    tests) SCHEME=SkyrimLauncherTests; SDK=macosx; DESTINATION='platform=macOS,arch=arm64'; CONFIGURATION=Debug ;;
    *) echo "Unsupported platform: $PLATFORM" >&2; exit 2 ;;
esac
OUTPUT="$ROOT/apple/build/$PLATFORM-$CONFIGURATION"
# One DerivedData for app builds keeps disk use bounded; tests keep their own so a test run
# never contends for the app build database.
DERIVED="$ROOT/apple/build/DerivedData"
[ "$PLATFORM" = tests ] && DERIVED="$ROOT/apple/build/DerivedData-tests"
mkdir -p "$OUTPUT" "$ROOT/apple/Resources"
# This wrapper points at the local native preparation tools on macOS. Mobile
# bundles intentionally have no checkout path, desktop process bridge or payload.
printf '%s\n' "$ROOT" > "$ROOT/apple/Resources/checkout-path.txt"
# Regenerate icons only when the original artwork changes, so concurrent builds never read a
# half-written catalog image.
ARTWORK="$ROOT/apple/Artwork/SkyrimAdjacent-original.png"
CATALOG_IMAGE="$ROOT/apple/Resources/Common.xcassets/LauncherArtwork.imageset/Artwork.png"
if [ ! -f "$CATALOG_IMAGE" ] || [ "$ARTWORK" -nt "$CATALOG_IMAGE" ]; then
    swift "$ROOT/apple/prepare_assets.swift"
fi
xcodegen generate --use-cache --spec "$ROOT/apple/project.yml" --project "$ROOT/apple" >/dev/null
set -- -project "$ROOT/apple/SkyrimApple.xcodeproj" \
    -scheme "$SCHEME" -configuration "$CONFIGURATION" -sdk "$SDK" \
    -destination "$DESTINATION" -derivedDataPath "$DERIVED" \
    -jobs "$BUILD_JOBS" COMPILER_INDEX_STORE_ENABLE=NO
if [ "$UNSIGNED" -eq 1 ]; then
    set -- "$@" CODE_SIGNING_ALLOWED=NO
elif { [ "$PLATFORM" = macos ] || [ "$PLATFORM" = tests ]; } && [ "$CONFIGURATION" != Cloud ]; then
    set -- "$@" CODE_SIGN_IDENTITY=-
else
    set -- "$@" -allowProvisioningUpdates
fi
ACTION=build
[ "$PLATFORM" = tests ] && ACTION=test
if ! xcodebuild "$@" "$ACTION" > "$OUTPUT/build.log" 2>&1; then
    grep -E 'error:|failed|Failing|XCTAssert' "$OUTPUT/build.log" | head -60
    tail -40 "$OUTPUT/build.log"
    exit 1
fi
[ "$ACTION" = test ] && grep -E "Executed|passed|failed" "$OUTPUT/build.log" | tail -5
printf '%s %s (%s). Log: %s\n' "$ACTION" "$SCHEME" "$CONFIGURATION" "$OUTPUT/build.log"
