#!/usr/bin/env python3
"""Side-by-side sheets of true stereo (the game rendered from each eye) against the compositor's depth
reprojection of the centre frame, from tools/stereo_capture.py captures.

    stereo_compare.py CAPTURE_DIR --out OUT_DIR [--only LOCATION ...] [--refine 2] [--near N]
    stereo_compare.py --shots DIR --base F --out OUT_DIR [--crop NAME=x,y,w,h ...]

For each triplet (shot-F-L.bmp, shot-F-R.bmp, shot-F-C.bmp + depth-F-C.f32 and stereo-F-*.txt):
  1. tools/immersive_shader_check.swift (the immersive screen's real Metal shaders, head mode) reprojects the
     centre frame and its depth to two eyes, with the same eye separation and field of view as the true capture:
     --ipd-scale d / (0.032 m * units per metre), --tangent the camera's right frustum tangent.
  2. tools/stereo_compose.swift lays out OUT_DIR/<location>/<F>/overview.png:
       true stereo L | R | centre,  reprojected L | R,  anaglyphs (true | reprojected),  |true - reprojected| x4
     and one crop-<name>.png per crop (hands and sword, water, fire, foliage...: the crops in the capture's
     manifest or --crop), at full resolution with sharp pixels.
Swift tools are built into OUT_DIR/bin once (swiftc -O). Nothing is written inside the repository. The left and
right frames are rendered one and two frames before the centre frame, so anything still moving at sgtm 0.02
(water, flames, foliage in the wind) may differ slightly between them for that reason alone.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
REPROJ_EYE_M = 0.032  # immersive_shader_check: each eye at +-0.032 m * --ipd-scale


def fail(message):
    sys.exit(f"stereo_compare: {message}")


def read_info(path):
    """stereo-F-<eye>.txt (runtime/d3d9_bridge.cpp stereo_write_info) as {key: [values]}."""
    info = {}
    if path.exists():
        for line in path.read_text().splitlines():
            key, *values = line.split()
            info[key] = values
    return info


def tool(name, bin_dir):
    """Builds tools/<name>.swift into bin_dir unless it is up to date; returns the executable."""
    source = HERE / f"{name}.swift"
    exe = bin_dir / name
    if not exe.exists() or exe.stat().st_mtime < source.stat().st_mtime:
        bin_dir.mkdir(parents=True, exist_ok=True)
        print(f"stereo_compare: building {exe}")
        subprocess.run(["swiftc", "-O", str(source), "-o", str(exe)], check=True)
    return exe


def full(path, rect=None):
    spec = {"path": str(path)}
    if rect:
        spec["rect"] = rect
    return spec


def within(rect, crop):
    """crop (normalized within rect) as a rect normalized over the whole image."""
    if not crop:
        return rect
    x, y, w, h = rect
    return [x + crop[0] * w, y + crop[1] * h, crop[2] * w, crop[3] * h]


def sheet(compose, out_png, title, rows, cell, nearest, work):
    spec = {"out": str(out_png), "title": title, "cell": cell, "nearest": nearest, "rows": rows}
    spec_path = work / (out_png.stem + ".json")
    spec_path.write_text(json.dumps(spec, indent=1))
    subprocess.run([str(compose), str(spec_path)], check=True)


def compare_triplet(shots, base, crops, out, args, units_per_metre, bin_dir):
    tag = f"{base:05d}"
    left, right, centre = (shots / f"shot-{tag}-{e}.bmp" for e in "LRC")
    depth = shots / f"depth-{tag}-C.f32"
    for path in (left, right, centre, depth):
        if not path.exists():
            print(f"stereo_compare: skipping {tag}: no {path}", file=sys.stderr)
            return
    info_l, info_c = read_info(shots / f"stereo-{tag}-L.txt"), read_info(shots / f"stereo-{tag}-C.txt")
    try:
        eye_units = float(info_l["eye_units"][0])
    except (KeyError, IndexError, ValueError):
        fail(f"{shots / f'stereo-{tag}-L.txt'} gives no eye_units")
    frustum = info_l.get("cam_used_frustum_lrtb_near_far") or info_c.get("cam_used_frustum_lrtb_near_far")
    tangent = float(frustum[1]) if frustum else 0.849
    near = float(frustum[4]) if frustum else None
    ipd_scale = eye_units / (REPROJ_EYE_M * units_per_metre)
    if info_l.get("clip_shift_k", ["0"])[0] in ("0", "-0"):
        print(f"stereo_compare: WARNING {tag}: the left frame was drawn with no eye shift (clip_shift_k 0): "
              "was LB_STEREO_EYE_UNITS set at launch, and was a world camera there?", file=sys.stderr)
    for key in ("menu_open",):
        if info_l.get(key, ["0"])[0] != "0" or info_c.get(key, ["0"])[0] != "0":
            print(f"stereo_compare: WARNING {tag}: a menu was open during the triplet", file=sys.stderr)

    out.mkdir(parents=True, exist_ok=True)
    work = out / "work"
    work.mkdir(exist_ok=True)
    check, compose = tool("immersive_shader_check", bin_dir), tool("stereo_compose", bin_dir)

    prefix = out / "reproj"
    command = [str(check), str(centre), str(depth), str(prefix), "--head", "--refine", f"{args.refine:g}",
               "--ipd-scale", f"{ipd_scale:.6f}", "--tangent", f"{tangent:.6f}", "--time-size", "64x64",
               "--units", f"{units_per_metre:g}", "--source", str(REPO / "apple/Sources/App/ImmersiveScreen.swift")]
    if args.near is not None:
        command += ["--near", f"{args.near:g}"]
    print("stereo_compare: " + " ".join(command))
    done = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    if done.returncode:
        fail(f"immersive_shader_check failed:\n{done.stdout}{done.stderr}")
    report = done.stdout.strip()
    sbs = Path(str(prefix) + "-sbs.png")
    halves = {"L": [0.0, 0.0, 0.5, 1.0], "R": [0.5, 0.0, 0.5, 1.0]}

    def rows_for(crop):
        tl, tr = full(left, within([0, 0, 1, 1], crop)), full(right, within([0, 0, 1, 1], crop))
        rl, rr = full(sbs, within(halves["L"], crop)), full(sbs, within(halves["R"], crop))
        tc = full(centre, within([0, 0, 1, 1], crop))
        return [
            {"label": f"true stereo: the game rendered from each eye (d = {eye_units:g} units each side)",
             "cells": [{"label": "true L", "source": tl}, {"label": "true R", "source": tr},
                       {"label": "centre (input to the reprojection)", "source": tc}]},
            {"label": f"depth reprojection of the centre frame (immersive shaders, --head --refine {args.refine:g}; "
                      "magenta = holes)",
             "cells": [{"label": "reprojected L", "source": rl}, {"label": "reprojected R", "source": rr}]},
            {"label": "anaglyph (red = left, cyan = right)",
             "cells": [{"label": "true", "source": {"anaglyph": [tl, tr]}},
                       {"label": "reprojected", "source": {"anaglyph": [rl, rr]}}]},
            {"label": "|true - reprojected| x4",
             "cells": [{"label": "L", "source": {"diff": [tl, rl], "gain": 4}},
                       {"label": "R", "source": {"diff": [tr, rr], "gain": 4}}]},
        ]

    title = (f"{shots.name} triplet {base}: tangent {tangent:.3f}, near {near if near is not None else '?'}, "
             f"ipd-scale {ipd_scale:.3f}. {report}")
    sheet(compose, out / "overview.png", title, rows_for(None), [960, 540], False, work)
    width, height = bmp_size(centre)
    for name, crop in crops.items():
        aspect = (crop[3] * height) / max(1e-6, crop[2] * width)
        cell_w = 640
        cell = [cell_w, max(64, int(round(cell_w * aspect)))]
        sheet(compose, out / f"crop-{name}.png", f"{shots.name} triplet {base}: {name} {crop}", rows_for(crop), cell,
              True, work)
    print(f"stereo_compare: {out}")


def bmp_size(path):
    header = path.read_bytes()[:26]
    return int.from_bytes(header[18:22], "little"), abs(int.from_bytes(header[22:26], "little", signed=True))


def parse_crop(text):
    name, _, values = text.partition("=")
    rect = [float(v) for v in values.split(",")]
    if len(rect) != 4:
        fail(f"--crop {text}: want NAME=x,y,w,h")
    return name, rect


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="?", help="a stereo_capture.py output folder (with manifest.json)")
    ap.add_argument("--out", required=True, help="where the sheets go (outside the repository)")
    ap.add_argument("--only", nargs="*", help="locations to compare (default: all in the manifest)")
    ap.add_argument("--shots", help="instead of a manifest: a folder of captures")
    ap.add_argument("--base", type=int, action="append", default=[], help="with --shots: triplet base frame(s)")
    ap.add_argument("--crop", action="append", default=[], metavar="NAME=x,y,w,h", help="extra crops (normalized)")
    ap.add_argument("--units-per-metre", type=float, help="game units per metre (default: manifest's, else 70)")
    ap.add_argument("--refine", type=float, default=2, help="immersive_shader_check --refine")
    ap.add_argument("--near", type=float, help="immersive_shader_check --near (default: the compositor's)")
    args = ap.parse_args(argv)

    out_root = Path(args.out).resolve()
    if out_root == REPO or REPO in out_root.parents:
        fail(f"--out {out_root} is inside the repository; pick a folder outside it")
    bin_dir = out_root / "bin"
    extra = dict(parse_crop(c) for c in args.crop)

    if args.shots:
        if not args.base:
            fail("--shots needs --base")
        shots = Path(args.shots).resolve()
        units = args.units_per_metre or 70
        for base in args.base:
            compare_triplet(shots, base, extra, out_root / shots.name / f"{base:05d}", args, units, bin_dir)
        return
    if not args.captures:
        fail("give a capture folder or --shots")
    captures = Path(args.captures).resolve()
    manifest = json.loads((captures / "manifest.json").read_text())
    units = args.units_per_metre or manifest.get("units_per_metre", 70)
    for name, location in manifest["locations"].items():
        if args.only and name not in args.only:
            continue
        crops = dict(location.get("crops", {}), **extra)
        if not location["triplets"]:
            print(f"stereo_compare: {name}: no complete triplets", file=sys.stderr)
        for base in location["triplets"]:
            compare_triplet(captures / location["dir"], base, crops, out_root / name / f"{base:05d}", args, units,
                            bin_dir)


if __name__ == "__main__":
    main()
