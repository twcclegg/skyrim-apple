#!/usr/bin/env python3
"""Replay a recorded camera through runtime/motion_path.c and say how smooth the display would be.

No headset and no Mac: record a run of any build with LB_CAMLOG=1 LB_FRAMETIMES=1 (every frame's camera and
frame time go to stderr), then

    tools/motion_sim.py run.log --game-fps 45

The log's own frames, at whatever rate the recording ran, are taken as where the player really was. The tool then
plays a slower game (every frame nearest to 1/game-fps apart), a display at --display-hz, and the compositor
asking motion_path.c where the camera is for each display frame, and compares three things per display frame with
where the player really was: the path, the newest game frame held still (what the compositor does without the
path), and the heading the same way (the right stick turning the view).

    step error: how far each display frame's movement is from the real movement over the same time. This is the
                judder: zero means the world moves exactly as the player does.
    behind:     how far the shown position trails where the player really is when the display shows it. That
                includes the 30 ms between drawing and showing, which is the same for both.

--synthetic writes no log and plays a made-up run (walk, strafe, stop, turn, fast travel) instead.
"""
import argparse
import ctypes
import math
from pathlib import Path
import random
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CAM = re.compile(r"cam \d+: rot \[([^\]]+)\] pos (\S+) (\S+) (\S+)")
CAM_NONE = re.compile(r"cam \d+: none")
FT = re.compile(r"\bft \d+ ([\d.]+)")


class Motion(ctypes.Structure):
    _fields_ = [("moved", ctypes.c_float * 3), ("offset", ctypes.c_float),
                ("has_moved", ctypes.c_int), ("has_offset", ctypes.c_int)]


def load(output):
    output.mkdir(parents=True, exist_ok=True)
    library = output / "libmotion_path.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-std=gnu11", str(ROOT / "runtime/motion_path.c"), "-o",
                    str(library), "-lm", "-lpthread"], check=True)
    lib = ctypes.CDLL(str(library))
    lib.motion_configure.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_double]
    lib.motion_camera.argtypes = [ctypes.c_double, ctypes.POINTER(ctypes.c_float)]
    lib.motion_offset.argtypes = [ctypes.c_double, ctypes.c_float, ctypes.c_int]
    lib.motion_query_at.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.c_float, ctypes.c_float,
                                    ctypes.c_float, ctypes.POINTER(Motion)]
    return lib


def read_log(path):
    """[(seconds, (x, y, z) or None, heading or None)], one per frame that logged both a camera and a time."""
    samples, camera, clock = [], False, 0.0
    for line in path.read_text(errors="replace").splitlines():
        found = CAM.search(line)
        if found:
            rot = [float(v) for v in found.group(1).replace("|", " ").split()]
            camera = (tuple(float(found.group(i)) for i in (2, 3, 4)), math.atan2(rot[0], rot[3]))
            continue
        if CAM_NONE.search(line):
            camera = (None, None)
            continue
        found = FT.search(line)
        if found:
            clock += float(found.group(1)) / 1000.0
            if camera:
                samples.append((clock, camera[0], camera[1]))
            camera = False
    return samples


def synthetic(rate=240.0):
    """Ten seconds at a high rate: run north, strafe, stop dead, turn on the spot, run again, fast travel, run."""
    samples, x, y, heading = [], 0.0, 0.0, 0.0
    for i in range(int(10 * rate)):
        t = i / rate
        forward = 370 if t < 2 or 5 <= t < 7 or t >= 7.5 else 0
        strafe = 250 if 2 <= t < 3 else 0
        if 3.5 <= t < 5:
            heading += 2.0 / rate  # the right stick: 115 degrees a second
        if abs(t - 7.5) < 0.5 / rate:
            x, y = x + 50000, y - 20000
        x += (forward * math.sin(heading) + strafe * math.cos(heading)) / rate
        y += (forward * math.cos(heading) - strafe * math.sin(heading)) / rate
        bob = 2.0 * math.sin(t * 12) if forward else 0.0
        samples.append((t, (x, y, 100 + bob), heading))
    return samples


def truth_at(samples, index, t):
    """The recording at time t (between its frames), starting the search at index. Returns (index, pos, heading)."""
    while index + 1 < len(samples) and samples[index + 1][0] <= t:
        index += 1
    a = samples[index]
    b = samples[min(index + 1, len(samples) - 1)]
    if a[1] is None or b[1] is None or b[0] <= a[0] or t <= a[0]:
        return index, a[1], a[2]
    f = min(1.0, (t - a[0]) / (b[0] - a[0]))
    jump = math.dist(a[1], b[1]) / (b[0] - a[0]) > 4000
    if jump:
        return index, (a[1] if f < 1 else b[1]), a[2]
    turn = (b[2] - a[2] + math.pi) % (2 * math.pi) - math.pi
    return index, tuple(a[1][k] + (b[1][k] - a[1][k]) * f for k in range(3)), a[2] + turn * f


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def simulate(lib, samples, game_fps, display_hz, lag_ms, ahead_ms, gpu_ms, seed):
    lib.motion_configure(1, -1.0 if lag_ms is None else lag_ms / 1000.0, ahead_ms / 1000.0)
    noise = random.Random(seed)
    # The slower game: the recording's frames nearest to 1/game_fps apart.
    frames, due = [], samples[0][0]
    for sample, following in zip(samples, samples[1:] + samples[-1:]):
        if abs(sample[0] - due) <= abs(following[0] - due) or sample[0] >= due:
            frames.append(sample)
            due += 1.0 / game_fps
            while due <= sample[0]:
                due += 1.0 / game_fps
    rows, pushed, shown, index = [], 0, None, 0
    previous = None
    t = samples[0][0] + 0.5
    end = samples[-1][0]
    while t < end:
        # Drawing starts 28 to 30 ms before the display shows the result, with what the game has presented by then.
        now = t - 0.030 + 0.002 * noise.random()
        while pushed < len(frames) and frames[pushed][0] <= now:
            when, position, heading = frames[pushed]
            if position is None:
                lib.motion_camera(when, None)
                lib.motion_offset(when, 0.0, 0)
            else:
                lib.motion_camera(when, (ctypes.c_float * 3)(*position))
                lib.motion_offset(when, heading, 1)
            pushed += 1
        # The compositor has the newest frame the GPU finished.
        ready = [f for f in frames[max(0, pushed - 4):pushed] if f[0] + gpu_ms / 1000.0 <= now]
        if ready:
            shown = ready[-1]
        index, real, real_heading = truth_at(samples, index, t)
        # A frame from the other side of a teleport or a loading screen is late, not juddering: left out.
        if shown and shown[1] is not None and real is not None and math.dist(shown[1], real) < 1000:
            motion = Motion()
            lib.motion_query_at(now, t, shown[1][0], shown[1][1], shown[1][2], ctypes.byref(motion))
            path = tuple(shown[1][k] + (motion.moved[k] if motion.has_moved else 0.0) for k in range(3))
            path_heading = motion.offset if motion.has_offset else shown[2]
            current = (t, real, real_heading, path, path_heading, shown[1], shown[2])
            if previous and t - previous[0] < 1.5 / display_hz:
                real_step = [real[k] - previous[1][k] for k in range(3)]
                if math.hypot(*real_step) * display_hz < 4000:  # not across a teleport
                    rows.append({
                        "path_step": math.dist([path[k] - previous[3][k] for k in range(3)], real_step),
                        "held_step": math.dist([shown[1][k] - previous[5][k] for k in range(3)], real_step),
                        "path_behind": math.dist(path, real),
                        "held_behind": math.dist(shown[1], real),
                        "path_turn": abs(wrap(path_heading - previous[4]) - wrap(real_heading - previous[2])),
                        "held_turn": abs(wrap(shown[2] - previous[6]) - wrap(real_heading - previous[2])),
                        "moving": math.hypot(*real_step) * display_hz > 20,
                    })
            previous = current
        else:
            previous = None
        t += 1.0 / display_hz
    return rows, len(frames)


def summary(rows, key, scale=1.0):
    values = sorted(row[key] * scale for row in rows)
    if not values:
        return "no data"
    mean = sum(values) / len(values)
    return f"mean {mean:7.3f}  95% {values[int(len(values) * 0.95)]:7.3f}  max {values[-1]:8.3f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", type=Path, help="stderr of a run with LB_CAMLOG=1 LB_FRAMETIMES=1")
    parser.add_argument("--synthetic", action="store_true", help="play a made-up run instead of a log")
    parser.add_argument("--game-fps", type=float, default=45.0)
    parser.add_argument("--display-hz", type=float, default=90.0)
    parser.add_argument("--lag-ms", type=float, default=None, help="LB_IMMERSIVE_MOTION_LAG_MS (default: one game frame)")
    parser.add_argument("--ahead-ms", type=float, default=20.0, help="LB_IMMERSIVE_MOTION_AHEAD_MS")
    parser.add_argument("--gpu-ms", type=float, default=10.0, help="time from Present to the frame reaching the compositor")
    parser.add_argument("--units-per-meter", type=float, default=70.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/motion-test")
    arguments = parser.parse_args()
    if arguments.synthetic == bool(arguments.log):
        parser.error("give a log or --synthetic")
    samples = synthetic() if arguments.synthetic else read_log(arguments.log)
    with_camera = [s for s in samples if s[1] is not None]
    if len(with_camera) < 100:
        sys.exit(f"only {len(with_camera)} frames with a camera and a frame time; record with LB_CAMLOG=1 LB_FRAMETIMES=1")
    length = samples[-1][0] - samples[0][0]
    print(f"recording: {len(samples)} frames over {length:.1f} s ({len(samples) / length:.0f} fps), "
          f"{len(with_camera)} with the world camera")
    rows, game_frames = simulate(load(arguments.output_dir), samples, arguments.game_fps, arguments.display_hz,
                                 arguments.lag_ms, arguments.ahead_ms, arguments.gpu_ms, arguments.seed)
    moving = [row for row in rows if row["moving"]]
    print(f"played as {game_frames / length:.1f} game fps on a {arguments.display_hz:.0f} Hz display: "
          f"{len(rows)} display frames, {len(moving)} of them with the player moving")
    centimetres = 100.0 / arguments.units_per_meter
    print("while moving, in centimetres:")
    print(f"  step error, path:   {summary(moving, 'path_step', centimetres)}")
    print(f"  step error, held:   {summary(moving, 'held_step', centimetres)}")
    print(f"  behind, path:       {summary(moving, 'path_behind', centimetres)}")
    print(f"  behind, held:       {summary(moving, 'held_behind', centimetres)}")
    print("all frames, heading in degrees:")
    print(f"  step error, path:   {summary(rows, 'path_turn', math.degrees(1))}")
    print(f"  step error, held:   {summary(rows, 'held_turn', math.degrees(1))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
