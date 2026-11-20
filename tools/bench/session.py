#!/usr/bin/env python3
"""A scripted headset session: interleaved bench runs of several launch configurations ("arms"), with a manifest.

    session.py PLAN presets                  write each arm's launch-env.json into the session folder, to read
    session.py PLAN check                    the plan's launch check; stops on an abort, a MoltenVK error or missing lines
    session.py PLAN run SEQUENCE [--from N] [--only N] [--pull-layers]
                                             the plan's runs of that name (e.g. warmup, timed, uncapped)
    session.py PLAN push ARM                 put one arm's launch-env.json on the device and stop (for play by hand)
    session.py PLAN ask                      the plan's questions, answered at the prompt, saved with the session
    session.py PLAN restore                  put the device's own launch-env.json back (taken before the first push)
    session.py PLAN status                   the runs so far
    session.py PLAN report                   report.py over the manifest -> SESSION/summary.md

PLAN is a JSON file (sessions/*.json, or a path). It names env fragments, arms built from them, and sequences of
(scenario, arm, run number) to run; see sessions/budget.json. The device's app applies
Documents/SkyrimNative/launch-env.json over the environment bench.py launches it with, so each arm's keys go into
that file, not onto bench's command line; bench's own keys (frame times, the exit frame, the scenario's input) are
unaffected. For each run the driver pushes the arm's file, runs bench.py with the scenario and a label
`<plan>-<tag>-<arm>-r<run>`, and appends a record (results directory, worst thermal state, clean exit) to the
session's manifest.jsonl. A run that reaches thermal state *serious* is flagged; report.py leaves it out of the
per-arm means.

Nothing about a particular person's setup lives here: the device is --device or SKYRIM_BENCH_DEVICE, the app's bundle
ID SKYRIM_BUNDLE_ID (as for bench.py), and the session folder --dir (default bench-results/sessions/<plan>, which git
ignores). --tag names what is under test, e.g. the installed build's commit.

--mac BUILD_DIR runs the same plan on the macOS build (a dry run of the driver and the scenarios): the arm's keys
are passed as --env, and the manifest is mac-manifest.jsonl, kept apart from the session's own.
"""
import argparse
import json
import os
import re
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


def say(text):
    print(f"\n=== {text}", flush=True)


def stop(text):
    print(f"\nSTOP: {text}", flush=True)
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


# ---------------------------------------------------------------- the session

class Session:
    def __init__(self, plan, args):
        self.plan = plan
        self.mac = args.mac
        self.game = args.game or os.environ.get("SKYRIM_BENCH_GAME")
        self.device = None if self.mac else (args.device or os.environ.get("SKYRIM_BENCH_DEVICE"))
        self.dir = Path(args.dir) if args.dir else bench.RESULTS / "sessions" / plan["name"]
        self.dir.mkdir(parents=True, exist_ok=True)
        self.manifest = self.dir / ("mac-manifest.jsonl" if self.mac else "manifest.jsonl")
        self.backup = self.dir / "launch-env.original.json"
        self.tag = re.sub(r"[^\w.-]+", "-", args.tag or "run")
        self.holding_lock = args.holding_lock

    def need_device(self):
        if not self.device:
            stop("pass --device or set SKYRIM_BENCH_DEVICE (or --mac BUILD_DIR for a dry run)")
        return bench.Device(self.device)

    def label(self, arm, run):
        return f"{self.plan['name']}-{self.tag}-{arm}-r{run}"

    def write_presets(self):
        folder = self.dir / "presets"
        folder.mkdir(exist_ok=True)
        for arm in self.plan["arms"]:
            (folder / f"{arm}.json").write_text(json.dumps(arm_env(self.plan, arm), indent=2, sort_keys=True) + "\n")
        print(f"wrote {len(self.plan['arms'])} presets to {folder}")

    def backup_launch_env(self, device):
        """Once per session: keep the device's own launch-env.json, or note that it had none."""
        none = self.dir / "launch-env.original.none"
        if self.backup.exists() or none.exists():
            return
        if device.pull(LAUNCH_ENV, self.backup):
            print(f"saved the device's launch-env.json to {self.backup}")
        else:
            none.write_text("the device had no launch-env.json\n")
            print("the device had no launch-env.json; `restore` will leave an empty one ({})")

    def push(self, arm):
        if self.mac:
            return
        device = self.need_device()
        self.backup_launch_env(device)
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / "launch-env.json"
            local.write_text(json.dumps(arm_env(self.plan, arm), indent=2, sort_keys=True) + "\n")
            done = device.push(local, LAUNCH_ENV)
        if done.returncode:
            stop(f"couldn't place {LAUNCH_ENV} on the device: {done.stderr.strip()[-400:]}")
        print(f"launch-env.json on the device: {arm}")

    def restore(self):
        if self.mac:
            return
        device = self.need_device()
        with tempfile.TemporaryDirectory() as tmp:
            src = self.backup
            if not src.exists():
                src = Path(tmp) / "launch-env.json"
                src.write_text("{}\n")
            done = device.push(src, LAUNCH_ENV)
        if done.returncode:
            stop(f"couldn't restore {LAUNCH_ENV}: {done.stderr.strip()[-400:]}")
        print("launch-env.json restored" + ("" if self.backup.exists() else " (empty: the device had none)"))

    def bench_run(self, scenario, arm, run, sequence_name):
        if self.mac and LOCK and LOCK.exists() and not self.holding_lock:
            stop(f"{LOCK} exists: another job owns this Mac (pass --holding-lock if it is yours)")
        self.push(arm)
        cmd = [sys.executable, str(HERE / "bench.py"), "run", scenario, "--label", self.label(arm, run)]
        env = dict(os.environ)
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
        done = subprocess.run(cmd, capture_output=True, text=True, env=env)
        out = done.stdout + done.stderr
        print(out[-1500:])
        m = re.search(r"bench: results in (\S+)", out)
        run_dir = Path(m[1]) if m else None
        meta = {}
        if run_dir and (run_dir / "summary.json").exists():
            meta = json.loads((run_dir / "summary.json").read_text()).get("meta", {})
        rec = {"utc": datetime.now(timezone.utc).isoformat(timespec="seconds"), "sequence": sequence_name,
               "scenario": scenario, "arm": arm, "run": run, "label": self.label(arm, run),
               "target": "mac" if self.mac else "device", "dir": str(run_dir) if run_dir else None,
               "bench_exit": done.returncode, "seconds": round(time.time() - t0),
               "thermal": meta.get("thermal", {}).get("worst", "unknown"),
               "complete": bool(meta.get("exited_cleanly"))}
        with open(self.manifest, "a") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"thermal: {rec['thermal']}   complete: {rec['complete']}   {rec['seconds']} s")
        if rec["thermal"] in ("serious", "critical"):
            print("THERMAL: this run reached serious or worse and is left out of comparisons. Let the headset cool "
                  "(off the head, a few minutes on AC) before the next run.")
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
                ok = device.pull(base + name, run_dir / name)
                print(f"  {frame} {name}: {'ok' if ok else 'missing'}")


# ---------------------------------------------------------------- steps

def cmd_check(s):
    check = s.plan.get("check")
    if not check:
        stop("the plan has no check")
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
    print("\nLAUNCH CHECK PASSED.")


def cmd_run(s, name, start=1, only=None, layers=False):
    plan = sequence(s.plan, name)
    for i, (scenario, arm, run) in enumerate(plan, 1):
        if i < start or (only and i != only):
            continue
        print(f"\n[{i}/{len(plan)}]")
        rec, run_dir, _ = s.bench_run(scenario, arm, run, name)
        if layers:
            s.pull_layers(run_dir)
        if not rec["complete"]:
            print(f"WARNING: run {i} didn't complete; carry on, then repeat it with --only {i} if there is time.")


def cmd_ask(s):
    qs = s.plan.get("questions", [])
    if not qs:
        stop("the plan has no questions")
    lines = [f"{datetime.now():%Y-%m-%d %H:%M}, {s.plan['name']}, {s.tag}"]
    for q in qs:
        lines.append(f"{q} -> {input(q + ' ').strip()}")
    path = s.dir / "answers.txt"
    with open(path, "a") as f:
        f.write("\n".join(lines) + "\n\n")
    print(f"saved to {path}")


def cmd_status(s):
    if not s.manifest.exists():
        print("no runs yet")
        return
    for line in s.manifest.read_text().splitlines():
        r = json.loads(line)
        print(f"{r['utc']}  {r.get('sequence', '-'):<9} {r['scenario']:<22} {r['arm']:<24} r{r['run']}  "
              f"thermal {r['thermal']:<8} {'ok' if r['complete'] else 'INCOMPLETE'}  {r['dir']}")


def cmd_report(s):
    import report
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
    ap.add_argument("step", choices=["presets", "check", "run", "push", "ask", "restore", "status", "report"])
    ap.add_argument("what", nargs="?", help="run: the sequence; push: the arm")
    ap.add_argument("--device", help="Vision Pro UDID (default: SKYRIM_BENCH_DEVICE)")
    ap.add_argument("--mac", metavar="BUILD_DIR", help="a dry run on a macOS build instead of the headset")
    ap.add_argument("--game", help="with --mac: the game folder (default: SKYRIM_BENCH_GAME)")
    ap.add_argument("--dir", help="the session folder (default: bench-results/sessions/<plan>)")
    ap.add_argument("--tag", help="what is under test, for the run labels, e.g. the installed build's commit")
    ap.add_argument("--from", dest="start", type=int, default=1, help="run: start at run N")
    ap.add_argument("--only", type=int, help="run: only run N")
    ap.add_argument("--holding-lock", action="store_true", help="with --mac: the machine's bench lock is yours")
    ap.add_argument("--pull-layers", action="store_true", help="run: also pull layer-1 shots and depth files")
    a = ap.parse_args(argv)
    s = Session(load_plan(a.plan), a)
    if a.step in ("run", "push") and not a.what:
        ap.error(f"{a.step} needs a {'sequence' if a.step == 'run' else 'arm'}")
    {"presets": s.write_presets, "check": lambda: cmd_check(s),
     "run": lambda: cmd_run(s, a.what, a.start, a.only, a.pull_layers),
     "push": lambda: s.push(a.what), "ask": lambda: cmd_ask(s), "restore": s.restore,
     "status": lambda: cmd_status(s), "report": lambda: cmd_report(s)}[a.step]()


if __name__ == "__main__":
    main()
