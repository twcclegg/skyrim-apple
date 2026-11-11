# Lifted-code performance log

One entry per change: hypothesis, diff summary, difftest result, before/after numbers, verdict. Work is on the
local branch `perf/lifted-arm64`. Numbers are from the M1 (MacBook Air, 8 cores) unless marked otherwise.
Private inputs (`gen/`, game files, logs, screenshots) stay in `~/SkyrimAVP/` and `bench-results/`, both outside
git.

## How things are measured

- **Static:** one generated file (`exe_14.c`, 978 functions) compiled with the build's flags, instructions
  counted from `llvm-objdump -d`. Cheap, and shows what the compiler did, but says nothing about which code is hot.
- **Runtime:** `tools/bench/bench.py run <scenario> --mac <build dir> --game <game dir>` on the macOS native
  build. The Mac build now logs `ft <frame> <frame ms> <Present ms> <work ms>`; *work* is the frame time without
  the 60 fps pacing sleep and without D3D9 `Present`, i.e. the game's own CPU time on the presenting thread. The
  60 fps cap stays on, so both sides of a comparison play the same scene.

## 2026-11-10 — setup

- `gen/` (manifest sha 5b68dc33…) and the game were already on the M1 in the planning session's scratch folder;
  cloned to `~/SkyrimAVP/` (APFS clones). Mint was not contacted.
- macOS build bootstrapped (`macos/bootstrap.sh`; the DirectX fetch step needs `cabextract`, which isn't
  installed and isn't needed because the game folder already has both DLLs).
- `macos/CMakeLists.txt`: `SKYRIM_GAME_DIR` (the manifest records a path on another volume), `LB_CONST_MEM`,
  `SKYRIM_GEN_FLAGS`. `macos/build_receipt.py`: `begin`/`seal` take `--game-dir`.
- `runtime/platform_sdl.c`: frame pacing log, `LB_FRAMETIMES` and `LB_EXIT_FRAME` on the Mac build too.
  `runtime/d3d9_bridge.cpp` reports the pacing sleep so the work time can be logged.
- `tools/bench/bench.py`: `--mac BUILD_DIR --game DIR` target; summaries and comparisons include work time.

## Static findings (exe_14.c, `-O2 -fno-strict-aliasing -fwrapv`, arm64)

| Variant | Instructions | Loads | Guest-base reloads | Acquire/release |
|---|---|---|---|---|
| baseline (`LB_STRICT_ORDER`) | 569,717 | 177,401 | 64,181 | 18,005 |
| + `LB_CONST_MEM` | 507,596 (−10.9%) | 118,454 | 4,460 | 18,051 |
| + `CPU *restrict c` | 506,565 (−0.2% more) | 117,944 | 4,460 | 18,051 |
| `LB_CONST_MEM`, `LB_STRICT_ORDER` off | 419,422 (−26.4%) | 106,312 | 4,437 | 0 |

- **Guest base reloads confirmed** (brief candidate 1): `MEM` is reached through the GOT and reloaded after
  almost every guest store, 11.3% of all instructions.
- **`restrict` on the CPU pointer doesn't help** (0.2%): the remaining CPU-struct traffic is spills and reloads
  around calls, which `restrict` can't remove. Dropped.
- **Strict ordering is expensive statically** (another 17%): each `TL32`/`TS32` carries an alignment test, a
  branch and a fenced slow path. Tried removing the test: unaligned `ldapr`/`stlr` raise SIGBUS on the M1
  (even inside one cache line), so the test has to stay while strict ordering is on.

## 2026-11-10 — where the time goes (M1, Riverwood, 1280×720 low preset)

Scenario `riverwood-walk-mac`: `coc riverwood` from the main menu, stand 10 s (*idle*, frames 2400–3000), run
forward 20 s (*run*, 3000–4200). The idle phase repeats within about 1%; the run phase varies by about 5%, so
verdicts use idle and report run alongside.

- **The built-in profiler misleads on macOS.** Its SIGPROF samples land mostly on threads in system calls: it
  reported 53% `__psynch_cvsignal` in the world, where a call-stack sample (`sample <pid> 10 1`) shows 31 of
  ~3,000 main-thread samples there. Turning it off (`noprof.txt`) changed nothing measurable (idle work
  10.95 / 11.68 / 11.39 ms against 11.36 / 11.39 / 11.49).
- **Main thread, on-CPU time by where it is spent** (one 10 s sample while standing in Riverwood):
  lifted code 79%, profiler signal handler 7%, locks 4%, indirect-call dispatch 2%, DXVK's D3D9 front end about
  3%. Two worker threads run lifted code about a third of the time; `dxvk-submit` and `dxvk-cs` use 14% and 9%
  of a core. On the M1 the frame is limited by the main thread's lifted code, not by graphics.
- **x87 is the hot part.** 48% of the main thread's lifted-code samples are in functions that use x87, and one
  function, `0x472580` (a plane/sphere test, called from culling), has 23% by itself. SSE register access is
  0.3% of sample-weighted lines, so SSE registers as locals (brief candidate 3) is not worth doing.
- During loading the first 30 s of CPU go mostly to `swtch_pri` (`Sleep(0)`/`sched_yield` spinning), not seen
  once in the world.

## 2026-11-10 — constant guest base (`LB_CONST_MEM`)

- **Hypothesis:** every guest store may alias the global `MEM`, so it is reloaded all the time.
- **Change:** `runtime/x86.h` declares `extern uint8_t *const MEM` when `LB_CONST_MEM` is defined; the generated
  code's target defines it, the runtime (which assigns `MEM` once in `main`) does not. No regen.
- **Static:** −10.9% instructions in `exe_14.c`, guest-base reloads 64,181 → 4,460.
- **Difftest:** not affected (header-only; the harness doesn't define it).
- **Runtime, idle work ms/frame:** baseline 11.36, 11.39, 11.49, 11.65 (mean 11.47); const 11.16, 10.84, 11.16
  (mean 11.05). **−3.7%**; every const run is below every baseline run, but the gap is only about three times
  the run-to-run spread. Run phase: 10.88 / 12.05 / 11.03 / 10.92 against 10.47 / 10.11 / 10.51.
- **Verdict:** keep. Small, free, no behaviour change.

## 2026-11-10 — x87 register cache in the lifter (`X87CACHE`, `LIFT_X87CACHE=0` turns it off)

- **Hypothesis:** the x87 stack lives in `c->st[]` at a runtime index, so each operation is a load and a store
  that can't be forwarded past guest stores, and `fnstsw`/`test` after a compare goes through memory too.
- **Change (`tools/lift.py`):** a function that uses x87 keeps the top index in a local `ft` and, within a
  basic block, the registers it touches in locals `fr0..fr7`, loaded on first use and written back at the end of
  the block and before any call, return or helper that takes the CPU. A compare also leaves C3/C2/C0 in a local,
  which `fnstsw` in the same block reads. `c->st[]`, `c->top` and the condition codes hold the same values as
  before at every point where other code can see them. Runtime: `SPILLF`/`RELOADF` (`lift_prelude.h`),
  `fcompare_sw`/`fstatus_sw`/`fstatus_top` (`x86.h`).
- **Checks:**
  - `tools/difftest.py`: report identical to the unmodified lifter's (1,462 instruction tests). On this Mac the
    unmodified lifter already shows 44 mnemonics with differences, most of them the test reading an empty x87
    register after a pop; the harness also died on SIGBUS from an unaligned atomic until it was taught to
    record a fault as that test's result.
  - New `tools/x87cache_test.py`: 400 real multi-block functions (359 with calls) lifted both ways and run on
    the same random states, callees replaced by a stub that hashes the CPU state it receives and sometimes
    returns a float: 2,133 runs compared, 0 different. Deliberately broken caches (no write-back, no top
    update) give 422 and 271 differences, so the test does see the failure modes.
  - Regenerated code has the same 96,653 functions; 15,235 use the cache.
- **Static:** `0x472580` 130 → 98 instructions, `0x474CD0` 185 → 117. Whole-file totals rise about 1% (the
  top index is stored and reloaded around calls in x87 functions).
- **Runtime, interleaved with the const build (idle / run work ms per frame):**
  const 10.99, 11.30, 11.01 / 10.15, 10.59, 10.12; x87 cache 9.37, 9.45, 9.44 / 8.95, 8.65, 8.72.
  **−15.1% idle, −14.8% run**, far outside the spread. An earlier pair (9.39, 9.31 against 11.29, 11.10) agrees.
  Screenshots at frames 2990 and 4190 show the same spot and view as the const build (the weather differs from
  run to run in either build).
- **Verdict:** keep. Needs a regen (the manifest check enforces that, since `tools/lift.py` changed).
- Three runs taken while the Mac was busy after a login (load average over 100) were discarded.

## 2026-11-10 — native replacement for `0x472580` (plane/sphere side test)

- **Hypothesis:** the function had 23% of the main thread's lifted-code samples, so a native version should
  save several percent of the frame.
- **Change:** `runtime/skyrim_native.c` (new), listed in `HOST_OVERRIDES_SKYRIM` in `tools/lift.py`, so the
  lifter emits the guest body as `f_472580_guest`. It reproduces the lifted body's results exactly, including
  the float temporaries in the argument slot, the stale x87 registers and the condition codes.
- **Check:** new `tools/native_test.py` runs the lifted guest body and the native function on 20,000 random
  states: 0 different. Four deliberate errors, one per result path (behind, between, in front, NaN), give
  2,619 / 14,362 / 1,881 / 1,138 differences.
- **Runtime (idle / run):** x87 cache alone 9.37, 9.45, 9.44 / 8.95, 8.65, 8.72; with the native function
  9.20, 9.31, 9.33 / 8.78, 8.64, 8.63. **−1.5% idle, −1.0% run.** Every native run is below every x87 run in
  idle, but the gain is a tenth of what the sample share suggested.
- **Verdict:** keep, as the worked example of the override mechanism, but it shows the limit of this approach:
  the function's time is not its instructions (98 → about 35) but, most likely, cache misses on the object data
  it reads while culling walks the scene. Hot-function replacement is not the next lever; candidate 8 is closed
  unless a profile shows a function whose cost is computation.

## Running total (M1, Riverwood idle, work ms per frame)

| Build | Mean of 3 | Against baseline |
|---|---|---|
| baseline (`perf-base`) | 11.47 (4 runs) | |
| + `LB_CONST_MEM` | 11.10 | −3% |
| + x87 register cache | 9.42 | −18% |
| + native `0x472580` | 9.28 | −19% |

## 2026-11-10 — lock-free COM object lookup in the D3D9 bridge

- **Hypothesis:** a second profile (after the x87 cache) showed 113 of 2,040 main-thread on-CPU samples in
  pthread mutex slow paths, 109 of them under `com_host`, which takes a recursive mutex on every D3D9 call only
  to index a vector that another thread might grow.
- **Change (`runtime/d3d9_bridge.cpp`):** the id → object table grows in chunks that never move, entries and
  the count are atomics, and `com_host` reads without the lock. Writers (`com_wrap`, `com_forget`) still hold it.
- **Runtime, six interleaved pairs (idle work ms):** before 9.26, 8.93, 9.22, 9.16, 9.38, 9.26 (mean 9.20);
  after 8.72, 8.77, 9.17, 8.77, 9.20, 9.03 (mean 8.94). **−2.8% idle**, lower in all six pairs; run phase
  8.68 → 8.32 (**−4.1%**).
- **Verdict:** keep. Not committed: the file also carries the planning session's uncommitted changes.
- **Same profile, other notes:** `0x472580` is still 8% of the main thread as a native function (so its cost
  is the memory it reads), the six-plane loop that calls it (`0xB1B6C0`) another 8%, indirect-call dispatch
  (`rt_call`) 3%.

Running total: baseline 11.47 → 8.94 ms idle (**−22%**).

## 2026-11-10 — `LB_STRICT_ORDER` off (measurement only; the decision is the maintainer's)

- **Build:** same code as the current best (x87 cache, const base, native function, lock-free COM lookup), with
  `-DLB_STRICT_ORDER=OFF`. The executable shrinks from 110 MB to 90 MB.
- **Runtime:** macOS was indexing in the background during every run (Spotlight and media analysis, load
  average 8–12), so the spread is about three times the usual and these are indicative only. Idle work ms,
  interleaved: strict 8.60, 8.84, 9.75, 9.29 (mean 9.12); off 8.17, 8.71, 7.91, 7.82 (mean 8.15).
  **About −11%**, off lower in all four pairs.
- **Failures:** none in five full runs (4,300 frames each, clean exits, no fatal or unimplemented-instruction
  lines). That is far too little to say it is safe: the ordering exists for races between threads, and this
  scenario exercises loading once and then a quiet scene.
- **Verdict:** worth the maintainer's attention as roughly a tenth of the main thread, but needs a quiet-machine
  rerun and long play sessions on several cores before anyone turns it off.

## 2026-11-10 — depth for stereo, and a visionOS engine with the changes

- **Frame structure** (one Riverwood frame, `LB_D3DTRACE` with the new render-target lines): 2,013 draws in
  about 27 passes. Two 512×512 targets (17 draws), a 512×1024 shadow map (268), a 960×540 reflection (197), the
  main scene into a 1280×720 16-bit float target (620), then **the scene's depth again into a 1280×720 R32F
  colour target (638 draws)**, effects and first-person passes, the luminance chain, and the final image.
- **Depth capture:** `LB_SHOT` now also writes that R32F target (`depth-<frame>.f32`). Values are D3D depth
  (0.92–1.0 in the test frame, 1 = sky); with the near plane at 15 units the nearest surface is 2.7 m away and
  the median 17 m.
- **Offline stereo** (`tools/stereo_proto.py`, 64 mm eye separation): largest shift 8.9 px per eye at 1280
  wide, 1.9% of pixels unresolved, all at depth edges. The HUD moves with the scene behind it, so it needs its
  own panel. Details in `IMMERSIVE_OPTIONS.md`.
- **visionOS engine:** `ipad/build.py engine --platform visionos` from the regenerated code with
  `LB_CONST_MEM`: `libSkyrimNative.a` sha256 e4849f52…, 143 MB (the previous one, 160 MB, is kept in
  `~/SkyrimAVP/engines/`). Built only; not packaged into the app, not installed, not run.

## 2026-11-10 — a busier scene: `whiterun-market-mac`

- **Why:** the Riverwood route runs into a boulder and stands in a quiet village. The new scenario does
  `coc whiterunorigin` (the city with its townspeople), stands at the gate looking up the street (*idle*,
  frames 1800–2400), runs to the market (*run*), then turns the view there in steps (*pan*, using the new
  scripted look keys `J`/`L` in `runtime/input.c`).
- **Scripting limits found:** a second console command arrives garbled (after the first Return, keystrokes are
  dropped until a movement key is pressed, and the old text stays in the input line), so there is no `tgm`;
  `coc whiterun` lands outside the walls with something attacking, which is why the city cell is used.
- **Current build, quiet machine:** idle 9.52 ms work per frame, run 8.18, pan 6.73.
- **Baseline against current, three interleaved pairs, taken while another session was compiling (load
  average 8–16, so indicative only):** idle 12.90, 12.51, 14.65 against 10.41, 9.87, 9.56; run 12.08, 9.52,
  12.16 against 9.54, 7.97, 7.36. About a quarter less in both, in line with Riverwood's −22%. The pan phase
  isn't comparable: the baseline binary predates the look keys and stands still there.
- Headset versions (`whiterun-market-avp`, `riverwood-walk-avp`) exist with placeholder frame numbers; they
  need one `bench.py calibrate` on the device first.

## 2026-11-10 — a profiler that can be trusted on Apple platforms (`runtime/prof.c`)

- **Problem:** the built-in profiler's SIGPROF samples land on whichever thread the kernel picks, in practice
  one in a system call, so its tables were wrong (53% `__psynch_cvsignal` where a stack sample shows 1%). On
  the headset there is no `sample` tool, so the built-in one is the only view of where time goes there.
- **Change:** on Apple arm64 a sampler thread reads, every 5 ms, the program counter of each thread that is on
  a core (`task_threads`, `thread_info` run state, `thread_get_state`). The 30 s report prints all threads and
  then the main thread alone. Other platforms keep SIGPROF.
- **Check:** main-thread table in Riverwood: `0x472580` 9.0%, `0xB1B6C0` 7.7%, `rt_call` 2.2%, `com_host` 1.8%,
  which matches `sample` on the same build (8%, 8%, 3%). Idle work with it running: 8.71 ms, no different from
  without.
- **Verdict:** keep; it is on by default like the old one (`noprof.txt` turns it off).

## 2026-11-10 — toolchain: guest atomics at unaligned addresses, and a triage of difftest's standing differences

- **Unaligned atomics.** x86 allows `xchg` with memory and `lock`-prefixed instructions at any address; ARM's
  atomic instructions raise SIGBUS on Apple CPUs when the address isn't a multiple of the operand size, which
  would kill the game. difftest hit exactly that (`xchg dword ptr [ecx + 0xe], ecx`, a shape harvested from the
  game). The lifter now emits `GA_XCHG`, `GA_LOAD`, `GA_CAS` and `GA_FETCH_*` (`runtime/lift_prelude.h`): the
  same atomic builtin when aligned, the operation under one process-wide lock when not. difftest: that case now
  matches the reference, nothing else changes. Needs a regen.
- **The other 43 mnemonics that differ from unicorn on this Mac, by cause:**
  - *Flags x86 leaves undefined* (not bugs): `imul`/`mul` (ZF, SF, PF), `shr`/`shrd`/`rcl`/`rcr` with a count
    other than 1 (OF), `bsr` (PF). 44 cases.
  - *The test, not the lifter:* x87 instructions that pop (the epilogue then reads an empty register; 50-odd
    cases), `xchg`/`sbb`/`xlatb` snippets that move the stack pointer or address outside the scratch area,
    `push es`/`push cs`, privileged instructions (`clts`, `lgdt`, `wbinvd`).
  - *A real difference, left alone:* the sign of a generated NaN. `sqrtss`/`sqrtps`/`rsqrt*` of a negative
    number give x86's negative quiet NaN (0xFFC00000) in unicorn and ARM's positive one (0x7FC00000) here; the
    same holds for any operation that makes a NaN. Code that looks only at "is NaN" is unaffected.
  - *Unimplemented:* `paddusw` on an xmm register traps; the game has one such site.
- No integer-result differences, so nothing here suggests a lifter bug affecting the game.

## 2026-11-10 — first headset A/B (M2 Vision Pro, visionOS 27.3, windowed 1280×720)

- **Scenario** `whiterun-market-avp` with `LB_MAX_FPS=120`, so the frame time shows the game's cost rather than
  the 60 fps cap (the previously installed build logs no work column): `coc whiterunorigin`, stand at the gate
  (*idle*, frames 6000–7200), run up the street (*run*). Calibration first: main menu steady at 60 fps,
  thermal state nominal.
- **Builds:** A = the app installed on 2026-11-09 (binary 17339b74…); B = the same launcher with the engine
  from this branch at commit 0b5b053 (binary 11835a06…, engine e4849f52…: x87 cache, constant guest base,
  native `0x472580`, lock-free COM lookup). Thomas approved the install and wore the headset.
- **Idle, mean frame ms (fps):** A 12.7 (78), 11.7 (85); B 9.0 (111), 9.0 (111). **About −26%**, two runs a
  side, each B run below each A run by more than the A runs differ from each other.
- **Run phase:** A 9.7, 9.4 ms; B 8.4, 8.4 ms, which is the 120 fps cap, so the gain there is clipped; B's own
  work time is 6.1–6.2 ms.
- `Present` stays at 0.0–0.1 ms in every pacing line, so in the window at this resolution the headset is
  limited by the game's CPU, as on the M1. All four runs exited cleanly at the exit frame.
- **Thermal state:** nominal through both A runs; it moved to fair during the first B run (the fourth
  consecutive run), so B's numbers were taken in the warmer state and are, if anything, pessimistic.
- **Not yet known:** the immersive screen's cost (compositor at 90 Hz plus the tap), busier or combat scenes,
  and long-session thermals. The depth build (`depth-v2`, binary 2569a7db…) was installed at the end of the
  session for the "Skyrim - M1" session's immersive test.

## 2026-11-10 — first immersive runs on the headset (logs only; the depth build is the "Skyrim - M1" session's)

Three sessions of `depth-v2` in the immersive space, logs in `bench-results/avp-immersive-20261110T1258/`:

- **It runs:** tap-only presents, colour and depth exported for all three slots, the compositor at 90 Hz in
  every report but the first (87), depth on in two sessions and off in one.
- **The game's frames don't all reach the display.** With the cap at 120 fps the game's own pacing shows
  110–120 fps in Whiterun, and with the normal cap 60 fps, but the compositor reports 40–51 new frames per
  second in the first case and 45–56 in the second. Depth on or off makes no difference. So the displayed game
  rate sits near 47 fps regardless of how fast the game renders, which points at the frame handoff (when a
  tapped frame counts as ready, or slots being held), not at the game's CPU or the depth pass.
- **CPU isn't short in immersive:** at the 60 fps cap the whole process uses about 90% of one core and the main
  thread 22–25%.
- The new profiler's tables appear in these logs, so it works on the device.
- Thermal state went from nominal to fair once, about two minutes into the second session.

## 2026-11-10 — ThinLTO for the generated code

- **Hypothesis:** inlining lifted functions across the 140 generated files removes spill/reload pairs at
  direct calls.
- **Change (build only):** `-DSKYRIM_GEN_FLAGS=-flto=thin` and `-flto=thin` on the link, in
  `macos/build/perf-lto`. The generated objects are bitcode; the link finished without memory trouble on the
  8 GB M1 (2 LTO threads, cache on disk).
- **Runtime, Whiterun idle work ms, interleaved (first pair dropped: load average 17):** without 9.52, 9.54;
  with 9.12, 9.48, 9.12 (and 9.33 on its warm-up run). Run phase 7.46, 7.52 against 7.08, 7.40, 7.11.
  **About −2 to −4%, and not in every run.**
- **Verdict:** not adopted. The gain is at the edge of the noise and it would complicate every platform's
  build. It also says something about the call-overhead idea in general: the compiler's own cross-file inlining
  buys little here, so a lifter-side register analysis would have to do much better than inlining to be worth
  its risk.

## 2026-11-10 — second immersive session on the headset (`depth-v5`, about 7 minutes of hand play)

Log in `bench-results/avp-immersive-20261110T1458/`. Build: app 8b3bd810…, engine f3c22183… (this branch's engine work including the
unaligned-atomics fix, plus the "Skyrim - M1" session's two-frame mailbox, five tap slots, depth scale 0.25 with
tear handling, and flat menus).

- **The handoff fix works.** Of 77 ten-second reports, 49 show 60 game frames per second reaching the display
  and another 20 show 53–59; the rest are loading. The game's own pacing lines have the same shape (64 of 80 at
  60 fps), so the display now follows the game. In the first session it sat near 47 whatever the game did.
- The compositor presented at 90 Hz in 76 of 77 reports (84 in the other).
- **Menus:** 18 changes logged. Flat with 3 open/1 pausing, 4 open/1 pausing, 5 open/2 pausing, and with 2 or
  3 open and none pausing (non-pausing menus, as the rule intended); depth with only the HUD open.
- **CPU:** the process used 43–70% of one core, the main thread 21–28%. Thermal state stayed nominal.
- **Thomas's verdict:** "Much better." His screenshots show clean edges on a rock and the sword at the
  reduced depth, and a flat main menu.

## 2026-11-10 (evening) — heavy combat scenario (`boss-fight-mac`) and its first profile

- **Scenario:** `tools/bench/scenarios/boss-fight-mac.json`, run with `--env LB_VPAD=0`. `coc whiterunorigin`,
  then at frame 1900 the console runs `bat benchboss`. `benchboss.txt` sits in the game folder (private, not in
  git) and contains:
  `tgm` / `fw 000C8220` (storm) / `player.placeatme 000F811C 1` and `000F811A 1` (two high-level dragons) /
  `00039D21 6`, `00037C47 4`, `00039D5E 4` (bandits: melee, archers, mages) / `00023AA8 2` (storm atronachs).
  Form IDs were read from the local Skyrim.esm by editor ID.
- **Two harness facts (observed):** with the virtual pad on, the A in `bat` acts as stick-left and garbles the
  command, hence `LB_VPAD=0` (and no scripted turning); that is probably also why a second console command used
  to arrive garbled. `coc` inside the batch file together with the spawns hung the load at the first frame.
- **Result, M1, low preset, 1280×720:** before the spawn 11.4–11.5 ms of work per frame (60 fps). During the
  fight, work per frame over four runs: early phase 31.8 / 26.1 / 14.2 / 46.0 ms, late 16.7 / 30.7 / 17.1 /
  45.5 ms, with single frames of 0.4–4 s. **Indicative only:** the fight isn't deterministic, and another
  session was running the game on the same Mac during at least the last two runs. What is safe to say: this
  scene drops the M1 from 60 fps to roughly 20–40, with hitches, where Whiterun without the fight holds 60.
- **Profile (`sample`, 12 s early and 10 s late, one run, the polluted one):**
  - Main thread on-CPU only 37% of the time; it waits on a condition variable for 34%. Two worker threads run
    lifted code at 64–67% of a core each. So the fight is limited by the game's job threads as much as by the
    main thread; lifter-wide gains (x87, memory ordering) apply to all three.
  - **Audio mixer thread** (`engine_main` / `route` / `next_frame` in `runtime/xaudio2.c`): 38–40% of a core
    in both samples. Native runtime code, not lifted. Not measured in a calm scene yet.
  - **Our own profiler report:** the watchdog thread's `prof_report` → `prof_table` calls `dladdr` once per
    sample; in the early sample it used 56% of a core (1,463 of 2,587 ms-samples). It runs on its own thread,
    so it doesn't show in frame work, but it is a core's worth of heat while it runs.
  - `dxvk-cs` spent 52% of the early sample in `swtch_pri` (a yield spin), gone in the late sample; probably
    waiting on pipeline compiles for the new actors. Not investigated.

## 2026-11-10 (evening) — "exempt stack accesses from strict ordering": already done, closed

- Idea: strict ordering costs about 11% (indicative); stack memory is thread-private, so skip it there.
- Read `tools/lift.py`: `stack_op` already exempts `esp`-based operands, `ebp`-based ones in functions with a
  frame pointer, and `fs:` (TEB). In `exe_14.c` (gen-x87) there are 18,216 ordered accesses against 44,431
  plain ones, and none of the ordered ones is `esp`- or `ebp`-based.
- What remains ordered is heap and global memory, plus stack memory reached through another register, which
  can't be proven private per instruction. No further safe cut here; the choice is still strict on or off,
  which is the maintainer's.

## 2026-11-10 (night) — preset sweep on the M1, and the profile at ultra

Whiterun market scenario, 1280×720, build `perf-x87` (rebuilt 20:17 from the head-tracking checkout, with
`LB_PRESET`), one run per preset. Not a quiet machine: system daemons used 1.6–4.3 cores before each run
(`audiomxd` pinned at 75%, `configd`, the ANE compiler). The low run's idle figure (9.62 ms) matches the quiet
baseline from earlier (about 9.5), so the noise is small at least there. Work = ms per frame outside Present
and pacing sleep; GPU = DXVK's busy time per frame (last three 10 s windows of each run).

| preset | idle (city gate view) | run | pan (market) | GPU ms/frame | fps in idle |
|---|---|---|---|---|---|
| low | 9.6 | 7.6 | 5.5 | 9.7–10.0 | 59.5 |
| medium | 10.6 | 8.5 | 5.6 | 10.5–13.2 | 59.6 |
| high | 13.1 (p95 16.9) | 11.3 | 6.3 | 10.2–12.4 | 59.1 |
| ultra (`veryhigh`) | 19.3 | 15.5 | 6.9 | 11.2–15.9 | 51.6 |

- CPU is the first limit at every preset: low → ultra doubles the work in the long view (9.6 → 19.3 ms) while
  GPU time goes from about 10 to at most 16 ms. High just holds 60 fps on the M1; ultra doesn't.
- The cost is in the view: looking down the street from the gate doubles, standing in the market barely moves
  (5.5 → 6.9). That points at draw distance and shadow settings (more objects through culling and draw
  submission), not per-pixel quality.
- An earlier pair of runs on a starved machine (3+ cores of other load) read 23–31 ms of GPU per frame at
  ultra. That was the starvation, not the GPU; discard it.
- **Profile at ultra** (`sample`, 14 s in the pan, starved run, so shape only): main thread 66% on-CPU, of
  which 87% lifted code and 8% DXVK's front end; top entries `f_472580` (culling test, already native) 5.6%,
  `com_host` 5.3%, then `f_B1B6C0`, `f_CAFD70`, `f_F531CC`, `f_CB0090`. Two job threads at 50–63% of a core in
  lifted code. Same picture as low, more of it; nothing new takes over.

## 2026-11-11 (overnight) — profile-guided build of the generated code, and preset repeats

Unattended batch on the M1 (`perf-x87` as the base). The machine was never quiet (`audiomxd` and `configd`
stuck busy), so each run waited 240 s and ran anyway. Instrumented build with `-fprofile-instr-generate`,
trained on the Whiterun, Riverwood and boss-fight scenarios (continuous mode, `%c` in `LLVM_PROFILE_FILE`,
because the game leaves through `_exit`), then `-fprofile-instr-use` (`perf-pgo`).

Whiterun, work ms per frame, four interleaved pairs; one pair dropped because my own builds overlapped it:

| phase | base (mean of 3) | profile-guided | change |
|---|---|---|---|
| idle (long street view) | 9.96 | 9.28 | −7% |
| run | 7.67 | 7.29 | −5% |
| pan (market) | 5.33 | 4.91 | −8% |

- Run-to-run spread on the base is about ±4%, so this is a real but modest gain. One profile-guided pan run
  read 9.73 ms against 4.8–5.0 for the others (dropped as an outlier, cause unknown).
- **Boss fight: no visible difference.** Overall work 12.5 / 14.2 / 12.5 ms on the base against
  14.2 / 15.5 / 11.7; the scenario varies more between runs than any effect of this size.
- The boss fight now holds 57–59 fps on the M1 (20–40 before the audio and profiler fixes on
  `visionos/head-tracking`).
- Not adopted yet: it needs a profile checked in or regenerated per build, and the visionOS engine would
  need the same profile applied. Worth it only if the headset shows the same few percent.

Preset repeats on the base (idle work ms, two runs each): low 9.4 / 9.7, medium 10.7 / 10.6, high 12.2 / 12.1,
ultra 17.1 / 15.3. They agree with the earlier sweep except ultra, which read 19.3 then on a busier machine.

## 2026-11-11 (overnight) — DXVK: pipeline compiles no longer hold the pipeline lock

Boss-fight sample: the DXVK command thread spent 1342 of 2587 samples yielding in the spinlock that
`DxvkGraphicsPipeline` held across a Metal pipeline compile. Vendored DXVK is 1.9.2; MoltenVK 1.4.2 has no
graphics pipeline library, and DXVK 2.x dropped the state cache, so upgrading isn't the fix. Changed
(`dxvk_graphics.{h,cpp}`): the lock is a mutex, the instance list is a `std::deque` (stable addresses), and
an instance is compiled outside the lock; a second thread wanting the same instance waits on a condition
variable. Opt-in `DXVK_ASYNC=1` (`dxvk_pipemanager`, two worker threads) skips draws whose pipeline isn't
ready instead of stalling. Measurements are queued (`cs-`, `async-`, `cold-` runs); results to follow here.

## 2026-11-11 (overnight) — where the lifted code's time goes, per instruction

From saved `sample` files, attributing samples to instruction offsets within lifted functions:

- 18–28% of lifted-code samples land on a function's **first instruction** (12% in the boss fight). The
  profile is flat (1,300+ functions, top 50 = 30%) and the text is about 95 MB, so this reads as instruction
  cache and branch-target misses on entry. To confirm with CPU counters before acting on it.
- About 15% is the entry loads of guest registers from the CPU struct, 4–5% reloads after calls, 2% spills.
- 70% of guest memory accesses are stack accesses.
- Ordered loads compile to `ldapr` with the macOS and visionOS deployment targets but to `ldar` (stronger,
  slower) with the iOS default. Worth telling upstream.

Two experiments from this:

- **Order file** (`-Wl,-order_file`, 5,390 hot functions from samples, relink only → `perf-order`). Boots;
  A/B queued (`ord-` runs).
- **Pinned guest registers** (`LB_PINNED_REGS`, off by default): the eight guest registers live in x21–x28
  as global register variables, generated code built with `-ffixed-x21 … -ffixed-x28`, the runtime enters
  lifted code through an assembly trampoline (`rt_enter_guest`). Removes the entry loads, spills and reloads
  between lifted functions. **Does not work yet.** Two bugs found and fixed, both from the compiler taking
  x21–x28 as preserved across calls (they are callee-saved in the host convention): a repeated register
  write after a call was dropped (fixed with an empty asm clobber after every call, `REGS_CHANGED`), and
  the marker for indirect calls sat after a branch join. It still crashes at the same point every run
  (`f_A50D50` reached with a null `this`, from `f_C686D0`); the unpinned build from the same generated code
  boots. Diagnosis continues; `LB_TRACE_ENTRY` (per-thread function-entry trace) was added for it, but heap
  addresses differ between runs, so traces diverge for innocent reasons within a few hundred calls.
  Later the same night:
  - Every call form in the generated code was checked to be followed by the right hand-over, and no host
    code in the Skyrim build calls lifted code except through `rt_enter_guest`.
  - With **all** generated code at `-O0` the pinned build boots (3 of 3); fully optimised it crashes (3 of 3).
    So it is tied to optimisation, not to the hand-over design.
  - Bisecting by file and then by function (linking `-O0` and `-O2` objects together, `#pragma clang optimize
    off` per function) did **not** converge: the same mixed binary crashed three times and, minutes later,
    booted. It pointed at `f_918B10` in `exe_26.c`, whose optimised machine code I read against the C line
    by line and found correct. `-O1`, `-fno-inline`, `-fno-optimize-sibling-calls` and disabling AArch64
    redundant-copy elimination don't change the full-file result.
  - So either there are several miscompiled functions on paths the game takes at random, or the failure
    depends on timing or on values that differ between runs. Not resolved. `LB_PINNED_CHECK` (registers and
    CPU struct compared at every entry and return) is in the prelude, untried.
  - Next steps when picked up: a deterministic start (fixed seed, no intro video) so bisection is
    trustworthy; `tools/difftest.py` built in pinned mode; a 10-run tally per configuration.

Ruled out tonight, with reasons: box64's CALL/RET optimisation (ours are already host calls), using the host
stack pointer for `esp` (−2.4% instructions statically, not worth the risk), the hardware TSO bit (not
available to ordinary processes), a newer MoltenVK (nothing relevant), a DXVK upgrade (see above).

Bug in the batch itself: the stage scripts waited on each other with `grep "a\|b"`, which macOS's grep
doesn't treat as alternation, so stages 2–5 sat idle from 01:20 to 02:36 before I noticed and restarted them.
The `cs-`, `async-` and `cold-` runs made between 02:51 and 03:56 are **not usable**: the pinned-register
bisection (compiles, links and test runs, even at background priority) ran alongside and the baseline read
18–23 ms where stage 1 read 12–14. The queue was stopped and restarted at 04:20 with nothing else running.

### Clean rerun, 04:30–07:07 (nothing else running; `audiomxd` still at ~110% throughout)

Work time per frame in ms, mean of runs (per-run values in `bench-results`, labels `cs-`, `async-`, `cold-`, `ord-`).

| Comparison | Scenario | Baseline | Variant | Verdict |
|---|---|---|---|---|
| DXVK lock fix (`perf-cs`), 4 pairs | boss fight overall | 21.55 | 20.86 | within noise (runs 17.6–22.3; one fast outlier carries the mean) |
| DXVK lock fix, 1 pair | Whiterun overall | 14.94 | 14.92 | no change |
| `DXVK_ASYNC=1`, warm cache, 3 runs | boss fight overall | 21.55 | 22.36 | no gain |
| Async vs sync, cold cache, 3 pairs | boss fight overall | 21.19 | 21.69 | no change; frames over 50 ms 23 → 19 (weak) |
| Order file (`perf-order`), 4 pairs | Whiterun overall | 13.76 | 14.50 | no gain, possibly worse (two runs with extra hitches) |
| Order file, 3 pairs | boss fight overall | 22.85 | 24.57 | no gain (one run with 77 frames over 50 ms) |
| Order file, 2 pairs, ultra | Whiterun overall | 15.45 | 16.09 | no gain |

None of the three helps on the Mac. The cold-cache async signal from the discarded data (17.9 vs 23.2 ms) did
not reproduce. The order file is dropped; the DXVK lock fix is harmless and stays as a correctness-neutral
change only if the maintainer wants it; `DXVK_ASYNC` stays off. Of the overnight work, only the profile-guided
build (−5 to −8% in Whiterun) measured as a gain.

## 2026-11-11 (morning) — pinned guest registers: boots, and the first A/B

**Cause of the crash.** Three LLVM code-generation passes miscompile code that writes the reserved registers
x21–x28. Found by bisecting compiler passes over the whole build (the same flags for all generated code),
which, unlike mixing `-O0` and `-O2` objects, gave the same answer every run:

| Generated code built with | Boots to frame 200 |
|---|---|
| `-O0` | 3/3 |
| `-O2` | 0/3 |
| IR optimiser off (`-Xclang -disable-llvm-passes`), `-O2` code generation | 0/3 |
| the same, plus every optional machine pass off | 3/3 |
| the same, plus only `-disable-postra-machine-licm` | 2/2 |
| `-O2`, `-disable-postra-machine-licm` | 0/4 |
| `-O2`, every optional machine pass off | 2/2 |
| `-O2`, `-disable-postra-machine-licm -disable-early-ifcvt -aarch64-enable-ccmp=false` | 2/2, then 5/5 |
| the same without either of the last two | 0/2 each |

All three move or speculate instructions (hoisting out of loops, turning branches into conditional selects
and conditional compares). The likely mechanism is that they treat a write to a reserved register as free to
move, because reserved registers are not tracked as live; **not confirmed in the assembly**. Register
allocator choice, coalescing, scheduling and copy propagation were tried and are not the cause. The three
flags are now set by `LB_PINNED_REGS` in `macos/CMakeLists.txt`. Their cost applies to all generated code and
is included in the numbers below.

**A/B**, `perf-pin` against `perf-x87` (which lacks only the DXVK lock change, measured as noise), alternating
runs, work ms/frame, overall mean. `audiomxd` was still busy.

| Scenario | Baseline runs | Pinned runs | Mean | Pairs won |
|---|---|---|---|---|
| Whiterun market | 14.22, 14.18, 14.86, 12.50 | 13.79, 12.94, 11.35, 11.51 | 13.94 → 12.40 (−11%) | 4/4 |
| Whiterun, very high preset | 14.01, 12.71 | 11.53, 11.64 | 13.36 → 11.59 (−13%) | 2/2 |
| Boss fight | 17.76, 18.00, 20.82 | 17.06, 18.37, 20.15 | 18.86 → 18.53 (−2%) | 2/3 |
| Riverwood walk | 19.66 | 19.22 | −2% | 1/1 |

Verdict: **a real gain in Whiterun, size uncertain.** Every Whiterun pair favours the pinned build (6 of 6),
but the ranges overlap and both builds drift by 2 ms between runs, so read it as "probably 5–10%", not 11%.
Boss fight and Riverwood are within noise. All ten pinned runs ran to their exit frame, the first full-length
runs of this build. Not tested: the headset, long play sessions, saves made under one build and loaded under
the other, and pinned together with PGO.
