# Corpus programs for the pinned-register compiler test

Each program is one self-contained C file that uses eight global register variables (`eax ecx edx ebx esp ebp
esi edi`, 32-bit unsigned, living in x21–x28) the way machine-translated x86 code does: the registers are the
working state, functions pass values through them, and they are read and written inside branches, loops and
conditions. `gate.py` builds each program unoptimised and optimised and compares the printed results, so a
program never states its own expected output.

## Rules

1. Start with `#include "pinned_test.h"`. Use nothing else except `<stdint.h>`, `<stdio.h>`, `<string.h>`,
   which it already includes. No other headers, no files, no time, no randomness, no input.
2. `main` is: `PIN_BEGIN();` then at least four scenarios, then `PIN_END(); return 0;`. A scenario sets the
   registers with `SET_REGS(a, c, d, b, sp, bp, si, di)`, optionally fills `mem`, calls the code under test and
   prints with `REPORT("name", value)`. Vary the inputs so different branches run: zero, one, large,
   all-bits-set, loop counts of 0, 1 and many.
3. Every call to one of your functions goes through `CALL(f(...))` (when you use the result) or
   `CALLV(f(...))`. This includes calls inside other functions, recursive calls and calls through function
   pointers. Never call a function bare. Do not call library functions except in `main` via `REPORT`.
4. Only unsigned 32-bit arithmetic (`uint32_t`; `uint64_t` for wide multiplies). Shift counts must be masked
   (`x << (n & 31)`). No signed overflow, no division or modulo by a value that can be zero, no pointers into
   anything but `mem`, and index `mem` only through `M(i)` or a constant below `MEM_WORDS` (256).
5. Every local variable is initialised where it is declared. No `static` locals, no globals of your own
   except `const` tables and function-pointer tables.
6. Every loop must provably end: bound it by a constant or by a masked value (`n & 63`), 10,000 iterations
   at most in total per scenario.
7. Mark at least two functions `__attribute__((noinline))`. Other functions may be `static` or `static inline`.
8. The point of the test is register traffic in control flow. Each program needs, somewhere:
   - a register written under an `if` whose condition is sometimes false,
   - a register read in a loop and then overwritten later in the same loop,
   - a register written inside a condition, like `if (a == 5 && (edx = b, ecx > b))`,
   - a register written before a call and read after it, and one written by the callee and read by the caller.
9. 40 to 150 lines. A comment at the top says in one line what the program models.

## Checking

```sh
python3 tests/pinned/gate.py path/to/dir
```

`REJECT` means the program breaks a rule; fix it. `MISMATCH` means the compiler produced different results at
different optimisation levels with the pinned build's flags: **do not change the program**, report it.
`examples/checksum_loop.c` is a complete example.
