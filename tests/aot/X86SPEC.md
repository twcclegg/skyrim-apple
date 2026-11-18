# x86 corpus programs for the translation test

Each program is one C file. `x86corpus.py` compiles it to 32-bit x86 at four optimisation levels, runs that
x86 under an emulator (the reference), translates it with the real lifter, and runs the translation with and
without pinned registers. All three must return the same value and leave the same data. The same source is
also built for the host with sanitisers, which rejects undefined behaviour.

## Rules

1. Define `uint32_t test_main(uint32_t a, uint32_t b)`. It is called once per input pair, each time from a
   fresh copy of the program's data. Its return value and the final contents of your global variables are
   what is compared, so make both depend on as much of the computation as possible.
2. No `#include` at all. `x86prog.h` is included for you: it provides `uint8_t … uint64_t`, `int8_t … int64_t`,
   `size_t`, and `memcpy`, `memset`, `memmove`. There is no other library: no `printf`, `malloc`, `strlen`.
3. Integers only: no `float`, `double`. No 64-bit division or modulo (`uint64_t` add, subtract, multiply
   and shifts are fine). Never divide or take a remainder by a value that can be zero, and never divide
   `INT32_MIN` by -1.
4. Signed arithmetic wraps (the build uses `-fwrapv`), but shift counts must be in range: mask them
   (`x << (n & 31)`), and do not left-shift negative values.
5. Stay inside your arrays. Index with masked or range-checked values. Initialise every local before use.
6. No casts between pointers and integers, and nothing that depends on the size of a pointer or `long`
   (the host check runs the same source as 64-bit code). Pointers into your own globals and locals,
   pointer arithmetic within an array, structs, unions of integers, bitfields, function pointers and
   recursion (at most a few hundred frames) are all welcome.
7. Bounded work: at most about 200,000 loop iterations per call.
8. 40 to 200 lines. Use several functions; mark at least two `__attribute__((noinline))` so real calls
   remain. A comment at the top says in one line what the program models.

Variety is the point: the goal is to make the compiler emit many different x86 instruction patterns and
calling shapes. Use `switch`, nested loops, early returns, byte and 16-bit data, signed comparisons,
carries across 64-bit values, tables of structs, callbacks.

## Checking

```sh
macos/.venv/bin/python tests/aot/x86corpus.py path/to/dir
```

`REJECT`: the program breaks a rule (the reason is printed); fix it. `UNLIFTED`: the translator does not
support an instruction the compiler chose; leave the file, it is a coverage note. `MISMATCH`: the
translation computed something different from the x86 original; **do not change the program**, report it.
`x86examples/sort_and_sum.c` is a complete example.
