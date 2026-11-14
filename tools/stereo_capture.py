#!/usr/bin/env python3
"""True two-eye stereo captures from the macOS native build, for comparing with the depth reprojection.

    stereo_capture.py --mac BUILD_DIR --game GAME_DIR --out OUT_DIR [--config tools/stereo_capture.json]
                      [--only NAME ...] [--eye-units 2.24] [--saves DIR] [--env KEY=VALUE ...]

For each location in the config (one game launch each, from the main menu with the default character) the
runtime gets LB_STEREO_EYE_UNITS and LB_STEREO_FRAMES (runtime/d3d9_bridge.cpp): every triplet base F renders
frame F from the left eye, F+1 from the right eye and F+2 from the centre, and writes shot-F-L.bmp,
shot-F-R.bmp, shot-F-C.bmp, depth-F-C.f32 and stereo-F-<eye>.txt into the save folder. The eye shift itself is
done inside DXVK's vertex shaders (third_party/dxvk include/native/dxvk_stereo.h). The scripted input (LB_KEYS)
comes from each location's timeline: console commands typed as tools/bench/bench.py types them, and held keys.

Results: OUT_DIR/<location>/ holds the captures, game.log and env.json, and OUT_DIR/manifest.json lists the
triplets with their crops for tools/stereo_compare.py. The save folder (INIs, DXVK state cache) defaults to
bench-results/stereo-saves, seeded once with the bench's INIs from bench-results/mac-saves so the graphics
settings match; the state cache is kept apart from the bench's because the stereo shaders differ from normal ones.

A timeline entry is {"at": FRAME, "console": "command"} (opens the console, types, Return, closes it again
unless "close": false; `coc` closes it itself) or {"at": FRAME, "key": "R", "frames": 4} (an SDL key name held).
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE / "bench"))
import bench  # noqa: E402  (tools/bench/bench.py: typer, Mac launcher)


def fail(message):
    sys.exit(f"stereo_capture: {message}")


def key_script(location):
    """LB_KEYS for a location's timeline."""
    entries = []
    for step in sorted(location["timeline"], key=lambda s: s["at"]):
        frame = step["at"]
        if "console" in step:
            entries.append(f"{frame}:`:2")
            frame += 20
            typed, frame = bench.typed(step["console"], frame)
            entries += typed
            entries.append(f"{frame}:Return:2")
            frame += 30
            if step.get("close", True):
                entries.append(f"{frame}:`:2")
        elif "key" in step:
            entries.append(f"{frame}:{step['key']}:{step.get('frames', 2)}")
        else:
            fail(f"timeline entry {step} has neither console nor key")
    if len(entries) > 256:
        fail(f"{location['name']} needs {len(entries)} key events; the runtime takes 256")
    return ",".join(entries)


def check_timeline(location):
    """Steps must not overlap the triplets or each other's console typing, and must end before the exit."""
    last = max(location["triplets"]) + 2
    if last >= location["exit_frame"]:
        fail(f"{location['name']}: exit_frame {location['exit_frame']} comes before the last capture ({last})")
    bases = sorted(location["triplets"])
    for a, b in zip(bases, bases[1:]):
        if b - a < 3:
            fail(f"{location['name']}: triplets {a} and {b} overlap (each takes 3 frames)")


def seed_saves(saves):
    """Copies the bench's INIs into a fresh save folder so the captures use the same graphics settings."""
    ini_dir = Path("Documents/My Games/Skyrim")
    source = REPO / "bench-results/mac-saves" / ini_dir
    target = saves / ini_dir
    target.mkdir(parents=True, exist_ok=True)
    for name in ("Skyrim.INI", "SkyrimPrefs.ini"):
        if (source / name).exists() and not (target / name).exists():
            shutil.copy2(source / name, target / name)
            print(f"stereo_capture: seeded {target / name} from the bench's save folder")


def collect(saves, out, triplets):
    """Moves this run's stereo files from the save folder into out."""
    moved = []
    for base in triplets:
        for eye in "LRC":
            tag = f"{base:05d}-{eye}"
            for name in (f"shot-{tag}.bmp", f"stereo-{tag}.txt", f"depth-{tag}.f32"):
                path = saves / name
                if path.exists():
                    shutil.move(str(path), out / name)
                    moved.append(name)
    return moved


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mac", required=True, metavar="BUILD_DIR", help="CMake build directory holding skyrim_macos")
    ap.add_argument("--game", required=True, help="the game folder")
    ap.add_argument("--out", required=True, help="where the captures go (outside the repository)")
    ap.add_argument("--config", default=str(HERE / "stereo_capture.json"))
    ap.add_argument("--only", nargs="*", help="location names to run (default: all)")
    ap.add_argument("--eye-units", type=float, help="half the eye separation in game units (default: the config's)")
    ap.add_argument("--saves", default=str(REPO / "bench-results/stereo-saves"), help="the game's save folder")
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VALUE", help="extra launch environment")
    args = ap.parse_args(argv)

    config = json.loads(Path(args.config).read_text())
    out_root = Path(args.out).resolve()
    if out_root == REPO or REPO in out_root.parents:
        fail(f"--out {out_root} is inside the repository; pick a folder outside it")
    eye_units = args.eye_units if args.eye_units is not None else config.get("eye_units", 2.24)
    locations = [l for l in config["locations"] if not args.only or l["name"] in args.only]
    if not locations:
        fail("no locations selected")
    for location in locations:
        check_timeline(location)

    target = bench.Mac(args.mac, args.game)
    target.preflight()
    saves = Path(args.saves).resolve()
    saves.mkdir(parents=True, exist_ok=True)
    target.saves = saves  # bench.Mac launches with the save folder and DXVK state cache here
    seed_saves(saves)

    # Batch files the timelines run with `bat`, in the game folder (as bench.py does for its scenarios).
    for name, lines in config.get("game_files", {}).items():
        text = "\n".join(lines) + "\n"
        path = target.game / name
        if not path.exists() or path.read_text() != text:
            path.write_text(text)

    manifest_path = out_root / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"locations": {}}
    manifest.update({"eye_units": eye_units, "units_per_metre": config.get("units_per_metre", 70)})
    out_root.mkdir(parents=True, exist_ok=True)

    for location in locations:
        name = location["name"]
        out = out_root / name
        out.mkdir(parents=True, exist_ok=True)
        triplets = sorted(location["triplets"])
        for base in triplets:  # leftovers of an earlier run with the same frames would be mistaken for this one
            for eye in "LRC":
                for stem in ("shot", "stereo", "depth"):
                    for path in saves.glob(f"{stem}-{base:05d}-{eye}.*"):
                        path.unlink()
        env = {"LB_LOG": "1", "LB_EXIT_FRAME": str(location["exit_frame"]), "LB_KEYS": key_script(location),
               "LB_STEREO_EYE_UNITS": f"{eye_units:g}", "LB_STEREO_FRAMES": ",".join(str(b) for b in triplets)}
        env.update(config.get("env", {}))
        env.update(location.get("env", {}))
        for item in args.env:
            key, _, value = item.partition("=")
            env[key] = value
        logs_before = set(target.list_files(""))
        timeout = location["exit_frame"] / 20 + 240
        print(f"stereo_capture: {name}: running to frame {location['exit_frame']} (timeout {timeout:.0f} s), "
              f"triplets {triplets}, d = {eye_units:g} units")
        console = target.launch(env, timeout)
        logs = sorted(set(target.list_files("")) - logs_before)
        if logs:
            target.pull(logs[-1], out / "game.log")
        (out / "env.json").write_text(json.dumps(env, indent=2, sort_keys=True))
        moved = collect(saves, out, triplets)
        complete = [b for b in triplets
                    if all((out / f"shot-{b:05d}-{e}.bmp").exists() for e in "LRC") and (out / f"depth-{b:05d}-C.f32").exists()]
        print(f"stereo_capture: {name}: {len(moved)} files, complete triplets {complete} -> {out}")
        if len(complete) < len(triplets):
            print(f"stereo_capture: WARNING {name}: missing captures; see {out / 'game.log'} ({console.strip()})",
                  file=sys.stderr)
        manifest["locations"][name] = {"dir": name, "triplets": complete, "crops": location.get("crops", {}),
                                       "description": location.get("description", "")}
        manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"stereo_capture: manifest {manifest_path}")


if __name__ == "__main__":
    main()
