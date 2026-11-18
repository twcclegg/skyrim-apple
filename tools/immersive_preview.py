#!/usr/bin/env python3
"""Offline preview of the immersive screen's depth-displaced grid (apple/Sources/App/ImmersiveScreen.swift).

Builds the same grid as the vertex shader from a Mac capture (LB_SHOT writes shot-N.bmp and depth-N.f32), then
renders it from each eye of a viewer sitting where the headset user does, so depth settings can be tuned without
the headset. It follows the shader step for step: nearest depth dilated over half a grid cell at each vertex,
triangles that span a depth edge laid flat at their farthest corner, and (--refine) the per-pixel depth search of
the fragment shader.

Usage: immersive_preview.py shot-N.bmp depth-N.f32 out_prefix [options]
Writes out_prefix-sbs.png (left | right) and out_prefix-anaglyph.png (red left, cyan right).

--metrics compares the result with the capture's own per-pixel depth: every game pixel is placed at its own
distance and seen from the same eyes, and for each output pixel the difference in distance is given as the
disparity error between the eyes (arc minutes at 64 mm; one headset pixel is about 1.8). Pixels the reference
does not cover (surfaces the game's camera did not see) are left out and counted as "uncovered".
"""
import argparse
import struct

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("shot")
ap.add_argument("depth")
ap.add_argument("out")
ap.add_argument("--scale", type=float, default=0.15, help="LB_IMMERSIVE_DEPTH_SCALE, meters behind the screen per game meter")
ap.add_argument("--max-behind", type=float, default=30.0, help="LB_IMMERSIVE_DEPTH_MAX_M")
ap.add_argument("--near", type=float, default=15.0, help="LB_IMMERSIVE_DEPTH_NEAR")
ap.add_argument("--units", type=float, default=70.0, help="LB_IMMERSIVE_DEPTH_UNITS_PER_M")
ap.add_argument("--dilate", action="store_true", help="accepted for old command lines; the nearest depth over half a grid cell is always used, as in the shader")
ap.add_argument("--tear", type=float, default=0.08, help="LB_IMMERSIVE_DEPTH_TEAR (distance ratio)")
ap.add_argument("--ipd", type=float, default=0.064)
ap.add_argument("--eye-back", type=float, default=3.0, help="window mode: ray origin in front of the screen, meters")
ap.add_argument("--viewer-x", type=float, default=0.0, help="head moved sideways, meters")
ap.add_argument("--head", action="store_true", help="LB_IMMERSIVE_HEAD=1: the frame at its true angular size and depth")
ap.add_argument("--head-scale", type=float, default=1.0, help="LB_IMMERSIVE_HEAD_SCALE")
ap.add_argument("--tan", type=float, nargs=2, default=[0.849, 0.478], help="--head: the frame's frustum tangents, right and top")
ap.add_argument("--grid", type=int, default=4, help="game pixels per grid cell (the renderer uses 4; other values show what a finer mesh would give)")
ap.add_argument("--refine", type=int, default=0, help="LB_IMMERSIVE_REFINE: per-pixel depth search steps in the fragment shader")
ap.add_argument("--ipd-scale", type=float, default=1.0, help="LB_IMMERSIVE_IPD_SCALE: eye separation multiplier for drawing")
ap.add_argument("--metrics", action="store_true", help="disparity error against per-pixel depth; also writes out_prefix-error.png")
args = ap.parse_args()

WIDTH, DIST, VIEW_PLANE = 2.8, 3.0, 2.0    # screen width and distance; head mode's plane distance (meters)
OUT_W, OUT_H = 1280, 720                   # preview camera
SS = 2                                     # surface samples per game pixel, each way
ARCMIN = 60 * 180 / np.pi
F = np.float32

raw = open(args.depth, "rb").read()
w, h = struct.unpack_from("<II", raw)
depth = np.frombuffer(raw, dtype="<f4", count=w * h, offset=8).reshape(h, w)
image = np.asarray(Image.open(args.shot).convert("RGB").resize((w, h)), dtype=np.uint8)

if args.head:
    eye_back, viewer_z = VIEW_PLANE, VIEW_PLANE
    half = VIEW_PLANE * np.array(args.tan, F)
    depth_scale, depth_max = args.head_scale, 300.0
    out_tan = np.array(args.tan, F)        # the preview camera shows the whole frame, one game pixel per pixel
else:
    eye_back, viewer_z = args.eye_back, DIST
    half = np.array([WIDTH / 2, WIDTH / 2 * h / w], F)
    depth_scale, depth_max = args.scale, args.max_behind
    out_tan = np.tan(np.radians(35.0)) * np.array([1, OUT_H / OUT_W], F)
GX, GY = w // args.grid + 1, h // args.grid + 1
ORIGIN = np.array([0, 0, eye_back], F)     # where the rays start: the game camera's place in screen space


def game_meters(uv, dilate):
    """View depth in game meters at uv; with dilate, the nearest over half a grid cell each way (vertex shader)."""
    def tap(du, dv):
        x = np.clip(((uv[..., 0] + du) * w).astype(np.int32), 0, w - 1)
        y = np.clip(((uv[..., 1] + dv) * h).astype(np.int32), 0, h - 1)
        return depth[y, x]
    d = tap(0, 0)
    if dilate:
        cu, cv = 0.5 / (GX - 1), 0.5 / (GY - 1)
        for du, dv in ((-cu, 0), (cu, 0), (0, -cv), (0, cv)):
            d = np.minimum(d, tap(du, dv))
    return (args.near / np.maximum(1 - d, 1e-6)) / args.units


def screen_point(uv):
    return np.stack([(uv[..., 0] * 2 - 1) * half[0], (1 - uv[..., 1] * 2) * half[1], np.zeros_like(uv[..., 0])], -1).astype(F)


def edge_fade(uv):
    edge = np.minimum(np.minimum(uv[..., 0], 1 - uv[..., 0]), np.minimum(uv[..., 1], 1 - uv[..., 1]))
    t = np.clip(edge / 0.04, 0, 1)
    return t * t * (3 - 2 * t)


def radial(uv, to_screen, dilate):
    """Distance from the ray origin of the game pixel at uv, before the edge fade (the shader's radial())."""
    m = game_meters(uv, dilate)
    if args.head:
        return to_screen * np.minimum(m * depth_scale, depth_max) / eye_back
    return to_screen + np.minimum(depth_scale * m, depth_max)


def radial_faded(uv):
    """Per-pixel distance from the ray origin, as the fragment shader's search uses it."""
    to_screen = np.linalg.norm(screen_point(uv) - ORIGIN, axis=-1)
    r = radial(uv, to_screen, False)
    return r if args.head else to_screen + (r - to_screen) * edge_fade(uv)


def place(uv, r, to_screen=None):
    """The point at distance r from the ray origin along the ray through uv on the screen."""
    ray = screen_point(uv) - ORIGIN
    if to_screen is None:
        to_screen = np.linalg.norm(ray, axis=-1)
    return ORIGIN + ray * (r / to_screen)[..., None]


# Grid vertices and the two triangles of each cell, as the vertex shader places them.
u, v = np.meshgrid(np.arange(GX, dtype=F) / (GX - 1), np.arange(GY, dtype=F) / (GY - 1))
vuv = np.stack([u, v], -1)
vts = np.linalg.norm(screen_point(vuv) - ORIGIN, axis=-1)
vr = radial(vuv, vts, True)
vfade = np.ones_like(vr) if args.head else edge_fade(vuv)
corner = lambda a, dx, dy: a[dy:GY - 1 + dy, dx:GX - 1 + dx]


def triangle(corners):
    """Positions of a triangle's three corners in every cell; a triangle spanning a depth edge is laid flat."""
    r = np.stack([corner(vr, dx, dy) for dx, dy in corners])
    far = r.max(0)
    if args.refine == 0:      # with the per-pixel search the mesh stays whole and the search settles the edges
        r = np.where(far > r.min(0) * (1 + args.tear), far, r)
    out = []
    for i, (dx, dy) in enumerate(corners):
        ts, fade = corner(vts, dx, dy), corner(vfade, dx, dy)
        out.append(place(corner(vuv, dx, dy), ts + (r[i] - ts) * fade, ts))
    return out


A = triangle(((0, 0), (1, 0), (0, 1)))
B = triangle(((1, 0), (1, 1), (0, 1)))

# Sample the mesh surface densely and splat it with a z-buffer.
X, Y = np.meshgrid((np.arange(w * SS, dtype=F) + 0.5) / (w * SS) * (GX - 1), (np.arange(h * SS, dtype=F) + 0.5) / (h * SS) * (GY - 1))
x0, y0 = np.minimum(X.astype(np.int32), GX - 2), np.minimum(Y.astype(np.int32), GY - 2)
fx, fy = (X - x0)[..., None], (Y - y0)[..., None]
upper = (fx + fy) < 1
mesh_points = np.where(upper,
                       A[0][y0, x0] + fx * (A[1][y0, x0] - A[0][y0, x0]) + fy * (A[2][y0, x0] - A[0][y0, x0]),
                       B[1][y0, x0] + (1 - fx) * (B[2][y0, x0] - B[1][y0, x0]) + (1 - fy) * (B[0][y0, x0] - B[1][y0, x0])).reshape(-1, 3)
sample_uv = np.stack([X / (GX - 1), Y / (GY - 1)], -1).reshape(-1, 2)
del A, B, x0, y0, fx, fy, upper

# The reference: every game pixel at its own depth (all of its samples at one distance).
pixel_uv = np.stack([(np.floor(sample_uv[:, 0] * w) + 0.5) / w, (np.floor(sample_uv[:, 1] * h) + 0.5) / h], -1).astype(F)
reference_points = place(sample_uv, radial_faded(pixel_uv)) if args.metrics else None


def splat(points, eye):
    """Index of the nearest point seen through each preview pixel (-1: none), and its distance from the eye."""
    rel = points - eye
    z = -rel[:, 2]
    ok = z > 0.05
    px = np.full(len(z), -1, np.int32)
    py = np.full(len(z), -1, np.int32)
    px[ok] = np.floor((rel[ok, 0] / z[ok] / out_tan[0] + 1) * OUT_W / 2)
    py[ok] = np.floor((1 - rel[ok, 1] / z[ok] / out_tan[1]) * OUT_H / 2)
    ok &= (px >= 0) & (px < OUT_W) & (py >= 0) & (py < OUT_H)
    ids = np.nonzero(ok)[0]
    ids = ids[np.argsort(-z[ids], kind="stable")]             # far first, near overwrites
    index = np.full((OUT_H, OUT_W), -1, np.int64)
    index[py[ids], px[ids]] = ids
    dist = np.where(index >= 0, np.linalg.norm(rel[np.maximum(index, 0)], axis=-1), np.inf)
    return index, dist


def refine(eye, uv):
    """The fragment shader's search: where this pixel's eye ray meets the per-pixel depth, starting from the mesh."""
    py, px = np.mgrid[0:OUT_H, 0:OUT_W]
    d = np.stack([((px + 0.5) / OUT_W * 2 - 1) * out_tan[0], (1 - (py + 0.5) / OUT_H * 2) * out_tan[1], -np.ones((OUT_H, OUT_W))], -1)
    d = (d / np.linalg.norm(d, axis=-1, keepdims=True)).astype(F)
    a = (eye - ORIGIN).astype(F)
    ad, aa = d @ a, a @ a

    def step(uv):
        r = radial_faded(uv)
        t = -ad + np.sqrt(np.maximum(ad * ad - aa + r * r, 0))
        from_origin = eye + d * t[..., None] - ORIGIN
        on = from_origin * (eye_back / np.maximum(-from_origin[..., 2], 1e-6))[..., None]
        return np.clip(np.stack([(on[..., 0] / half[0] + 1) / 2, (1 - on[..., 1] / half[1]) / 2], -1), 0, 1).astype(F), r, t

    r_before = t_before = uv_before = None
    for _ in range(args.refine):
        uv_before = uv
        uv, r_before, t_before = step(uv)
    # A ray that keeps jumping between a near and a far surface sees neither's own pixel: it looks past the near
    # surface's edge at something the game's camera did not see. Show the far one there.
    _, r_after, t_after = step(uv)
    back = r_before > r_after * (1 + args.tear)
    return np.where(back[..., None], uv_before, uv), np.where(back, t_before, t_after)


def colour(uv, covered):
    x = np.clip((uv[..., 0] * w).astype(np.int32), 0, w - 1)
    y = np.clip((uv[..., 1] * h).astype(np.int32), 0, h - 1)
    return np.where(covered[..., None], image[y, x], 0).astype(np.uint8)


views, errors, uncovered, holes = [], [], [], []
for side in (-1, 1):
    eye = np.array([args.viewer_x + side * args.ipd * args.ipd_scale / 2, 0, viewer_z], F)
    index, dist = splat(mesh_points, eye)
    covered = index >= 0
    uv = sample_uv[np.maximum(index, 0)]
    if args.refine > 0:
        # The whole mesh has no gaps, but its stretched triangles are sampled too thinly here to cover every pixel:
        # start those pixels from the nearest covered one on their row, as the rasterizer's interpolation would.
        inside = np.maximum.accumulate(covered, 1) & np.maximum.accumulate(covered[:, ::-1], 1)[:, ::-1]
        last = np.maximum.accumulate(np.where(covered, np.arange(OUT_W)[None, :], 0), 1)
        uv = np.take_along_axis(uv, last[..., None], 1)
        covered = inside
        uv, dist = refine(eye, uv)
    views.append(colour(uv, covered))
    if args.metrics:
        ref_index, ref_dist = splat(reference_points, eye)
        both = covered & (ref_index >= 0)
        inside = np.maximum.accumulate(covered, 1) & np.maximum.accumulate(covered[:, ::-1], 1)[:, ::-1]
        holes.append((inside & ~covered).sum() / inside.sum())
        errors.append(np.where(both, args.ipd * np.abs(1 / dist - 1 / ref_dist) * ARCMIN, np.nan))
        uncovered.append(1 - both.mean())

Image.fromarray(np.concatenate(views, 1)).save(args.out + "-sbs.png")
Image.fromarray(np.stack([views[0][..., 0], views[1][..., 1], views[1][..., 2]], -1)).save(args.out + "-anaglyph.png")
m = game_meters(vuv, False)
mode = f"head, scale {depth_scale}" if args.head else f"window, scale {depth_scale}, up to {depth_max} m behind"
print(f"{mode}; grid {GX}x{GY}, refine {args.refine}, eye separation x{args.ipd_scale}; game depth {m.min():.2f}-{m.max():.0f} m")
if args.metrics:
    e = np.concatenate([x[~np.isnan(x)] for x in errors])
    print(f"disparity error, arcmin: mean {e.mean():.2f}, 95th percentile {np.percentile(e, 95):.2f}; "
          f"over 2': {100 * (e > 2).mean():.2f}%, over 5': {100 * (e > 5).mean():.2f}%; not in the reference {100 * np.mean(uncovered):.2f}%; holes in the picture {100 * np.mean(holes):.2f}%")
    heat = np.clip(np.nan_to_num(errors[0]) / 10, 0, 1)        # left eye; white = 10 arc minutes or more
    Image.fromarray((heat * 255).astype(np.uint8)).save(args.out + "-error.png")
