# Plain heap accesses in loop-free functions (`LIFT_PLAIN_ACYCLIC=1`)

Off by default. **Measured: no meaningful gain on the x86 corpus** (below). Kept as an opt-in so it can
be tried on the game with one environment variable; drop it if that shows nothing either.

## What it does

Guest memory other threads can see (everything but the thread's own stack) is read and written through
`TL8`..`TL32` and `TS8`..`TS32`. In the default arm64 build these are `volatile`, so the compiler keeps
every access the x86 code makes: it cannot hoist a flag test out of a spin loop, or merge or drop
accesses. With `LIFT_PLAIN_ACYCLIC=1`, a function with no cycle among its blocks, no computed jump
without a table, and no tail jump into lifted code uses `PL`/`PS` instead: plain loads and stores.

Why this keeps spin loops working: a function without a cycle cannot spin by itself. A loop elsewhere
that calls it stores a return address in guest memory on every iteration, so after the host compiler
inlines the callee into the loop, its loads still follow a store that may alias them and stay in the loop.
A tail jump into lifted code could be inlined into a loop that has no such store, so those functions are
excluded.

What it does not change: the hardware ordering. The default arm64 build was already not x86-ordered
(plain `ldr`/`str`), and the compiler may now also reorder accesses it can prove do not overlap, within
these functions. With `-DLB_STRICT_ORDER`, `PL`/`PS` are the acquire/release `TL`/`TS` again.

## Tests

- `x86corpus.py --modes plain,acyclic,slots-acyclic` runs the corpus with it on. These builds use
  `-DLB_GUEST_VOLATILE`, so the remaining accesses are volatile on any host, as on arm64. The 300-program
  corpus subset and the hand-written sets pass.
- `x86threads.py` runs two guest threads on shared memory: one spins until the other sets a flag, once by
  calling a loop-free function each time round and once in its own loop. Both finish with the option
  off and on. With `LIFT_PLAIN_ACYCLIC=all` (plain accesses everywhere, a deliberately wrong build), the
  spin in its own loop never ends, so the test can see the bug it guards against. The call-based spin
  still finishes: the host compiler inlines the loop-free callee into the loop and its load stays inside.

## Result

clang `-O2` corpus programs, generated code compiled for arm64 (static counts): 55% of heap accesses are
in functions that qualify, but instructions, loads and stores fall by under 0.5%, with or without stack-slot
promotion. The x86 compiler already removed the redundant loads it could see; what is left repeats exactly
those, and with every guest store possibly aliasing every other, the host compiler finds little more to
merge. The game may differ (MSVC is more cautious about aliasing than clang), which is the only reason
to try it there.
