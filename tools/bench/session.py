#!/usr/bin/env python3
"""A scripted headset session: interleaved bench runs of several launch configurations ("arms"), with a manifest.

    session.py PLAN list                     every sequence's runs, numbered, with the time estimate (touches nothing)
    session.py PLAN presets                  write each arm's launch-env.json into the session folder, to read
    session.py PLAN check                    the plan's launch check; stops on an abort, a MoltenVK error or missing lines
    session.py PLAN run SEQUENCE [--from N] [--only N] [--pull-layers]
                                             the plan's runs of that name (e.g. warmup, timed, uncapped)
    session.py PLAN push ARM                 put one arm's launch-env.json on the device and stop (for play by hand)
    session.py PLAN ask                      the plan's questions, answered at the prompt, saved with the session
    session.py PLAN restore                  put the device's own launch-env.json back (check and run do this
                                             themselves when they end, however they end)
    session.py PLAN status                   the runs so far
    session.py PLAN report                   report.py over the manifest -> SESSION/summary.md

PLAN is a JSON file (sessions/*.json, or a path). It names env fragments, arms built from them, and sequences of
(scenario, arm, run number) to run; see sessions/budget.json. The device's app applies
Documents/SkyrimNative/launch-env.json over the environment bench.py launches it with, so each arm's keys go into
that file, not onto bench's command line; bench's own keys (frame times, the exit frame, the scenario's input) are
unaffected. For each run the driver pushes the arm's file, runs bench.py with the scenario and a label
`<plan>-<tag>-<arm>-r<run>`, and appends a record (results directory, worst thermal state, clean exit, build and
tag) to the session's manifest.jsonl. Before its first push the driver keeps the device's own launch-env.json in the
session folder (or notes that it had none; any other devicectl error stops it), and `check` and `run` put it back when
they end, whether they finish, stop, time out or are interrupted; `push` leaves the arm on until `restore`.

After each run the driver reads the log to check that the arm reached the game (check_run): on a device every key
echoed by `native: launch-env.json: K=V`, and wherever the arm sets them, the compositor's foveation and depth-search
steps, the frame cap, the depth copy, live stereo, and with --build the installed build's commit. A run that fails a
check is recorded with its problems and left out of report.py's per-arm means, as is a run that reaches thermal
state *serious*. After a serious run the driver waits --cooldown seconds (default 300) before the next; serious twice
in a row, or critical, stops the session. Any stop or interruption in the middle of a sequence prints the command
that resumes it. On a device it waits for the headset to be
unlocked before each run, since a locked headset suspends the app. The plan's "minutes" (per scenario) give the time
estimate: `list` prints it, and each run prints the time spent and the time left.

Nothing about a particular person's setup lives here: the device is --device or SKYRIM_BENCH_DEVICE, the app's bundle
ID SKYRIM_BUNDLE_ID (as for bench.py), and the session folder --dir (default bench-results/sessions/<plan>-<tag>,
which git ignores). --tag names what is under test (default: --build, the installed build's commit), so each build
gets its own folder; pass the same --build or --tag to every step. A session folder takes one build: `check` and
`run` refuse to add runs of another --build to its manifest.

--mac BUILD_DIR runs the same plan on the macOS build (a dry run of the driver and the scenarios): the arm's keys
are passed as --env, and the manifest is mac-manifest.jsonl, kept apart from the session's own.
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402

LAUNCH_ENV = "Documents/SkyrimNative/launch-env.json"
# SKYRIM_BENCH_LOCK: a file whose presence means another job owns this Mac; Mac dry runs refuse to start while it exists.
LOCK = Path(os.environ["SKYRIM_BENCH_LOCK"]) if os.environ.get("SKYRIM_BENCH_LOCK") else None
FATAL = re.compile(r"\[mvk-error\]|mvk-error|fatal|device lost|abort|Assertion|terminating", re.I)
SOURCE_COMMIT = re.compile(r'"source_commit":"([0-9a-f]{7,40})"')
FOVEATION = re.compile(r"immersive: layer \d+, foveation (on|off)")
# devicectl's error when the file to copy isn't there (any other error is not taken to mean "no file")
MISSING = re.compile(r"no such file|doesn.t exist|does not exist|NSCocoaErrorDomain error 260\b|"
                     r"NSPOSIXErrorDomain error 2\b", re.I)
RESULTS_IN = re.compile(r"^bench: results in (.+?)\s*$", re.M)
COOLDOWN_S = 300
LOCK_POLL_S = 10
STATE = {"resume": None}  # the command that resumes the sequence under way, for stop() to print


def say(text):
    print(f"\n=== {text}", flush=True)


def stop(text):
    resume = STATE["resume"]
    print(f"\nSTOP: {text}" + (f" Resume with `{resume}`." if resume else ""), flush=True)
    sys.exit(1)


# ---------------------------------------------------------------- the plan

def load_plan(name):
    path = Path(name)
    if not path.exists():
        path = HERE / "sessions" / f"{name}.json"
    if not path.exists():
        stop(f"no session plan {name}")
    plan = json.loads(path.read_text())
    plan.setdefault("name", path.stem)
    return plan


def arm_env(plan, arm):
    """An arm's launch environment: its fragments (names in the plan's "env", or inline objects) merged in order."""
    if arm not in plan.get("arms", {}):
        stop(f"the plan has no arm {arm}; it has {', '.join(plan.get('arms', {}))}")
    env = {}
    for part in plan["arms"][arm]:
        if isinstance(part, str):
            if part not in plan.get("env", {}):
                stop(f"arm {arm} names env fragment {part}, which the plan doesn't define")
            env.update(plan["env"][part])
        else:
            env.update(part)
    return {k: str(v) for k, v in env.items()}


def sequence(plan, name):
    """[(scenario, arm, run)] for a sequence: a list of triples, or {"scenarios", "rounds", "arms"} expanded
    scenario by scenario, round by round, arm by arm (so arms alternate within each round)."""
    seq = plan.get("sequences", {}).get(name)
    if seq is None:
        stop(f"the plan has no sequence {name}; it has {', '.join(plan.get('sequences', {}))}")
    if isinstance(seq, dict):
        return [(s, a, r) for s in seq["scenarios"] for r in seq["rounds"] for a in seq["arms"]]
    return [tuple(x) for x in seq]


def minutes(plan, scenario):
    """The plan's estimate for one run of a scenario, in minutes (None when the plan doesn't give one)."""
    return plan.get("minutes", {}).get(scenario)


def estimate(plan, runs):
    """(total minutes, runs without an estimate) for [(scenario, arm, run)]."""
    known = [minutes(plan, s) for s, _, _ in runs]
    return sum(m for m in known if m is not None), sum(1 for m in known if m is None)


def check_run(text, env, device=True, immersive=False, build=None):
    """What a run's log (game.log and console.txt) says about its arm: a list of problems, empty when everything
    that can be checked is as the arm asked.

    On a device the app echoes each key it applies from launch-env.json (`native: launch-env.json: K=V`, or
    `... ignoring K`), and the compositor logs its foveation and depth-search steps; on the Mac the keys go in as
    the environment, which nothing echoes. The runtime logs the frame cap, the depth copy and live stereo on both.
    The build check reads the runtime's identity line (`"source_commit":"<commit>"`), which the device build logs."""
    bad = []
    if device:
        missing = [k for k, v in env.items() if f"native: launch-env.json: {k}={v}" not in text]
        if missing:
            bad.append(f"keys not applied: {', '.join(missing)}")
        ignored = sorted(set(re.findall(r"native: launch-env\.json: ignoring (\S+)", text)))
        if ignored:
            bad.append(f"keys ignored: {', '.join(ignored)}")
        if re.search(r"native: ignoring \S*launch-env\.json", text):
            bad.append("launch-env.json not read (not a JSON object)")
    if build:
        seen = sorted(set(SOURCE_COMMIT.findall(text)))
        if seen and not any(c.startswith(build) or build.startswith(c) for c in seen):
            bad.append(f"build {', '.join(seen)}, not {build}")
        elif not seen and device:
            bad.append("no build commit in the log")
    if device and immersive:
        if "immersive:" not in text:
            bad.append("no immersive: lines (not Play Immersive)")
        if "LB_FOVEATION" in env:
            want = "on" if env["LB_FOVEATION"] == "1" else "off"
            got = FOVEATION.findall(text)
            if want not in got:
                bad.append(f"foveation not {want}" + (" (the layer may not support it)" if want == "on" else ""))
        if "LB_IMMERSIVE_REFINE" in env:
            steps = int(float(env["LB_IMMERSIVE_REFINE"]))
            if f"immersive: per-pixel depth search: {steps} steps" not in text:
                bad.append(f"depth search not {steps} steps")
    if "LB_MAX_FPS" in env:
        fps = int(float(env["LB_MAX_FPS"]))
        want = "present pacing: off, 0 fps" if fps <= 0 else f"present pacing: at most {fps} fps"
        if want not in text:
            bad.append(f"frame cap not {'off' if fps <= 0 else fps}")
    copy = "LB_DEPTH_COPY: the R32F depth pass is a copy" in text
    if env.get("LB_DEPTH_COPY") == "1" and not copy:
        bad.append("depth copy not on")
    if env.get("LB_DEPTH_COPY") == "0" and copy:
        bad.append("depth copy on")
    if "LB_DEPTH_COPY: copy failed" in text:
        bad.append("depth copy failed")
    live = "stereo: live single-pass stereo" in text
    frames = "immersive: stereo frames: each eye draws its own picture" in text
    if env.get("LB_STEREO_MULTIVIEW") == "1":
        if not live or (device and immersive and not frames):
            bad.append("stereo not live")
    elif live or frames:
        bad.append("stereo on in a mono arm")
    fatal = sorted({m[0] for m in FATAL.finditer(text)})
    if fatal:
        bad.append(f"fatal-looking lines: {', '.join(fatal)}")
    return bad


# ---------------------------------------------------------------- the session

class Session:
    def __init__(self, plan, args):
        self.plan = plan
        self.mac = args.mac
        self.game = args.game or os.environ.get("SKYRIM_BENCH_GAME")
        self.device = None if self.mac else (args.device or os.environ.get("SKYRIM_BENCH_DEVICE"))
        self.build = args.build
        self.tag = re.sub(r"[^\w.-]+", "-", args.tag or args.build or "run")
        # one folder per plan and tag (a build by default), so a second build never lands in the first's manifest
        self.dir = Path(args.dir) if args.dir else bench.RESULTS / "sessions" / f"{plan['name']}-{self.tag}"
        self.manifest = self.dir / ("mac-manifest.jsonl" if self.mac else "manifest.jsonl")
        self.backup = self.dir / "launch-env.original.json"
        self.none = self.dir / "launch-env.original.none"
        self.holding_lock = args.holding_lock
        self.cooldown = args.cooldown
        self.pushed = False      # this process put an arm's launch-env.json on the device
        self.backed_up = False   # ...after keeping the device's own
        flags = [f"--{k} {v}" for k, v in (("build", args.build), ("tag", args.tag), ("dir", args.dir),
                                           ("mac", args.mac)) if v]
        self.command = f"session.py {getattr(args, 'plan', None) or plan['name']}" + "".join(" " + f for f in flags)

    def need_folder(self):
        """For the steps that read a session (status, report, restore): stop when the folder isn't there."""
        if self.dir.is_dir():
            return
        others = sorted(p.name for p in self.dir.parent.glob(f"{self.plan['name']}-*") if p.is_dir())
        stop(f"no session folder {self.dir}" + (f"; there are {', '.join(others)} (pass the --build or --tag they "
                                                 "were made with, or --dir)" if others else ""))

    def check_build(self):
        """A session folder holds one build: refuse to add runs of another to its manifest."""
        if not self.manifest.exists():
            return
        builds = {r.get("build") for r in map(json.loads, filter(str.strip, self.manifest.read_text().splitlines()))
                  if "build" in r}
        other = sorted(str(b) for b in builds if b != self.build)
        if other:
            stop(f"{self.manifest} holds runs of build {', '.join(other)}, not {self.build}: pass another --tag or "
                 "--dir so the report doesn't average different builds together")

    def device_call(self, what, fn, *args):
        """A devicectl call that stops the session, rather than raising, when the device doesn't answer."""
        try:
            return fn(*args)
        except subprocess.TimeoutExpired as expired:
            stop(f"devicectl didn't answer within {expired.timeout:.0f} s while {what}: check that the headset is "
                 "connected, awake and unlocked")

    def need_device(self):
        if not self.device:
            stop("pass --device or set SKYRIM_BENCH_DEVICE (or --mac BUILD_DIR for a dry run)")
        return bench.Device(self.device)

    def wait_unlocked(self):
        """Before a device run: a locked headset suspends the app, so wait until it is unlocked (or can't tell)."""
        if self.mac:
            return
        device, n = self.need_device(), 0
        while device.unlocked() is False:
            if n % 6 == 0:
                print(f"{datetime.now():%H:%M} the headset is locked: put it on (or up on your forehead) and unlock it",
                      flush=True)
            n += 1
            time.sleep(LOCK_POLL_S)

    def label(self, arm, run):
        return f"{self.plan['name']}-{self.tag}-{arm}-r{run}"

    def write_presets(self):
        folder = self.dir / "presets"
        folder.mkdir(parents=True, exist_ok=True)
        for arm in self.plan["arms"]:
            (folder / f"{arm}.json").write_text(json.dumps(arm_env(self.plan, arm), indent=2, sort_keys=True) + "\n")
        print(f"wrote {len(self.plan['arms'])} presets to {folder}")

    def is_arm_file(self, text):
        """Whether a launch-env.json is one of this plan's arms (left on the device by an interrupted session)."""
        try:
            env = json.loads(text)
        except ValueError:
            return False
        return isinstance(env, dict) and any({k: str(v) for k, v in env.items()} == arm_env(self.plan, a)
                                             for a in self.plan.get("arms", {}))

    def backup_launch_env(self, device):
        """Before this process's first push: keep the device's own launch-env.json, or note that it had none. If the
        device holds one of the plan's arms, an earlier session didn't put the device's own back, so the backup
        taken then is kept. Any devicectl error other than "no such file" stops the session."""
        if self.backed_up:
            return
        self.dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / "launch-env.json"
            done = self.device_call("reading the device's launch-env.json", device.copy_from, LAUNCH_ENV, local)
            if done.returncode == 0 and local.exists():
                text = local.read_text(errors="replace")
                if not self.is_arm_file(text):
                    self.backup.write_text(text)
                    self.none.unlink(missing_ok=True)
                    print(f"saved the device's launch-env.json to {self.backup}")
                elif self.backup.exists() or self.none.exists():
                    print("the device holds one of this plan's arms (an earlier session didn't finish); keeping the "
                          "backup of its own file taken before that")
                else:
                    self.none.write_text("the device held one of the plan's arms and no backup was found\n")
                    print("WARNING: the device holds one of this plan's arms and this folder has no backup of its own "
                          "file (it may be in another session folder); `restore` will leave an empty one ({})")
            elif MISSING.search(done.stderr + done.stdout):
                self.none.write_text("the device had no launch-env.json\n")
                self.backup.unlink(missing_ok=True)
                print("the device had no launch-env.json; `restore` will leave an empty one ({})")
            else:
                stop(f"couldn't read the device's launch-env.json, so it can't be kept for `restore`: "
                     f"{(done.stderr or done.stdout).strip()[-400:]}")
        self.backed_up = True

    def push(self, arm):
        if self.mac:
            return
        device = self.need_device()
        self.backup_launch_env(device)
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / "launch-env.json"
            local.write_text(json.dumps(arm_env(self.plan, arm), indent=2, sort_keys=True) + "\n")
            self.pushed = True  # from here on the device may hold the arm's file, whatever devicectl says
            done = self.device_call(f"placing {LAUNCH_ENV}", device.push, local, LAUNCH_ENV)
        if done.returncode:
            stop(f"couldn't place {LAUNCH_ENV} on the device: {done.stderr.strip()[-400:]}")
        print(f"launch-env.json on the device: {arm}")

    def restore(self):
        if self.mac:
            return
        if not self.backup.exists() and not self.none.exists():
            stop(f"nothing to restore: {self.dir} holds no backup of the device's launch-env.json")
        device = self.need_device()
        with tempfile.TemporaryDirectory() as tmp:
            src = self.backup
            if not src.exists():
                src = Path(tmp) / "launch-env.json"
                src.write_text("{}\n")
            try:
                done = device.push(src, LAUNCH_ENV)
                why = done.stderr.strip()[-400:] if done.returncode else None
            except subprocess.TimeoutExpired as expired:
                why = f"devicectl didn't answer within {expired.timeout:.0f} s"
        if why is not None:
            STATE["resume"] = None
            stop(f"couldn't restore {LAUNCH_ENV} ({why}); the device keeps the last arm's settings until "
                 f"`{self.command} restore` succeeds")
        self.pushed = False
        print("launch-env.json restored" + ("" if self.backup.exists() else " (empty: the device had none)"))

    def bench_run(self, scenario, arm, run, sequence_name):
        if self.mac and LOCK and LOCK.exists() and not self.holding_lock:
            stop(f"{LOCK} exists: another job owns this Mac (pass --holding-lock if it is yours)")
        self.wait_unlocked()
        self.push(arm)
        cmd = [sys.executable, str(HERE / "bench.py"), "run", scenario, "--label", self.label(arm, run)]
        # bench.py's own BENCH_* options (immersive, re-entry, ...) come from the plan, never from the shell
        env = {k: v for k, v in os.environ.items() if not k.startswith("BENCH_")}
        if self.mac:
            cmd += ["--mac", self.mac] + (["--game", self.game] if self.game else [])
            for k, v in arm_env(self.plan, arm).items():
                cmd += ["--env", f"{k}={v}"]
        else:
            cmd += ["--device", self.need_device().udid]
            if self.plan.get("immersive"):
                env["BENCH_IMMERSIVE"] = "1"  # Play Immersive: stereo, the compositor and head tracking need it
        say(f"{scenario}  {arm}  run {run}")
        t0 = time.time()
        lines = []
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env) as proc:
            for line in proc.stdout:  # shown as it comes: a run takes minutes
                print(line, end="", flush=True)
                lines.append(line)
        out = "".join(lines)
        m = RESULTS_IN.search(out)
        run_dir = Path(m[1]) if m else None
        meta = {}
        if run_dir and (run_dir / "summary.json").exists():
            meta = json.loads((run_dir / "summary.json").read_text()).get("meta", {})
        text = "\n".join((run_dir / n).read_text(errors="replace") for n in ("game.log", "console.txt")
                         if run_dir and (run_dir / n).exists())
        problems = check_run(text, arm_env(self.plan, arm), device=not self.mac,
                             immersive=bool(self.plan.get("immersive")), build=self.build) if text else ["no log"]
        rec = {"utc": datetime.now(timezone.utc).isoformat(timespec="seconds"), "sequence": sequence_name,
               "scenario": scenario, "arm": arm, "run": run, "label": self.label(arm, run),
               "target": "mac" if self.mac else "device", "dir": str(run_dir) if run_dir else None,
               "build": self.build, "tag": self.tag, "bench_exit": proc.returncode,
               "seconds": round(time.time() - t0),
               "thermal": meta.get("thermal", {}).get("worst", "unknown"),
               "complete": bool(meta.get("exited_cleanly")), "problems": problems}
        self.dir.mkdir(parents=True, exist_ok=True)
        with open(self.manifest, "a") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"thermal: {rec['thermal']}   complete: {rec['complete']}   {rec['seconds']} s   "
              + (f"CHECK: {'; '.join(problems)}" if problems else "the arm's keys are confirmed in the log"))
        if rec["thermal"] in ("serious", "critical"):
            print(f"THERMAL: this run reached {rec['thermal']} and is left out of comparisons.")
        return rec, run_dir, out

    def pull_layers(self, run_dir):
        """Both eyes' shots and depths of every screenshot frame (LB_SHOT writes layer 1 and depth files next to
        the shot it logs); bench.py itself pulls only the shot."""
        if self.mac or not run_dir:
            return
        device = self.need_device()
        log = (run_dir / "game.log").read_text(errors="replace")
        for path, frame in sorted(set(re.findall(r"^LB_SHOT frame \d+ \d+x\d+ -> (.+shot-(\d{5})\.bmp)\s*$", log, re.M))):
            m = bench.CONTAINER.search(path)
            if not m:
                continue
            base = m[1][:-len(f"shot-{frame}.bmp")]
            for name in (f"shot-{frame}-layer1.bmp", f"depth-{frame}.f32", f"depth-{frame}-layer1.f32"):
                ok = self.device_call(f"pulling {name}", device.pull, base + name, run_dir / name)
                print(f"  {frame} {name}: {'ok' if ok else 'missing'}")


# ---------------------------------------------------------------- steps

def cmd_check(s):
    check = s.plan.get("check")
    if not check:
        stop("the plan has no check")
    s.check_build()
    rec, run_dir, _ = s.bench_run(check["scenario"], check["arm"], 1, "check")
    if not run_dir:
        stop("the launch check produced no results (the app may have aborted at launch). Keep the bench output above "
             "and the newest Documents/SkyrimLogs log, and end the session here.")
    text = "\n".join((run_dir / n).read_text(errors="replace") for n in ("game.log", "console.txt")
                     if (run_dir / n).exists())
    problems = sorted({m[0] for m in FATAL.finditer(text)})
    want = check.get("expect", []) + ([] if s.mac else check.get("expect_device", []))
    missing = [w for w in want if w not in text]
    eyes = next((line for line in text.splitlines() if "immersive: eyes: tangents" in line), None)
    if eyes:
        print("eye line:", eyes.split("immersive: ", 1)[1])
    if problems or not rec["complete"]:
        stop(f"launch check failed ({', '.join(problems) or 'the run did not reach its exit frame'}); results in "
             f"{run_dir}")
    if missing:
        stop(f"the launch check ran, but these lines are missing: {missing}. Check the arm's keys. Results: {run_dir}")
    if rec["problems"]:
        stop(f"the launch check ran, but the arm didn't reach the game as asked: {'; '.join(rec['problems'])}. "
             f"Results: {run_dir}")
    print("\nLAUNCH CHECK PASSED.")


def cmd_run(s, name, start=1, only=None, layers=False):
    """Runs a sequence. Thermal state: after a serious run, wait s.cooldown seconds; serious twice in a row, or
    critical, stops the session. Any stop prints the command that resumes the sequence."""
    plan = sequence(s.plan, name)
    s.check_build()
    chosen = [(i, x) for i, x in enumerate(plan, 1) if i >= start and (not only or i == only)]
    total, unknown = estimate(s.plan, [x for _, x in chosen])
    say(f"{name}: {len(chosen)} runs, about {total:.0f} min"
        + (f" (+{unknown} without an estimate)" if unknown else ""))
    t0, serious = time.time(), 0
    for k, (i, (scenario, arm, run)) in enumerate(chosen):
        print(f"\n[{i}/{len(plan)}]")
        STATE["resume"] = None if only else f"{s.command} run {name} --from {i}"
        rec, run_dir, _ = s.bench_run(scenario, arm, run, name)
        if layers:
            s.pull_layers(run_dir)
        if not rec["complete"]:
            print(f"WARNING: run {i} didn't complete; carry on, then repeat it with --only {i} if there is time.")
        elif rec["problems"]:
            print(f"WARNING: run {i} failed its checks and is left out of the means; repeat it with --only {i} "
                  "once the cause is fixed.")
        if k + 1 == len(chosen):
            break
        STATE["resume"] = f"{s.command} run {name} --from {chosen[k + 1][0]}"
        if rec["thermal"] == "critical":
            stop("thermal critical. Let the headset cool.")
        if rec["thermal"] == "serious":
            serious += 1
            if serious >= 2:
                stop("thermal serious twice in a row. Let the headset cool.")
            print(f"Cooling down for {s.cooldown // 60} min {s.cooldown % 60} s: take the headset off and leave it "
                  "on AC, then put it back on (or up on your forehead).", flush=True)
            time.sleep(s.cooldown)
        elif rec["thermal"] != "unknown":  # a run with no results says nothing about the headset's temperature
            serious = 0
        left, unknown = estimate(s.plan, [x for _, x in chosen[k + 1:]])
        print(f"    {(time.time() - t0) / 60:.0f} min so far; about {left:.0f} min left"
              + (f" (+{unknown} runs without an estimate)" if unknown else ""), flush=True)
    STATE["resume"] = None  # the sequence is done (a stop or interruption leaves it for the message)


def cmd_list(s):
    for name in s.plan.get("sequences", {}):
        runs = sequence(s.plan, name)
        total, unknown = estimate(s.plan, runs)
        say(f"{name}: {len(runs)} runs, about {total:.0f} min" + (f" (+{unknown} without an estimate)" if unknown
                                                                    else ""))
        for i, (scenario, arm, run) in enumerate(runs, 1):
            m = minutes(s.plan, scenario)
            print(f"{i:>3}  {scenario:<22} {arm:<24} r{run}  {'~%.1f min' % m if m is not None else '?'}"
                  f"{'  (warm-up)' if run == 0 else ''}")
    check = s.plan.get("check")
    m = minutes(s.plan, check["scenario"]) if check else None
    runs = [x for n in s.plan.get("sequences", {}) for x in sequence(s.plan, n)]
    total, unknown = estimate(s.plan, runs)
    print(f"\nall sequences: about {total + (m or 0):.0f} min with the launch check, plus {s.cooldown // 60} min per "
          "thermal cool-down")


def cmd_ask(s):
    qs = s.plan.get("questions", [])
    if not qs:
        stop("the plan has no questions")
    s.dir.mkdir(parents=True, exist_ok=True)
    lines = [f"{datetime.now():%Y-%m-%d %H:%M}, {s.plan['name']}, {s.tag}"]
    for q in qs:
        lines.append(f"{q} -> {input(q + ' ').strip()}")
    path = s.dir / "answers.txt"
    with open(path, "a") as f:
        f.write("\n".join(lines) + "\n\n")
    print(f"saved to {path}")


def cmd_status(s):
    s.need_folder()
    if not s.manifest.exists():
        print("no runs yet")
        return
    for line in s.manifest.read_text().splitlines():
        r = json.loads(line)
        print(f"{r['utc']}  {r.get('sequence', '-'):<9} {r['scenario']:<22} {r['arm']:<24} r{r['run']}  "
              f"thermal {r['thermal']:<8} {'ok' if r['complete'] else 'INCOMPLETE'}  {r['dir']}"
              + (f"\n    CHECK: {'; '.join(r['problems'])}" if r.get("problems") else ""))


def cmd_report(s):
    import report
    s.need_folder()
    out = s.dir / ("summary.md" if not s.mac else "mac-summary.md")
    report.main(["--manifest", str(s.manifest), "--out", str(out)])
    answers = s.dir / "answers.txt"
    if answers.exists():
        with open(out, "a") as f:
            f.write("\n## Answers\n\n" + "\n".join("- " + line for line in answers.read_text().splitlines() if line)
                    + "\n")
    print(f"written to {out}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("plan", help="a session plan: sessions/<name>.json or a path")
    ap.add_argument("step", choices=["list", "presets", "check", "run", "push", "ask", "restore", "status", "report"])
    ap.add_argument("what", nargs="?", help="run: the sequence; push: the arm")
    ap.add_argument("--device", help="Vision Pro UDID (default: SKYRIM_BENCH_DEVICE)")
    ap.add_argument("--mac", metavar="BUILD_DIR", help="a dry run on a macOS build instead of the headset")
    ap.add_argument("--game", help="with --mac: the game folder (default: SKYRIM_BENCH_GAME)")
    ap.add_argument("--dir", help="the session folder (default: bench-results/sessions/<plan>-<tag>)")
    ap.add_argument("--build", help="the installed build's commit: each run's log must show it (and the default tag)")
    ap.add_argument("--tag", help="what is under test, for the run labels (default: --build)")
    ap.add_argument("--from", dest="start", type=int, default=1, help="run: start at run N")
    ap.add_argument("--only", type=int, help="run: only run N")
    ap.add_argument("--holding-lock", action="store_true", help="with --mac: the machine's bench lock is yours")
    ap.add_argument("--pull-layers", action="store_true", help="run: also pull layer-1 shots and depth files")
    ap.add_argument("--cooldown", type=int, default=COOLDOWN_S, help="run: seconds to wait after a serious run")
    a = ap.parse_args(argv)
    s = Session(load_plan(a.plan), a)
    if a.step in ("run", "push") and not a.what:
        ap.error(f"{a.step} needs a {'sequence' if a.step == 'run' else 'arm'}")
    step = {"list": lambda: cmd_list(s), "presets": s.write_presets, "check": lambda: cmd_check(s),
            "run": lambda: cmd_run(s, a.what, a.start, a.only, a.pull_layers),
            "push": lambda: push_by_hand(s, a.what), "ask": lambda: cmd_ask(s), "restore": restore_by_hand(s),
            "status": lambda: cmd_status(s), "report": lambda: cmd_report(s)}[a.step]
    signal.signal(signal.SIGTERM, _exit_on_signal)
    signal.signal(signal.SIGHUP, _exit_on_signal)
    try:
        step()
    except subprocess.TimeoutExpired as expired:  # a devicectl call not already guarded
        stop(f"devicectl didn't answer within {expired.timeout:.0f} s: check that the headset is connected and awake")
    except KeyboardInterrupt:
        resume = STATE["resume"]
        print("\ninterrupted" + (f"; resume with `{resume}`" if resume else ""), flush=True)
        raise SystemExit(130)
    finally:
        if a.step in ("check", "run") and s.pushed:  # never leave an arm's settings on the device for ordinary play
            s.restore()


def _exit_on_signal(signum, _frame):
    raise KeyboardInterrupt  # so check and run restore the device's launch-env.json on the way out


def push_by_hand(s, arm):
    s.push(arm)
    if not s.mac:
        print(f"the arm stays on the device until `{s.command} restore`")


def restore_by_hand(s):
    def step():
        s.need_folder()
        s.restore()
    return step


if __name__ == "__main__":
    main()
