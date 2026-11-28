#!/usr/bin/env python3
"""Checks bench.py's summary, the per-phase budget (budget.py), the report and the session plan on a synthetic log.

No game, device or real log is needed: the log below is made up, in the formats the runtime, DXVK and the
compositor print.
"""
import argparse
import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402
import budget  # noqa: E402
import report  # noqa: E402
import session  # noqa: E402

PHASES = {"idle": [100, 200], "fight": [200, 300]}


def synthetic_log():
    """Frames 1-299 at 22.2 ms, a loading window first (the game at 20 fps), then two phases."""
    out = ["native: thermal nominal", "present pacing: at most 45 fps (LB_MAX_FPS)",
           "hitch: sampling frames longer than 40 ms", "info:  DXVK: Read 1234 valid state cache entries",
           "immersive: layer 7, foveation off, started"]
    for n in range(1, 300):
        if n == 50:  # during loading: a slow window, which overall must leave out
            out += ["dxvk: gpu busy 10%, 5.0 ms per frame over 40 frames (20 fps)",
                    "immersive: 90 Hz presented, 20 fps from the game",
                    "immersive: compositor gpu 9.00 ms per frame, p95 9.50 ms, max 9.90 ms",
                    "immersive cadence: 40 game frames over 180 refreshes; held 1: 0, 2: 0, 3: 0, 4+: 40; "
                    "replaced unseen: 0; game gpu latency mean 30.00 ms, max 90.00 ms"]
        if n == 120:
            out.append("pipe compiled ahead: 1.5 ms")
        if n == 250:  # two compiles at draw, both belonging to frame 250 (the next ft line)
            out += ["pipe compiled at draw: 30.0 ms", "pipe compiled at draw: 12.5 ms"]
        if n == 295:  # a screenshot stalls the frame whose ft line follows it (after the fight's windows end)
            out.append("LB_SHOT frame 295 1280x880 -> /tmp/shot-00295.bmp")
        ms = {250: 61.0, 295: 120.0}.get(n, 22.2)
        out.append(f"ft {n} {ms:.2f} 0.01 {8.0 if n < 200 else 12.0:.2f} 1500")
        if ms > 40:
            out.append(f"hitch: frame {n} took {ms:.0f} ms, 3 stack samples")
        if n == 199:  # windows ending in idle
            out += ["dxvk: gpu busy 50%, 11.0 ms per frame over 90 frames (45 fps)",
                    "immersive: 90 Hz presented, 45 fps from the game",
                    "immersive: compositor gpu 3.00 ms per frame, p95 3.50 ms, max 4.00 ms",
                    "immersive cadence: 90 game frames over 180 refreshes; held 1: 0, 2: 90, 3: 0, 4+: 0; "
                    "replaced unseen: 0; game gpu latency mean 14.00 ms, max 16.00 ms",
                    "power: battery 90% (charging), thermal fair, low power 0, cpu 1.00 cores"]
        if n == 290:  # windows mostly in the fight
            out += ["dxvk: gpu busy 60%, 13.0 ms per frame over 90 frames (45 fps)",
                    "immersive: 90 Hz presented, 44 fps from the game",
                    "immersive: compositor gpu 4.00 ms per frame, p95 5.00 ms, max 6.00 ms",
                    "immersive cadence: 88 game frames over 180 refreshes; held 1: 2, 2: 84, 3: 1, 4+: 1; "
                    "replaced unseen: 1; game gpu latency mean 16.00 ms, max 40.00 ms"]
    out.append("bench: exit frame 299 reached")
    return "\n".join(out) + "\n"


class BudgetTest(unittest.TestCase):
    @staticmethod
    def parse(text):
        *_, s, _ = bench.parse_log(text, {"name": "synthetic", "phases": PHASES})
        return s

    def setUp(self):
        self.text = synthetic_log()
        *_, self.summary, self.exited = bench.parse_log(self.text, {"name": "synthetic", "phases": PHASES})

    def test_phase_stats(self):
        idle, fight = self.summary["idle"], self.summary["fight"]
        self.assertEqual(idle["frames"], 100)
        self.assertEqual(idle["work_mean_ms"], 8.0)
        self.assertEqual(fight["over_33ms"], 1)  # frame 250; frame 295 is the screenshot's stall, left out
        self.assertEqual(self.summary["meta"]["screenshot_frames"], {295: 120.0})

    def test_windows_by_phase(self):
        idle, fight = self.summary["idle"]["budget"], self.summary["fight"]["budget"]
        self.assertEqual(idle["gpu_busy"]["ms"], 11.0)
        self.assertTrue(idle["gpu_busy"]["capped"])
        self.assertEqual(idle["compositor_gpu"]["mean_ms"], 3.0)
        self.assertEqual(idle["cadence"]["held"], {"1": 0, "2": 90, "3": 0, "4+": 0})
        self.assertEqual(idle["cadence"]["game_gpu_latency_mean_ms"], 14.0)
        self.assertEqual(fight["gpu_busy"]["ms"], 13.0)
        self.assertEqual(fight["cadence"]["held_2_share"], round(84 / 88, 3))
        self.assertEqual(fight["cadence"]["game_gpu_latency_max_ms"], 40.0)
        self.assertEqual(fight["compositor_gpu"]["p95_ms"], 5.0)

    def with_menu(self):
        """The synthetic log plus a window before the phases at the frame cap with little GPU work (a menu)."""
        ft = "ft 80 22.20 0.01 8.00 1500\n"
        self.assertIn(ft, self.text)
        return self.text.replace(ft, ft + "dxvk: gpu busy 5%, 2.0 ms per frame over 30 frames (45 fps)\n")

    def test_overall_keeps_phase_windows(self):
        *_, s, _ = bench.parse_log(self.with_menu(), {"phases": PHASES})
        b = s["overall"]["budget"]
        self.assertEqual(b["basis"], {"gpu_busy": "phases", "cadence": "phases", "compositor_gpu": "phases"})
        self.assertEqual(b["gpu_busy"]["windows"], 2)  # neither the loading window nor the menu
        self.assertEqual(b["gpu_busy"]["ms_range"], [11.0, 13.0])
        self.assertEqual(b["compositor_gpu"]["mean_range_ms"], [3.0, 4.0])
        self.assertEqual(b["cadence"]["game_gpu_latency_mean_ms"], round((14 * 90 + 16 * 88) / 178, 2))
        # the whole-run figure bench.py always had still covers every window
        self.assertEqual(s["gpu"]["windows"], 4)

    def test_overall_without_phases_keeps_steady_windows(self):
        *_, s, _ = bench.parse_log(self.with_menu())
        b = s["overall"]["budget"]
        self.assertEqual(b["basis"], {"gpu_busy": "steady", "cadence": "steady", "compositor_gpu": "steady"})
        self.assertEqual(b["gpu_busy"]["windows"], 3)  # the menu runs at the cap, so only loading is left out
        self.assertEqual(b["gpu_busy"]["ms_range"], [2.0, 13.0])

    def test_compiles_and_hitches(self):
        fight, idle = self.summary["fight"]["budget"], self.summary["idle"]["budget"]
        self.assertEqual(fight["pipelines"], {"at_draw": 2, "at_draw_ms": 42.5, "at_draw_max_ms": 30.0,
                                              "ahead": 0, "ahead_ms": 0})
        self.assertEqual(idle["pipelines"]["ahead"], 1)
        self.assertEqual(fight["hitches"], {"threshold_ms": 40.0, "frames": 1, "max_ms": 61.0})
        self.assertEqual(idle["hitches"]["frames"], 0)

    def test_run_facts(self):
        meta = self.summary["meta"]
        self.assertEqual(meta["thermal"], {"worst": "fair", "last": "fair", "seen": ["nominal", "fair"]})
        self.assertEqual(meta["frame_cap_fps"], 45)
        self.assertEqual(meta["state_cache_entries"], 1234)
        self.assertEqual(meta["compositor"], {"foveation": False, "refine_steps": None})
        self.assertTrue(self.exited)

    def test_uncapped_keeps_every_window(self):
        text = self.text.replace("present pacing: at most 45 fps (LB_MAX_FPS)", "present pacing: off, 0 fps (LB_MAX_FPS)")
        *_, s, _ = bench.parse_log(text, {"phases": PHASES})
        b = s["overall"]["budget"]
        self.assertFalse(b["gpu_busy"]["capped"])
        self.assertEqual((b["gpu_busy"]["windows"], b["basis"]["gpu_busy"]), (2, "phases"))
        *_, s, _ = bench.parse_log(text)
        b = s["overall"]["budget"]
        self.assertEqual((b["gpu_busy"]["windows"], b["basis"]["gpu_busy"]), (3, "all"))

    def test_log_without_frame_times(self):
        """A play session without LB_FRAMETIMES: everything lands in overall."""
        text = "\n".join(line for line in self.text.splitlines() if not line.startswith("ft "))
        *_, s, _ = bench.parse_log(text)
        b = s["overall"]["budget"]
        self.assertEqual(b["gpu_busy"]["windows"], 2)
        self.assertEqual(b["pipelines"]["at_draw"], 2)
        self.assertEqual(b["pipelines"]["ahead"], 1)

    def test_old_log_has_no_budget_lines(self):
        *_, s, _ = bench.parse_log("ft 1 16.6 0.1\nft 2 16.7 0.1\n")
        self.assertEqual(s["overall"]["budget"]["pipelines"]["at_draw"], 0)
        self.assertNotIn("cadence", s["overall"]["budget"])
        self.assertEqual(s["meta"]["thermal"]["worst"], "unknown")


class ReportTest(unittest.TestCase):
    def test_reparse_and_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / "20260101T000000Z-synthetic-budget-test-stereo-r1"
            run.mkdir()
            (run / "game.log").write_text(synthetic_log())
            (run / "console.txt").write_text("")
            (run / "summary.json").write_text(json.dumps(
                {"meta": {"phases": PHASES, "label": "budget-test-stereo-r1", "scenario": "synthetic",
                          "exited_cleanly": True}}))
            s = bench.reparse(run)
            self.assertEqual(s["fight"]["budget"]["pipelines"]["at_draw"], 2)
            self.assertEqual(s["meta"]["label"], "budget-test-stereo-r1")
            manifest = Path(tmp) / "manifest.jsonl"
            manifest.write_text(json.dumps({"dir": str(run), "arm": "stereo", "run": 1}) + "\n")
            out = Path(tmp) / "summary.md"
            with contextlib.redirect_stdout(io.StringIO()):
                report.main(["--manifest", str(manifest), "--out", str(out)])
            text = out.read_text()
            self.assertIn("| 1 | fight | 99 | 12.00 / 12.0 | 22.2 | 1 | yes | 2/84/1/1 |", text)
            self.assertIn("2 / 42 / 30", text)
            self.assertIn("| synthetic | budget-test | stereo | 1 | idle |", text)
            self.assertIn("| yes | - |", text)  # a record without "problems" was not checked

            # a run whose manifest record lists failed checks: shown in the runs table, left out of the means
            manifest.write_text(json.dumps({"dir": str(run), "arm": "stereo", "run": 1,
                                            "problems": ["frame cap not 45"]}) + "\n")
            with contextlib.redirect_stdout(io.StringIO()):
                report.main(["--manifest", str(manifest), "--out", str(out)])
            text = out.read_text()
            self.assertIn("| yes | frame cap not 45 |", text)
            self.assertNotIn("| synthetic | budget-test | stereo | 1 | idle |", text)


class WindowTest(unittest.TestCase):
    """budget.py's placing of windows in phases, on hand-made scans."""

    def scan(self, gpu=(), cadence=(), cap=45):
        sc = budget.scan("")
        sc.update(cap=cap, gpu=list(gpu), cadence=list(cadence))
        return sc

    @staticmethod
    def gpu(end, frames, ms):
        return {"start": end - frames, "end": end, "pct": 50.0, "ms": ms, "frames": frames, "fps": 45.0}

    @staticmethod
    def cadence(end, frames, held, lat_max=20.0):
        return {"start": end - frames, "end": end, "frames": frames, "refreshes": 2 * frames, "held": held,
                "unseen": 0, "fps": 45.0, "lat_mean": 15.0, "lat_max": lat_max}

    def test_split_window_counts_half_in_each_phase(self):
        """A 100-frame window logged at frame 99 covers frames 0-99: half in [0, 50), half in [50, 100)."""
        sc = self.scan([self.gpu(99, 100, 10.0)])
        for span in ((0, 50), (50, 100)):
            b = budget.phase_budget(sc, span)
            self.assertEqual((b["gpu_busy"]["frames"], b["gpu_busy"]["ms"]), (50, 10.0), span)

    def test_window_weighted_by_its_share(self):
        """A window a quarter inside the phase brings a quarter of its frames, and no range or maximum."""
        sc = self.scan([self.gpu(124, 100, 40.0), self.gpu(224, 100, 10.0)])
        b = budget.phase_budget(sc, (100, 300))
        self.assertEqual(b["gpu_busy"]["frames"], 125)
        self.assertEqual(b["gpu_busy"]["ms"], round((40 * 25 + 10 * 100) / 125, 3))
        self.assertEqual(b["gpu_busy"]["ms_range"], [10.0, 10.0])

    def test_console_windows_count_for_overall(self):
        """Windows only in the console can't be placed in phases, but overall keeps them."""
        windowed = ("dxvk: gpu busy", "immersive cadence", "immersive: compositor")
        text = "\n".join(line for line in synthetic_log().splitlines() if not line.startswith(windowed))
        console = "dxvk: gpu busy 50%, 11.0 ms per frame over 90 frames (45 fps)\n"
        *_, s, _ = bench.parse_log(text, {"phases": PHASES}, console)
        self.assertEqual(s["overall"]["budget"]["gpu_busy"]["ms"], 11.0)
        self.assertNotIn("gpu_busy", s["idle"]["budget"])
        self.assertNotIn("gpu_busy", s["fight"]["budget"])

    def test_screenshot_window_dropped_when_a_clean_one_remains(self):
        sc = self.scan([self.gpu(150, 100, 10.0), self.gpu(250, 100, 30.0)],
                       [self.cadence(150, 100, [0, 100, 0, 0]), self.cadence(250, 100, [0, 99, 0, 1], 300.0)])
        b = budget.phase_budget(sc, (50, 300), stalled={200: 300.0})
        self.assertEqual((b["gpu_busy"]["windows"], b["gpu_busy"]["ms"]), (1, 10.0))
        self.assertEqual(b["gpu_busy"]["screenshots"], {"windows": 1, "dropped": True})
        self.assertEqual(b["cadence"]["held"]["4+"], 0)
        self.assertEqual(b["cadence"]["game_gpu_latency_max_ms"], 20.0)

    def test_screenshot_window_kept_and_corrected_when_alone(self):
        sc = self.scan(cadence=[self.cadence(250, 100, [0, 98, 0, 2], 300.0)])
        b = budget.phase_budget(sc, (150, 300), stalled={200: 300.0})
        c = b["cadence"]
        self.assertEqual((c["held"], c["game_frames"]), ({"1": 0, "2": 98, "3": 0, "4+": 1}, 99))
        self.assertEqual(c["screenshots"], {"windows": 1, "dropped": False})

    def test_overall_follows_each_phase(self):
        """A screenshot inside the fight's windows: the fight has to keep them, and so does overall, although the
        idle phase has clean windows."""
        text = synthetic_log().replace("ft 280 ", "LB_SHOT frame 280 1280x880 -> /tmp/shot-00280.bmp\nft 280 ")
        s = BudgetTest.parse(text)
        self.assertEqual(s["fight"]["budget"]["gpu_busy"]["screenshots"], {"windows": 1, "dropped": False})
        overall = s["overall"]["budget"]
        self.assertEqual((overall["gpu_busy"]["windows"], overall["gpu_busy"]["ms_range"]), (2, [11.0, 13.0]))
        self.assertEqual(overall["gpu_busy"]["screenshots"], {"windows": 1, "dropped": False})
        self.assertEqual(overall["cadence"]["held"]["4+"], 0)  # the fight's one held-4+ frame was the screenshot

    def test_frame_budget(self):
        b = BudgetTest.parse(synthetic_log())
        fight = b["fight"]["budget"]["frame_budget"]
        self.assertEqual(fight, {"budget_ms": 22.22, "refresh_hz": 90.0, "late_ms": 25.0, "frames": 99, "late": 1,
                                 "fits": True, "over": []})
        self.assertTrue(b["overall"]["budget"]["frame_budget"]["fits"])

    def test_frame_budget_counts_frames_held_a_third_refresh(self):
        """At 45 fps a frame that misses a refresh lasts 33.3 ms: under the 33.4 ms of over_33ms, but late."""
        text = synthetic_log().replace("ft 210 22.20 0.01 12.00", "ft 210 33.30 0.01 12.00")
        b = BudgetTest.parse(text)
        self.assertEqual(b["fight"]["over_33ms"], 1)
        self.assertEqual(b["fight"]["budget"]["frame_budget"]["late"], 2)

    def test_frame_budget_does_not_fit(self):
        text = synthetic_log().replace("game gpu latency mean 16.00 ms", "game gpu latency mean 24.00 ms")
        b = BudgetTest.parse(text)
        self.assertEqual((b["fight"]["budget"]["frame_budget"]["fits"], b["fight"]["budget"]["frame_budget"]["over"]),
                         (False, ["game gpu"]))
        self.assertTrue(b["idle"]["budget"]["frame_budget"]["fits"])
        overall = b["overall"]["budget"]["frame_budget"]
        self.assertEqual((overall["fits"], overall["over"]), (False, ["game gpu"]))

    def test_uncapped_has_no_budget(self):
        text = synthetic_log().replace("present pacing: at most 45 fps", "present pacing: off, 0 fps")
        b = BudgetTest.parse(text)
        self.assertEqual(b["fight"]["budget"]["frame_budget"], {"budget_ms": None, "fits": None})


class ReportGroupTest(unittest.TestCase):
    def test_two_builds_in_one_manifest_stay_apart(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "manifest.jsonl"
            recs = []
            for build, ms in (("buildA", "30.00"), ("buildB", "20.00")):
                run = Path(tmp) / f"x-budget-{build}-stereo-r1"
                run.mkdir()
                (run / "game.log").write_text(synthetic_log().replace("22.20 0.01", f"{ms} 0.01"))
                (run / "summary.json").write_text(json.dumps(
                    {"meta": {"phases": PHASES, "label": f"budget-{build}-stereo-r1", "scenario": "s",
                              "exited_cleanly": True}}))
                recs.append({"dir": str(run), "arm": "stereo", "run": 1, "label": f"budget-{build}-stereo-r1"})
            recs.append({"dir": str(Path(tmp) / "gone"), "arm": "stereo", "run": 2, "label": "budget-buildA-stereo-r2"})
            manifest.write_text("".join(json.dumps(r) + "\n" for r in recs))
            out = Path(tmp) / "summary.md"
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
                report.main(["--manifest", str(manifest), "--out", str(out)])
            text = out.read_text()
            self.assertIn("| s | budget-buildA | stereo | 1 | idle | 8.00 / 8.0 | 30.0 |", text)
            self.assertIn("| s | budget-buildB | stereo | 1 | idle | 8.00 / 8.0 | 20.0 |", text)
            self.assertIn("no longer exists", err.getvalue())

    def test_tag_of(self):
        self.assertEqual(report.tag_of("budget-0123abc-stereo-880-r2", "stereo-880"), "budget-0123abc")
        self.assertEqual(report.tag_of("budget-0123abc-stereo-880-r2"), "budget-0123abc-stereo-880")


# A device stereo run whose log shows every key of STEREO_ENV arriving: made-up lines in the formats the app
# (native: launch-env.json, the identity's source_commit), the runtime and the compositor print.
STEREO_ENV = {"LB_MAX_FPS": "45", "LB_STEREO_MULTIVIEW": "1", "LB_FOVEATION": "1", "LB_IMMERSIVE_REFINE": "2",
              "LB_DEPTH_COPY": "1"}
STEREO_LOG = "\n".join([
    *(f"native: launch-env.json: {k}={v}" for k, v in STEREO_ENV.items()),
    'native: identity {"source_commit":"0123abcd","platform":"test"}',
    "present pacing: at most 45 fps (LB_MAX_FPS)",
    "LB_DEPTH_COPY: the R32F depth pass is a copy of the main pass's depth",
    "stereo: live single-pass stereo, d 2.240 units",
    "immersive: layer 81, foveation on, started",
    "immersive: per-pixel depth search: 2 steps",
    "immersive: stereo frames: each eye draws its own picture (eye units 2.24)",
    "ft 1 22.20 0.01 8.00 1500",
]) + "\n"


class CheckRunTest(unittest.TestCase):
    def check(self, text=STEREO_LOG, env=STEREO_ENV, **kw):
        kw = {"device": True, "immersive": True, "build": "0123abcd", **kw}
        return session.check_run(text, env, **kw)

    def test_all_applied(self):
        self.assertEqual(self.check(), [])
        self.assertEqual(self.check(build="0123abcd4567"), [])  # a longer commit matches the logged short one

    def test_key_not_echoed_or_ignored(self):
        text = STEREO_LOG.replace("native: launch-env.json: LB_FOVEATION=1\n", "")
        self.assertEqual(self.check(text), ["keys not applied: LB_FOVEATION"])
        text = STEREO_LOG + "native: launch-env.json: ignoring HOME\n"
        self.assertEqual(self.check(text), ["keys ignored: HOME"])

    def test_compositor_settings(self):
        text = STEREO_LOG.replace("foveation on", "foveation off").replace("2 steps", "4 steps")
        self.assertEqual(self.check(text), ["foveation not on (the layer may not support it)",
                                            "depth search not 2 steps"])

    def test_build(self):
        self.assertEqual(self.check(build="fedc9876"), ["build 0123abcd, not fedc9876"])
        text = STEREO_LOG.replace('"source_commit":"0123abcd",', "")
        self.assertEqual(self.check(text), ["no build commit in the log"])
        self.assertEqual(self.check(text, build=None), [])

    def test_frame_cap_depth_copy_stereo(self):
        env = {**STEREO_ENV, "LB_MAX_FPS": "0", "LB_DEPTH_COPY": "0"}
        text = STEREO_LOG.replace("LB_MAX_FPS=45", "LB_MAX_FPS=0").replace("LB_DEPTH_COPY=1", "LB_DEPTH_COPY=0")
        self.assertEqual(self.check(text, env), ["frame cap not off", "depth copy on"])
        text = STEREO_LOG.replace("stereo: live single-pass stereo, d 2.240 units\n", "")
        self.assertEqual(self.check(text), ["stereo not live"])
        mono = {k: v for k, v in STEREO_ENV.items() if k != "LB_STEREO_MULTIVIEW"}
        self.assertEqual(self.check(env=mono), ["stereo on in a mono arm"])
        self.assertEqual(self.check(STEREO_LOG + "LB_DEPTH_COPY: copy failed (hr 80004005); the pass is drawn\n"),
                         ["depth copy failed"])

    def test_fatal_and_mac(self):
        self.assertEqual(self.check(STEREO_LOG + "[mvk-error] VK_ERROR_DEVICE_LOST\n"),
                         ["fatal-looking lines: [mvk-error]"])
        # the Mac: keys go in as the environment (nothing echoes them) and there is no compositor
        text = "\n".join(line for line in STEREO_LOG.splitlines()
                         if not line.startswith(("native:", "immersive:"))) + "\n"
        self.assertEqual(self.check(text, device=False), [])


class SessionLoopTest(unittest.TestCase):
    """cmd_run's thermal handling, with bench runs replaced by records of a given thermal state."""

    def run_loop(self, thermals):
        plan = {"name": "t", "minutes": {"a": 2.0},
                "arms": {"x": [{"LB_MAX_FPS": "45"}]}, "sequences": {"s": [["a", "x", n] for n in range(1, 6)]}}
        slept, ran = [], []
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(mac="build", game=None, device=None, dir=tmp, tag=None, build=None,
                                      holding_lock=False, cooldown=7)
            s = session.Session(plan, args)
            s.bench_run = lambda scenario, arm, run, name: (
                ran.append(run) or ({"complete": True, "problems": [], "thermal": thermals[run - 1]}, None, ""))
            real_sleep, session.time.sleep = session.time.sleep, slept.append
            try:
                with contextlib.redirect_stdout(io.StringIO()) as out:
                    try:
                        session.cmd_run(s, "s")
                        stopped = None
                    except SystemExit:
                        stopped = out.getvalue().split("STOP: ", 1)[1].strip()
            finally:
                session.time.sleep = real_sleep
        return ran, slept, stopped

    def test_cool_down_then_carry_on(self):
        ran, slept, stopped = self.run_loop(["nominal", "serious", "fair", "serious", "nominal"])
        self.assertEqual((ran, slept, stopped), ([1, 2, 3, 4, 5], [7, 7], None))

    def test_serious_twice_stops(self):
        ran, slept, stopped = self.run_loop(["nominal", "serious", "serious", "nominal", "nominal"])
        self.assertEqual((ran, slept), ([1, 2, 3], [7]))
        self.assertIn("--from 4", stopped)

    def test_unknown_does_not_reset_serious(self):
        ran, slept, stopped = self.run_loop(["serious", "unknown", "serious", "nominal", "nominal"])
        self.assertEqual((ran, slept), ([1, 2, 3], [7]))
        self.assertIn("run s --from 4", stopped)

    def test_critical_stops(self):
        ran, slept, stopped = self.run_loop(["critical", "nominal", "nominal", "nominal", "nominal"])
        self.assertEqual((ran, slept), ([1], []))
        self.assertIn("--from 2", stopped)


class FakeDevice:
    """bench.Device's calls that session.py makes, against a dict standing in for the app's container."""

    def __init__(self, udid="fake", files=None, error=None, hang=False):
        self.udid, self.files, self.error, self.hang = udid, dict(files or {}), error, hang

    def copy_from(self, source, destination):
        if self.hang:
            raise subprocess.TimeoutExpired("devicectl", 120)
        if self.error:
            return subprocess.CompletedProcess([], 1, "", self.error)
        if source not in self.files:
            return subprocess.CompletedProcess([], 1, "", "ERROR: The file doesn't exist. "
                                                         "(NSCocoaErrorDomain error 260.)")
        Path(destination).write_text(self.files[source])
        return subprocess.CompletedProcess([], 0, "", "")

    def push(self, source, destination):
        if self.hang:
            raise subprocess.TimeoutExpired("devicectl", 120)
        self.files[destination] = Path(source).read_text()
        return subprocess.CompletedProcess([], 0, "", "")

    def unlocked(self):
        return True


PLAN = {"name": "t", "minutes": {"a": 2.0}, "arms": {"x": [{"LB_MAX_FPS": "45"}]},
        "sequences": {"s": [["a", "x", n] for n in range(1, 4)]}}


def args(**kw):
    return argparse.Namespace(**{"mac": None, "game": None, "device": "fake", "dir": None, "tag": None,
                                 "build": None, "holding_lock": False, "cooldown": 0, **kw})


def quietly(fn, *a):
    """Runs fn, returning (its STOP message or None, everything printed)."""
    with contextlib.redirect_stdout(io.StringIO()) as out:
        try:
            fn(*a)
            stopped = None
        except SystemExit:
            stopped = out.getvalue().split("STOP: ", 1)[1].strip() if "STOP: " in out.getvalue() else "exit"
    return stopped, out.getvalue()


class SessionFolderTest(unittest.TestCase):
    def test_default_folder_per_tag_and_list_touches_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            real, bench.RESULTS = bench.RESULTS, Path(tmp)
            try:
                s = session.Session(PLAN, args(build="0123abcd"))
                self.assertEqual(s.dir, Path(tmp) / "sessions" / "t-0123abcd")
                self.assertEqual(session.Session(PLAN, args(build="0123abcd", tag="wide")).dir.name, "t-wide")
                quietly(session.cmd_list, s)
                self.assertEqual(list(Path(tmp).iterdir()), [])
                stopped, _ = quietly(session.cmd_status, s)
                self.assertIn("no session folder", stopped)
            finally:
                bench.RESULTS = real

    def test_refuses_another_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "manifest.jsonl").write_text(json.dumps({"build": "aaaa111", "label": "x"}) + "\n")
            s = session.Session(PLAN, args(dir=tmp, build="bbbb222"))
            stopped, _ = quietly(session.cmd_run, s, "s")
            self.assertIn("holds runs of build aaaa111, not bbbb222", stopped)
            session.Session(PLAN, args(dir=tmp, build="aaaa111")).check_build()  # the same build may add runs


class LaunchEnvTest(unittest.TestCase):
    def session(self, tmp, device):
        s = session.Session(PLAN, args(dir=tmp))
        s.need_device = lambda: device
        return s

    def test_backup_and_restore(self):
        with tempfile.TemporaryDirectory() as tmp:
            dev = FakeDevice(files={session.LAUNCH_ENV: '{"LB_WINDOW": "1920x1080"}\n'})
            s = self.session(tmp, dev)
            quietly(s.push, "x")
            self.assertEqual(json.loads(dev.files[session.LAUNCH_ENV]), {"LB_MAX_FPS": "45"})
            quietly(s.restore)
            self.assertEqual(json.loads(dev.files[session.LAUNCH_ENV]), {"LB_WINDOW": "1920x1080"})

    def test_left_over_arm_keeps_the_earlier_backup(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "launch-env.original.json").write_text('{"MINE": "1"}\n')
            dev = FakeDevice(files={session.LAUNCH_ENV: '{"LB_MAX_FPS": "45"}'})
            s = self.session(tmp, dev)
            quietly(s.push, "x")
            self.assertEqual(Path(tmp, "launch-env.original.json").read_text(), '{"MINE": "1"}\n')

    def test_no_file_versus_devicectl_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            s = self.session(tmp, FakeDevice())
            quietly(s.push, "x")
            self.assertTrue(Path(tmp, "launch-env.original.none").exists())
        with tempfile.TemporaryDirectory() as tmp:
            dev = FakeDevice(error="ERROR: The device is not connected. (com.apple.dt.CoreDeviceError error 4000.)")
            stopped, _ = quietly(self.session(tmp, dev).push, "x")
            self.assertIn("couldn't read the device's launch-env.json", stopped)
            self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), [])
            self.assertNotIn(session.LAUNCH_ENV, dev.files)

    def test_timeout_is_a_stop(self):
        with tempfile.TemporaryDirectory() as tmp:
            stopped, _ = quietly(self.session(tmp, FakeDevice(hang=True)).push, "x")
            self.assertIn("didn't answer within 120 s", stopped)

    def test_run_restores_when_it_stops(self):
        """main: a sequence that stops (or is interrupted) after a push puts the device's own file back."""
        with tempfile.TemporaryDirectory() as tmp:
            plan = Path(tmp, "t.json")
            plan.write_text(json.dumps(PLAN))
            dev = FakeDevice(files={session.LAUNCH_ENV: '{"MINE": "1"}'})
            calls = []

            def fake_run(self, scenario, arm, run, name):
                calls.append(run)
                self.push(arm)
                if run == 2:
                    raise KeyboardInterrupt
                return {"complete": True, "problems": [], "thermal": "nominal"}, None, ""

            saved = (session.bench.Device, session.Session.bench_run, signal.getsignal(signal.SIGTERM),
                     signal.getsignal(signal.SIGHUP))
            session.bench.Device = lambda udid: dev
            session.Session.bench_run = fake_run
            try:
                stopped, out = quietly(session.main, [str(plan), "run", "s", "--device", "fake", "--dir", tmp])
            finally:
                session.bench.Device, session.Session.bench_run = saved[:2]
                signal.signal(signal.SIGTERM, saved[2])
                signal.signal(signal.SIGHUP, saved[3])
            self.assertEqual(calls, [1, 2])
            self.assertIn(f"resume with `session.py {plan} --dir {tmp} run s --from 2`", out)
            self.assertIn("launch-env.json restored", out)
            self.assertEqual(dev.files[session.LAUNCH_ENV], '{"MINE": "1"}')


class BenchRunTest(unittest.TestCase):
    def test_environment_and_results_path(self):
        """bench.py's own options don't leak in from the operator's shell, and a results path may hold spaces."""
        seen = {}

        class FakePopen:
            def __init__(self, cmd, env=None, **kw):
                seen["env"] = env
                self.stdout = iter(["bench: running a\n", "bench: results in /no where/run 1  \n"])
                self.returncode = 0

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        with tempfile.TemporaryDirectory() as tmp:
            s = session.Session(PLAN, args(dir=tmp, mac="build", device=None, build="0123abcd"))
            real, session.subprocess.Popen = session.subprocess.Popen, FakePopen
            os.environ["BENCH_REENTER"] = "30"
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    rec, run_dir, _ = s.bench_run("a", "x", 1, "s")
            finally:
                session.subprocess.Popen = real
                del os.environ["BENCH_REENTER"]
            self.assertNotIn("BENCH_REENTER", seen["env"])
            self.assertEqual(run_dir, Path("/no where/run 1"))
            self.assertEqual((rec["build"], rec["tag"], rec["problems"]), ("0123abcd", "0123abcd", ["no log"]))
            self.assertEqual(json.loads(Path(tmp, "mac-manifest.jsonl").read_text())["dir"], "/no where/run 1")


class PlanTest(unittest.TestCase):
    def test_budget_plan(self):
        plan = session.load_plan("budget")
        env = session.arm_env(plan, "stereo-1100-uncapped")
        self.assertEqual(env["LB_MAX_FPS"], "0")  # a later fragment overrides an earlier one
        self.assertEqual(env["LB_WINDOW"], "1600x1100")
        self.assertNotIn("LB_FOV", env)  # stereo arms leave the field of view to the eyes
        timed = session.sequence(plan, "timed")
        self.assertEqual(len(timed), 12)
        self.assertEqual(timed[:3], [("boss-fight-avp", "stereo-1100", 1), ("boss-fight-avp", "stereo-880", 1),
                                     ("boss-fight-avp", "mono-880", 1)])
        for scenario, arm, _ in timed + session.sequence(plan, "warmup") + session.sequence(plan, "uncapped"):
            self.assertTrue((HERE / "scenarios" / f"{scenario}.json").exists(), scenario)
            session.arm_env(plan, arm)
        total, unknown = session.estimate(plan, timed)
        # six boss fights at 4.4 min and six market runs at 4.0; every scenario has an estimate
        self.assertEqual((round(total, 1), unknown), (50.4, 0))
        self.assertIsNotNone(session.minutes(plan, plan["check"]["scenario"]))


if __name__ == "__main__":
    unittest.main()
