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

## x86 corpus and the stack-slot builds

`x86corpus.py` (rules in `X86SPEC.md`) compares each program's x86 code under Unicorn with the lifted code
in several builds, chosen with `--modes`:

| mode | lifter | build |
|---|---|---|
| `plain` | default | default |
| `pinned` | default | `LB_PINNED` (arm64 hosts only) |
| `slots` | `LIFT_SLOTS=1` | default |
| `slots-check` | `LIFT_SLOTS=1` | `-DLB_SLOT_CHECK`: promoted slots mirrored in guest memory and compared |
| `slots-pinned` | `LIFT_SLOTS=1` | `LB_PINNED` (arm64 hosts only) |

The default is every mode the host can run. `docs/STACK_SLOTS.md` describes the promotion.

```sh
macos/.venv/bin/python tests/aot/x86corpus.py tests/aot/x86examples tests/aot/x86stack
macos/.venv/bin/python tests/aot/x86gen.py /tmp/gen 300 1 && macos/.venv/bin/python tests/aot/x86corpus.py /tmp/gen
```

- `x86stack/` holds hand-written programs aimed at the stack: callees that overwrite or take the address
  of their arguments, variadic callees, calls through tables in mixed conventions, narrow locals, 64-bit
  spills, recursion, run-time-sized frames, structs by value, and (`msvc_shapes.c`, in assembly with a C
  twin for the host check) the shapes MSVC emits that clang and gcc do not.
- `x86gen.py OUTDIR [COUNT] [SEED]` writes reproducible random programs of the same kind;
  `x86gen.py --float` writes float-heavy ones.
- `x86float/` holds float programs. The emulator evaluates x87 in 80 bits and the lifter in 64, so run
  them with `--reference plain`: every build is compared with the plain lifted build, bit for bit.
  `--x86-flags="-msse -mfpmath=sse"` puts the floats in SSE registers instead.
- The runner also works on Linux, where the lifted code is built for the host instead of arm64:
  `--cc clang --native-cc gcc` if clang has no sanitiser runtime, and `--x86-cc gcc`,
  `--x86-flags=-mrtd` (callee-popped arguments), `--x86-flags=-fno-omit-frame-pointer` and the like
  to get other stack shapes from the same sources.
- `X86CORPUS_REPS=n` in the environment of a built `run-<mode>` binary repeats each call n times, for
  timing one build against another.
