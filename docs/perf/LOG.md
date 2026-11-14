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

## 2026-11-14 — the first-person draws (M5, Riverwood, low preset, 1280×720; twcclegg/skyrim-apple#23 chunk 1.1)

Traced with `LB_D3DTRACE` (now with draw numbers, viewports, the world camera at each Clear, and several frame
ranges) in three states: empty hands (`riverwood-walk-mac`), and an iron sword with Flames readied (scripted with
`tools/stereo_capture.py` and a private config, world slowed with `sgtm 0.02`). Checked by skipping draws
(`LB_FP_SKIP`, below) and comparing `LB_SHOT` pictures of neighbouring frames.

- **What marks them:** the first-person camera's projection. The game sets `D3DTS_PROJECTION` before each pass
  even with shaders; the first-person one has near plane 5 (`_43 = -5.00007`), the world's 15. The world
  NiCamera's frustum is unchanged meanwhile (near 15), so reading it does not help. Same field of view here (65).
- **Three places per frame, the same meshes each time** (sword and Flames: 10 + 5 + 14 = 29 draws; empty hands:
  10 + 5 + 10 = 25, nothing visible on screen):
  - *mask:* in the 960×540 pass (the `refl` class), depth only, then red only. Skipping them changes only the
    shading on the hands and sword, so this pass is most likely the screen-space sun-shadow mask, not a reflection.
  - *prepass:* first in the main pass, colour writes off: the hands' depth, so the world is not drawn behind them.
    Skipping it changes nothing visible (the late pass covers the area).
  - *late pass:* after the world's effects and the sun's first occlusion query, before tone mapping:
    `Clear(ZBUFFER)` on the 16-bit float scene target (the world's colour stays), the projection with near 5,
    the lit meshes (5) and **the flame (3 additive draws) inside the same run**, with Flames also one draw into a
    cleared back-buffer-sized A8R8G8B8 target; then `Clear(ZBUFFER)` on the R32F depth target, whose colour (the
    world's depth) is **not** cleared, and the 5 opaque meshes again, writing their depth over the world's. Then
    the scene target with the world's projection (the sun's second query) and the post chain.
- **Skipping everything first-person** removes the hands, sword and flame and the glow above the hand; nothing else
  changes beyond frame-to-frame foliage noise. Skipping only the late pass leaves flat silhouettes in the clear
  colour (the prepass kept the world out). The flame's light on the ground is world shading and stays.
- **Third person:** no Clear of depth only and no near-5 projection: 0 first-person draws. **Tab menu in first
  person:** the game keeps drawing all 29. **Loading screen:** it has a Clear of depth only before its model, but
  with the world's near plane; not counted.
- **Detector** (`d3d_first_person()`, `skyrim_first_person_pass()` with the kind, `skyrim_first_person_listen()`):
  on from such a Clear until a projection with another near plane or a target that is not back-buffer-sized, which
  learns the near plane; elsewhere on whenever the projection set last has that near plane. `LB_PASSES` counts all
  of them as `fp` (they were `main`, `refl` and `fx`), and `LB_PASSES=2` logs an `fpdbg` line per frame. Class
  totals still equal the `ft` draw count (checked on 1,847-, 1,852-, 1,947- and 2,075-draw frames).
- **The 57 missing effect draws in alternate-eye mode are the weather, not stereo.** On this branch, plain mono
  runs gave 146 (`fp` now 10 of them) or 203 effect draws per frame depending on the run, and so did
  `LB_STEREO_EYE_UNITS=2.24` alone; the 203 runs have an overcast, cloudy sky in their screenshots, the 146 runs
  (including the alternate-eye one) a clear one. Which draws they are is not pinned down: the cloudy run's 180-draw
  effects run is mostly one alpha-blended shader without depth writes (142 draws); no clear-sky trace was taken.
