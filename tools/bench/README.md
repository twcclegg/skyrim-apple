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

## Scenarios

JSON in `scenarios/`. From the main menu they open the console, type commands (`tgm`, then `coc <cell>`, which
gives the default character in the same spot every run, with no save), hold keys, and name frame ranges as
phases. The frame numbers are placeholders. Run `calibrate` once on each device, look at the screenshots to see
when the main menu is up, and adjust `console_at`, the phases and `exit_frame`.

## Caveats

- On a device the headset must be worn and awake; visionOS suspends apps otherwise.
- Input is timed in frames, so a slow load shifts everything after it. The screenshots show whether the run went
  where it should. A run that didn't reach its exit frame is flagged and shouldn't be compared.
- Note the thermal state (`native: thermal …` in the notes) before comparing runs.
- Simulator runs need the visionOS Simulator build installed with the game staged in its `Documents/Skyrim`.
  They measure CPU-side changes, not the M2's GPU.
