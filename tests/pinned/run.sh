#!/bin/bash
# Checks that the compiler flags LB_PINNED_REGS uses (macos/CMakeLists.txt) give correct code for writes to
# reserved registers, at every optimisation level. Then reports, without failing, which of the known compiler
# bugs the installed compiler still shows when the flags are left out. arm64 only.
# usage: tests/pinned/run.sh [compiler]      (default: cc)
set -u
CC=${1:-cc}; D=$(cd "$(dirname "$0")" && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
[ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ] || { echo "skipped: not arm64"; exit 0; }
FIXED="-ffixed-x21 -ffixed-x22"
SAFE="-mllvm -disable-postra-machine-licm -mllvm -disable-early-ifcvt -mllvm -aarch64-enable-ccmp=false"
build() { $CC $FIXED "$@" "$D/reserved_regs.c" -o "$T/t" 2> "$T/err" || { cat "$T/err"; return 2; }; "$T/t" > "$T/out"; }
bad=0
for O in -O0 -O1 -O2 -O3 -Os; do
  # -aarch64-stress-ccmp makes the conditional-compare pass ignore its cost model; with the pass off it must not matter
  for X in "" "-mllvm -aarch64-stress-ccmp"; do
    if build $O $SAFE $X; then echo "ok    $O $X"; else echo "FAIL  $O $X"; cat "$T/out"; bad=1; fi
  done
done
echo "--- without the flags (informational: bugs the installed compiler still has)"
for X in "-O2" "-O2 -mllvm -aarch64-stress-ccmp"; do
  build $X; r=$?
  if [ $r = 0 ]; then echo "clean $X"; else echo "buggy $X"; sed 's/^/        /' "$T/out"; fi
done
exit $bad
