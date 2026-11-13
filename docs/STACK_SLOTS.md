# Stack slots as C locals (`LIFT_SLOTS=1`)

Off by default. Set `LIFT_SLOTS=1` when running `tools/lift.py` to turn it on; the generated code needs
nothing else. On the game it runs the Whiterun and boss-fight benchmark scenarios to the end, also in the
`-DLB_SLOT_CHECK` build (see "Testing" and "On the game").

## What it does

The plain translation keeps the guest stack in guest memory. `mov eax, [esp+0x1C]` is
`eax = M32(esp + 0x1C)`, `push esi` is a store, `pop esi` is a load. The C compiler cannot improve on that:
the stack is part of the same 4 GiB array as everything else, so any guest store in between may have
changed the value and it has to be loaded again.

With `LIFT_SLOTS=1`, in functions whose stack address never leaves them, the slots a function reads back
become C locals:

```c
/* push ebx; push esi; sub esp, 8; mov [esp+4], ecx; ...; mov eax, [esp+4]; ...; pop esi; pop ebx */
const uint32_t fb_ = esp; SL_INIT(32, sl_4, -4); SL_INIT(32, sl_8, -8); SL_INIT(32, sl_C, -12);
{ uint32_t v_ = ebx; esp -= 4; SL_W(32, sl_4, -4, esp, (v_)); }      /* sl_4 = ebx */
{ uint32_t v_ = esi; esp -= 4; SL_W(32, sl_8, -8, esp, (v_)); }
esp = sub32(esp, 0x8u, &f);
SL_W(32, sl_C, -12, (uint32_t)(esp + 0x4u), (ecx));                   /* sl_C = ecx */
...
eax = SL_R(32, sl_C, -12, (uint32_t)(esp + 0x4u));                    /* eax = sl_C */
```

`sl_C` is the cell 0xC bytes below the value esp had on entry (`fb_`). In a normal build `SL_R` and `SL_W`
are the local and an assignment to it; the offset and address arguments are only used by the checking
build. `esp` itself still moves exactly as before, because callees and everything that stays in memory
depend on it.

What stays in guest memory, in every function: return addresses, outgoing arguments, incoming arguments,
the saved `ebp` of a frame-pointer function (so the guest's `ebp` chain stays walkable), anything touched
by locked instructions or by a use of its address other than a plain load or store (`memcpy` of a 16-byte
SSE value, an 80-bit x87 load), and any slot the function writes but never reads back.

Float locals are promoted too. An x87, SSE or MMX load or store of a stack slot (`fld dword [esp+10h]`,
`fstp qword [esp+8]`, `fnstcw`, `fistp`, `movss`, `movq`) reads or writes the cell's local, which holds
exactly the bytes guest memory would hold: `fr0 = (double)sl_f32(SL_R(32, sl_28, -40, ...))` and
`SL_W(32, sl_2C, -44, ..., sl_u32((float)fr0))`. The `sl_f32`/`sl_u32` bit casts compile to register
moves or nothing, so a float that x87 code spills and reloads can stay in a register.

## How a function qualifies (`tools/stackslots.py`)

1. **Its stack address stays inside it.** `esp` may only be used as the base of a plain `[esp+N]` operand,
   be moved by a constant, and be copied to `ebp` as a frame pointer (`mov ebp, esp`); `ebp`, while it is
   the frame pointer, only as the base of `[ebp+N]` and to restore `esp`. Anything else rejects the whole
   function: `lea reg, [esp+N]`, `mov reg, esp`, an indexed `[esp+reg*4]`, `push esp`, `mov fs:[0], esp`
   (every function with a C++ exception frame), `pushad`, `setjmp`.
2. **The height of `esp` is known at every stack access.** The analysis follows pushes, pops and constant
   adjustments through the function's blocks and requires paths to agree where they join. What a call adds
   to `esp` on return comes from:
   - the callee's own `ret N`, when it is a lifted function. If the callee's balance was proven by the same
     analysis the result is used as is; if it was only read off its `ret` instructions, the call gets a
     guard (below);
   - otherwise (a call through a pointer or table, an import, a host replacement) it is an unknown, solved
     from the function itself: `esp` must be back at the return address at every `ret`, and equal on both
     sides of a join. Callee pops are non-negative, so a sum of them that must be zero makes each zero.
     A solved value also gets a guard. If the unknowns cannot be solved, the function is not promoted.
3. **Each cell is used simply.** Accesses are grouped into cells of bytes that are used together. A cell is
   promoted if it is 1, 2, 4 or 8 bytes; every access is a plain integer operand, a push or a pop, or a
   plain load or store by an x87, SSE or MMX instruction (the lifter records this while translating the
   function once, then translates it again; for instructions other than integer operands it looks at the
   text the instruction produced, and any use of the address other than `M8`..`M64`/`MF32`/`MF64` loads
   and plain assignments keeps the cell in memory); it is read back by name
   somewhere in the function, or is a register save (pushed from and popped into the same one of `ebx`,
   `esi`, `edi`, `ebp`); and on every path each read follows a write of the whole cell. A call ends the
   life of anything below `esp` at that moment.
4. **No lifted callee reaches it.** For each direct call, the bytes the callee touches at and above its
   return address are known when the callee itself passed step 1 and 2; any cell in that range stays in
   memory.

## What is proven and what is assumed

Proven, given that the x86 code runs correctly to begin with: the stack heights (rule 2), that no pointer
to the frame exists outside the function (rule 1), and that a promoted cell's value never depends on what
memory held before the function wrote it (rule 3).

Checked at run time: after every call whose effect on `esp` was solved or read off the callee's `ret`
rather than proven, and after every call made while `ebp` is the frame pointer, `SL_GUARD` compares `esp`
(and `ebp`) with where the analysis placed them and stops with `rt_unimpl` at the call's address if they
differ. One compare and a never-taken branch per such call. (When the callee's pop is not known at all and
the function only continues through `ebp`, as in `call x; leave; ret`, only `ebp` is checked.) The guard is
a hard stop, not a fallback: if the analysis is wrong about a callee in the game, the game stops at that
call with its address rather than running on with misplaced locals.

Assumed, not checked in a normal build:

- **A callee that is not a lifted function with a known reach reads only the stack bytes its caller
  wrote as arguments and never reads back.** That covers calls through pointers, imports, and lifted
  callees that take the address of their own arguments. The case it would get wrong: the caller pushes an
  argument, the callee reads it, and the caller later reads the same slot by name or pops it back into the
  callee-saved register it was pushed from. Compilers do not emit that (the slot belongs to the callee after
  the call), and where the callee is a lifted function with a known reach it is checked instead of assumed
  (`p7` in `tests/aot/x86stack/msvc_shapes.c`).
- **Nothing reads another thread's stack or a suspended frame's slots** (a garbage collector scanning
  stacks, an unwinder). The runtime treats C++ exceptions and `RtlUnwind` as fatal, so no unwinder runs.

## The checking build (`-DLB_SLOT_CHECK`)

Compile the generated code with `-DLB_SLOT_CHECK` and every promoted cell is also kept in guest memory,
XORed with a key. Each read compares the local with memory, and each access compares the address the x86
operand names with the cell's address. This catches a wrong stack height, anything that writes a promoted
cell behind the function's back, and (because memory holds a keyed value) a callee that reads one, which
then computes garbage instead of quietly working. It is the build to soak the game with before trusting
the normal one. A failure prints the function, both addresses and both values, and aborts.

## Output of the lifter

`<module>_slots.json` next to the generated code, and a one-line summary on stdout:

- `promoted_functions`, `cells`, `accesses_promoted`: what was done (accesses are counted statically);
- `accesses_kept_in_promoted_functions`: stack accesses left in memory in those functions, by reason;
- `functions_not_promoted`: every other function, by the first reason found.

The reasons to look at on the game are `callee pop unknown` and `stack address taken`. The first is the
limit of rule 2: an `esp`-based function with two calls through pointers whose pops cannot be told apart,
or an `ebp`-based one that pops its saved registers after such a call. A guess checked by the existing
guard (callee pops what was pushed for it, or nothing) would recover those; it is left out until the
numbers say it matters. The second includes every function with an exception frame; treating
`mov fs:[0], esp` as harmless (no unwinder runs) would recover those.

## Testing

`tests/aot/x86corpus.py` runs each program as x86 under Unicorn and as lifted code in several builds; it now
also lifts with `LIFT_SLOTS=1` and runs that as `slots`, `slots-check` and, on arm64, `slots-pinned`.
`tests/aot/x86stack/` holds hand-written programs aimed at the stack, including MSVC-style shapes in
assembly; `tests/aot/x86gen.py` generates any number of stack-heavy programs. See `tests/aot/README.md`.

Float programs (`tests/aot/x86float/`, `x86gen.py --float`) cannot be compared with the emulator, which
evaluates x87 in 80 bits where the lifter uses 64. `x86corpus.py --reference plain` compares every other
build with the plain lifted build instead, bit for bit.

## Results so far (x86 corpus on an x86-64 Linux host, 2026-11-12)

Correctness: every program agrees with the Unicorn reference in the `plain`, `slots` and `slots-check`
builds. 300 generated programs (`x86gen.py`, seed 2) at `-O0`, `-O1`, `-O2` and `-Os` with clang; the
hand-written `x86stack/` set and `x86examples/` at all four levels with clang and gcc; and subsets of 50
generated programs under gcc `-O1/-O2`, gcc with `-mpush-args -mno-accumulate-outgoing-args`,
`-mrtd` (callee-popped arguments, clang and gcc) and `-fno-omit-frame-pointer`. `pinned` and
`slots-pinned` need an arm64 host and have not run; the promoted code compiles for arm64 with and without
pinned registers.

The corpus catches a broken rule. With one rule removed at a time (21 programs, `-O1/-O2`):

| rule removed | result |
|---|---|
| stack address escapes (rule 1) | 14 of 21 programs wrong |
| outgoing arguments kept in memory (rule 3) | 8 wrong, one crashes |
| a lifted callee's reach (rule 4) | `msvc_shapes.c` wrong; `slots-check` names the function and cell |
| guard expects the wrong height | 6 stop at the guard |
| define before use (rule 3) / a call ends what is below `esp` | no difference |

The last two rules only change what a read of never-written stack memory returns, which compiled code
does not depend on; they are kept as a margin and the corpus cannot exercise them.

Effect, clang `-O2` generated programs, generated code compiled for arm64 (static counts, compile only):
5% fewer instructions, 10% fewer loads, 9% fewer stores; with pinned registers 3%, 12%, 10%. Run time of
the same lifted code on the x86-64 host: −10% at `-O2`, −7% at `-Os`, −13% at `-O0` (three interleaved runs each, identical
output). About two thirds of functions are promoted at `-O2` and 40–58% at the other levels; nearly all
the rest take a stack address. These are compiler-generated
test programs, not MSVC output and not the game: the share of the game's functions that qualify is not
known until the lifter runs over it (see `exe_slots.json`).

Floats: 60 generated float programs plus the three in `x86float/`, built with clang using x87, clang
using SSE for floats (`-msse -mfpmath=sse`) and gcc, all match the plain lifted build in the `slots` and
`slots-check` builds. Storing a float local by value conversion instead of its bits makes all 63 fail.
At clang `-O2` the promoted code has about 18% fewer arm64 instructions, 17–18% fewer loads and 21–24%
fewer stores (x87 and SSE alike; gcc's code 10–12%), and runs 19% faster on the x86-64 host. (The lifter
has no `cvtss2sd`, so SSE2 doubles are not tested; the game evidently does not use it.)

## Not done

- Incoming arguments (`[esp+4]` and up) stay in memory.
- 80-bit x87 values (`fld tbyte`) and 16-byte SSE values stay in memory.
- Functions rejected for the reasons above.
- No measurement on the game or on arm64 hardware.

## On the game

M1 MacBook Air, macOS build with pinned registers, slots off against on, alternating order, work ms per
frame (mean of the runs, with the range).

| Scenario, phase | Off | On | Change | Runs each |
|---|---|---|---|---|
| Whiterun, overall | 8.95 (8.94-8.95) | 8.90 (8.83-8.97) | -0.5% | 2 |
| Boss fight, overall | 11.55 (11.06-12.14) | 11.45 (11.19-11.71) | -0.9% | 6 |
| Boss fight, calm (before combat) | 8.57 (8.48-8.83) | 8.70 (8.36-9.37) | +1.5% | 6 |

**No measurable gain.** Every difference is smaller than the spread between runs of the same build. An
earlier, shorter comparison (three boss-fight runs each) showed 7.5% less work with slots on; six runs each
did not reproduce it, so that was run-to-run variation. Not run on a headset.

Two faults showed only on the game and are fixed: a local's cell reused as an outgoing argument after the
frame was released, and a jump table that leaves the function with the frame up (the CRT's `memmove`). Both
have shapes in `tests/aot/x86stack/msvc_shapes.c`.
