#!/usr/bin/env python3
"""Frame-budget report for a set of bench runs: one Markdown file that says, phase by phase, whether a headset
configuration fits its frame budget (1000 / the run's frame cap: 22.2 ms at 45 fps).

    report.py --manifest SESSION/manifest.jsonl [--out SESSION/summary.md] [--json all.json]
    report.py RESULTS_DIR [RESULTS_DIR ...]      bench result directories, re-parsed with today's parser
    report.py GAME_LOG                           a log pulled by hand (a play session): one whole-run row

Every run is re-parsed from its game.log and console.txt (bench.py `reparse`), so results recorded before a parser
change report the same figures as new ones. Nothing in the result directories is changed.

Per run and phase: work mean and p95, frame p95, late frames (longer than the budget plus a quarter refresh: 25 ms
at 45 fps on 90 Hz, so every frame held for a third refresh counts), whether the phase fits (work p95 and game GPU
latency within the budget; budget.py's `frame_budget`), immersive cadence (refreshes each game frame
was held: the target is every frame held 2 at 45 fps on 90 Hz), game GPU latency (an upper bound on the game
frame's GPU time, see ipad/immersive_bridge.mm), compositor GPU time per refresh, DXVK GPU busy per frame (and
whether the frame cap was on: capped busy time on Apple GPUs understates the work, so compare it only with other
capped runs), pipelines compiled at draw time (count, total and longest ms), hitch-sampler frames (LB_HITCH_MS), and
the worst thermal state of the run. Then a mean per scenario, tag (what is under test: the build, by default) and arm
over complete runs below *serious*, leaving out warm-up runs (run 0) and runs whose manifest record lists failed
checks (session.py's check_run), with how many of them fit. Windows that contain a screenshot frame are marked `s`
where they could not be left out (budget.py).
"""
import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

BAD_THERMAL = ("serious", "critical")


def f(x, nd=1):
    return "-" if x is None else f"{x:.{nd}f}"


def span(r, nd=1):
    return "-" if not r else f"{r[0]:.{nd}f}-{r[1]:.{nd}f}" if r[0] != r[1] else f"{r[0]:.{nd}f}"


def load(path, scenario=None):
    """A summary for a results directory (re-parsed) or a plain log (one whole-run phase); None, with a warning,
    when it has no log to parse."""
    path = Path(path)
    if path.is_dir() and not (path / "game.log").exists():
        print(f"report: skipping {path}: no game.log", file=sys.stderr)
        return None
    if path.is_dir():
        s = bench.reparse(path, scenario)
        s["meta"].setdefault("label", path.name)
        return s
    *_, s, exited = bench.parse_log(path.read_text(errors="replace"), scenario)
    s["meta"].update({"label": path.name, "scenario": (scenario or {}).get("name"), "exited_cleanly": exited})
    return s


def tag_of(label, arm=None):
    """The tag in a session run label `<plan>-<tag>-<arm>-r<run>` (with the plan's name in front when the arm isn't
    known): the label without the run number and the arm."""
    base = re.sub(r"-r\d+$", "", label or "")
    return base[:-len(arm) - 1] if arm and base.endswith("-" + arm) else base


def runs_from(args):
    """[(summary, arm, run number, tag)] from the manifest or the paths given (arm and tag None for a path)."""
    out = []
    if args.manifest:
        for line in Path(args.manifest).read_text().splitlines():
            if not line.strip():
                continue
            rec = json.loads(line)
            if not rec.get("dir"):
                continue  # a run that produced no results: session.py's status lists it
            if not Path(rec["dir"]).exists():
                print(f"report: skipping {rec.get('label')}: {rec['dir']} no longer exists", file=sys.stderr)
                continue
            s = load(rec["dir"])
            if s is None:
                continue
            if rec.get("problems") is not None:  # session.py's checks that the arm reached the game
                s["meta"]["checks"] = rec["problems"]
            arm = rec.get("arm")
            tag = rec.get("tag") or tag_of(rec.get("label") or s["meta"].get("label"), arm)
            out.append((s, arm, rec.get("run"), tag))
    for p in args.paths:
        s = load(p)
        if s is None:
            continue
        label = s["meta"].get("label", "")
        m = re.search(r"-r(\d+)$", label)
        out.append((s, None, int(m[1]) if m else None, None))  # the label (as the arm) names the tag too
    return out


def phases_of(s):
    names = [k for k, v in s.items() if k not in ("meta", "gpu") and isinstance(v, dict)]
    named = [k for k in names if k != "overall"]
    return named or ["overall"]


def row(s, phase):
    """The report's figures for one phase of one run, as a flat dict (None where the log has no such line)."""
    p, b = s.get(phase, {}), s.get(phase, {}).get("budget", {})
    cad, comp, gpu = b.get("cadence") or {}, b.get("compositor_gpu") or {}, b.get("gpu_busy") or {}
    pipes, hitch = b.get("pipelines") or {}, b.get("hitches") or {}
    held = cad.get("held")
    fb = b.get("frame_budget") or {}
    return {
        "frames": p.get("frames"), "work_mean": p.get("work_mean_ms"), "work_p95": p.get("work_p95_ms"),
        "frame_p95": p.get("p95_ms"), "over_250": p.get("over_250ms"),
        "budget_ms": fb.get("budget_ms"), "late": fb.get("late") if fb.get("budget_ms") else None,
        "fits": fb.get("fits"), "over": fb.get("over") or [],
        "shots": any((b.get(k) or {}).get("screenshots", {}).get("dropped") is False
                     for k in ("cadence", "gpu_busy", "compositor_gpu")),
        "held": held, "held_2_share": cad.get("held_2_share"),
        "game_gpu": cad.get("game_gpu_latency_mean_ms"), "game_gpu_max": cad.get("game_gpu_latency_max_ms"),
        "comp": comp.get("mean_ms"), "comp_range": comp.get("mean_range_ms"), "comp_p95": comp.get("p95_ms"),
        "busy": gpu.get("ms"), "busy_range": gpu.get("ms_range"), "busy_pct_range": gpu.get("pct_range"),
        "capped": gpu.get("capped"),
        "at_draw": pipes.get("at_draw"), "at_draw_ms": pipes.get("at_draw_ms"),
        "at_draw_max": pipes.get("at_draw_max_ms"), "ahead": pipes.get("ahead"),
        "hitches": hitch.get("frames") if hitch else None,
    }


def checks(meta):
    """The Runs table's checks cell: '-' when the run wasn't checked (no session manifest), else ok or the problems."""
    c = meta.get("checks")
    return "-" if c is None else "; ".join(c) if c else "ok"


def fits_cell(r):
    """'yes', 'no (work p95, game gpu)', or '-' when there is no budget (uncapped) or nothing to judge by."""
    return "-" if r["fits"] is None else "yes" if r["fits"] else f"no ({', '.join(r['over'])})"


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def render(runs):
    out = []
    p = out.append
    p("## Runs\n")
    p("| # | run | scenario | arm | thermal (worst) | frame cap | clean exit | checks |")
    p("|---|---|---|---|---|---|---|---|")
    for i, (s, arm, _, _) in enumerate(runs, 1):
        m = s["meta"]
        cap = m.get("frame_cap_fps")
        p(f"| {i} | {m.get('label')} | {m.get('scenario') or '-'} | {arm or '-'} | "
          f"{m.get('thermal', {}).get('worst', 'unknown')} | {'-' if cap is None else 'off' if cap == 0 else cap} | "
          f"{'yes' if m.get('exited_cleanly') else 'NO'} | {checks(m)} |")

    p("\n## Per phase\n")
    p("Work and frame times in ms; *held* counts game frames shown for 1/2/3/4+ refreshes (target: all 2); "
      "*game gpu* is the mean GPU latency per game frame (an upper bound); *compositor* is GPU ms per refresh, mean "
      "and the range of its 5 s windows; *gpu busy* is DXVK's per-frame figure and its window range ('c' = capped); "
      "*at draw* is pipelines compiled at draw time: count / total ms / longest ms. *late* counts frames longer than "
      "the budget (1000 / frame cap) plus a quarter refresh; *fits* says whether work p95 and game gpu are within the "
      "budget. `overall`'s windows are those of the named phases, or without phases those where the game held at "
      "least 90% of its cap; 's' marks figures whose windows include a screenshot's stall.\n")
    p("| # | phase | frames | work mean / p95 | frame p95 | late | fits | held 1/2/3/4+ | game gpu mean / max | "
      "compositor mean (range) | gpu busy ms (range) | at draw | ahead | hitches |")
    p("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for i, (s, arm, _, _) in enumerate(runs, 1):
        for phase in phases_of(s):
            r = row(s, phase)
            held = "/".join(str(r["held"][k]) for k in ("1", "2", "3", "4+")) + (" s" if r["shots"] else "") \
                if r["held"] else "-"
            busy = "-" if r["busy"] is None else \
                f"{r['busy']:.1f} ({span(r['busy_range'])}){' c' if r['capped'] else ''}"
            draw = "-" if r["at_draw"] is None else \
                f"{r['at_draw']} / {r['at_draw_ms']:.0f} / {r['at_draw_max']:.0f}"
            p(f"| {i} | {phase} | {r['frames'] or '-'} | {f(r['work_mean'], 2)} / {f(r['work_p95'])} | "
              f"{f(r['frame_p95'])} | {'-' if r['late'] is None else r['late']} | {fits_cell(r)} | {held} | "
              f"{f(r['game_gpu'])} / {f(r['game_gpu_max'])} | {f(r['comp'], 2)} ({span(r['comp_range'], 2)}) | {busy} | "
              f"{draw} | {'-' if r['ahead'] is None else r['ahead']} | "
              f"{'-' if r['hitches'] is None else r['hitches']} |")

    groups = defaultdict(list)
    for s, arm, run, tag in runs:
        m = s["meta"]
        if not m.get("exited_cleanly") or m.get("thermal", {}).get("worst") in BAD_THERMAL or run == 0 \
                or m.get("checks"):
            continue
        # by tag too, so two builds (or other tags) in one manifest never share a mean
        groups[(m.get("scenario") or "-", tag or "-", arm or tag_of(m.get("label", "")))].append(s)
    if groups:
        p("\n## Per scenario, tag and arm (mean of complete runs below serious that passed their checks, warm-ups "
          "left out)\n")
        p("| scenario | tag | arm | runs | phase | work mean / p95 | frame p95 | late | fits | held 2 share | "
          "game gpu | compositor | gpu busy | at draw (count / ms) | hitches |")
        p("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for (scen, tag, arm), ss in sorted(groups.items()):
            for phase in phases_of(ss[0]):
                rs = [row(s, phase) for s in ss if phase in s]
                g = lambda k: mean([r[k] for r in rs])  # noqa: E731
                share = g("held_2_share")
                judged = [r["fits"] for r in rs if r["fits"] is not None]
                fits = f"{sum(judged)}/{len(judged)}" if judged else "-"
                p(f"| {scen} | {tag} | {arm} | {len(ss)} | {phase} | {f(g('work_mean'), 2)} / {f(g('work_p95'))} | "
                  f"{f(g('frame_p95'))} | {f(g('late'))} | {fits} | {'-' if share is None else f'{share:.0%}'} | "
                  f"{f(g('game_gpu'))} | {f(g('comp'), 2)} | {f(g('busy'))} | "
                  f"{f(g('at_draw'), 0)} / {f(g('at_draw_ms'), 0)} | {f(g('hitches'))} |")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="bench result directories or game logs")
    ap.add_argument("--manifest", help="a session manifest (session.py), one JSON record per run")
    ap.add_argument("--out", help="also write the Markdown here")
    ap.add_argument("--json", help="write every re-parsed summary here")
    args = ap.parse_args(argv)
    if not args.manifest and not args.paths:
        ap.error("give a manifest or result directories")
    runs = runs_from(args)
    if not runs:
        sys.exit("report: no runs found")
    text = render(runs)
    print(text)
    if args.out:
        Path(args.out).write_text(text)
    if args.json:
        Path(args.json).write_text(json.dumps([{"arm": a, "run": n, "tag": g, **s} for s, a, n, g in runs], indent=2))


if __name__ == "__main__":
    main()
