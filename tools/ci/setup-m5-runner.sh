#!/bin/sh
# Sets up this Mac as the private self-hosted runner for twcclegg/skyrim-apple (see docs/CI.md).
#
# Run it as the dedicated, non-admin CI user (mac-ci), not as root and not as anyone's own account.
# It writes only inside that user's home: ~/actions-runner and ~/skyrim-ci. No Homebrew, no sudo, no
# system settings. It never asks for or stores a GitHub token: you register the runner yourself afterwards
# with the command it prints.
#
# Usage: tools/ci/setup-m5-runner.sh /path/to/your/Skyrim
#   The game folder must hold TESV.exe, binkw32.dll, the Data folder, and also TESV_unpacked.exe,
#   d3dx9_42.dll and X3DAudio1_7.dll, so generation needs neither .NET nor cabextract.
set -eu

RUNNER_VERSION=2.338.2
RUNNER_SHA256=25a065af409ffa317ab4d5a3b614dd15fc9c40f2172bc1b35a1b1258e919a388
REPO_URL=https://github.com/twcclegg/skyrim-apple
LABELS=skyrim-private

GAME=${1:-}
stop() { echo "STOPPED: $*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || stop "don't run this as root."
[ "$(uname -s)/$(uname -m)" = Darwin/arm64 ] || stop "needs an Apple Silicon Mac."
if id -Gn | tr ' ' '\n' | grep -qx admin; then
  stop "$(id -un) is an administrator. Use the non-admin CI user (mac-ci)."
fi
[ -n "$GAME" ] || stop "usage: $0 /path/to/your/Skyrim"
GAME=$(cd "$GAME" && pwd)
for f in TESV.exe TESV_unpacked.exe binkw32.dll d3dx9_42.dll X3DAudio1_7.dll Data; do
  [ -e "$GAME/$f" ] || stop "$GAME has no $f."
done
xcode-select -p >/dev/null 2>&1 || stop "no Xcode or Command Line Tools selected (the owner decides; this script won't install them)."
xcodebuild -version >/dev/null 2>&1 || stop "full Xcode is needed for the visionOS build (xcodebuild failed)."
xcrun --sdk xros --show-sdk-path >/dev/null 2>&1 || stop "Xcode has no visionOS SDK installed."
python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' || stop "python3 3.10 or newer is needed."
PYDIR=$(dirname "$(command -v python3)")

CI="$HOME/skyrim-ci"
RUNNER="$HOME/actions-runner"
mkdir -p "$CI/gen" "$CI/tools"

echo "== cmake and ninja into $CI/tools (pip, user-local)"
[ -x "$CI/tools/bin/python" ] || python3 -m venv "$CI/tools"
"$CI/tools/bin/python" -m pip install --quiet --upgrade pip
"$CI/tools/bin/python" -m pip install --quiet 'cmake>=3.28,<5' 'ninja>=1.11'
"$CI/tools/bin/cmake" --version | head -1

echo "== GitHub Actions runner $RUNNER_VERSION into $RUNNER"
mkdir -p "$RUNNER"
if [ ! -x "$RUNNER/config.sh" ]; then
  TARBALL="$RUNNER/actions-runner-osx-arm64-$RUNNER_VERSION.tar.gz"
  curl -fsSL -o "$TARBALL" \
    "https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/actions-runner-osx-arm64-$RUNNER_VERSION.tar.gz"
  echo "$RUNNER_SHA256  $TARBALL" | shasum -a 256 -c - || stop "runner download checksum mismatch."
  tar xzf "$TARBALL" -C "$RUNNER"
  rm "$TARBALL"
fi

# The runner reads .env into every job's environment.
cat > "$RUNNER/.env" <<ENV
PATH=$CI/tools/bin:$PYDIR:/usr/bin:/bin:/usr/sbin:/sbin
SKYRIM_CI_GAME_DIR=$GAME
SKYRIM_CI_HOME=$CI
ENV

cat <<NEXT

Setup done. Nothing is registered yet.

1. In a browser: $REPO_URL/settings/actions/runners/new  (choose macOS, ARM64).
   Copy only the token from the "Configure" step.
2. On this Mac, as $(id -un):
     cd $RUNNER
     ./config.sh --url $REPO_URL --labels $LABELS --name m5-air --unattended --token <paste token>
3. To lend the Mac to CI, run in a terminal and leave it open (Ctrl-C stops it):
     cd $RUNNER && ./run.sh
   Queued jobs start as soon as it connects. Keep the Mac on AC power with the lid open while it runs.

To remove everything later: ./config.sh remove --token <a removal token>, then delete $RUNNER and $CI.
NEXT
