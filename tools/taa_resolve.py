#!/usr/bin/env python3
"""How good would a hand-written temporal upscale be? A prototype, scored against the truth, on Linux.

The visionOS SDK marks MetalFX's temporal scaler unavailable, so a temporal upscale there may have to be the
compositor's own shader. This tries the usual one in numpy on frames the desktop build dumps at the full size
(LB_SHOT=<from>-<to> LB_CAMLOG=1, no jitter; see docs/handoff/temporal-upscale.md).

Each full-size frame gives a half-size one by keeping one pixel of every 2x2 block, a different one each frame.
That is exactly what a half-size render with jitter of a quarter pixel and a texture bias of -1 samples, with
its depth. The resolve sees only those, and the cameras; the full-size frame of the same instant is what it
should arrive at, so every frame can be scored, standing or moving.

The resolve, per output pixel: fetch last frame's output where depth and the cameras say this surface was,
keep it within the range of the new frame's 3x3 neighbourhood, and move it towards the new frame, quickly where
a new sample falls on the pixel and slowly elsewhere. A variant is those steps joined by "-":

  alone     the new frame upscaled bilinearly by itself (what there is without any of this)
  held      history fetched from the same pixel (no motion vectors)
  motion    history fetched where the surface was
  cubic     ... with a bicubic fetch (bilinear blurs a little more every frame)
  dilate    ... taking the motion of the nearest surface in the 3x3 neighbourhood
  noclamp   history is not limited to the neighbourhood's range
  varclip   ... limited to mean +- 1.25 standard deviations instead of smallest and largest
  reject    no history where the frame before saw a different surface (depth differs by over 10%)
  loose     ... unless that depth is within the range of the earlier frame's 3x3 neighbourhood
  speed     trust the new frame more the faster the pixel moves (all of it at 16 output pixels a frame)

Printed per span of frames: PSNR of the world (the compass and crosshair left out), PSNR of the compass, and how
much of the truth's fine detail the result has.

Needs numpy and Pillow. Dumps are game imagery: keep them out of the repository.
"""
import argparse
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import taa_sim  # noqa: E402

PHASES = [(0, 0), (1, 1), (1, 0), (0, 1)]


def catmull(image, x, y):
    """Bicubic (Catmull-Rom) fetch at pixel positions (pixel centres at +0.5)."""
    height, width = image.shape[:2]
    x, y = x - 0.5, y - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]

    def weights(f):
        return [-0.5 * f + f * f - 0.5 * f ** 3, 1 - 2.5 * f * f + 1.5 * f ** 3,
                0.5 * f + 2 * f * f - 1.5 * f ** 3, -0.5 * f * f + 0.5 * f ** 3]
    wx, wy = weights(fx), weights(fy)
    out = 0
    for j in range(4):
        row = 0
        rows = np.clip(y0 + j - 1, 0, height - 1)
        for i in range(4):
            row = row + wx[i] * image[rows, np.clip(x0 + i - 1, 0, width - 1)]
        out = out + wy[j] * row
    return out


def around(image):
    """Each pixel's 3x3 neighbourhood, as nine pictures."""
    pad = ((1, 1), (1, 1)) + ((0, 0),) * (image.ndim - 2)
    padded = np.pad(image, pad, mode="edge")
    height, width = image.shape[:2]
    return [padded[dy:dy + height, dx:dx + width] for dy in range(3) for dx in range(3)]


def linear(cam, depth):
    return cam["near"] * cam["far"] / (cam["far"] - depth.astype(np.float64) * (cam["far"] - cam["near"]))


def ahead_of(cam, before, depth, width, height):
    """How far along the earlier camera's forward axis each pixel's surface is."""
    jx, jy = cam["jitter"]
    u = (np.arange(width) + 0.5 - jx) / width
    v = (np.arange(height) + 0.5 - jy) / height
    left, right, top, bottom = cam["frustum"]
    z = linear(cam, depth)
    x = z * (left + u[None, :] * (right - left))
    y = z * (top - v[:, None] * (top - bottom))
    world = cam["pos"] + z[..., None] * cam["forward"] + x[..., None] * cam["right"] + y[..., None] * cam["up"]
    return (world - before["pos"]) @ before["forward"]


def resolve(run, frames, variant, cams):
    """Yields (frame, result, truth) for each frame."""
    steps = set(variant.split("-"))
    history = before = before_z = None
    for count, frame in enumerate(frames):
        truth = taa_sim.shot(run, frame)
        H, W = truth.shape[:2]
        h, w = H // 2, W // 2
        a, b = PHASES[count % 4]
        low = truth[b:2 * h:2, a:2 * w:2]
        low_depth = taa_sim.depth(run, frame)[b:2 * h:2, a:2 * w:2]
        # the kept pixel's centre is (a - 0.5) / 2 of a small pixel right of the small pixel's centre, and the
        # game's convention is that a pixel shows the unjittered picture at its centre minus the jitter
        cam = dict(cams[frame], jitter=(0.25 - a / 2, 0.25 - b / 2))
        jx, jy = cam["jitter"]

        qx = (np.arange(2 * w) + 0.5) / 2 + jx
        qy = (np.arange(2 * h) + 0.5) / 2 + jy
        alone = taa_sim.sample(low, *np.meshgrid(qx, qy))
        ix = np.clip(np.floor(qx).astype(int), 0, w - 1)
        iy = np.clip(np.floor(qy).astype(int), 0, h - 1)
        dx, dy = (ix + 0.5 - qx) * 2, (iy + 0.5 - qy) * 2
        near = np.exp(-(dy[:, None] ** 2 + dx[None, :] ** 2) / (2 * 0.45 ** 2))[..., None]

        def up(small):
            return small[iy][:, ix]

        if history is None or "alone" in steps:
            out = alone
        else:
            blend = 0.04 + 0.46 * near
            if "held" in steps:
                hx, hy = np.meshgrid(np.arange(2 * w) + 0.5, np.arange(2 * h) + 0.5)
            else:
                depth = np.minimum.reduce(around(low_depth)) if "dilate" in steps else low_depth
                ub, vb, uc, vc, _ = taa_sim.previous_position(cam, before, depth, w, h)
                mx, my = up(ub - uc), up(vb - vc)
                hx = (np.arange(2 * w)[None, :] + 0.5) + 2 * mx
                hy = (np.arange(2 * h)[:, None] + 0.5) + 2 * my
                if "speed" in steps:
                    blend = np.maximum(blend, np.clip(2 * np.hypot(mx, my)[..., None] / 16, 0, 1))
                if "reject" in steps:
                    bx = np.clip(np.floor(ub + before["jitter"][0]).astype(int), 0, w - 1)
                    by = np.clip(np.floor(vb + before["jitter"][1]).astype(int), 0, h - 1)
                    expected = ahead_of(cam, before, depth, w, h)
                    if "loose" in steps:
                        # the frame before sampled another corner of the pixel, so at an edge it may have seen
                        # the neighbour: accept anything within the range of its 3x3 neighbourhood
                        nine = around(before_z)
                        nearest, farthest = np.minimum.reduce(nine)[by, bx], np.maximum.reduce(nine)[by, bx]
                        gone = (expected < 0.9 * nearest) | (expected > 1.1 * farthest)
                    else:
                        gone = np.abs(before_z[by, bx] - expected) > 0.1 * np.maximum(expected, 1)
                    blend = np.where(up(gone)[..., None], 1.0, blend)
            fetched = catmull(history, hx, hy) if "cubic" in steps else taa_sim.sample(history, hx, hy)
            if "varclip" in steps:
                nine = np.stack(around(low))
                mean, spread = nine.mean(axis=0), 1.25 * nine.std(axis=0)
                fetched = np.clip(fetched, up(mean - spread), up(mean + spread))
            elif "noclamp" not in steps:
                nine = around(low)
                fetched = np.clip(fetched, up(np.minimum.reduce(nine)), up(np.maximum.reduce(nine)))
            outside = (hx < 0) | (hx > 2 * w) | (hy < 0) | (hy > 2 * h)
            blend = np.where(outside[..., None], 1.0, blend)
            out = fetched + blend * (alone - fetched)
        history, before, before_z = out, cam, linear(cam, low_depth)
        yield frame, out, truth[:2 * h, :2 * w]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", help="directory of full-size frames, their depth and log.txt")
    parser.add_argument("--frames", required=True, help="first-last")
    parser.add_argument("--span", action="append", default=[], metavar="NAME=FIRST-LAST",
                        help="frames to average and report under that name (repeatable); default: all of them")
    parser.add_argument("--variant", action="append", default=[],
                        help="default: alone, held, motion-cubic, motion-cubic-noclamp, motion-cubic-reject-speed")
    parser.add_argument("--compass", default="380,15,900,75", help="left,top,right,bottom of the compass at 1280x720")
    parser.add_argument("--log")
    parser.add_argument("--save", help="directory for each variant's last frame (game imagery: not in the repository)")
    arguments = parser.parse_args()
    cams = taa_sim.cameras(arguments.log or Path(arguments.run) / "log.txt")
    frames = taa_sim.frame_range(arguments.frames)
    spans = {}
    for text in arguments.span or [f"all={arguments.frames}"]:
        name, _, numbers = text.partition("=")
        wanted = taa_sim.frame_range(numbers)
        spans[name] = (wanted[0], wanted[-1])
    variants = arguments.variant or ["alone", "held", "motion-cubic", "motion-cubic-noclamp", "motion-cubic-reject-speed"]
    print(f"{'':36s}" + "".join(f"{name + ': world dB, compass dB, detail':>44s}" for name in spans))
    for variant in variants:
        rows = []
        for frame, out, truth in resolve(arguments.run, frames, variant, cams):
            height, width = truth.shape[:2]
            sx, sy = width / 1280, height / 720
            left, top, right, bottom = (int(n) for n in arguments.compass.split(","))
            world = np.ones((height, width), bool)
            world[:int((bottom + 15) * sy)] = False
            world[height // 2 - 20:height // 2 + 20, width // 2 - 20:width // 2 + 20] = False
            compass = np.zeros((height, width), bool)
            compass[int(top * sy):int(bottom * sy), int(left * sx):int(right * sx)] = True
            rows.append((frame, taa_sim.psnr(out, truth, world), taa_sim.psnr(out, truth, compass),
                         taa_sim.detail(out, world) / taa_sim.detail(truth, world)))
        line = f"{variant:36s}"
        for first, last in spans.values():
            part = [row for row in rows if first <= row[0] <= last]
            line += f"{'no frames':>44s}" if not part else (
                f"{np.mean([r[1] for r in part]):24.2f}, {np.mean([r[2] for r in part]):6.2f}, "
                f"{100 * np.mean([r[3] for r in part]):5.0f}%  ")
        print(line, flush=True)
        if arguments.save:
            from PIL import Image
            Path(arguments.save).mkdir(parents=True, exist_ok=True)
            Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(Path(arguments.save) / f"{variant}-{frames[-1]:05d}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
