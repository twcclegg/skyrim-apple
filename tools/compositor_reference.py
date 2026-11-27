#!/usr/bin/env python3
"""Numpy reference of the immersive compositor's warp (apple/Sources/App/ImmersiveScreen.swift), to check the
cost switches against today's path without a GPU.

It follows the Metal shaders step for step in float32: the vertex stage (nearest depth, the five-tap minimum half a
grid cell each way, the D3D depth to distance), a rasterizer with a depth buffer and perspective-correct
interpolation in place of the GPU's, and the fragment stage's per-pixel depth search with its final "tear" test.
The frame is drawn in head mode (LB_IMMERSIVE_HEAD=1: each pixel at its true distance) unless --window.

    compositor_reference.py SHOT.bmp DEPTH.f32 [options]     a capture (LB_SHOT writes both files)
    compositor_reference.py --synthetic [SEED] [options]     a made-up scene: needles, blades and posts 1 to 3
                                                             game pixels wide in front of ground, a wall and sky

What it checks, per eye place (--offset, metres from where the frame's rays start; --yaw, degrees):

  seed   LB_IMMERSIVE_SEED: the per-game-frame seed texture and the indexed mesh give every triangle corner the
         same position and uv as today's unindexed vertex shader. Must be bit for bit; any difference fails.
  exit   LB_IMMERSIVE_REFINE_EXIT: the search with the early exit against the same number of steps without it.
         The exit is taken when a step moves the game pixel shown by less than --exit game pixels and leaves it
         on the depth texel that step read (so one more step would read the same depth again, and the last "tear"
         test could not turn it back). --exit-any-texel tries the distance alone: it lets through steps that end
         on the far side of a depth edge, which further steps would have moved by many pixels.
  grid   LB_IMMERSIVE_GRID: a coarser mesh (default 161x91) against today's 321x181, same search.
  steps  for scale: today's path with 2 steps against today's with 4 (what "2 is enough" rests on).

For exit, grid and steps it prints how far the game pixel shown moves (game pixels) and how much the picture
changes (8-bit sRGB levels, bilinear colour as the shader samples it), over the whole picture and over the pixels
within 2 game pixels of a depth edge (a neighbouring depth more than 1 + tear times as far: foliage outlines).
It also prints depth fetches per output pixel, the cost the exit is there to cut.

With --truth (head mode only) each variant is also compared with where the eye's ray really meets the capture's
per-pixel depth (found by walking the ray across the game image, every --truth-stride'th output pixel): the share
of pixels more than 1 game pixel off, and the disparity error between the eyes in arc minutes at 64 mm, as
tools/immersive_preview.py --metrics gives it. Rays that pass behind a near surface's edge onto something the game's
camera did not see have no true answer and are left out ("unseen").

The exit code is 0 only if every tolerance in TOLERANCE holds (printed at the end). --out PREFIX writes
PREFIX-<check>-<offset>.png: today's picture, the variant's, and their difference times 8.

The captures are the game's 1280x720 at 65 degrees (754 game pixels per unit tangent, grid cells of 4 game pixels).
The headset configuration (1600x1100 an eye over about 120 degrees) has 464 per unit tangent and cells of 5 by 6;
pass --tan and such a capture to check it like for like. Captures stay out of the repository.
"""
import argparse
import struct
import sys

import numpy as np
from PIL import Image

F = np.float32
ARCMIN = 60 * 180 / np.pi

# What "the same picture" means here. uv moves are in game pixels; shares are of the pixels the mesh covers.
TOLERANCE = {
    # Early exit against the same step count without it: share of pixels whose game pixel moves by more than
    # a quarter and by more than one game pixel, over the picture and over depth-edge pixels.
    "exit_over_quarter": 0.002, "exit_over_one": 0.0005, "exit_edge_over_one": 0.005,
    # Coarser grid against today's grid, against the truth: the share more than 1 game pixel off may not rise by
    # more than this (absolute), over the picture and over depth-edge pixels.
    "grid_truth_rise": 0.002, "grid_edge_truth_rise": 0.01,
}

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("shot", nargs="?")
ap.add_argument("depth", nargs="?")
ap.add_argument("--synthetic", nargs="?", const=1, type=int, metavar="SEED", help="a made-up 1280x720 scene instead of a capture")
ap.add_argument("--tan", type=float, nargs=2, default=[0.849427, 0.477803], help="the frame's frustum tangents, right and top (LB_CAMLOG prints them)")
ap.add_argument("--near", type=float, default=15.0, help="LB_IMMERSIVE_DEPTH_NEAR")
ap.add_argument("--units", type=float, default=70.0, help="LB_IMMERSIVE_DEPTH_UNITS_PER_M")
ap.add_argument("--tear", type=float, default=0.08, help="LB_IMMERSIVE_DEPTH_TEAR")
ap.add_argument("--window", action="store_true", help="window mode (a 2.8 m screen 3 m away, depth scaled behind it) instead of head mode")
ap.add_argument("--scale", type=float, default=0.15, help="--window: LB_IMMERSIVE_DEPTH_SCALE")
ap.add_argument("--offset", type=float, nargs=3, action="append", metavar=("X", "Y", "Z"),
                help="the eye, metres from the rays' origin (x right, y up, z back); repeatable. Default: 0.032 0 0, "
                     "0.01 0.004 0.006 and 0.1 0.02 0.03")
ap.add_argument("--yaw", type=float, default=0.0, help="the eye looks this many degrees to the right of the frame's centre (steering lag)")
ap.add_argument("--density", type=float, default=1031.0, help="output pixels per unit tangent (1031 is 18 per degree, the headset's drawable)")
ap.add_argument("--grid", type=int, nargs=2, default=[321, 181], help="today's grid, columns and rows")
ap.add_argument("--coarse", type=int, nargs=2, default=[161, 91], help="the coarser grid to check (LB_IMMERSIVE_GRID)")
ap.add_argument("--steps", type=int, nargs="+", default=[4, 2], help="search step counts to check the exit and the grid at")
ap.add_argument("--exit", type=float, default=0.25, help="LB_IMMERSIVE_REFINE_EXIT: game pixels")
ap.add_argument("--exit-any-texel", action="store_true",
                help="the simpler rule the shader does not use: stop on the distance alone, even when the step crossed into another depth texel")
ap.add_argument("--truth", action="store_true", help="also compare with the true ray intersection (slower)")
ap.add_argument("--truth-stride", type=int, default=2)
ap.add_argument("--out", help="prefix for pictures")
args = ap.parse_args()

# ---------------------------------------------------------------------------------------------------------------
# The scene.

def synthetic(seed, w=1280, h=720):
    """Colour and D3D depth of a scene with foliage-like outlines: thin near things over far ones."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    metres = np.full((h, w), 3000.0)                              # sky
    colour = np.stack([120 + 60 * y / h, 150 + 50 * y / h, 220 - 20 * y / h], -1)
    ground = y > h * 0.55                                         # ground: nearer toward the bottom
    gm = 1.7 * (h * 0.5) / np.maximum(y - h * 0.5, 1e-3) / 1.0
    metres[ground] = gm[ground]
    checks = ((np.floor(x / 8) + np.floor(y / 6)) % 2)[..., None]
    colour[ground] = (np.array([70, 90, 50]) + 50 * checks + rng.integers(0, 30, (h, w, 1)))[ground]
    wall = (x > w * 0.62) & (y > h * 0.2) & (y <= h * 0.55)       # a slanted wall
    wm = 6 + 10 * (x - w * 0.62) / (w * 0.38)
    metres[wall] = wm[wall]
    colour[wall] = (np.array([110, 100, 90]) + 40 * ((np.floor(x / 5) + np.floor(y / 9)) % 2)[..., None])[wall]

    def stroke(x0, y0, x1, y1, width, m, rgb):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        t = np.linspace(0, 1, n)
        for dx in range(width):
            for dy in range(width):
                px = np.clip(np.round(x0 + (x1 - x0) * t).astype(int) + dx, 0, w - 1)
                py = np.clip(np.round(y0 + (y1 - y0) * t).astype(int) + dy, 0, h - 1)
                nearer = metres[py, px] > m
                metres[py[nearer], px[nearer]] = m
                colour[py[nearer], px[nearer]] = rgb
    for _ in range(900):                                          # pine needles: 1 px, every direction, 1 to 6 m
        cx, cy = rng.uniform(0.15, 0.6) * w, rng.uniform(0.05, 0.7) * h
        a, length = rng.uniform(0, 2 * np.pi), rng.uniform(8, 60)
        stroke(cx, cy, cx + np.cos(a) * length, cy + np.sin(a) * length, 1, rng.uniform(1.0, 6.0),
               np.array([40, 110, 40]) + rng.integers(-30, 30, 3))
    for _ in range(500):                                          # grass blades: 1 to 2 px, upright, 0.8 to 3 m
        cx, length = rng.uniform(0, 1) * w, rng.uniform(20, 120)
        stroke(cx, h - 1, cx + rng.uniform(-25, 25), h - 1 - length, int(rng.integers(1, 3)), rng.uniform(0.8, 3.0),
               np.array([150, 140, 60]) + rng.integers(-30, 30, 3))
    for cx, m in ((0.08, 1.2), (0.7, 2.5), (0.9, 4.0)):           # posts: 3 px and wider
        stroke(cx * w, 0, cx * w + 6, h - 1, 3 + int(6 / m), m, np.array([90, 60, 40]))
    d = 1.0 - args.near / (metres * args.units)                   # D3D depth with the far plane far away
    return np.clip(colour, 0, 255).astype(np.uint8), d.astype(F)


if args.synthetic is not None:
    image, depth = synthetic(args.synthetic)
    h, w = depth.shape
    name = f"synthetic scene {args.synthetic}"
else:
    if not args.shot or not args.depth:
        ap.error("give SHOT.bmp and DEPTH.f32, or --synthetic")
    raw = open(args.depth, "rb").read()
    w, h = struct.unpack_from("<II", raw)
    depth = np.frombuffer(raw, dtype="<f4", count=w * h, offset=8).reshape(h, w)
    image = np.asarray(Image.open(args.shot).convert("RGB").resize((w, h)), dtype=np.uint8)
    name = args.shot
SIZE = np.array([w, h], F)

HEAD = not args.window
if HEAD:
    EYE_BACK = F(2.0)                                             # viewPlane
    HALF = (EYE_BACK * np.array(args.tan, F)).astype(F)
    DEPTH_SCALE, MAX_BEHIND = F(1.0), F(300.0)
    out_tan = np.array(args.tan, np.float64)
else:
    EYE_BACK = F(3.0)
    HALF = np.array([1.4, 1.4 * h / w], F)
    DEPTH_SCALE, MAX_BEHIND = F(args.scale), F(30.0)
    out_tan = np.tan(np.radians(35.0)) * np.array([1, h / w])
NEAR, UNITS, TEAR = F(args.near), F(args.units), F(args.tear)
ORIGIN = np.array([0, 0, EYE_BACK], F)
OUT_W, OUT_H = int(round(2 * out_tan[0] * args.density)), int(round(2 * out_tan[1] * args.density))

# ---------------------------------------------------------------------------------------------------------------
# The shaders' functions, in float32.

def texel(uv):
    """The texel a nearest, clamp-to-edge sampler reads at uv."""
    x = np.clip(np.floor(uv[..., 0] * SIZE[0]).astype(np.int32), 0, w - 1)
    y = np.clip(np.floor(uv[..., 1] * SIZE[1]).astype(np.int32), 0, h - 1)
    return x, y


def raw_depth(uv, grid=None):
    """behind_screen_raw's fetches: the depth at uv; with a grid, the nearest of it and four taps half a cell away."""
    x, y = texel(uv)
    d = depth[y, x]
    if grid is not None:
        cell = (F(0.5) / (np.array(grid, F) - F(1))).astype(F)
        for du, dv in ((-cell[0], 0), (cell[0], 0), (0, -cell[1]), (0, cell[1])):
            x, y = texel(uv + np.array([du, dv], F))
            d = np.minimum(d, depth[y, x])
    return d


def metres(d):
    return ((NEAR / np.maximum(F(1) - d, F(1e-6))) / UNITS).astype(F)


def screen_point(uv):
    return np.stack([(uv[..., 0] * F(2) - F(1)) * HALF[0], (F(1) - uv[..., 1] * F(2)) * HALF[1],
                     np.zeros_like(uv[..., 0])], -1).astype(F)


def edge_fade(uv):
    edge = np.minimum(np.minimum(uv[..., 0], F(1) - uv[..., 0]), np.minimum(uv[..., 1], F(1) - uv[..., 1]))
    t = np.clip(edge / F(0.04), 0, 1).astype(F)
    return t * t * (F(3) - F(2) * t)


def radial(m, to_screen):
    """The shader's radial(), from the view depth in game metres."""
    if HEAD:
        return (to_screen * np.minimum(m * DEPTH_SCALE, MAX_BEHIND) / EYE_BACK).astype(F)
    return (to_screen + np.minimum(DEPTH_SCALE * m, MAX_BEHIND)).astype(F)


def pixel_radial(uv):
    to_screen = np.linalg.norm(screen_point(uv) - ORIGIN, axis=-1).astype(F)
    r = radial(metres(raw_depth(uv)), to_screen)
    return r if HEAD else to_screen + (r - to_screen) * edge_fade(uv)


def place(uv, d):
    """screen_vertex with the search on (the mesh kept whole): the vertex at uv whose dilated depth is d."""
    on = screen_point(uv)
    ray = on - ORIGIN
    to_screen = np.linalg.norm(ray, axis=-1).astype(F)
    r = radial(metres(d), to_screen)
    fade = F(1) if HEAD else edge_fade(uv)
    return (ORIGIN + ray * (F(1) + (r - to_screen) * fade / to_screen)[..., None]).astype(F)


CORNERS = np.array([(0, 0), (1, 0), (0, 1), (1, 0), (1, 1), (0, 1)], np.uint32)


def mesh_today(grid):
    """Today's unindexed draw: vertex id -> (uv, position), 6 per cell, each from its own depth fetches."""
    gx, gy = grid
    ids = np.arange((gx - 1) * (gy - 1) * 6, dtype=np.uint32)
    cell_index, corner = ids // 6, ids % 6
    cell = np.stack([cell_index % (gx - 1), cell_index // (gx - 1)], -1)
    step = (F(1) / (np.array(grid, F) - F(1))).astype(F)
    uv = ((cell + CORNERS[corner]).astype(F) * step).astype(F)
    return uv, place(uv, raw_depth(uv, grid))


def mesh_seeded(grid):
    """LB_IMMERSIVE_SEED: the seed pass (one texel per grid vertex, the same five-tap minimum of the raw depth),
    then the indexed draw: vertex id -> one read of the seed; six indices per cell."""
    gx, gy = grid
    step = (F(1) / (np.array(grid, F) - F(1))).astype(F)
    ty, tx = np.mgrid[0:gy, 0:gx].astype(np.uint32)              # seed_fragment: uint2(position.xy)
    seed = raw_depth((np.stack([tx, ty], -1).astype(F) * step).astype(F), grid)
    ids = np.arange(gx * gy, dtype=np.uint32)                     # screen_vertex_seeded
    cell = np.stack([ids % gx, ids // gx], -1)
    uv = (cell.astype(F) * step).astype(F)
    position = place(uv, seed[cell[:, 1], cell[:, 0]])
    cells = np.arange((gx - 1) * (gy - 1), dtype=np.uint32)       # the index buffer, as the Swift builds it
    cx, cy = cells % (gx - 1), cells // (gx - 1)
    indices = ((cy[:, None] + CORNERS[None, :, 1]) * gx + cx[:, None] + CORNERS[None, :, 0]).reshape(-1)
    return uv[indices], position[indices], seed


def rasterize(uv, position, eye, rotation):
    """Per output pixel: the nearest triangle's interpolated uv and position (Out.uv, Out.local), and coverage."""
    rel = (position.astype(np.float64) - eye) @ rotation          # into the eye's axes (x right, y up, z back)
    wclip = -rel[:, 2]
    px = (rel[:, 0] / wclip / out_tan[0] + 1) * OUT_W / 2
    py = (1 - rel[:, 1] / wclip / out_tan[1]) * OUT_H / 2
    tri = lambda a: a.reshape(-1, 3)
    X, Y, IW = tri(px), tri(py), tri(1 / wclip)
    ok = (tri(wclip) > 0.02).all(1)
    x0 = np.clip(np.floor(X.min(1)), 0, OUT_W).astype(np.int64)
    x1 = np.clip(np.ceil(X.max(1)), 0, OUT_W).astype(np.int64)
    y0 = np.clip(np.floor(Y.min(1)), 0, OUT_H).astype(np.int64)
    y1 = np.clip(np.ceil(Y.max(1)), 0, OUT_H).astype(np.int64)
    bw, bh = x1 - x0, y1 - y0
    area = (X[:, 1] - X[:, 0]) * (Y[:, 2] - Y[:, 0]) - (Y[:, 1] - Y[:, 0]) * (X[:, 2] - X[:, 0])
    ok &= (bw > 0) & (bh > 0) & (np.abs(area) > 1e-12)
    zbuf = np.zeros(OUT_W * OUT_H)
    hit = np.full(OUT_W * OUT_H, -1, np.int64)
    bary = np.zeros((OUT_W * OUT_H, 3))
    key = bw * 65536 + bh
    for k in np.unique(key[ok]):
        members = np.nonzero(ok & (key == k))[0]
        kw, kh = int(k // 65536), int(k % 65536)
        per = max(1, 1_500_000 // (kw * kh))
        for start in range(0, len(members), per):
            t = members[start:start + per]
            sx = (x0[t, None, None] + np.arange(kw)[None, None, :]) + 0.5
            sy = (y0[t, None, None] + np.arange(kh)[None, :, None]) + 0.5
            ax, ay, bx, by, cx, cy = (v[t, None, None] for v in (X[:, 0], Y[:, 0], X[:, 1], Y[:, 1], X[:, 2], Y[:, 2]))
            ar = area[t, None, None]
            l0 = ((bx - sx) * (cy - sy) - (by - sy) * (cx - sx)) / ar
            l1 = ((cx - sx) * (ay - sy) - (cy - sy) * (ax - sx)) / ar
            l2 = 1 - l0 - l1
            inside = (l0 >= -1e-9) & (l1 >= -1e-9) & (l2 >= -1e-9)
            if not inside.any():
                continue
            n, yy, xx = np.nonzero(inside)
            tt = t[n]
            b = np.stack([l0[n, yy, xx] * IW[tt, 0], l1[n, yy, xx] * IW[tt, 1], l2[n, yy, xx] * IW[tt, 2]], -1)
            iw = b.sum(1)
            flat = (y0[tt] + yy) * OUT_W + x0[tt] + xx
            better = iw > zbuf[flat]
            order = np.argsort(iw[better], kind="stable")         # nearest written last
            f = flat[better][order]
            zbuf[f] = iw[better][order]
            hit[f] = tt[better][order]
            bary[f] = (b[better] / iw[better, None])[order]
    covered = hit >= 0
    t = np.maximum(hit, 0)
    out_uv = np.einsum("pc,pcd->pd", bary, uv.reshape(-1, 3, 2)[t]).astype(F)
    out_local = np.einsum("pc,pcd->pd", bary, position.reshape(-1, 3, 3)[t]).astype(F)
    return out_uv, out_local, covered


def search(uv0, local, eye, steps, exit_px=0.0):
    """refined_uv: the uv shown, depth fetches made, and the distance along the eye's ray of what is shown."""
    eye = eye.astype(F)
    a = (eye - ORIGIN).astype(F)
    d = local - eye
    d = (d / np.linalg.norm(d, axis=-1, keepdims=True)).astype(F)
    ad, aa = (d @ a).astype(F), F(a @ a)

    def step(uv, ad, d):
        r = pixel_radial(uv)
        t = (-ad + np.sqrt(np.maximum(ad * ad - aa + r * r, F(0)))).astype(F)
        from_origin = eye + d * t[:, None] - ORIGIN
        on = from_origin * (EYE_BACK / np.maximum(-from_origin[:, 2], F(1e-6)))[:, None]
        new = np.clip(np.stack([(on[:, 0] / HALF[0] + F(1)) * F(0.5), (F(1) - on[:, 1] / HALF[1]) * F(0.5)], -1), 0, 1)
        return new.astype(F), r, t

    n = len(uv0)
    uv, before = uv0.copy(), uv0.copy()
    r_before, t_shown = np.zeros(n, F), np.zeros(n, F)
    fetches = np.zeros(n, np.int32)
    active = np.ones(n, bool)
    for _ in range(steps):
        i = np.nonzero(active)[0]
        before[i] = uv[i]
        new, r, t = step(uv[i], ad[i], d[i])
        r_before[i], t_shown[i], uv[i] = r, t, new
        fetches[i] += 1
        if exit_px > 0:
            moved = (new - before[i]) * SIZE
            settled = (moved * moved).sum(1) < F(exit_px) * F(exit_px)
            if not args.exit_any_texel:                           # and still on the depth texel just read
                settled &= (np.floor(new * SIZE) == np.floor(before[i] * SIZE)).all(1)
            active[i[settled]] = False
    if steps > 0:
        i = np.nonzero(active)[0]
        _, r_after, t_after = step(uv[i], ad[i], d[i])
        fetches[i] += 1
        back = r_before[i] > r_after * (F(1) + TEAR)
        uv[i[back]] = before[i[back]]
        t_shown[i[~back]] = t_after[~back]
    return uv, fetches, t_shown


def truth(eye, rotation, stride):
    """Where each eye ray first meets the per-pixel depth (head mode): every game pixel is a patch facing the
    camera at its view depth. Returns the output pixels tried, their uv and distance along the ray, and which of
    them hit a patch (the others pass behind a near edge: unseen by the game's camera)."""
    ys, xs = np.mgrid[0:OUT_H:stride, 0:OUT_W:stride]
    index = (ys * OUT_W + xs).reshape(-1)
    dc = np.stack([((xs + 0.5) / OUT_W * 2 - 1) * out_tan[0], (1 - (ys + 0.5) / OUT_H * 2) * out_tan[1],
                   -np.ones(xs.shape)], -1).reshape(-1, 3)
    d = dc @ rotation.T
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    a = eye.astype(np.float64) - ORIGIN
    kappa = -d[:, 2]
    forward = kappa > 1e-6
    # On the frame's plane the ray's picture is a straight line in 1/depth: on(q) = B + C q, q = 1/view depth.
    B = EYE_BACK * d[:, :2] / kappa[:, None]
    C = EYE_BACK * (a[None, :2] + d[:, :2] * (a[2] / kappa)[:, None])
    half = HALF.astype(np.float64)
    to_uv = lambda on: np.stack([(on[:, 0] / half[0] + 1) / 2, (1 - on[:, 1] / half[1]) / 2], -1)
    m = metres(depth).astype(np.float64) * float(DEPTH_SCALE)
    m = np.minimum(m, float(MAX_BEHIND))
    q_max = 1 / m.min()
    length = np.linalg.norm(C / half * np.array([w, h]) / 2, axis=1) * q_max   # game pixels walked
    samples = int(np.ceil(length.max() * 2)) + 2
    n = len(index)
    found = np.zeros(n, bool)
    uv_true, depth_true = np.zeros((n, 2)), np.zeros(n)

    def patch(q_of, texel_xy, rows):
        """Does the ray, at the depth of the given texel, pass through that texel? (rows: which rays)"""
        z = m[texel_xy[1], texel_xy[0]]
        uv = to_uv(B[rows] + C[rows] / z[:, None])
        inside = (np.floor(uv[:, 0] * w) == texel_xy[0]) & (np.floor(uv[:, 1] * h) == texel_xy[1]) & (1 / z <= q_of)
        return inside, uv, z

    live = np.nonzero(forward)[0]
    prev = None
    for s in range(samples):
        if len(live) == 0:
            break
        q = q_max * (1 - s / (samples - 1))
        uv = to_uv(B[live] + C[live] * q)
        onframe = (uv >= 0).all(1) & (uv < 1).all(1)
        tx = np.clip(np.floor(uv[:, 0] * w).astype(np.int64), 0, w - 1)
        ty = np.clip(np.floor(uv[:, 1] * h).astype(np.int64), 0, h - 1)
        behind = onframe & (1 / max(q, 1e-12) >= m[ty, tx])
        if behind.any():
            rows = live[behind]
            hit_now, uv_now, z_now = patch(np.inf, (tx[behind], ty[behind]), rows)
            if prev is not None:                                  # or the texel of the sample before, on its way out
                hit_prev, uv_prev, z_prev = patch(np.inf, (prev[0][behind], prev[1][behind]), rows)
                use_prev = hit_prev & prev[2][behind]
            else:
                use_prev = np.zeros(len(rows), bool)
                uv_prev, z_prev = uv_now, z_now
            ok = use_prev | hit_now
            found[rows] = ok
            uv_true[rows] = np.where(use_prev[:, None], uv_prev, uv_now)
            depth_true[rows] = np.where(use_prev, z_prev, z_now)
        keep = ~behind
        prev = (tx[keep], ty[keep], onframe[keep])
        live = live[keep]
    # Distance along the eye's ray of the hit: the point at view depth z on the ray.
    t_true = (depth_true + a[2]) / np.maximum(kappa, 1e-6)
    return index, uv_true, t_true, found


def colour(uv):
    """The fragment shader's colour: one bilinear, clamp-to-edge fetch of an sRGB texture, written as sRGB."""
    linear = np.where(image <= 10, image / 255 / 12.92, ((image / 255 + 0.055) / 1.055) ** 2.4)
    x, y = uv[:, 0].astype(np.float64) * w - 0.5, uv[:, 1].astype(np.float64) * h - 0.5
    x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    cx = lambda v: np.clip(v, 0, w - 1)
    cy = lambda v: np.clip(v, 0, h - 1)
    c = (linear[cy(y0), cx(x0)] * (1 - fx) + linear[cy(y0), cx(x0 + 1)] * fx) * (1 - fy) \
        + (linear[cy(y0 + 1), cx(x0)] * (1 - fx) + linear[cy(y0 + 1), cx(x0 + 1)] * fx) * fy
    srgb = np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)
    return np.round(srgb * 255)


# Game pixels within 2 of a depth edge: a neighbour more than (1 + tear) times as far, or as near.
_m = metres(depth)
_lo, _hi = _m.copy(), _m.copy()
for dy in range(-2, 3):
    for dx in range(-2, 3):
        shifted = np.roll(np.roll(_m, dy, 0), dx, 1)
        _lo, _hi = np.minimum(_lo, shifted), np.maximum(_hi, shifted)
EDGE = _hi > _lo * (1 + args.tear)
del _lo, _hi


def on_edge(uv):
    x, y = texel(uv)
    return EDGE[y, x]


# ---------------------------------------------------------------------------------------------------------------
# The checks.

failures = []


def check(ok, text):
    if not ok:
        failures.append(text)
    return "ok" if ok else "FAIL"


def compare(label, base, variant, covered, edge):
    """How far the game pixel shown moves between two results, and how much the picture changes."""
    moved = np.linalg.norm((variant - base) * SIZE, axis=1)[covered]
    e = edge[covered]
    diff = np.abs(colour(variant[covered]) - colour(base[covered])).max(1)
    share = lambda mask, of: float(mask[of].mean()) if of.any() else 0.0
    everywhere = np.ones(len(moved), bool)
    stats = {"quarter": share(moved > 0.25, everywhere), "one": share(moved > 1, everywhere),
             "edge_quarter": share(moved > 0.25, e), "edge_one": share(moved > 1, e)}
    print(f"  {label}: game pixel moved > 0.25: {100 * stats['quarter']:.3f}% (edges {100 * stats['edge_quarter']:.2f}%), "
          f"> 1: {100 * stats['one']:.3f}% (edges {100 * stats['edge_one']:.2f}%), max {moved.max():.1f}; "
          f"colour mean {diff.mean():.3f}, > 8 levels: {100 * (diff > 8).mean():.3f}% (edges {100 * share(diff > 8, e):.2f}%)")
    return stats


def picture(path, base, variant, covered):
    a = np.where(covered[:, None], colour(base), 0).reshape(OUT_H, OUT_W, 3)
    b = np.where(covered[:, None], colour(variant), 0).reshape(OUT_H, OUT_W, 3)
    sheet = np.concatenate([a, b, np.clip(np.abs(a - b) * 8, 0, 255)], 1).astype(np.uint8)
    Image.fromarray(sheet).save(path)


print(f"{name}: {w}x{h}, {'head' if HEAD else 'window'} mode, depth {_m.min():.2f}-{_m.max():.0f} m, "
      f"{100 * EDGE.mean():.1f}% of game pixels within 2 of a depth edge; output {OUT_W}x{OUT_H}")

# seed: bit for bit.
for grid in (tuple(args.grid), tuple(args.coarse)):
    uv_a, pos_a = mesh_today(grid)
    uv_b, pos_b, seed = mesh_seeded(grid)
    same = np.array_equal(uv_a, uv_b) and np.array_equal(pos_a.view(np.uint32), pos_b.view(np.uint32))
    print(f"seed {grid[0]}x{grid[1]}: {len(uv_a)} triangle corners from {seed.size} seed texels, "
          f"positions and uv bit for bit the same as the unindexed mesh: {check(same, f'seed {grid}: indexed mesh differs')}")

offsets = args.offset or [[0.032, 0, 0], [0.01, 0.004, 0.006], [0.1, 0.02, 0.03]]
yaw = np.radians(args.yaw)
rotation = np.array([[np.cos(yaw), 0, np.sin(yaw)], [0, 1, 0], [-np.sin(yaw), 0, np.cos(yaw)]])  # eye axes as columns
meshes = {tuple(args.grid): mesh_today(tuple(args.grid)), tuple(args.coarse): mesh_today(tuple(args.coarse))}
for offset in offsets:
    eye = ORIGIN.astype(np.float64) + np.array(offset)
    tag = "_".join(f"{v:g}" for v in offset)
    print(f"eye {offset} m from the rays' origin, yaw {args.yaw:g} deg")
    seeds = {grid: rasterize(uv, position, eye, rotation) for grid, (uv, position) in meshes.items()}
    uv0, local, covered = seeds[tuple(args.grid)]
    uvc, localc, coveredc = seeds[tuple(args.coarse)]
    both = covered & coveredc
    print(f"  mesh covers {100 * covered.mean():.1f}% of the output; coarse grid {100 * coveredc.mean():.1f}%")
    results = {}
    for steps in sorted(set(args.steps) | {2, 4}):
        results[("today", steps)] = search(uv0, local, eye, steps)
        results[("exit", steps)] = search(uv0, local, eye, steps, args.exit)
        results[("grid", steps)] = search(uvc, localc, eye, steps)
        results[("grid+exit", steps)] = search(uvc, localc, eye, steps, args.exit)
    edge = on_edge(results[("today", 4)][0])
    print(f"  {100 * edge[covered].mean():.1f}% of output pixels show a game pixel within 2 of a depth edge")
    s = compare("steps 2 against 4, today's path (for scale)", results[("today", 4)][0], results[("today", 2)][0], covered, edge)
    for steps in args.steps:
        f0, f1 = results[("today", steps)][1][covered], results[("exit", steps)][1][covered]
        s = compare(f"exit {args.exit:g} at {steps} steps", results[("today", steps)][0], results[("exit", steps)][0], covered, edge)
        print(f"    depth fetches per pixel {f0.mean():.2f} -> {f1.mean():.2f}; settled after one step: {100 * (f1 == 1).mean():.1f}% "
              f"(edges {100 * (f1[edge[covered]] == 1).mean():.1f}%)  "
              + check(s["quarter"] <= TOLERANCE["exit_over_quarter"] and s["one"] <= TOLERANCE["exit_over_one"]
                      and s["edge_one"] <= TOLERANCE["exit_edge_over_one"], f"exit at {steps} steps, eye {offset}"))
        if args.out:
            picture(f"{args.out}-exit{steps}-{tag}.png", results[("today", steps)][0], results[("exit", steps)][0], covered)
        compare(f"grid {args.coarse[0]}x{args.coarse[1]} at {steps} steps", results[("today", steps)][0], results[("grid", steps)][0], both, edge)
        fg = results[("grid+exit", steps)][1][both]
        compare(f"grid {args.coarse[0]}x{args.coarse[1]} with exit at {steps} steps", results[("today", steps)][0],
                results[("grid+exit", steps)][0], both, edge)
        print(f"    depth fetches per pixel with the coarse grid and the exit: {fg.mean():.2f}")
        if args.out:
            picture(f"{args.out}-grid{steps}-{tag}.png", results[("today", steps)][0], results[("grid", steps)][0], both)
    if args.truth and HEAD:
        index, uv_true, t_true, found = truth(eye, rotation, args.truth_stride)
        print(f"  against the true ray intersection ({len(index)} rays; unseen by the game's camera: "
              f"{100 * (1 - found[covered[index]].mean()):.2f}%):")
        table = {}
        for key in sorted(results, key=lambda k: (k[1], k[0])):
            uv, _, t = results[key]
            use = found & (both if "grid" in key[0] else covered)[index]
            off = np.linalg.norm((uv[index] - uv_true) * SIZE, axis=1)[use]
            disparity = 0.064 * np.abs(1 / np.maximum(t[index][use], 1e-6) - 1 / np.maximum(t_true[use], 1e-6)) * ARCMIN
            e = edge[index][use]
            table[key] = (float((off > 1).mean()), float((off > 1)[e].mean()) if e.any() else 0.0)
            print(f"    {key[0]:>9} {key[1]} steps: > 1 game pixel off {100 * table[key][0]:.3f}% (edges {100 * table[key][1]:.2f}%); "
                  f"disparity mean {disparity.mean():.3f}', over 2': {100 * (disparity > 2).mean():.3f}%, over 5': {100 * (disparity > 5).mean():.3f}%")
        for steps in args.steps:
            for variant in ("grid", "grid+exit"):
                rise = table[(variant, steps)][0] - table[("today", steps)][0]
                edge_rise = table[(variant, steps)][1] - table[("today", steps)][1]
                print(f"    {variant} at {steps} steps against today's at {steps}: {100 * rise:+.3f}% (edges {100 * edge_rise:+.2f}%)  "
                      + check(rise <= TOLERANCE["grid_truth_rise"] and edge_rise <= TOLERANCE["grid_edge_truth_rise"],
                              f"{variant} at {steps} steps, eye {offset}: further from the truth"))
    elif args.truth:
        print("  --truth needs head mode")

print("tolerances: " + ", ".join(f"{k} {100 * v:g}%" for k, v in TOLERANCE.items()))
if failures:
    print("FAILED: " + "; ".join(failures))
    sys.exit(1)
print("all checks passed")
