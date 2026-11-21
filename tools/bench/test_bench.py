#!/usr/bin/env python3
"""Checks bench.py's summary, the per-phase budget (budget.py), the report and the session plan on a synthetic log.

No game, device or real log is needed: the log below is made up, in the formats the runtime, DXVK and the
compositor print.
"""
import argparse
import contextlib
import io
import json
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
        if n == 271:  # a screenshot stalls the frame whose ft line follows it
            out.append("LB_SHOT frame 271 1280x880 -> /tmp/shot-00271.bmp")
        ms = {250: 61.0, 271: 120.0}.get(n, 22.2)
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
    def setUp(self):
        self.text = synthetic_log()
        *_, self.summary, self.exited = bench.parse_log(self.text, {"name": "synthetic", "phases": PHASES})

    def test_phase_stats(self):
        idle, fight = self.summary["idle"], self.summary["fight"]
        self.assertEqual(idle["frames"], 100)
        self.assertEqual(idle["work_mean_ms"], 8.0)
        self.assertEqual(fight["over_33ms"], 1)  # frame 250; frame 271 is the screenshot's stall, left out
        self.assertEqual(self.summary["meta"]["screenshot_frames"], {271: 120.0})

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
            self.assertIn("| 1 | fight | 99 | 12.00 / 12.0 | 22.2 | 1 | 2/84/1/1 |", text)
            self.assertIn("2 / 42 / 30", text)
            self.assertIn("| synthetic | stereo | 1 | idle |", text)
            self.assertIn("| yes | - |", text)  # a record without "problems" was not checked

            # a run whose manifest record lists failed checks: shown in the runs table, left out of the means
            manifest.write_text(json.dumps({"dir": str(run), "arm": "stereo", "run": 1,
                                            "problems": ["frame cap not 45"]}) + "\n")
            with contextlib.redirect_stdout(io.StringIO()):
                report.main(["--manifest", str(manifest), "--out", str(out)])
            text = out.read_text()
            self.assertIn("| yes | frame cap not 45 |", text)
            self.assertNotIn("| synthetic | stereo | 1 | idle |", text)


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

    def test_critical_stops(self):
        ran, slept, stopped = self.run_loop(["critical", "nominal", "nominal", "nominal", "nominal"])
        self.assertEqual((ran, slept), ([1], []))
        self.assertIn("--from 2", stopped)


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
