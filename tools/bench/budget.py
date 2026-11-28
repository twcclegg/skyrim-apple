"""Frame-budget figures from a game log, per phase: what a headset session needs to answer "does it fit".

bench.py adds these to every phase of summary.json (as `budget`) and to its meta; report.py tabulates them. All the
lines are optional, so an older log, or a Mac log with no compositor, gives empty blocks rather than an error.

Lines read, by what logs them:
  ft <frame> <ms> ...                                   LB_FRAMETIMES: frame numbers place every other line in time
  pipe compiled at draw: <ms> / pipe compiled ahead: <ms>   LB_PIPE_LOG=1 (DXVK). A compile belongs to the frame
                                                        whose ft line follows it.
  hitch: frame <n> took <ms> ms, ...                    LB_HITCH_MS: the frames the hitch sampler caught
  hitch: sampling frames longer than <ms> ms            its threshold
  dxvk: gpu busy <pct>%, <ms> ms per frame over <n> frames (<fps> fps)
                                                        LB_POWER_LOG_S: one window of <n> frames
  immersive cadence: <n> game frames over <r> refreshes; held 1: a, 2: b, 3: c, 4+: d; replaced unseen: u
      [; game gpu latency mean <x> ms, max <y> ms]      the compositor, every 900 refreshes
  immersive: <hz> Hz presented, <fps> fps from the game the compositor, every 5 s, before...
  immersive: compositor gpu <mean> ms per frame, p95 <p> ms, max <m> ms   ...its own GPU time per refresh
  native: thermal <state> / power: ... thermal <state>  the run's thermal state
  present pacing: at most <n> fps (LB_MAX_FPS) / present pacing: off, 0 fps   the frame cap
  DXVK: Read <n> valid state cache entries

The windowed lines (gpu busy, cadence, compositor) carry no frame number. Each is placed at the last ft frame logged
before it and covers the frames (last - n, last]: n is its frame count (gpu busy, cadence), or the frames since the
previous line of its kind (compositor). A window contributes to a phase in proportion to the share of its frames
inside the phase: means and counts are weighted by that share, so a window half outside a phase brings half its
weight. Ranges and maxima come from the windows at least half inside (all contributing windows if none is). With no
ft lines (a play session without LB_FRAMETIMES) everything lands in `overall`; so do windows read from the console,
which has no ft lines to place them by.

`overall` also covers loading and menus, where a window says little about the budget: a loading screen runs far
below the cap, and a menu can run at the cap with almost no GPU work. So when the scenario names phases, `overall`'s
windowed blocks keep the windows that count towards any phase, with the same weights (`basis: "phases"`). Without
phases (a log pulled by hand) they keep the steady windows, those in which the game delivered at least 90% of its
frame cap, when there are any (`basis: "steady"`; otherwise `basis: "all"`). Compiles and hitches in `overall` cover
the whole run.

Screenshot frames stall for hundreds of ms, so a window that contains one shows a held-4+ frame, a high game GPU
latency and a low GPU busy share that the configuration didn't cause. A block leaves such windows out when it keeps
at least one window at least half inside the phase without a screenshot. Otherwise it keeps them, takes one held-4+
frame (and one game frame) off the cadence per screenshot frame, and says so: `screenshots: {"windows": k,
"dropped": false}` (with `true` when they were left out). `overall` takes each window as the phases did.

`frame_budget` is the phase measured against the frame cap from meta.frame_cap_fps (the runtime's `present pacing`
line): `budget_ms` is 1000 / cap; `late` counts the frames (screenshot frames left out) longer than the budget plus a
quarter refresh, which at 45 fps on 90 Hz is 25 ms and catches every frame held for a third refresh (33.3 ms);
`fits` is true when the work p95 and the game GPU latency mean (where the log has them) are both within the budget,
and `over` names whichever isn't. Uncapped runs have no budget. The refresh rate is the compositor's `Hz presented`
(90 when the log has none). `overall`'s `late` and `fits` cover the named phases when there are any.
"""
import re

FT = re.compile(r"^ft (\d+) ([\d.]+)")
PIPE = re.compile(r"pipe compiled (at draw|ahead): ([\d.]+) ms")
HITCH = re.compile(r"^hitch: frame (\d+) took ([\d.]+) ms")
HITCH_THRESHOLD = re.compile(r"^hitch: sampling frames longer than ([\d.]+) ms")
GPU_BUSY = re.compile(r"dxvk: gpu busy ([\d.]+)%, ([\d.]+) ms per frame over (\d+) frames(?: \(([\d.]+) fps\))?")
CADENCE = re.compile(r"immersive cadence: (\d+) game frames over (\d+) refreshes; held 1: (\d+), 2: (\d+), 3: (\d+), "
                     r"4\+: (\d+); replaced unseen: (\d+)(?:; game gpu latency mean ([\d.]+) ms, max ([\d.]+) ms)?")
PRESENTED = re.compile(r"immersive: ([\d.]+) Hz presented, ([\d.]+) fps from the game")
COMPOSITOR = re.compile(r"immersive: compositor gpu ([\d.]+) ms per frame, p95 ([\d.]+) ms, max ([\d.]+) ms")
THERMAL = re.compile(r"(?:native: |power: .*?)thermal (nominal|fair|serious|critical)\b")
CAP = re.compile(r"present pacing: (?:at most (\d+) fps|off,)")
STATE_CACHE = re.compile(r"DXVK: Read (\d+) valid state cache entries")
FOVEATION = re.compile(r"immersive: layer \d+, foveation (on|off)")
REFINE = re.compile(r"immersive: per-pixel depth search: (\d+) steps")
THERMAL_LEVELS = ["nominal", "fair", "serious", "critical"]
STEADY = 0.9  # a window is steady when the game delivered at least this share of its frame cap
DEFAULT_HZ = 90.0
MOSTLY = 0.5  # a window at least this share inside a phase sets its ranges and maxima


def _r(x, nd=2):
    return None if x is None else round(x, nd)


def _span(values):
    return [_r(min(values)), _r(max(values))] if values else None


def scan(text):
    """One pass over a game log: frames, compiles, hitches, windowed lines and run-level facts."""
    out = {"frames": [], "times": {}, "compiles": [], "hitches": [], "gpu": [], "cadence": [], "compositor": [],
           "thermal": [], "cap": None, "state_cache": None, "hitch_ms": None, "foveation": None, "refine_steps": None,
           "hz": []}
    last = None         # the last ft frame number seen
    pending = []        # compiles waiting for their frame's ft line
    hz, fps = None, None  # the latest "Hz presented" line, for the compositor and cadence lines after it
    prev_comp = None
    for line in text.splitlines():
        m = FT.match(line)
        if m:
            last = int(m[1])
            out["frames"].append(last)
            out["times"][last] = float(m[2])
            out["compiles"] += [(last, kind, ms) for kind, ms in pending]
            pending = []
            continue
        m = PIPE.search(line)
        if m:
            pending.append((m[1], float(m[2])))
            continue
        m = HITCH.match(line)
        if m:
            out["hitches"].append((int(m[1]), float(m[2])))
            continue
        m = HITCH_THRESHOLD.match(line)
        if m:
            out["hitch_ms"] = float(m[1])
            continue
        m = GPU_BUSY.search(line)
        if m:
            n = int(m[3])
            out["gpu"].append({"start": None if last is None else last - n, "end": last, "pct": float(m[1]),
                               "ms": float(m[2]), "frames": n, "fps": float(m[4]) if m[4] else None})
            continue
        m = CADENCE.search(line)
        if m:
            n, refreshes = int(m[1]), int(m[2])
            rate = n / refreshes * (hz or DEFAULT_HZ) if refreshes else None
            out["cadence"].append({"start": None if last is None else last - n, "end": last, "frames": n,
                                   "refreshes": refreshes, "held": [int(m[i]) for i in (3, 4, 5, 6)],
                                   "unseen": int(m[7]), "fps": rate,
                                   "lat_mean": float(m[8]) if m[8] else None,
                                   "lat_max": float(m[9]) if m[9] else None})
            continue
        m = PRESENTED.search(line)
        if m:
            hz, fps = float(m[1]), float(m[2])
            out["hz"].append(hz)
            continue
        m = COMPOSITOR.search(line)
        if m:
            start = prev_comp if prev_comp is not None else (out["frames"][0] if out["frames"] else last)
            out["compositor"].append({"start": start, "end": last, "mean": float(m[1]), "p95": float(m[2]),
                                      "max": float(m[3]), "fps": fps})
            prev_comp, fps = last, None
            continue
        m = THERMAL.search(line)
        if m:
            out["thermal"].append(m[1])
            continue
        m = CAP.search(line)
        if m and out["cap"] is None:
            out["cap"] = int(m[1]) if m[1] else 0
            continue
        m = STATE_CACHE.search(line)
        if m:
            out["state_cache"] = int(m[1])
            continue
        m = FOVEATION.search(line)
        if m:
            out["foveation"] = m[1] == "on"
            continue
        m = REFINE.search(line)
        if m:
            out["refine_steps"] = int(m[1])
    out["compiles"] += [(None, kind, ms) for kind, ms in pending]  # after the last frame: whole run only
    return out


def _share(w, span):
    """The share of a window's frames, (start, end], that lies inside the phase span [a, b)."""
    if w["end"] is None:  # before the first frame, or from the console: whole run only
        return 0.0
    a, b = span
    start = w["start"] if w["start"] is not None else w["end"]
    if w["end"] <= start:  # no frames counted: the frame it was logged at
        return 1.0 if a <= w["end"] < b else 0.0
    inside = min(w["end"] + 1, b) - max(start + 1, a)
    return max(0, inside) / (w["end"] - start)


def _has_shot(w, stalled):
    """How many screenshot frames lie in the window (start, end]."""
    if w["end"] is None or w["start"] is None:
        return 0
    return sum(1 for f in stalled if w["start"] < f <= w["end"])


def _steady(windows, cap):
    if not cap:
        return windows, "all"
    keep = [w for w in windows if w.get("fps") is not None and w["fps"] >= STEADY * cap]
    return (keep, "steady") if keep else (windows, "all")


def _mostly(ws):
    """The windows that set ranges and maxima: those at least half inside, or all of them if none is."""
    return [w for w, k in ws if k >= MOSTLY] or [w for w, _ in ws]


def _gpu_block(ws, cap):
    if not ws:
        return None
    frames = sum(w["frames"] * k for w, k in ws)
    # frame-weighted, like bench.py's whole-run figure; by share alone if no window has frames
    weight = (lambda w, k: w["frames"] * k) if frames else (lambda w, k: k)
    n = sum(weight(w, k) for w, k in ws)
    top = _mostly(ws)
    return {"windows": len(ws), "frames": round(frames),
            "ms": _r(sum(w["ms"] * weight(w, k) for w, k in ws) / n, 3),
            "pct": _r(sum(w["pct"] * weight(w, k) for w, k in ws) / n),
            "ms_range": _span([w["ms"] for w in top]), "pct_range": _span([w["pct"] for w in top]),
            "capped": None if cap is None else cap > 0}


def _cadence_block(ws):
    if not ws:
        return None
    held = [sum(w["held"][i] * k for w, k in ws) for i in range(4)]
    frames = sum(held)
    lat = [(w, k) for w, k in ws if w["lat_mean"] is not None]
    lat_frames = sum(w["frames"] * k for w, k in lat)
    top = [w for w in _mostly(lat)] if lat else []
    return {"windows": len(ws), "game_frames": round(sum(w["frames"] * k for w, k in ws)),
            "refreshes": round(sum(w["refreshes"] * k for w, k in ws)),
            "held": dict(zip(("1", "2", "3", "4+"), (round(h) for h in held))),
            "held_2_share": _r(held[1] / frames, 3) if frames else None,
            "replaced_unseen": round(sum(w["unseen"] * k for w, k in ws)),
            "game_gpu_latency_mean_ms": _r(sum(w["lat_mean"] * w["frames"] * k for w, k in lat) / lat_frames)
            if lat_frames else None,
            "game_gpu_latency_mean_range_ms": _span([w["lat_mean"] for w in top]),
            "game_gpu_latency_max_ms": _r(max(w["lat_max"] for w in top)) if top else None}


def _compositor_block(ws):
    if not ws:
        return None
    n = sum(k for _, k in ws)
    top = _mostly(ws)
    return {"windows": len(ws), "mean_ms": _r(sum(w["mean"] * k for w, k in ws) / n),
            "mean_range_ms": _span([w["mean"] for w in top]), "p95_ms": _r(max(w["p95"] for w in top)),
            "max_ms": _r(max(w["max"] for w in top))}


def _without_shots(ws, stalled, cadence=False):
    """[(window, share)] with screenshot windows left out when a clean window at least half inside remains; else all
    of them, the cadence's held-4+ count corrected by one per screenshot frame. Returns ([(original window, window,
    share)], flag or None)."""
    shot = [w for w, _ in ws if _has_shot(w, stalled)]
    if not shot:
        return [(w, w, k) for w, k in ws], None
    clean = [(w, k) for w, k in ws if not _has_shot(w, stalled)]
    if any(k >= MOSTLY for _, k in clean):
        return [(w, w, k) for w, k in clean], {"windows": len(shot), "dropped": True}
    out = []
    for w, k in ws:
        n = min(_has_shot(w, stalled), w["held"][3]) if cadence else 0
        out.append((w, dict(w, held=w["held"][:3] + [w["held"][3] - n], frames=w["frames"] - n) if n else w, k))
    return out, {"windows": len(shot), "dropped": False}


def _place(windows, span, within, stalled, cadence):
    """A block's [(window, share)] and screenshot flag. A phase takes each window by its share inside it. Overall
    with phases (`within`) takes each window as each phase did (its shares summed, at most 1), so a screenshot
    window one phase had to keep isn't dropped because another phase had clean windows; console windows, which
    can't be placed, count in full. Overall without phases takes every window in full."""
    if span is not None:
        kept, flag = _without_shots([(w, k) for w in windows if (k := _share(w, span)) > 0], stalled, cadence)
        return [(w, k) for _, w, k in kept], flag
    if not within:
        kept, flag = _without_shots([(w, 1.0) for w in windows], stalled, cadence)
        return [(w, k) for _, w, k in kept], flag
    acc, shot, dropped = {}, set(), True
    for s in within:
        kept, flag = _without_shots([(w, k) for w in windows if (k := _share(w, tuple(s))) > 0], stalled, cadence)
        for orig, w, k in kept:
            acc.setdefault(id(orig), [w, 0.0])[1] += k
        if flag:
            shot.update(id(w) for w in windows if _has_shot(w, stalled) and _share(w, tuple(s)) > 0)
            dropped = dropped and flag["dropped"]
    placed = [(w, min(1.0, k)) for w, k in acc.values()] + [(w, 1.0) for w in windows if w["end"] is None]
    return placed, {"windows": len(shot), "dropped": dropped} if shot else None


def phase_budget(sc, span=None, stalled=(), steady=False, within=None):
    """The budget block for one phase: span is [start, end) in frames, or None for the whole run (overall).
    For overall, `within` (the scenario's phase spans) takes the windows as the phases did (see _place); without
    it, `steady` keeps the windows at 90% or more of the cap."""
    cap = sc["cap"]
    inside = (lambda f: f is not None and span[0] <= f < span[1]) if span else (lambda f: True)
    blocks = {}
    for key in ("gpu", "cadence", "compositor"):
        ws = sc[key]
        if steady and not within and span is None:
            ws, blocks[key + "_basis"] = _steady(ws, cap)
        blocks[key], blocks[key + "_shots"] = _place(ws, span, within, stalled, key == "cadence")
    basis = None
    if within:
        basis = {"gpu_busy": "phases", "cadence": "phases", "compositor_gpu": "phases"}
    elif steady:
        basis = {"gpu_busy": blocks["gpu_basis"], "cadence": blocks["cadence_basis"],
                 "compositor_gpu": blocks["compositor_basis"]}
    draw = [ms for f, kind, ms in sc["compiles"] if kind == "at draw" and inside(f)]
    ahead = [ms for f, kind, ms in sc["compiles"] if kind == "ahead" and inside(f)]
    hitches = [ms for f, ms in sc["hitches"] if inside(f) and f not in stalled]
    out = {
        "gpu_busy": _gpu_block(blocks["gpu"], cap),
        "cadence": _cadence_block(blocks["cadence"]),
        "compositor_gpu": _compositor_block(blocks["compositor"]),
        "pipelines": {"at_draw": len(draw), "at_draw_ms": _r(sum(draw), 1), "at_draw_max_ms": _r(max(draw, default=0), 1),
                      "ahead": len(ahead), "ahead_ms": _r(sum(ahead), 1)},
        "hitches": {"threshold_ms": sc["hitch_ms"], "frames": len(hitches), "max_ms": _r(max(hitches, default=0), 0)}
        if sc["hitch_ms"] is not None or hitches else None,
    }
    for key, name in (("gpu", "gpu_busy"), ("cadence", "cadence"), ("compositor", "compositor_gpu")):
        if out[name] is not None and blocks[key + "_shots"]:
            out[name]["screenshots"] = blocks[key + "_shots"]
    if basis:
        out["basis"] = basis
    return {k: v for k, v in out.items() if v is not None}


def refresh_hz(sc):
    """The display's refresh rate: the compositor's highest `Hz presented`, rounded, else 90."""
    return float(round(max(sc["hz"]))) if sc["hz"] else DEFAULT_HZ


def frame_budget(sc, phase, block, spans, stalled=()):
    """A phase against its frame budget (see the module docstring): spans are the [start, end) frame spans it
    covers (None: every frame), phase its bench stats (work p95), block its budget block (game GPU latency)."""
    cap = sc["cap"]
    if not cap:
        return {"budget_ms": None, "fits": None}
    budget_ms = 1000.0 / cap
    hz = refresh_hz(sc)
    late_ms = budget_ms + 0.25 * 1000.0 / hz
    times = [ms for f, ms in sc["times"].items() if f not in stalled and ms > 0
             and (spans is None or any(a <= f < b for a, b in spans))]
    work = phase.get("work_p95_ms")
    gpu = (block.get("cadence") or {}).get("game_gpu_latency_mean_ms")
    over = [name for name, x in (("work p95", work), ("game gpu", gpu)) if x is not None and x > budget_ms]
    known = work is not None or gpu is not None
    return {"budget_ms": _r(budget_ms), "refresh_hz": hz, "late_ms": _r(late_ms), "frames": len(times),
            "late": sum(1 for ms in times if ms > late_ms),
            "fits": (not over) if known else None, "over": over}


def run_facts(sc):
    """Run-level facts for summary.json's meta."""
    seen = []
    for t in sc["thermal"]:
        if t not in seen:
            seen.append(t)
    thermal = {"worst": max(sc["thermal"], key=THERMAL_LEVELS.index), "last": sc["thermal"][-1], "seen": seen} \
        if sc["thermal"] else {"worst": "unknown", "last": "unknown", "seen": []}
    out = {"thermal": thermal, "frame_cap_fps": sc["cap"], "state_cache_entries": sc["state_cache"],
           "hitch_threshold_ms": sc["hitch_ms"]}
    if sc["foveation"] is not None or sc["refine_steps"] is not None:  # the compositor's settings (immersive runs)
        out["compositor"] = {"foveation": sc["foveation"], "refine_steps": sc["refine_steps"]}
    return out


def add_to_summary(summary, text, phases, stalled=(), console=""):
    """Adds `budget` to every phase of a bench summary (overall over the phases' windows, or the steady ones when
    there are no phases) and run facts to its meta.
    Windowed lines are taken from the game log; only if it has none are the console's used, in overall alone
    (the console has no ft lines to place them by)."""
    sc = scan(text)
    if console and not (sc["gpu"] or sc["cadence"] or sc["compositor"]):
        other = scan(console)
        for key in ("gpu", "cadence", "compositor"):
            sc[key] = [dict(w, start=None, end=None) for w in other[key]]
        if not sc["thermal"]:
            sc["thermal"] = other["thermal"]
        if not sc["hz"]:
            sc["hz"] = other["hz"]
    spans = [tuple(s) for s in phases.values()]
    fits = []
    for name, s in summary.items():
        if name in ("meta", "gpu", "overall") or not isinstance(s, dict) or name not in phases:
            continue
        s["budget"] = phase_budget(sc, tuple(phases[name]), stalled)
        s["budget"]["frame_budget"] = frame_budget(sc, s, s["budget"], [tuple(phases[name])], stalled)
        fits.append(s["budget"]["frame_budget"])
    if isinstance(summary.get("overall"), dict):
        s = summary["overall"]
        s["budget"] = phase_budget(sc, None, stalled, steady=True, within=spans or None)
        fb = frame_budget(sc, s, s["budget"], spans or None, stalled)
        if fits and fb["budget_ms"] is not None:  # the phases decide: every one of them must fit
            known = [x["fits"] for x in fits if x["fits"] is not None]
            fb["fits"] = all(known) if known else None
            fb["over"] = sorted({o for x in fits for o in x["over"]})
        s["budget"]["frame_budget"] = fb
    summary.setdefault("meta", {}).update(run_facts(sc))
    return summary
