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
2. The launch environment turns on `LB_FRAMETIMES=1` (one `ft <frame> <ms> <Present ms>` log line per frame) and
   `LB_EXIT_FRAME` (a clean quit), and passes the scenario's input as `LB_KEYS` and its screenshot frames as
   `LB_SHOT`.
3. Pulls the new log and screenshots, then writes `frametimes.csv`, `summary.json` (per phase: fps, mean, median,
   p95, p99, max, frames over 33/50/250 ms, Present time), `env.json` and the device console. A screenshot
   stalls its frame for 50-300 ms (a GPU readback and two files written), so those frames are left out of the
   statistics; `summary.json` lists them under `meta.screenshot_frames`. On a Mac it also records how much the
   system swapped during the run (`meta.paging_mb`) and warns past 1 GB: with little memory, swapping can make
   a run several times slower for no reason in the build under test.

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
