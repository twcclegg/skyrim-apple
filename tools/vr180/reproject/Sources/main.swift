import Foundation

setvbuf(stdout, nil, _IOLBF, 0)

let usage = """
usage:
  reproject --in DIR --block B --out DIR [options]     one block: eqr-B-C.png + dist-B-C.f32 -> reproj-B-L.png, reproj-B-R.png
  reproject --in DIR --out DIR [--all] [options]       every block in DIR with both inputs
  reproject selftest --size 2048 --out DIR [options]   synthetic scene: reproject and compare with ray-cast eye views
  reproject synth --size 5760 --out DIR [--block S]    write a synthetic eqr/dist input (and --truth: true-B-L/R.png)
  reproject compare A.png B.png                        PSNR between two images
  reproject evaluate --in DIR --out DIR [--block B] [--near 140] [--margin 16]
                                                       reproj-B-L/R.png in --out against true eqr-B-L/R.png in --in:
                                                       PSNR overall and where the distance is under --near units
                                                       (grown by --margin px), with the centre image as a baseline
options:
  --eye-units 2.24   half the eye separation, game units (eyes at x = -e and +e; 70 units = 1 m)
  --tear 0.08        a mesh triangle whose far corner is more than (1 + tear) times as far as its near one is torn
  --refine 2         per-pixel depth search steps after the mesh (the compositor's refine)
  --tear-px 1       ...and only if the parallax opens or folds it by more than this many output pixels
  --skirt 1          extend torn foreground half a pixel towards the background (0: off)
  --ss 1             supersampling per axis (ss x ss samples per output pixel, box-averaged)
  --fill smear       disocclusion fill: smear (stretch the background edge) or mirror (reflect the background)
  --fill-skip 2      pixels the stretched-background fill skips past the edge pixel
  --filter cubic     source filter, depth-aware: cubic (Catmull-Rom), lanczos (Lanczos-3) or bilinear
  --head             accepted for compatibility: equirect output is always true-depth (the compositor's head mode)
"""

var positional: [String] = []
var options: [String: String] = [:]
var flags = Set<String>()
var argv = Array(CommandLine.arguments.dropFirst())
let booleanFlags: Set<String> = ["all", "head", "truth", "help"]
while !argv.isEmpty {
    let a = argv.removeFirst()
    if a.hasPrefix("--") {
        let name = String(a.dropFirst(2))
        if booleanFlags.contains(name) { flags.insert(name); continue }
        guard !argv.isEmpty else { fail("\(a) needs a value\n\(usage)") }
        options[name] = argv.removeFirst()
    } else {
        positional.append(a)
    }
}
if flags.contains("help") || (positional.isEmpty && options.isEmpty) { print(usage); exit(0) }

func number(_ name: String, _ fallback: Float) -> Float {
    guard let s = options[name] else { return fallback }
    guard let v = Float(s) else { fail("--\(name) takes a number") }
    return v
}

var params = Params()
params.eyeUnits = number("eye-units", params.eyeUnits)
params.tear = number("tear", params.tear)
params.refine = Int(number("refine", Float(params.refine)))
params.ss = max(1, Int(number("ss", Float(params.ss))))
params.fillSkip = number("fill-skip", params.fillSkip)
params.tearPx = number("tear-px", params.tearPx)
params.skirt = number("skirt", 1) != 0
if let f = options["fill"] {
    guard f == "smear" || f == "mirror" else { fail("--fill is smear or mirror") }
    params.mirror = f == "mirror"
}
if let f = options["filter"] {
    guard let i = ["bilinear", "cubic", "lanczos"].firstIndex(of: f) else { fail("--filter is cubic, lanczos or bilinear") }
    params.filter = i
}

func directory(_ name: String) -> String {
    guard let d = options[name] else { fail("--\(name) DIR is required\n\(usage)") }
    return d
}
func ensureDirectory(_ d: String) {
    do { try FileManager.default.createDirectory(atPath: d, withIntermediateDirectories: true) } catch { fail("cannot create \(d): \(error)") }
}

switch positional.first {
case "compare":
    guard positional.count == 3 else { fail(usage) }
    compareImages(positional[1], positional[2])
case "evaluate":
    let input = directory("in"), out = directory("out")
    let list = options["block"].map { [$0] } ?? blocks(in: input)
    for b in list {
        evaluateReal(inDir: input, outDir: out, block: b, near: number("near", 140), margin: Int(number("margin", 16)))
        shiftReport(inDir: input, outDir: out, block: b, near: number("near", 140))
    }
case "selftest":
    let out = directory("out"); ensureDirectory(out)
    selftest(gpu: GPU(), size: Int(number("size", 2048)), outDir: out, params: params, spp: Int(number("spp", 4)))
case "synth":
    let out = directory("out"); ensureDirectory(out)
    let gpu = GPU(), size = Int(number("size", 5760)), block = options["block"] ?? "S", spp = Int(number("spp", 4))
    writeSynthInput(gpu: gpu, size: size, outDir: out, block: block, spp: spp)
    if flags.contains("truth") { writeSynthTruth(gpu: gpu, size: size, outDir: out, block: block, spp: spp, eyeUnits: params.eyeUnits) }
case nil:
    let input = directory("in"), out = directory("out")
    ensureDirectory(out)
    let gpu = GPU()
    let list: [String]
    if let b = options["block"] { list = [b] } else { list = blocks(in: input) }
    guard !list.isEmpty else { fail("no eqr-B-C.png + dist-B-C.f32 pairs in \(input)") }
    let t0 = now()
    for b in list { processBlock(gpu: gpu, inDir: input, block: b, outDir: out, params: params) }
    if list.count > 1 { print(String(format: "%d blocks in %.1f s", list.count, now() - t0)) }
default:
    fail("unknown command \(positional[0])\n\(usage)")
}
