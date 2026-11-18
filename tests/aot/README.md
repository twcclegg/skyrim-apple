# Game-free AOT fixtures

Run on an Apple Silicon Mac using the project's isolated Python environment:

```sh
macos/.venv/bin/python tests/aot/run.py
macos/.venv/bin/python tests/aot/run.py --strict-order --output-dir macos/build/aot-fixtures-strict
```

The runner authors a small PE32 file with known x86 instruction sequences,
passes it and a function map through the real `tools/lift.py`, compiles the
emitted C with Apple Clang for ARM64, and executes it through the production
runtime dispatcher. No game binary, assets, Wine, Rosetta, Unicorn, CPU JIT,
or runtime code generation is used. Python requires the same `capstone` and
`pefile` packages as the upstream lifter.

Assertions cover arithmetic flags, unaligned memory, a backward branch,
cdecl and stdcall arguments, direct and cached indirect calls, packed SSE,
x87 addition and rounding, atomic compare/exchange, overlapping forward
string copies, and Windows FS-relative access. Expected results are defined
independently in `harness.c`.

Three boundary cases cover the places where control crosses between lifted and host code: a host shim
called from lifted code that itself runs lifted code before returning, `setjmp`/`longjmp` across a lifted
frame, and four host threads running lifted code at once.

`--pinned` builds the lifted code the way `LB_PINNED_REGS` builds the game (guest registers in x21–x28,
the three unsafe LLVM passes off) and must give the same results:

```sh
macos/.venv/bin/python tests/aot/run.py --pinned --output-dir macos/build/aot-fixtures-pinned
```

Two additional subprocess checks execute generated functions containing an
unknown indirect call and an unsupported software interrupt. Both must reach
the production runtime's explicit fatal path; no translator fallback is used.

The optional strict-order run enables the runtime's acquire/release guest
memory accesses. This does not prove multicore x86 ordering for the game.

Generated PE/C, logs, the ARM64 executable, and a hashed JSON receipt remain
under the chosen output directory, outside the game's ignored `gen/` tree.
These tests prove the selected synthetic paths compile and execute; they
do not establish Skyrim boot, completeness, fidelity, or performance.
