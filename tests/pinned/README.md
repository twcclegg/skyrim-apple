# Compiler checks for pinned guest registers

`LB_PINNED_REGS` keeps the guest registers in x21–x28 as global register variables (arm64). Three LLVM
code-generation passes move or speculate writes to reserved registers, so `macos/CMakeLists.txt` turns them
off for generated code. These tests check the compiler, not the game, and need no game files.

- `run.sh [compiler]`: `reserved_regs.c` holds the smallest shapes that showed each bug. Built with the
  pinned build's flags it must pass at `-O0` to `-O3` and `-Os`. It is then built without the flags and the
  script reports which bugs the installed compiler still has (informational).
- `fuzz.py [-n N] [-s SEED] [--cc CC] [--raw]`: random programs over the eight registers (branches, loops,
  writes inside conditions, calls), each built at `-O0` and at five optimised configurations; the printed
  final states must match. `--raw` leaves the flags out, to show that the test finds the known bugs.

Run both after changing the compiler, the Xcode version, or the flags. A failure in `fuzz.py` with the flags
on means another pass has the same flaw; keep the program (`--keep DIR`) and bisect the pass.
