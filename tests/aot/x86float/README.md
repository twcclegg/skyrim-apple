# Float programs for the stack-slot builds

Like `../x86stack/`, but with `float` and `double` arithmetic, so the compiler spills floats to the stack
through x87 (`fld`/`fstp`, `fnstcw`/`fldcw`, `fistp`) or SSE (`movss`, `movsd`) instructions. The lifter
evaluates x87 in 64-bit doubles while x86 uses 80 bits, so these are not compared with the emulator; every
lifted build must instead match the plain lifted build exactly:

```sh
macos/.venv/bin/python tests/aot/x86corpus.py --reference plain tests/aot/x86float
macos/.venv/bin/python tests/aot/x86corpus.py --reference plain --x86-flags="-msse2 -mfpmath=sse" tests/aot/x86float
```

The rules of `../X86SPEC.md` apply except rule 3: floats are allowed (still no 64-bit division).
