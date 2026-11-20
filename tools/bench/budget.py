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
before it, and its window starts <n> frames earlier (gpu busy, cadence) or at the previous line of its kind
(compositor). A window counts towards a phase when at least half of it lies inside the phase. With no ft lines (a
play session without LB_FRAMETIMES) everything lands in `overall`.

`overall` also covers loading and menus, where the game runs far below its cap and a window says little about
the budget. So `overall` keeps only steady windows, those in which the game delivered at least 90% of its frame
cap, when there are any (`basis: "steady"`; otherwise `basis: "all"`). Named phases are play by construction and
keep every window, hitches included.
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


def _r(x, nd=2):
    return None if x is None else round(x, nd)


def _span(values):
    return [_r(min(values)), _r(max(values))] if values else None


def scan(text):
    """One pass over a game log: frames, compiles, hitches, windowed lines and run-level facts."""
    out = {"frames": [], "compiles": [], "hitches": [], "gpu": [], "cadence": [], "compositor": [],
           "thermal": [], "cap": None, "state_cache": None, "hitch_ms": None, "foveation": None, "refine_steps": None}
    last = None         # the last ft frame number seen
    pending = []        # compiles waiting for their frame's ft line
    hz, fps = None, None  # the latest "Hz presented" line, for the compositor and cadence lines after it
    prev_comp = None
    for line in text.splitlines():
        m = FT.match(line)
        if m:
            last = int(m[1])
            out["frames"].append(last)
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


def _in_window(w, span):
    """Whether a window (start, end in frames) counts towards the phase span [a, b)."""
    if span is None:
        return True
    a, b = span
    if w["end"] is None:  # before the first frame, or from the console: whole run only
        return False
    start = w["start"] if w["start"] is not None else w["end"]
    if w["end"] <= start:
        return a <= w["end"] < b
    overlap = min(w["end"], b) - max(start, a)
    return overlap >= 0.5 * (w["end"] - start)


def _steady(windows, cap):
    if not cap:
        return windows, "all"
    keep = [w for w in windows if w.get("fps") is not None and w["fps"] >= STEADY * cap]
    return (keep, "steady") if keep else (windows, "all")


def _gpu_block(ws, cap):
    if not ws:
        return None
    frames = sum(w["frames"] for w in ws)
    n = frames or len(ws)  # frame-weighted, like bench.py's whole-run figure; plain mean if no window has frames
    weight = (lambda w: w["frames"]) if frames else (lambda w: 1)
    return {"windows": len(ws), "frames": frames,
            "ms": _r(sum(w["ms"] * weight(w) for w in ws) / n, 3),
            "pct": _r(sum(w["pct"] * weight(w) for w in ws) / n),
            "ms_range": _span([w["ms"] for w in ws]), "pct_range": _span([w["pct"] for w in ws]),
            "capped": None if cap is None else cap > 0}


def _cadence_block(ws):
    if not ws:
        return None
    held = [sum(w["held"][i] for w in ws) for i in range(4)]
    frames = sum(held)
    lat = [w for w in ws if w["lat_mean"] is not None]
    lat_frames = sum(w["frames"] for w in lat)
    return {"windows": len(ws), "game_frames": sum(w["frames"] for w in ws),
            "refreshes": sum(w["refreshes"] for w in ws),
            "held": dict(zip(("1", "2", "3", "4+"), held)),
            "held_2_share": _r(held[1] / frames, 3) if frames else None,
            "replaced_unseen": sum(w["unseen"] for w in ws),
            "game_gpu_latency_mean_ms": _r(sum(w["lat_mean"] * w["frames"] for w in lat) / lat_frames)
            if lat_frames else None,
            "game_gpu_latency_mean_range_ms": _span([w["lat_mean"] for w in lat]),
            "game_gpu_latency_max_ms": _r(max(w["lat_max"] for w in lat)) if lat else None}


def _compositor_block(ws):
    if not ws:
        return None
    return {"windows": len(ws), "mean_ms": _r(sum(w["mean"] for w in ws) / len(ws)),
            "mean_range_ms": _span([w["mean"] for w in ws]), "p95_ms": _r(max(w["p95"] for w in ws)),
            "max_ms": _r(max(w["max"] for w in ws))}


def phase_budget(sc, span=None, stalled=(), steady=False):
    """The budget block for one phase: span is [start, end) in frames, or None for the whole run (overall)."""
    cap = sc["cap"]
    inside = (lambda f: f is not None and span[0] <= f < span[1]) if span else (lambda f: True)
    gpu = [w for w in sc["gpu"] if _in_window(w, span)]
    cad = [w for w in sc["cadence"] if _in_window(w, span)]
    comp = [w for w in sc["compositor"] if _in_window(w, span)]
    basis = None
    if steady:
        gpu, b1 = _steady(gpu, cap)
        cad, b2 = _steady(cad, cap)
        comp, b3 = _steady(comp, cap)
        basis = {"gpu_busy": b1, "cadence": b2, "compositor_gpu": b3}
    draw = [ms for f, kind, ms in sc["compiles"] if kind == "at draw" and inside(f)]
    ahead = [ms for f, kind, ms in sc["compiles"] if kind == "ahead" and inside(f)]
    hitches = [ms for f, ms in sc["hitches"] if inside(f) and f not in stalled]
    out = {
        "gpu_busy": _gpu_block(gpu, cap),
        "cadence": _cadence_block(cad),
        "compositor_gpu": _compositor_block(comp),
        "pipelines": {"at_draw": len(draw), "at_draw_ms": _r(sum(draw), 1), "at_draw_max_ms": _r(max(draw, default=0), 1),
                      "ahead": len(ahead), "ahead_ms": _r(sum(ahead), 1)},
        "hitches": {"threshold_ms": sc["hitch_ms"], "frames": len(hitches), "max_ms": _r(max(hitches, default=0), 0)}
        if sc["hitch_ms"] is not None or hitches else None,
    }
    if basis:
        out["basis"] = basis
    return {k: v for k, v in out.items() if v is not None}


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
    """Adds `budget` to every phase of a bench summary (overall over steady windows) and run facts to its meta.
    Windowed lines are taken from the game log; only if it has none are the console's used, for overall alone
    (the console has no ft lines to place them by)."""
    sc = scan(text)
    if console and not (sc["gpu"] or sc["cadence"] or sc["compositor"]):
        other = scan(console)
        for key in ("gpu", "cadence", "compositor"):
            sc[key] = [dict(w, start=None, end=None) for w in other[key]]
        if not sc["thermal"]:
            sc["thermal"] = other["thermal"]
    for name, s in summary.items():
        if name in ("meta", "gpu") or not isinstance(s, dict):
            continue
        if name == "overall":
            s["budget"] = phase_budget(sc, None, stalled, steady=True)
        elif name in phases:
            s["budget"] = phase_budget(sc, tuple(phases[name]), stalled)
    summary.setdefault("meta", {}).update(run_facts(sc))
    return summary
