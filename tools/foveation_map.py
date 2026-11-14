#!/usr/bin/env python3
"""Rate maps for foveating the game's own render (docs/handoff/foveated-render.md).

The game draws one planar frame over a wide frustum. A planar projection already spends more pixels per degree
away from the centre (by 1/cos^2 of the angle), and the eye needs fewer there. This computes a separable map
(one rate per column band, one per row band, as MTLRasterizationRateLayerDescriptor takes them) that keeps the
centre at full rate and lets the angular resolution fall off outside an inner cone, and prints what it costs.

  foveation_map.py table                       the comparison table in the note
  foveation_map.py rates --width 2560 ...      the rates as JSON (LB_GAME_FOVEATION_MAP, the Mac probe)
  foveation_map.py warp   in.png out.png ...   a uniform image -> what the game would render with the map
  foveation_map.py unwarp in.png out.png ...   and back, as the compositor would show it (bilinear)
  foveation_map.py unwarp shot.bmp out.png --lut lut.json
                                               the same for a real capture, with the map Metal really made

warp/unwarp need numpy and Pillow; table and rates need nothing. The physical size printed here is the sum of
the requested rates; Metal may round rates up and split cells, so the real size comes from the device
(physicalSize(layer:)) and is at least this. --lut takes the file tools/ratemap_probe.swift or
runtime/foveation_provider.mm writes: {"screen": [w, h], "physical": [w, h], "x": [...], "y": [...]}, x and y
being the screen coordinate of every physical pixel edge. An LB_SHOT capture of a foveated frame is the
game's size with the warped picture in its top-left corner; unwarp --lut crops that itself.
"""
import argparse
import json
import math
import sys

# Horizontal frustum of the 16:9 game frame for each LB_FOV, from docs/handoff/stereo-quality.md (computed
# there from the tangents in docs/HEAD_TRACKING.md).
FRUSTUM_DEG = {"65": 80.7, "80": 96.4, "100": 115.6}


def axis_rates(cells, half_tan, inner_deg, falloff_deg, floor):
    """Rates for `cells` equal bands across one axis whose half-extent has tangent `half_tan`.

    Angular density wanted, relative to the centre: 1 inside `inner_deg`, then 1 / (1 + (angle - inner) /
    falloff), never below `floor`. The planar projection gives sec^2(angle) logical pixels per centre pixel, so
    the rate is density * cos^2(angle), capped at 1. Each band takes the highest rate inside it.
    """
    rates = []
    for i in range(cells):
        best = 0.0
        for k in range(9):
            x = ((i + k / 8.0) / cells) * 2.0 - 1.0
            angle = math.degrees(math.atan(abs(x) * half_tan))
            density = 1.0 if angle <= inner_deg else max(floor, 1.0 / (1.0 + (angle - inner_deg) / falloff_deg))
            best = max(best, min(1.0, density * math.cos(math.radians(angle)) ** 2))
        rates.append(round(best, 4))
    return rates


def build(width, height, frustum_deg, inner_deg, falloff_deg, floor, cells):
    half_tan_x = math.tan(math.radians(frustum_deg) / 2.0)
    half_tan_y = half_tan_x * height / width
    cols = axis_rates(cells, half_tan_x, inner_deg, falloff_deg, floor)
    rows = axis_rates(max(2, round(cells * height / width)), half_tan_y, inner_deg, falloff_deg, floor)
    phys_w = math.ceil(sum(cols) * width / len(cols))
    phys_h = math.ceil(sum(rows) * height / len(rows))
    centre_ppd = (width / 2.0) / half_tan_x * math.radians(1.0)
    return {"screen": [width, height], "frustum_deg": frustum_deg, "horizontal": cols, "vertical": rows,
            "physical_estimate": [phys_w, phys_h], "centre_px_per_deg": round(centre_ppd, 2)}


def density_at(m, angle_deg):
    """Horizontal pixels per degree the map leaves at `angle_deg` off centre along the horizontal axis."""
    width = m["screen"][0]
    half_tan = math.tan(math.radians(m["frustum_deg"]) / 2.0)
    x = math.tan(math.radians(angle_deg)) / half_tan
    if x >= 1.0:
        return None
    cols = m["horizontal"]
    rate = cols[min(len(cols) - 1, int((x + 1.0) / 2.0 * len(cols)))]
    return m["centre_px_per_deg"] * rate / math.cos(math.radians(angle_deg)) ** 2


def table(args):
    base = 1280 * 720
    print("| LB_FOV | Logical size | Centre px/deg | Physical size (requested rates) | Pixels vs 1280x720 "
          "| px/deg at 20 / 35 / 50 deg |")
    print("|---|---|---|---|---|---|")
    for fov in ("100", "80"):
        for width in (1280, 1920, 2560, 3200, 3840):
            height = width * 9 // 16
            m = build(width, height, FRUSTUM_DEG[fov], args.inner, args.falloff, args.floor, args.cells)
            pw, ph = m["physical_estimate"]
            dens = " / ".join("-" if d is None else f"{d:.1f}" for d in (density_at(m, a) for a in (20, 35, 50)))
            print(f"| {fov} | {width}x{height} | {m['centre_px_per_deg']:.1f} | {pw}x{ph} | {pw * ph / base:.2f}x "
                  f"| {dens} |")


def cumulative(rates, n):
    """Physical coordinate (0..sum) of each of n+1 logical band edges, as a list; piecewise linear in between."""
    out, acc = [0.0], 0.0
    for r in rates:
        acc += r * n / len(rates)
        out.append(acc)
    return out


def unwarp_lut(args):
    import numpy as np
    from PIL import Image
    with open(args.lut) as f:
        lut = json.load(f)
    (sw, sh), (pw, ph) = lut["screen"], lut["physical"]
    if len(lut["x"]) != pw + 1 or len(lut["y"]) != ph + 1:
        sys.exit("the table needs one screen coordinate per physical pixel edge")
    img = np.asarray(Image.open(args.input).convert("RGB"), dtype=np.float32)
    h, w = img.shape[:2]
    if (w, h) == (sw, sh):
        img = img[:ph, :pw]
    elif (w, h) != (pw, ph):
        sys.exit(f"input is {w}x{h}; expected the capture ({sw}x{sh}) or the warped picture ({pw}x{ph})")
    xs = np.clip(np.interp(np.arange(sw) + 0.5, lut["x"], np.arange(pw + 1)) - 0.5, 0, pw - 1)
    ys = np.clip(np.interp(np.arange(sh) + 0.5, lut["y"], np.arange(ph + 1)) - 0.5, 0, ph - 1)
    Image.fromarray(bilinear(np, img, xs, ys)).save(args.output)
    print(f"unwarp: {pw}x{ph} -> {sw}x{sh} (table {args.lut})")


def bilinear(np, img, xs, ys):
    h, w = img.shape[:2]
    x0, y0 = np.floor(xs).astype(int), np.floor(ys).astype(int)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = (xs - x0)[None, :, None], (ys - y0)[:, None, None]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bottom = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return np.clip(top * (1 - fy) + bottom * fy + 0.5, 0, 255).astype(np.uint8)


def remap(args, to_physical):
    import numpy as np
    from PIL import Image
    if args.lut:
        if to_physical:
            sys.exit("--lut only goes with unwarp")
        return unwarp_lut(args)
    img = np.asarray(Image.open(args.input).convert("RGB"), dtype=np.float32)
    h, w = img.shape[:2]
    m = build(w if to_physical else args.width, h if to_physical else args.width * 9 // 16,
              args.frustum, args.inner, args.falloff, args.floor, args.cells)
    sw, sh = m["screen"]
    pw, ph = m["physical_estimate"]

    def axis(rates, screen, phys, src_len, dst_len):
        edges = np.array(cumulative(rates, screen)) * (phys / max(cumulative(rates, screen)[-1], 1e-9))
        logical_edges = np.linspace(0, screen, len(edges))
        centres = np.arange(dst_len) + 0.5
        # warp: output is physical, look up the logical position; unwarp: the reverse.
        src = np.interp(centres, edges, logical_edges) if to_physical else np.interp(centres, logical_edges, edges)
        return np.clip(src - 0.5, 0, src_len - 1)

    if to_physical:
        xs, ys = axis(m["horizontal"], sw, pw, w, pw), axis(m["vertical"], sh, ph, h, ph)
    else:
        if (w, h) != (pw, ph):
            sys.exit(f"input is {w}x{h}, the map's physical size for --width {args.width} is {pw}x{ph}")
        xs, ys = axis(m["horizontal"], sw, pw, w, sw), axis(m["vertical"], sh, ph, h, sh)
    out = bilinear(np, img, xs, ys)
    Image.fromarray(out).save(args.output)
    print(f"{'warp' if to_physical else 'unwarp'}: {w}x{h} -> {out.shape[1]}x{out.shape[0]} "
          f"(screen {sw}x{sh}, physical {pw}x{ph})")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=("table", "rates", "warp", "unwarp"))
    p.add_argument("input", nargs="?")
    p.add_argument("output", nargs="?")
    p.add_argument("--width", type=int, default=2560, help="logical width (height is 16:9)")
    p.add_argument("--frustum", type=float, default=FRUSTUM_DEG["100"], help="horizontal frustum in degrees")
    p.add_argument("--inner", type=float, default=20.0, help="half-angle kept at the centre's sharpness")
    p.add_argument("--falloff", type=float, default=30.0, help="degrees past the inner cone to halve sharpness")
    p.add_argument("--floor", type=float, default=0.3, help="lowest angular sharpness relative to the centre")
    p.add_argument("--cells", type=int, default=32, help="bands across the width")
    p.add_argument("--lut", help="unwarp with the real map's table instead of the requested rates")
    args = p.parse_args()
    if args.command == "table":
        table(args)
    elif args.command == "rates":
        print(json.dumps(build(args.width, args.width * 9 // 16, args.frustum, args.inner, args.falloff,
                               args.floor, args.cells)))
    else:
        if not args.input or not args.output:
            p.error("warp/unwarp need an input and an output image")
        remap(args, args.command == "warp")


if __name__ == "__main__":
    main()
