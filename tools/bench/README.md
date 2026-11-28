# Benchmark runs

`bench.py` runs Skyrim through a scripted scenario on a Vision Pro (or a visionOS Simulator), collects every
frame's time, and compares runs. Results go to `bench-results/` (git-ignored, because logs and screenshots
come from private game files).

    python3 tools/bench/bench.py calibrate --device <udid>                 # screenshots every 300 frames
    python3 tools/bench/bench.py run riverwood-walk --device <udid> --label baseline
    python3 tools/bench/bench.py run riverwood-walk --device <udid> --label strict-order-off \
        --baseline bench-results/<baseline dir>
    python3 tools/bench/bench.py compare bench-results/<a> bench-results/<b>

## What a run does

1. Launches the installed app with `-SkyrimBenchAutoPlay YES`. The launcher presses Play once the files verify,
   without its input gate. It never installs anything.
2. The launch environment turns on `LB_FRAMETIMES=1` (one `ft <frame> <ms> <Present ms> <work ms> <draw calls>`
   log line per frame) and
   `LB_EXIT_FRAME` (a clean quit), and passes the scenario's input as `LB_KEYS` and its screenshot frames as
   `LB_SHOT`.
3. Pulls the new log and screenshots, then writes `frametimes.csv`, `summary.json` (per phase: fps, mean, median,
   p95, p99, max, frames over 33/50/250 ms, Present time, work time, draw calls per frame), `env.json` and the device console. A screenshot
   stalls its frame for 50-300 ms (a GPU readback and two files written), so those frames are left out of the
   statistics; `summary.json` lists them under `meta.screenshot_frames`. On a Mac it also records how much the
   system swapped during the run (`meta.paging_mb`) and warns past 1 GB: with little memory, swapping can make
   a run several times slower for no reason in the build under test.

## Draws per pass (`LB_PASSES`)

`--env LB_PASSES=1` adds one line per frame next to the `ft` line, with the frame's draw calls and primitives by
the kind of pass they belong to (the classes add up to the `ft` draw count; `rt` counts render-target changes):

    pass <frame> shadow=<draws>/<prims> refl=… main=… depth=… fp=… mask=… fx=… post=… ui=… other=… rt=<n>

The classes are guessed from the bound targets (size and format relative to the back buffer, whether a
depth-stencil is bound, depth-only clears, quads); the rules and which of them are unverified are in
`runtime/d3d9_bridge.cpp` (`pass_class`). `LB_PASSES=2` also logs, for the frames in `LB_D3DTRACE=<from>:<to>` (the
first frame without it), one `passdbg <frame> <class> <w>x<h> fmt=<n> ds=<0|1> draws=<n> prims=<n>` line per run
of draws of one class into one target, to check the rules against a real frame.

`--env LB_PASSES_GPU=1` (with `LB_PASSES`) adds GPU milliseconds per class, from Direct3D 9 timestamp queries
issued by the bridge at every change of target or class and at Present, read back without flushing a few frames
later. Each frame gets one more line once its timestamps are in:

    passgpu <frame> shadow=<ms> refl=… … other=<ms> total=<ms> marks=<timestamps> zero=<spans that read 0>

`bench.py` merges it into the frame's `pass` line as the third number (`gpu_ms_mean`) and keeps `total`, `marks`
and `zero` as `gpu.total`, `gpu.marks` and `gpu.zero`. GPU idle inside a frame counts towards the span it falls
in; the Present blit after the last timestamp does not count. `LB_PASSES_GPU=2` also logs every span of one frame
in 300 (`passgpudbg <frame> <span> <class> <ms>`).

## GPU busy time and per-pass-class counts

Both are optional; a log without them parses as before.

- GPU busy: runs set `LB_POWER_LOG_S=2` (unless the scenario or `--env` sets it), so the game prints
  `dxvk: gpu busy <N>%, <X> ms per frame over <F> frames` every 2 s on stderr. `summary.json` gets a top-level
  `gpu` object: `{"windows", "frames", "gpu_busy_ms", "gpu_busy_pct"}`, both means weighted by each window's frame
  count. These lines carry no frame numbers, so they can't be tied to phases: the figure covers the whole run
  (loading and menus included). `compare` prints it as one line.
- Pass classes: with `LB_PASSES=1` (add `--env LB_PASSES=1`) the game prints one line per frame,
  `pass <frame> shadow=<draws>/<prims> refl=... main=620/41233/3.21 ... rt=<n>`. Every `key=a/b[/c]` is parsed
  generically, so new classes or the third number (GPU ms) need no parser change. Each phase (and `overall`)
  gets `passes`: `{class: {"frames", "draws_mean", "prims_mean", "gpu_ms_mean"}}`; `gpu_ms_mean` appears only when
  the lines carry it, and a single-number key such as `rt` gives `{"frames", "value_mean"}`. Screenshot frames are
  excluded, as for frame times. `compare` prints a per-class table (draws, prims, GPU ms; baseline->candidate and
  the ratio) per phase when both runs have `passes`. Pass lines are read from the game log and the console.

## Frame budget per phase (`budget`)

Every phase in `summary.json` (and `overall`) also gets a `budget` object, from lines the runtime, DXVK and the
immersive compositor print anyway (`budget.py` lists them). Blocks whose lines a log lacks are left out, so Mac and
older logs parse as before.

- `gpu_busy`: DXVK's `gpu busy` windows (`LB_POWER_LOG_S`): frame-weighted `ms` and `pct`, the range over windows,
  and `capped` (the frame cap was on). Busy time measured under the cap understates the work on Apple GPUs, so
  compare capped runs only with capped runs, and take headroom from an uncapped run (`LB_MAX_FPS=0`).
- `cadence`: the compositor's `immersive cadence` windows: how many game frames were held for 1, 2, 3 or 4+
  refreshes (at 45 fps on 90 Hz every frame should be held 2; `held_2_share`), frames replaced unseen, and the game
  GPU latency mean (weighted by frames) and max. That latency is an upper bound on the game frame's GPU time (see
  `ipad/immersive_bridge.mm`).
- `compositor_gpu`: the compositor's own GPU time per refresh: the mean of its 5 s windows, their range, the
  highest window p95 and the max. There are two refreshes per game frame at 45 fps.
- `pipelines`: pipelines compiled at draw time (`LB_PIPE_LOG=1`): count, total ms, longest ms; and compiled ahead.
  A compile counts towards the frame whose `ft` line follows it.
- `hitches`: frames the hitch sampler caught (`LB_HITCH_MS`), with its threshold and the longest. Screenshot frames
  are left out, as for frame times.
- `frame_budget`: the phase against the frame cap in `meta.frame_cap_fps`: `budget_ms` (1000 / cap), `late` (frames
  longer than the budget plus a quarter refresh: 25 ms at 45 fps on 90 Hz, so every frame held for a third refresh
  counts, which `over_33ms` misses), and `fits`: work p95 and the game GPU latency mean within the budget (`over`
  names whichever isn't). Uncapped runs have no budget. `overall` fits when every phase does.

The windowed lines carry no frame number, so each is placed at the last `ft` frame before it and covers the frames
of its window. A window contributes to a phase in proportion to the share of its frames inside it; ranges and
maxima come from the windows at least half inside. `overall` covers loading and menus too (a menu can run at the cap
with almost no GPU work), so its windowed blocks keep the windows that count towards any of the scenario's phases
(`basis: "phases"`); without phases, those in which the game delivered at least 90% of its frame cap (`basis:
"steady"`), when there are any. A log without `ft` lines (a play session) puts everything in `overall`, as do
windows read from the console.

Screenshot frames are left out of frame times and hitches, and the windows that contain one are left out of the
windowed blocks, since a screenshot's stall shows up as a held-4+ frame, a high game GPU latency and a low busy
share. Where that would leave a phase without a window at least half inside, the windows are kept, the cadence loses
one held-4+ frame per screenshot, and the block says so (`screenshots: {"windows": k, "dropped": false}`; the report
marks those figures `s`).

`meta` gets the run's `thermal` state (`worst`, `last`, `seen`), `frame_cap_fps` (from the runtime's `present
pacing` line, not the environment: on a device `launch-env.json` overrides it), `state_cache_entries`,
`hitch_threshold_ms` and, for immersive runs, the compositor's `foveation` and `refine_steps`. A run that reaches
*serious* prints a warning.

`bench.py parse <results dir>` re-parses a run with today's parser (`--write` updates its `summary.json`).

## Headset sessions (`session.py`, `report.py`)

`session.py` runs a session plan: several launch configurations (arms) interleaved across scenarios, with a
manifest. A plan (`sessions/*.json`) names env fragments, arms built from them, a launch check, sequences of
`[scenario, arm, run]` (or `{"scenarios", "rounds", "arms"}`, which alternates arms within each round), and optional
questions. `sessions/budget.json` is the frame-budget session: stereo 1600x1100 and 1280x880 and mono 1280x880,
two rounds each in the boss fight and the Whiterun market, after two warm-up runs, then one uncapped run.

    export SKYRIM_BENCH_DEVICE=<udid> SKYRIM_BUNDLE_ID=<the installed app's bundle ID>
    python3 tools/bench/session.py budget list                       # the runs, numbered, and the time estimate
    python3 tools/bench/session.py budget check --build <commit>     # stops on an abort or missing stereo lines
    python3 tools/bench/session.py budget run warmup --build <commit> # untimed: fills DXVK's pipeline cache
    python3 tools/bench/session.py budget run timed --build <commit>  # --from N / --only N to resume or repeat
    python3 tools/bench/session.py budget run uncapped --build <commit>
    python3 tools/bench/session.py budget report --build <commit>    # bench-results/sessions/budget-<commit>/summary.md

On a device the app applies `Documents/SkyrimNative/launch-env.json` over the environment bench launches it with, so
the driver puts each arm's keys in that file rather than on bench's command line. Before its first push it saves the
device's own file in the session folder (or notes that there was none; any other devicectl error stops it), and
`check` and `run` put it back when they end, however they end: finished, stopped, timed out or interrupted. `restore`
does the same by hand, after `push` or if putting it back failed.
The plan's `"immersive": true` starts the game through Play Immersive (`BENCH_IMMERSIVE=1`), which stereo and the
compositor need. Runs are labelled `<plan>-<tag>-<arm>-r<run>`; run 0 is a warm-up. `push ARM` puts one arm on the
device for play by hand; `--pull-layers` also pulls both eyes' shots and depth files. `--mac BUILD_DIR` runs the
same plan on the macOS build as a dry run (its own manifest; there is no compositor there). The session folder
(`--dir`, default `bench-results/sessions/<plan>-<tag>`, the tag being `--tag` or else `--build`) holds the manifest,
the saved `launch-env.json` and the answers; pass the same `--build` or `--tag` to every step. One folder takes one
build: `check` and `run` refuse to add runs of another `--build`. The driver runs bench with the plan's own
`BENCH_IMMERSIVE` and never passes on `BENCH_*` options from the shell.

After each run the driver checks in the log that the arm reached the game, and records any problem in the manifest:
on a device, every key echoed as `native: launch-env.json: K=V` (none ignored); wherever the arm sets them, the
compositor's foveation (`immersive: layer N, foveation on|off`) and depth-search steps, the frame cap (`present
pacing: …`), the depth copy and live stereo (and no stereo lines in a mono arm); with `--build`, the commit in the
runtime's identity line; and no fatal-looking lines. The launch check stops on any problem; later runs carry on and
say which run to repeat. Thermal state: after a run at *serious* the driver waits `--cooldown` seconds (default
300) while the headset rests on AC; *serious* twice in a row, or *critical*, stops the session. Any stop or Ctrl-C in
the middle of a sequence prints the command that resumes it. Before each device run it waits until the headset is unlocked (a locked headset suspends
the app). The plan's `"minutes"` per scenario give the estimate that `list` prints and each run updates (time spent,
time left).

`report.py` re-parses runs (a manifest, result directories, or a hand-pulled game log) and writes one Markdown
report: per run and phase, work mean and p95, frame p95, late frames and whether the phase fits its budget, held
counts, game GPU latency, compositor GPU, GPU busy (capped or not), at-draw compiles, hitches and thermal state; then
a mean per scenario, tag and arm over complete runs below *serious* that passed their checks, without warm-ups, with
how many fit. `--json` writes every re-parsed
summary.

`test_bench.py` checks all of this on a synthetic log.

## Scenarios

JSON in `scenarios/`. From the main menu they open the console, type commands (`tgm`, then `coc <cell>`, which
gives the default character in the same spot every run, with no save), hold keys, and name frame ranges as
phases. The frame numbers are placeholders. Run `calibrate` once on each device, look at the screenshots to see
when the main menu is up, and adjust `console_at`, the phases and `exit_frame`.

## Caveats

- On a device the headset must be worn and awake; visionOS suspends apps otherwise.
- Input is timed in frames, so a slow load shifts everything after it. The screenshots show whether the run went
  where it should. A run that didn't reach its exit frame is flagged and shouldn't be compared.
- Note the thermal state (`meta.thermal` in `summary.json`) before comparing runs.
- Simulator runs need the visionOS Simulator build installed with the game staged in its `Documents/Skyrim`.
  They measure CPU-side changes, not the M2's GPU.
