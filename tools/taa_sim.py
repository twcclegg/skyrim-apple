#!/usr/bin/env python3
"""What the game's frames give a temporal upscaler, measured without a headset or a Mac.

Works on frames the desktop build dumps (LB_SHOT=<from>-<to> with LB_CAMLOG=1 writes shot-<frame>.bmp,
depth-<frame>.f32 and a "cam <frame>: ..." line per frame to the log; see docs/handoff/temporal-upscale.md).
Three questions, one subcommand each:

  shifts     Does each frame move by the jitter the log says it has? (run with LB_JITTER=<phases>x16, so the
             shifts are whole pixels)
  accumulate Do jittered small frames hold the detail of a picture rendered at twice the size? Adds them up on
             the larger grid, each at the place its jitter says, and compares with the larger render; the same
             sum with the jitter's sign flipped, and plain upscales of one frame, are the yardsticks.
  reproject  Do depth and camera say where each pixel was in the frame before? Fetches the frame before at
             that place and compares with the frame itself: the motion vectors the compositor gives MetalFX
             are this arithmetic.

This is not MetalFX: `accumulate` is the simplest possible sum over a still scene and says nothing about how good
the real scaler looks. It shows that the information is in the frames and that the signs are right. Two runs of
the game differ in their leaves, water and shadows, which caps the PSNR well below what the numbers would be
against the same instant (--reference-again shows the cap); `detail` does not depend on that.
tools/metalfx_temporal_probe.swift runs the real scaler over the same dumps on a Mac.

Needs numpy and Pillow. Dumps are game imagery: keep them out of the repository.
"""
import argparse
from pathlib import Path
import re
import sys

import numpy as np
from PIL import Image

CAM = re.compile(
    r"^cam (\d+): rot \[([^\]]+)\] pos (\S+) (\S+) (\S+) frustum l (\S+) r (\S+) t (\S+) b (\S+) near (\S+) far (\S+)"
    r" fov \S+(?: jitter (\S+) (\S+))?")


def cameras(log):
    """frame -> dict(rot 3x3, pos, frustum l r t b, near, far, jitter), from the log's cam lines."""
    found = {}
    for line in Path(log).read_text(errors="replace").splitlines():
        match = CAM.match(line)
        if not match:
            continue
        rot = np.array([float(x) for x in match.group(2).replace("|", " ").split()]).reshape(3, 3)
        values = [float(x) for x in match.groups()[2:11]]
        found[int(match.group(1))] = {
            "forward": rot[:, 0], "up": rot[:, 1], "right": rot[:, 2], "pos": np.array(values[0:3]),
            "frustum": values[3:7], "near": values[7], "far": values[8],
            "jitter": (float(match.group(12) or 0), float(match.group(13) or 0)),
        }
    return found


def shot(run, frame):
    return np.asarray(Image.open(Path(run) / f"shot-{frame:05d}.bmp").convert("RGB")).astype(np.float32)


def depth(run, frame):
    raw = (Path(run) / f"depth-{frame:05d}.f32").read_bytes()
    width, height = np.frombuffer(raw[:8], dtype=np.uint32)
    return np.frombuffer(raw[8:], dtype=np.float32).reshape(height, width)


def frame_range(text):
    first, _, last = text.partition("-")
    return list(range(int(first), int(last or first) + 1))


def psnr(a, b, mask=None):
    error = (a - b) ** 2
    if mask is not None:
        error = error[mask]
    mse = float(error.mean())
    return float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def detail(image, mask=None):
    """Mean step in brightness from a pixel to its neighbours: how much fine structure a picture has, whatever
    it is a picture of (two runs of the game never show the same leaves and shadows)."""
    grey = image.mean(axis=2)
    steps = np.abs(np.diff(grey, axis=1))[:-1, :] + np.abs(np.diff(grey, axis=0))[:, :-1]
    return float(steps[mask[:-1, :-1]].mean() if mask is not None else steps.mean())


def best_shift(a, b, reach):
    """How far b's picture sits right and down of a's, in whole pixels."""
    height, width = a.shape[:2]
    m = reach + 8
    best = None
    core = b[m:height - m, m:width - m]
    for dy in range(-reach, reach + 1):
        for dx in range(-reach, reach + 1):
            error = np.abs(a[m - dy:height - m - dy, m - dx:width - m - dx] - core).mean()
            if best is None or error < best[0]:
                best = (error, dx, dy)
    return best[1], best[2]


def shifts(arguments):
    cams = cameras(arguments.log or Path(arguments.run) / "log.txt")
    frames = frame_range(arguments.frames)
    # Small pictures of the middle are enough to find a whole-pixel shift, and much quicker.
    def middle(frame):
        image = shot(arguments.run, frame).mean(axis=2)
        height, width = image.shape
        return image[height // 4:3 * height // 4, width // 4:3 * width // 4]
    base = frames[0]
    first = middle(base)
    wrong = 0
    for frame in frames[1:]:
        dx, dy = best_shift(first, middle(frame), arguments.reach)
        want = (cams[frame]["jitter"][0] - cams[base]["jitter"][0], cams[frame]["jitter"][1] - cams[base]["jitter"][1])
        ok = abs(dx - want[0]) <= 0.51 and abs(dy - want[1]) <= 0.51
        wrong += not ok
        print(f"frame {frame}: log says {want[0]:+.2f} {want[1]:+.2f} px from frame {base}, picture moved {dx:+d} {dy:+d}"
              f"{'' if ok else '   <-- differs'}")
    print(f"{len(frames) - 1} frames, {wrong} differ")
    return 1 if wrong else 0


def accumulate_frames(images, jitters, scale, sign, sigma):
    """Sum of small frames on a grid `scale` times larger. A point of the scene at p (small-frame pixels, no
    jitter) is at p + jitter in a frame; each output pixel takes, from each frame, the sample nearest to it,
    weighted by how near."""
    height, width = images[0].shape[:2]
    out = np.zeros((height * scale, width * scale, 3), dtype=np.float64)
    weight = np.zeros((height * scale, width * scale, 1), dtype=np.float64)
    px = (np.arange(width * scale) + 0.5) / scale
    py = (np.arange(height * scale) + 0.5) / scale
    for image, (jx, jy) in zip(images, jitters):
        qx, qy = px + sign * jx, py + sign * jy
        ix = np.clip(np.floor(qx).astype(int), 0, width - 1)
        iy = np.clip(np.floor(qy).astype(int), 0, height - 1)
        dx, dy = (ix + 0.5 - qx) * scale, (iy + 0.5 - qy) * scale
        w = np.exp(-(dy[:, None] ** 2 + dx[None, :] ** 2) / (2 * sigma * sigma))[..., None]
        out += w * image[iy][:, ix]
        weight += w
    return (out / np.maximum(weight, 1e-9)).astype(np.float32)


def accumulate(arguments):
    cams = cameras(arguments.log or Path(arguments.low) / "log.txt")
    frames = frame_range(arguments.frames)
    images = [shot(arguments.low, f) for f in frames]
    jitters = [cams[f]["jitter"] for f in frames]
    reference = shot(arguments.reference, arguments.reference_frame)
    scale = reference.shape[1] // images[0].shape[1]
    if reference.shape[0] != images[0].shape[0] * scale or reference.shape[1] != images[0].shape[1] * scale:
        sys.exit(f"reference {reference.shape[1]}x{reference.shape[0]} is not a whole multiple of {images[0].shape[1]}x{images[0].shape[0]}")
    spread = np.array(jitters)
    print(f"{len(frames)} frames of {images[0].shape[1]}x{images[0].shape[0]}, scale {scale}; jitter x {spread[:, 0].min():+.3f}"
          f"..{spread[:, 0].max():+.3f}, y {spread[:, 1].min():+.3f}..{spread[:, 1].max():+.3f} px")
    # Pixels that hold still: the same in two reference frames, and not the interface (which never jitters).
    still = None
    if arguments.reference_other is not None:
        other = shot(arguments.reference, arguments.reference_other)
        still = np.abs(reference - other).max(axis=2) < arguments.still
        print(f"still pixels (reference frames {arguments.reference_frame} and {arguments.reference_other} agree within "
              f"{arguments.still}): {100 * still.mean():.1f}%")
    size = (reference.shape[1], reference.shape[0])
    plain = shot(arguments.low, arguments.plain_frame) if arguments.plain_frame is not None else images[0]
    results = {}
    for name, method in (("nearest", Image.NEAREST), ("bilinear", Image.BILINEAR), ("bicubic", Image.BICUBIC),
                         ("lanczos", Image.LANCZOS)):
        results[f"one frame, {name}"] = np.asarray(
            Image.fromarray(plain.astype(np.uint8)).resize(size, method)).astype(np.float32)
    results["sum, no jitter known (all at 0)"] = accumulate_frames(images, [(0, 0)] * len(images), scale, 1, arguments.sigma)
    results["sum, jitter as logged"] = accumulate_frames(images, jitters, scale, 1, arguments.sigma)
    results["sum, jitter sign flipped"] = accumulate_frames(images, jitters, scale, -1, arguments.sigma)
    if arguments.reference_again:
        results["the larger render, run again"] = shot(arguments.reference_again, arguments.reference_frame)
    full = detail(reference, still)
    print(f"{'':36} {'PSNR all':>9} {'PSNR still':>11} {'detail':>7}")
    for name, image in results.items():
        line = f"{name:36} {psnr(image, reference):9.2f}"
        line += f" {psnr(image, reference, still):11.2f}" if still is not None else f" {'':11}"
        print(line + f" {100 * detail(image, still) / full:6.0f}%")
    print("detail: mean brightness step between neighbouring pixels, as a share of the larger render's")
    if arguments.save:
        Path(arguments.save).mkdir(parents=True, exist_ok=True)
        for name, image in results.items():
            Image.fromarray(np.clip(image + 0.5, 0, 255).astype(np.uint8)).save(
                Path(arguments.save) / (re.sub(r"[^a-z0-9]+", "-", name).strip("-") + ".png"))
        Image.fromarray(reference.astype(np.uint8)).save(Path(arguments.save) / "reference.png")
    logged = psnr(results["sum, jitter as logged"], reference, still)
    return 0 if logged > psnr(results["sum, jitter sign flipped"], reference, still) and \
        logged > psnr(results["one frame, bicubic"], reference, still) else 1


def previous_position(cam, before, depth_now, width, height):
    """For each pixel of a frame, where the same point of the scene was in the frame before, in pixels, with
    neither frame's jitter in it. The compositor's motion-vector pass does the same per pixel."""
    jx, jy = cam["jitter"]
    u = (np.arange(width) + 0.5 - jx) / width
    v = (np.arange(height) + 0.5 - jy) / height
    l, r, t, b = cam["frustum"]
    near, far = cam["near"], cam["far"]
    z = near * far / (far - depth_now.astype(np.float64) * (far - near))
    x = z * (l + u[None, :] * (r - l))
    y = z * (t - v[:, None] * (t - b))
    world = (cam["pos"][None, None, :] + z[..., None] * cam["forward"] + x[..., None] * cam["right"]
             + y[..., None] * cam["up"])
    rel = world - before["pos"]
    zb = rel @ before["forward"]
    lb, rb, tb, bb = before["frustum"]
    ub = (rel @ before["right"] / zb - lb) / (rb - lb)
    vb = (tb - rel @ before["up"] / zb) / (tb - bb)
    return ub * width, vb * height, u[None, :] * width + 0 * ub, v[:, None] * height + 0 * vb, zb > 0


def sample(image, x, y):
    """Bilinear fetch at pixel positions (pixel centres at +0.5); outside the picture gives the edge."""
    height, width = image.shape[:2]
    x, y = x - 0.5, y - 0.5
    x0 = np.clip(np.floor(x).astype(int), 0, width - 2)
    y0 = np.clip(np.floor(y).astype(int), 0, height - 2)
    fx = np.clip(x - x0, 0, 1)[..., None]
    fy = np.clip(y - y0, 0, 1)[..., None]
    top = image[y0, x0] * (1 - fx) + image[y0, x0 + 1] * fx
    bottom = image[y0 + 1, x0] * (1 - fx) + image[y0 + 1, x0 + 1] * fx
    return top * (1 - fy) + bottom * fy


def reproject(arguments):
    cams = cameras(arguments.log or Path(arguments.run) / "log.txt")
    frames = frame_range(arguments.frames)
    held_sum = moved_sum = 0.0
    count = 0
    for frame in frames[1:]:
        if frame not in cams or frame - 1 not in cams:
            print(f"frame {frame}: no camera")
            continue
        now, was = shot(arguments.run, frame), shot(arguments.run, frame - 1)
        height, width = now.shape[:2]
        cam, before = cams[frame], cams[frame - 1]
        ub, vb, uc, vc, ahead = previous_position(cam, before, depth(arguments.run, frame), width, height)
        motion = np.hypot(ub - uc, vb - vc)
        jx, jy = before["jitter"]
        fetched = sample(was, ub + jx, vb + jy)
        inside = ahead & (ub > 1) & (ub < width - 1) & (vb > 1) & (vb < height - 1)
        if arguments.skip_top:
            inside[:int(arguments.skip_top * height)] = False
        held = float(np.abs(now - was)[inside].mean())
        moved = float(np.abs(now - fetched)[inside].mean())
        held_sum += held
        moved_sum += moved
        count += 1
        print(f"frame {frame}: camera moved {np.linalg.norm(cam['pos'] - before['pos']):6.2f} units, picture moves "
              f"{np.median(motion[inside]):5.2f} px (median) {np.percentile(motion[inside], 95):6.2f} px (95%); "
              f"difference from the frame before {held:6.2f}, from the frame before fetched where depth says {moved:6.2f}")
        if arguments.save:
            Path(arguments.save).mkdir(parents=True, exist_ok=True)
            both = np.concatenate([np.abs(now - was).max(axis=2), np.abs(now - fetched).max(axis=2)], axis=0)
            Image.fromarray(np.clip(both * 3, 0, 255).astype(np.uint8)).save(Path(arguments.save) / f"reproject-{frame:05d}.png")
    if not count:
        return 1
    print(f"mean difference: {held_sum / count:.2f} holding the frame before, {moved_sum / count:.2f} fetching it where "
          f"depth and camera say ({100 * (1 - moved_sum / held_sum):.0f}% less)")
    return 0 if moved_sum < held_sum else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    s = commands.add_parser("shifts", help="each frame's whole-pixel shift against the log")
    s.add_argument("run")
    s.add_argument("--frames", required=True, help="first-last")
    s.add_argument("--log")
    s.add_argument("--reach", type=int, default=20, help="largest shift looked for, pixels")
    s.set_defaults(run_command=shifts)
    a = commands.add_parser("accumulate", help="jittered small frames against a larger render")
    a.add_argument("low", help="directory of the small jittered frames and their log.txt")
    a.add_argument("reference", help="directory of the larger render")
    a.add_argument("--frames", required=True, help="first-last of the small frames")
    a.add_argument("--plain-frame", type=int, help="a small frame without jitter, for the one-frame upscales")
    a.add_argument("--reference-frame", type=int, required=True)
    a.add_argument("--reference-other", type=int, help="a second reference frame: pixels that differ are not still")
    a.add_argument("--reference-again", help="a second run of the larger render: how far two runs agree at best")
    a.add_argument("--still", type=float, default=6, help="largest difference of a still pixel, 0-255")
    a.add_argument("--sigma", type=float, default=0.45, help="width of a sample's weight, output pixels")
    a.add_argument("--log")
    a.add_argument("--save", help="directory for the pictures (game imagery: not in the repository)")
    a.set_defaults(run_command=accumulate)
    r = commands.add_parser("reproject", help="the frame before, fetched where depth and camera say")
    r.add_argument("run")
    r.add_argument("--frames", required=True, help="first-last")
    r.add_argument("--log")
    r.add_argument("--skip-top", type=float, default=0, help="fraction of the height to leave out (the compass)")
    r.add_argument("--save")
    r.set_defaults(run_command=reproject)
    arguments = parser.parse_args()
    return arguments.run_command(arguments)


if __name__ == "__main__":
    sys.exit(main())
