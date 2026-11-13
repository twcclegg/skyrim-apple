#!/usr/bin/env python3
"""Repeatable Skyrim benchmark runs on a Vision Pro (or the visionOS Simulator), and their comparison.

    bench.py run SCENARIO [--label NAME] [--device UDID | --simulator UDID] [--env KEY=VALUE ...] [--baseline DIR]
    bench.py calibrate [--frames 6000] [--every 300] ...   screenshots every N frames, to time a scenario
    bench.py compare BASELINE CANDIDATE                     per-phase frame-time deltas
    bench.py parse LOG [--scenario SCENARIO]                summarize a log pulled by hand

A scenario (scenarios/*.json) is frame-timed scripted input for the runtime's LB_KEYS: console commands typed from
the main menu (`coc` into a cell gives the default character in the same place every run, without anyone's save),
keys held for a route, screenshots (LB_SHOT) to check the run went where intended, and named phases whose frame
times are summarized separately. The runtime logs each frame (LB_FRAMETIMES=1) and quits at LB_EXIT_FRAME.
Taking a screenshot stalls its frame for 50-300 ms (a GPU readback and two files written), so those frames are left
out of the statistics and listed in the summary's meta as screenshot_frames.

The app must already be installed with the game files in place; this tool never installs anything. On a device the
headset has to be worn and awake, because visionOS suspends apps when it isn't. Results go to bench-results/
(git-ignored): the log, frametimes.csv, summary.json and the screenshots.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
RESULTS = REPO / "bench-results"


def _bundle_id():
    """The app's bundle ID: SKYRIM_BUNDLE_ID, else the one in the last visionOS app build."""
    if os.environ.get("SKYRIM_BUNDLE_ID"):
        return os.environ["SKYRIM_BUNDLE_ID"]
    import plistlib
    info = REPO / "ipad/build/visionos/DerivedData/Build/Products/Release-xros/Skyrim.app/Info.plist"
    try:
        return plistlib.loads(info.read_bytes())["CFBundleIdentifier"]
    except (OSError, KeyError, plistlib.InvalidFileException):
        return "SET-SKYRIM_BUNDLE_ID"


BUNDLE = _bundle_id()
FRAME_MS = 1000.0 / 60.0
FT = re.compile(r"^ft (\d+) ([\d.]+) ([\d.]+)(?: ([\d.]+))?(?: (\d+))?$")
SHOT = re.compile(r"^LB_SHOT frame (\d+) \d+x\d+ -> (.+)$")
SHOT_ANY = re.compile(r"^LB_SHOT frame \d+ ")  # any screenshot line: the next frame time logged includes its stall
CONTAINER = re.compile(r"/Containers/Data/Application/[0-9A-F-]+/(.+)$")
NOTABLE = ("native: thermal", "bench:", "immersive:", "pointer:", "fatal", "device lost", "rt_unimpl", "quit requested")

# SDL scancode names for the characters a console command may use (runtime/platform_sdl.c load_key_script).
KEY_NAMES = {" ": "Space", ".": ".", "-": "-", "_": None, "\n": "Return"}


def fail(message):
    sys.exit(f"bench: {message}")


# ---------------------------------------------------------------- scenario -> LB_KEYS

def typed(text, frame, hold=2, gap=4):
    """Key presses that type `text` starting at `frame`; returns (entries, next free frame)."""
    entries = []
    for ch in text.lower():
        if ch.isalnum():
            name = ch.upper()
        elif ch in KEY_NAMES and KEY_NAMES[ch]:
            name = KEY_NAMES[ch]
        else:
            fail(f"can't type {ch!r} in a console command")
        entries.append(f"{frame}:{name}:{hold}")
        frame += hold + gap
    return entries, frame


def key_script(scenario):
    """The LB_KEYS string for a scenario: console commands first, then held keys."""
    entries = []
    frame = scenario.get("console_at", 0)
    commands = scenario.get("console", [])
    if commands:
        entries.append(f"{frame}:`:2")  # open the console
        frame += 20
        for command in commands:
            more, frame = typed(command, frame)
            entries += more
            entries.append(f"{frame}:Return:2")
            frame += scenario.get("command_gap", 30)
        if scenario.get("console_close", True):  # `coc` closes the console itself; a second ` would reopen it
            entries.append(f"{frame}:`:2")
    for held in scenario.get("keys", []):
        entries.append(f"{held['start']}:{held['key']}:{held['frames']}")
    if len(entries) > 256:
        fail(f"scenario needs {len(entries)} key events; the runtime takes 256")
    if any("," in e for e in entries):
        fail("a key name contains a comma")
    return ",".join(entries)


def load_scenario(name):
    path = Path(name)
    if not path.exists():
        path = HERE / "scenarios" / f"{name}.json"
    if not path.exists():
        fail(f"no scenario {name}")
    scenario = json.loads(path.read_text())
    scenario.setdefault("name", path.stem)
    return scenario


# ---------------------------------------------------------------- targets

class Device:
    def __init__(self, udid):
        self.udid = udid

    def devicectl(self, *args, timeout=120):
        return subprocess.run(["xcrun", "devicectl", *args], capture_output=True, text=True, timeout=timeout)

    def preflight(self):
        listed = self.devicectl("list", "devices").stdout
        line = next((l for l in listed.splitlines() if self.udid in l), "")
        if "connected" not in line and "available" not in line:
            fail(f"device {self.udid} isn't connected:\n{line or listed}")
        apps = self.devicectl("device", "info", "apps", "--device", self.udid, "--bundle-id", BUNDLE).stdout
        if BUNDLE not in apps:
            fail(f"{BUNDLE} isn't installed on {self.udid}; install it first (this tool never installs)")

    def launch(self, env, timeout):
        """Runs the app until it exits; returns the console text."""
        command = ["xcrun", "devicectl", "device", "process", "launch", "--device", self.udid, "--console",
                   "--terminate-existing", "--environment-variables", json.dumps(env), BUNDLE,
                   "--", "-SkyrimBenchAutoPlay", "YES", *(["-SkyrimBenchImmersive", "YES"] if os.environ.get("BENCH_IMMERSIVE") else [])]  # "--": devicectl would read -S… as its own options
        try:
            done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
            return done.stdout + done.stderr
        except subprocess.TimeoutExpired as expired:
            return (expired.stdout or b"").decode(errors="replace") + "\nbench: launch timed out\n"

    def place_game_file(self, name, text):
        """Writes a small text file into the game folder (Documents/Skyrim), e.g. a console batch file."""
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / name
            local.write_text(text)
            done = self.devicectl("device", "copy", "to", "--device", self.udid, "--domain-type", "appDataContainer",
                                  "--domain-identifier", BUNDLE, "--source", str(local),
                                  "--destination", f"Documents/Skyrim/{name}")
            if done.returncode != 0:
                fail(f"couldn't place {name} in the game folder: {done.stderr.strip()[-500:]}")

    def list_files(self, folder):
        out = self.devicectl("device", "info", "files", "--device", self.udid, "--domain-type", "appDataContainer",
                             "--domain-identifier", BUNDLE, "--subdirectory", folder).stdout
        return re.findall(r"^(\S+\.log)\s", out, re.M)

    def pull(self, source, destination):
        done = self.devicectl("device", "copy", "from", "--device", self.udid, "--domain-type", "appDataContainer",
                              "--domain-identifier", BUNDLE, "--source", source, "--destination", str(destination))
        return done.returncode == 0 and Path(destination).exists()


class Simulator:
    """A booted visionOS Simulator with the app installed and its game files staged."""

    def __init__(self, udid):
        self.udid = udid
        self.container = None

    def preflight(self):
        done = subprocess.run(["xcrun", "simctl", "get_app_container", self.udid, BUNDLE, "data"],
                              capture_output=True, text=True)
        if done.returncode:
            fail(f"{BUNDLE} isn't installed on simulator {self.udid}: {done.stderr.strip()}")
        self.container = Path(done.stdout.strip())

    def launch(self, env, timeout):
        child = dict(os.environ, **{f"SIMCTL_CHILD_{k}": v for k, v in env.items()})
        command = ["xcrun", "simctl", "launch", "--console", "--terminate-running-process", self.udid, BUNDLE,
                   "-SkyrimSimulatorAutoPlay", "YES"]
        try:
            done = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=child)
            return done.stdout + done.stderr
        except subprocess.TimeoutExpired as expired:
            subprocess.run(["xcrun", "simctl", "terminate", self.udid, BUNDLE], capture_output=True)
            return (expired.stdout or b"").decode(errors="replace") + "\nbench: launch timed out\n"

    def list_files(self, folder):
        path = self.container / folder
        return [p.name for p in path.glob("*.log")] if path.is_dir() else []

    def pull(self, source, destination):
        path = self.container / source
        if not path.exists():
            return False
        Path(destination).write_bytes(path.read_bytes())
        return True


class Mac:
    """The macOS native build (macos/): the same lifted code on the Mac's CPU, for iterating without a headset.

    `build` is a CMake build directory holding skyrim_macos; `game` the game folder. Saves, INIs and the DXVK
    state cache live in bench-results/mac-saves, so the first run after a rebuild warms the shader cache and
    shouldn't be compared."""

    def __init__(self, build, game):
        self.build = Path(build).resolve()
        self.game = Path(game).resolve()
        self.logs = RESULTS / "mac-logs"
        self.saves = RESULTS / "mac-saves"

    def preflight(self):
        if not (self.build / "skyrim_macos").exists():
            fail(f"no skyrim_macos in {self.build}; build it with -DSKYRIM_BUILD_GAME=ON")
        if not self.game.is_dir():
            fail(f"no game folder {self.game}")
        cache = (self.build / "CMakeCache.txt").read_text()
        m = re.search(r"^SKYRIM_MOLTENVK_LIBRARY:\w+=(.+)$", cache, re.M)
        if not m:
            fail("the build's CMakeCache.txt names no MoltenVK library")
        self.moltenvk = m[1]
        self.logs.mkdir(parents=True, exist_ok=True)
        self.saves.mkdir(parents=True, exist_ok=True)

    def launch(self, env, timeout):
        child = dict(os.environ, **env, SDL_VULKAN_LIBRARY=self.moltenvk, DXVK_STATE_CACHE_PATH=str(self.saves),
                     DXVK_LOG_PATH=str(self.saves), DXVK_CONFIG_FILE=str(REPO / "macos/runtime.conf"))
        log = self.logs / f"{datetime.now(timezone.utc):%Y%m%dT%H%M%S}.log"
        with open(log, "w") as out:
            try:
                subprocess.run([str(self.build / "skyrim_macos"), str(self.game), str(self.saves)], stdout=out,
                               stderr=subprocess.STDOUT, timeout=timeout, env=child)
            except subprocess.TimeoutExpired:
                out.write("\nbench: launch timed out\n")
        return f"log: {log}\n"

    def list_files(self, folder):
        return [p.name for p in self.logs.glob("*.log")]

    def pull(self, source, destination):
        path = Path(source)
        if not path.is_absolute():
            path = self.logs / path.name
        if not path.exists():
            return False
        Path(destination).write_bytes(path.read_bytes())
        return True


# ---------------------------------------------------------------- parsing

def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    k = (len(ordered) - 1) * p / 100
    lo, hi = int(k), min(int(k) + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def stats(frames):
    """frames: list of (frame, ms, present_ms, work_ms or None, draw calls or None)."""
    times = [f[1] for f in frames if f[1] > 0]
    presents = [f[2] for f in frames]
    work = [f[3] for f in frames if f[1] > 0 and f[3] is not None]
    draws = [f[4] for f in frames if f[1] > 0 and f[4] is not None]
    if not times:
        return {"frames": 0}
    total = sum(times)
    return {
        "frames": len(times),
        "seconds": round(total / 1000, 2),
        "fps": round(1000 * len(times) / total, 2),
        "mean_ms": round(total / len(times), 2),
        "median_ms": round(statistics.median(times), 2),
        "p95_ms": round(percentile(times, 95), 2),
        "p99_ms": round(percentile(times, 99), 2),
        "max_ms": round(max(times), 2),
        "over_33ms": sum(t > 33.4 for t in times),
        "over_50ms": sum(t > 50 for t in times),
        "over_250ms": sum(t > 250 for t in times),
        "present_mean_ms": round(sum(presents) / len(presents), 2),
        "present_max_ms": round(max(presents), 2),
        **({"work_mean_ms": round(sum(work) / len(work), 3), "work_median_ms": round(statistics.median(work), 3),
            "work_p95_ms": round(percentile(work, 95), 2), "work_p99_ms": round(percentile(work, 99), 2)}
           if work else {}),
        **({"draws_mean": round(sum(draws) / len(draws), 1), "draws_p95": round(percentile(draws, 95))}
           if draws else {}),
    }


def paging_counters():
    """macOS swap-ins, swap-outs and page-outs since boot, in MB; None where vm_stat isn't available."""
    try:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    page = re.search(r"page size of (\d+) bytes", out)
    counts = {k: re.search(rf"^{k}:\s+(\d+)", out, re.M) for k in ("Swapins", "Swapouts", "Pageouts")}
    if not page or not all(counts.values()):
        return None
    return {k.lower(): int(m[1]) * int(page[1]) / 2**20 for k, m in counts.items()}


def parse_log(text, scenario=None):
    frames, shots, notes, stalled, pending = [], {}, [], {}, False
    for line in text.splitlines():
        m = FT.match(line)
        if m:
            frames.append((int(m[1]), float(m[2]), float(m[3]), float(m[4]) if m[4] else None,
                           int(m[5]) if m[5] else None))
            if pending:
                stalled[int(m[1])] = float(m[2])
                pending = False
            continue
        if SHOT_ANY.match(line):
            pending = True
        m = SHOT.match(line)
        if m:
            shots[int(m[1])] = m[2]
        if line.startswith(NOTABLE) or any(n in line for n in NOTABLE[3:]):
            notes.append(line)
    timed = [f for f in frames if f[0] not in stalled]
    summary = {"overall": stats(timed)}
    for name, (start, end) in (scenario or {}).get("phases", {}).items():
        summary[name] = stats([f for f in timed if start <= f[0] < end])
    summary["meta"] = {"screenshot_frames": stalled}
    exited = any("exit frame" in n and "reached" in n for n in notes)
    return frames, shots, notes, summary, exited


# ---------------------------------------------------------------- commands

def target_of(args):
    if args.simulator:
        return Simulator(args.simulator)
    if args.mac:
        return Mac(args.mac, args.game or os.environ.get("SKYRIM_BENCH_GAME") or fail("--mac needs --game"))
    return Device(args.device or os.environ.get("SKYRIM_BENCH_DEVICE") or fail("pass --device or --simulator"))


def execute(scenario, args):
    target = target_of(args)
    target.preflight()
    env = {"LB_FRAMETIMES": "1", "LB_EXIT_FRAME": str(scenario["exit_frame"]), "LB_LOG": "1"}
    keys = key_script(scenario)
    if keys:
        env["LB_KEYS"] = keys
    if scenario.get("shots"):
        env["LB_SHOT"] = ",".join(str(s) for s in scenario["shots"][:16])
    env.update(scenario.get("env", {}))
    for item in args.env:
        key, _, value = item.partition("=")
        env[key] = value
    # "game_files": {name: [lines]} - small files the scenario needs in the game folder (a `bat` batch file).
    for name, lines in scenario.get("game_files", {}).items():
        text = "\n".join(lines) + "\n"
        if hasattr(target, "place_game_file"):
            target.place_game_file(name, text)
        elif hasattr(target, "game"):
            path = Path(target.game) / name
            if not path.exists() or path.read_text() != text:
                path.write_text(text)
    before = set(target.list_files("Documents/SkyrimLogs"))
    timeout = scenario["exit_frame"] / 20 + 240  # a third of real time, plus loading
    started = datetime.now(timezone.utc)
    print(f"bench: running {scenario['name']} to frame {scenario['exit_frame']} (timeout {timeout:.0f} s)")
    paging = paging_counters() if isinstance(target, Mac) else None
    console = target.launch(env, timeout)
    if paging:
        after = paging_counters()
        paging = {k: round(after[k] - v) for k, v in paging.items()} if after else None
    logs = sorted(set(target.list_files("Documents/SkyrimLogs")) - before)
    if not logs:
        fail(f"the run wrote no new log. Console:\n{console[-2000:]}")
    label = re.sub(r"[^\w.-]+", "-", args.label or "run")
    out = RESULTS / f"{started:%Y%m%dT%H%M%SZ}-{scenario['name']}-{label}"
    out.mkdir(parents=True)
    log_path = out / "game.log"
    if not target.pull(f"Documents/SkyrimLogs/{logs[-1]}", log_path):
        fail("couldn't copy the log off the device")
    (out / "console.txt").write_text(console)
    (out / "env.json").write_text(json.dumps(env, indent=2, sort_keys=True))
    frames, shots, notes, summary, exited = parse_log(log_path.read_text(errors="replace"), scenario)
    for frame, path in shots.items():
        m = CONTAINER.search(path)
        if m:
            target.pull(m[1], out / f"shot-{frame:05d}.bmp")
        elif isinstance(target, Mac):
            target.pull(path, out / f"shot-{frame:05d}.bmp")
    with open(out / "frametimes.csv", "w") as csv:
        csv.write("frame,frame_ms,present_ms,work_ms,draws\n")
        for frame, ms, present, work, draws in frames:
            csv.write(f"{frame},{ms},{present},{'' if work is None else work},{'' if draws is None else draws}\n")
    if paging:
        summary["meta"]["paging_mb"] = paging
    summary["meta"] = {**summary["meta"], "scenario": scenario["name"], "label": label, "started_utc": started.isoformat(),
                       "exited_cleanly": exited, "notes": notes[-40:], "log": logs[-1]}
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    print_summary(summary)
    print(f"bench: results in {out}")
    if not exited:
        print("bench: WARNING the run didn't reach its exit frame; don't compare it", file=sys.stderr)
    if paging and paging["swapins"] + paging["swapouts"] > 1024:
        print(f"bench: WARNING the Mac swapped {paging['swapins']} MB in and {paging['swapouts']} MB out during the "
              "run; memory pressure slows everything, so check this run before comparing it", file=sys.stderr)
    if args.baseline:
        compare(Path(args.baseline), out)
    return out


def print_summary(summary):
    print(f"{'phase':<12}{'frames':>8}{'fps':>8}{'mean':>8}{'p95':>8}{'p99':>8}{'max':>9}{'>33ms':>7}{'>250':>6}")
    for name, s in summary.items():
        if name == "meta" or not s.get("frames"):
            continue
        print(f"{name:<12}{s['frames']:>8}{s['fps']:>8.1f}{s['mean_ms']:>8.1f}{s['p95_ms']:>8.1f}{s['p99_ms']:>8.1f}"
              f"{s['max_ms']:>9.1f}{s['over_33ms']:>7}{s['over_250ms']:>6}"
              + (f"   work {s['work_mean_ms']:.2f} / p95 {s['work_p95_ms']:.1f}" if "work_mean_ms" in s else "")
              + (f"   draws {s['draws_mean']:.0f}" if "draws_mean" in s else ""))


def compare(baseline, candidate, threshold=0.05):
    a = json.loads((baseline / "summary.json").read_text())
    b = json.loads((candidate / "summary.json").read_text())
    worse = False
    print(f"bench: {candidate.name} vs {baseline.name} (negative = faster)")
    for phase in b:
        if phase == "meta" or phase not in a or not a[phase].get("frames") or not b[phase].get("frames"):
            continue
        cells = []
        for key in ("mean_ms", "p95_ms", "p99_ms", "work_mean_ms", "work_p95_ms", "draws_mean"):
            if key not in a[phase] or key not in b[phase]:
                continue
            old, new = a[phase][key], b[phase][key]
            change = (new - old) / old if old else 0
            worse |= key == "mean_ms" and change > threshold
            cells.append(f"{key.removesuffix('_ms')} {old:.1f}->{new:.1f} ({change:+.1%})")
        print(f"  {phase:<10} " + "  ".join(cells))
    for run in (a, b):
        if not run.get("meta", {}).get("exited_cleanly"):
            print(f"bench: WARNING {run.get('meta', {}).get('label')} didn't exit cleanly", file=sys.stderr)
    return 1 if worse else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest="step", required=True)

    def target_options(step):
        step.add_argument("--device", help="Vision Pro UDID (default: SKYRIM_BENCH_DEVICE)")
        step.add_argument("--simulator", help="booted visionOS Simulator UDID instead of a device")
        step.add_argument("--mac", metavar="BUILD_DIR", help="a macOS native build directory instead of a device")
        step.add_argument("--game", help="with --mac: the game folder (default: SKYRIM_BENCH_GAME)")
        step.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                          help="extra launch environment (LB_, DXVK_, MVK_, SDL_ prefixes)")
        step.add_argument("--label", help="a name for this run, e.g. the change under test")

    step = steps.add_parser("run", help="run a scenario")
    step.add_argument("scenario")
    step.add_argument("--baseline", help="a results directory to compare against")
    target_options(step)
    step = steps.add_parser("calibrate", help="screenshots every N frames, no input, to time a scenario")
    step.add_argument("--frames", type=int, default=4800)
    step.add_argument("--every", type=int, default=300)
    target_options(step)
    step = steps.add_parser("compare")
    step.add_argument("baseline")
    step.add_argument("candidate")
    step = steps.add_parser("parse")
    step.add_argument("log")
    step.add_argument("--scenario")
    args = parser.parse_args(argv)

    if args.step == "run":
        execute(load_scenario(args.scenario), args)
    elif args.step == "calibrate":
        shots = list(range(args.every, args.frames + 1, args.every))[:16]
        args.baseline = None
        execute({"name": "calibrate", "exit_frame": args.frames, "shots": shots}, args)
    elif args.step == "compare":
        sys.exit(compare(Path(args.baseline), Path(args.candidate)))
    elif args.step == "parse":
        scenario = load_scenario(args.scenario) if args.scenario else None
        *_, summary, exited = parse_log(Path(args.log).read_text(errors="replace"), scenario)
        print_summary(summary)
        if not exited:
            print("bench: the log doesn't show a clean exit at the exit frame")


if __name__ == "__main__":
    main()
