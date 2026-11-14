// The offline first-person hybrid for tools/stereo_compare.py --hybrid (twcclegg/skyrim-apple#23 chunk 1.2): the
// centre world reprojected to each eye by the immersive shaders, with the first-person layer rendered truly per eye
// composited on top, plus the numbers that compare it with full true stereo and with plain reprojection.
//
//   swiftc -O tools/stereo_hybrid.swift -o /tmp/stereo_hybrid
//   /tmp/stereo_hybrid spec.json
//
// spec.json (paths; the frames of one LB_STEREO_HYBRID group, runtime/d3d9_bridge.cpp):
//   {"L", "R", "C"}            full true stereo and the centre (shot-F-*.bmp)
//   {"FL", "FR", "W"}          first-person-only eyes and the centre without first-person draws
//   {"depth_C", "depth_FL", "depth_FR", "depth_W"}   their depth-F-*.f32 (the scene's R32F depth target)
//   "reproj_C", "reproj_W", "reproj_layer_C"   immersive_shader_check side-by-side PNGs (left | right) of C, of W
//                              and of layer-key-C.png (phase 1, below), all reprojected with depth_C or depth_W
//   "out"                      folder for the results; "crops": {name: [x, y, w, h]} (normalized, top-left)
//   "noise"                    colour difference (0-255) below which FL and W count as equal (default 6)
//
// The first-person layer of an eye, from FL (that eye's hands over the centre world) and W (the centre world alone,
// the same game moment):
//   opaque    pixels whose depth differs between FL and W: the late pass's opaque meshes write the R32F depth target
//             over the world's, and nothing else differs there since the clock was held. Taken from FL as they are.
//   additive  elsewhere, max(0, FL - W) per channel beyond the noise: the flame and its glow (additive billboards
//             that write no depth). Added to the reprojected world. This is a difference of tone-mapped pictures, not
//             the additive blend in the 16-bit scene target, so bright flame over a bright world is under-added.
// Run twice: without "reproj_layer_C" it writes only layer-key-C.png (the centre's own layer: red = additive, green =
// opaque, from C against W with depth_C) for the caller to reproject like C; with it, everything else.
// Writes, in "out": hybrid-L/R.png (over the reprojection of W, the world without mono hands), hybridc-L/R.png
// (over the reprojection of C, which still has the centre eye's hands and flame), layer-L/R.png (opaque in colour,
// additive x2, the rest black), and metrics.json (printed too):
//   per crop and eye, mean |X - true| over RGB (0-255) for X = reprojection of C (b), hybrid (c), hybrid over C (c2)
//   per crop, the disparity (x_left - x_right in pixels; positive = nearer than the screen at infinity) of the
//   opaque first-person pixels and of the flame in (a), (b) and (c) (objectDisparity below says how)
//   checks: mean |FL - L| inside the opaque layer (~0 if the first-person-only shift draws the same hands as full
//   stereo), mean |FL - C| outside the layer and the centre's hands (~0: the same world), |W - C| likewise
import AppKit
import CoreGraphics
import Foundation

func fail(_ text: String) -> Never {
    FileHandle.standardError.write(("stereo_hybrid: " + text + "\n").data(using: .utf8)!)
    exit(1)
}

/// An RGBA8 picture, rows top to bottom.
struct Picture {
    var width: Int, height: Int
    var pixels: [UInt8]

    init(width: Int, height: Int) {
        self.width = width; self.height = height
        pixels = [UInt8](repeating: 0, count: width * height * 4)
    }

    init(path: String) {
        guard let image = NSImage(contentsOfFile: path)?.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
            fail("cannot read \(path)")
        }
        self.init(width: image.width, height: image.height)
        let w = width, h = height
        pixels.withUnsafeMutableBytes { buffer in
            let context = CGContext(data: buffer.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
    }

    /// The left or right half (a side-by-side picture), resampled to w x h if needed (nearest).
    func half(_ right: Bool, width w: Int, height h: Int) -> Picture {
        let hw = width / 2, x0 = right ? hw : 0
        var out = Picture(width: w, height: h)
        for y in 0..<h {
            let sy = min(height - 1, y * height / h)
            for x in 0..<w {
                let sx = x0 + min(hw - 1, x * hw / w)
                for c in 0..<4 { out.pixels[(y * w + x) * 4 + c] = pixels[(sy * width + sx) * 4 + c] }
            }
        }
        return out
    }

    func luma(_ i: Int) -> Double {
        0.299 * Double(pixels[i * 4]) + 0.587 * Double(pixels[i * 4 + 1]) + 0.114 * Double(pixels[i * 4 + 2])
    }

    func write(_ path: String) {
        var copy = pixels
        let provider = CGDataProvider(data: Data(bytes: &copy, count: copy.count) as CFData)!
        let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
        guard let png = NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:]) else { fail("cannot encode \(path)") }
        do { try png.write(to: URL(fileURLWithPath: path)) } catch { fail("cannot write \(path): \(error)") }
    }
}

func readDepth(_ path: String) -> (Int, Int, [Float]) {
    guard let data = FileManager.default.contents(atPath: path), data.count > 8 else { fail("cannot read \(path)") }
    let w = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) })
    let h = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self) })
    guard data.count >= 8 + w * h * 4 else { fail("\(path) is shorter than its header says") }
    var values = [Float](repeating: 0, count: w * h)
    values.withUnsafeMutableBytes { dst in data.withUnsafeBytes { src in dst.copyMemory(from: UnsafeRawBufferPointer(rebasing: src[8..<(8 + w * h * 4)])) } }
    return (w, h, values)
}

guard CommandLine.arguments.count == 2,
      let specData = FileManager.default.contents(atPath: CommandLine.arguments[1]),
      let spec = try? JSONSerialization.jsonObject(with: specData) as? [String: Any] else { fail("usage: stereo_hybrid spec.json") }
func path(_ key: String) -> String {
    guard let p = spec[key] as? String else { fail("spec needs \(key)") }
    return p
}
let outDir = path("out")
let noise = spec["noise"] as? Double ?? 6
let crops = (spec["crops"] as? [String: [Double]]) ?? [:]

let L = Picture(path: path("L")), R = Picture(path: path("R")), C = Picture(path: path("C"))
let FL = Picture(path: path("FL")), FR = Picture(path: path("FR")), W = Picture(path: path("W"))
let width = C.width, height = C.height
for p in [L, R, FL, FR, W] where p.width != width || p.height != height { fail("frames differ in size") }
func depthAt(_ key: String) -> [Float] {
    let (w, h, values) = readDepth(path(key))
    guard w == width, h == height else { fail("\(key) is \(w)x\(h), the frames \(width)x\(height)") }
    return values
}
let dW = depthAt("depth_W")

struct Layer {
    var opaque: [Bool]
    var add: [Double]   // RGB per pixel, 3 per pixel
    func addSum(_ i: Int) -> Double { opaque[i] ? 0 : add[i * 3] + add[i * 3 + 1] + add[i * 3 + 2] }
    func any(_ i: Int) -> Bool { opaque[i] || addSum(i) > 0 }
}

/// The first-person layer of a frame F drawn at the same moment as W: opaque where the depth differs, else F - W.
func layer(_ F: Picture, _ dF: [Float]) -> Layer {
    var opaque = [Bool](repeating: false, count: width * height)
    var add = [Double](repeating: 0, count: width * height * 3)
    for i in 0..<(width * height) {
        let a = dF[i], b = dW[i]
        if abs(a - b) > 1e-4 * max(abs(a), abs(b), 1e-6) { opaque[i] = true; continue }
        var any = false
        var d = [0.0, 0.0, 0.0]
        for c in 0..<3 {
            d[c] = Double(F.pixels[i * 4 + c]) - Double(W.pixels[i * 4 + c])
            if d[c] > noise { any = true }
        }
        if any { for c in 0..<3 { add[i * 3 + c] = max(0, d[c]) } }
    }
    return Layer(opaque: opaque, add: add)
}

// Phase 1 (no "reproj_layer_C" yet): the centre's own layer as a key picture for the reprojection, so the numbers
// can find the hands and flame in (b): red = additive intensity (mean of RGB), green = 255 where opaque.
if spec["reproj_layer_C"] == nil {
    let centre = layer(C, depthAt("depth_C"))
    var key = Picture(width: width, height: height)
    for i in 0..<(width * height) {
        key.pixels[i * 4] = UInt8(min(255, centre.addSum(i) / 3))
        key.pixels[i * 4 + 1] = centre.opaque[i] ? 255 : 0
        key.pixels[i * 4 + 3] = 255
    }
    key.write(outDir + "/layer-key-C.png")
    print(outDir + "/layer-key-C.png")
    exit(0)
}

let reprojC = Picture(path: path("reproj_C")), reprojW = Picture(path: path("reproj_W"))
let reprojKey = Picture(path: path("reproj_layer_C"))
let bL = reprojC.half(false, width: width, height: height), bR = reprojC.half(true, width: width, height: height)
let wL = reprojW.half(false, width: width, height: height), wR = reprojW.half(true, width: width, height: height)
let kL = reprojKey.half(false, width: width, height: height), kR = reprojKey.half(true, width: width, height: height)
let layerL = layer(FL, depthAt("depth_FL")), layerR = layer(FR, depthAt("depth_FR"))

func composite(_ F: Picture, _ lay: Layer, over base: Picture) -> Picture {
    var out = base
    for i in 0..<(width * height) {
        if lay.opaque[i] {
            for c in 0..<3 { out.pixels[i * 4 + c] = F.pixels[i * 4 + c] }
        } else {
            for c in 0..<3 { out.pixels[i * 4 + c] = UInt8(min(255, Double(base.pixels[i * 4 + c]) + lay.add[i * 3 + c])) }
        }
        out.pixels[i * 4 + 3] = 255
    }
    return out
}
func visual(_ F: Picture, _ lay: Layer) -> Picture {
    var out = Picture(width: width, height: height)
    for i in 0..<(width * height) {
        for c in 0..<3 {
            out.pixels[i * 4 + c] = lay.opaque[i] ? F.pixels[i * 4 + c] : UInt8(min(255, 2 * lay.add[i * 3 + c]))
        }
        out.pixels[i * 4 + 3] = 255
    }
    return out
}

let cL = composite(FL, layerL, over: wL), cR = composite(FR, layerR, over: wR)
let c2L = composite(FL, layerL, over: bL), c2R = composite(FR, layerR, over: bR)
cL.write(outDir + "/hybrid-L.png"); cR.write(outDir + "/hybrid-R.png")
c2L.write(outDir + "/hybridc-L.png"); c2R.write(outDir + "/hybridc-R.png")
visual(FL, layerL).write(outDir + "/layer-L.png"); visual(FR, layerR).write(outDir + "/layer-R.png")

// ---------------------------------------------------------------- numbers

struct Box { var x0: Int, y0: Int, x1: Int, y1: Int }  // half-open
func boxOf(_ rect: [Double], padX: Int = 0) -> Box {
    Box(x0: max(0, Int(rect[0] * Double(width)) - padX), y0: Int(rect[1] * Double(height)),
        x1: min(width, Int((rect[0] + rect[2]) * Double(width)) + padX), y1: min(height, Int((rect[1] + rect[3]) * Double(height))))
}
func mad(_ a: Picture, _ b: Picture, _ box: Box, where keep: ((Int) -> Bool)? = nil) -> Double {
    var sum = 0.0, n = 0
    for y in box.y0..<box.y1 {
        for x in box.x0..<box.x1 {
            let i = y * width + x
            if let keep = keep, !keep(i) { continue }
            for c in 0..<3 { sum += abs(Double(a.pixels[i * 4 + c]) - Double(b.pixels[i * 4 + c])) }
            n += 3
        }
    }
    return n > 0 ? sum / Double(n) : 0
}
func round2(_ v: Double) -> Double { (v * 100).rounded() / 100 }
/// Weighted mean x of a weight over a box; nil if the box holds no weight.
func centroidX(_ box: Box, _ weight: (Int) -> Double) -> Double? {
    var sx = 0.0, s = 0.0
    for y in box.y0..<box.y1 {
        for x in box.x0..<box.x1 {
            let w = weight(y * width + x)
            if w > 0 { sx += w * Double(x); s += w }
        }
    }
    return s > 0 ? sx / s : nil
}
/// The shift dx (left x - right x) that best matches the left picture inside box with the right one, by weighted
/// mean absolute luminance difference (weights from the left picture), refined to a fraction of a pixel.
func blockMatch(_ a: Picture, _ b: Picture, _ box: Box, range: Int = 200, weight: (Int) -> Double) -> Double? {
    var costs: [Int: Double] = [:]
    var best = Double.infinity, bestDx = 0
    for dx in -range...range {
        var sum = 0.0, wsum = 0.0
        for y in box.y0..<box.y1 {
            for x in box.x0..<box.x1 {
                let xr = x - dx
                if xr < 0 || xr >= width { continue }
                let i = y * width + x, w = weight(i)
                if w <= 0 { continue }
                sum += w * abs(a.luma(i) - b.luma(y * width + xr))
                wsum += w
            }
        }
        if wsum <= 0 { continue }
        costs[dx] = sum / wsum
        if sum / wsum < best { best = sum / wsum; bestDx = dx }
    }
    if best == .infinity { return nil }
    if let cm = costs[bestDx - 1], let cp = costs[bestDx + 1], cm - 2 * best + cp > 0 {
        return Double(bestDx) + 0.5 * (cm - cp) / (cm - 2 * best + cp)
    }
    return Double(bestDx)
}

let whole = Box(x0: 0, y0: 0, x1: width, y1: height)
var metrics: [String: Any] = ["size": [width, height], "noise": noise]

// Disparity of the first-person objects in each crop (the crop widened by 120 px each side, so an object near its
// edge is not cut differently in the two eyes): x_left - x_right of the weighted centroid, in pixels.
//   opaque  hand, arm or sword: the opaque layer. (a) and (c) share FL's and FR's layer (the late pass draws the same
//           pixels with or without the world shifted); (b) finds the centre's layer where the reprojection put it.
//   flame   the additive layer's intensity, likewise.
// And, from the pictures alone, the flame by block matching: the box around the bright additive pixels (sum over
// 120), weighted by how much brighter than the box's 75th percentile each left pixel is.
func objectDisparity(_ box: Box) -> [String: Any] {
    var out: [String: Any] = [:]
    let trueOpaque = (centroidX(box) { layerL.opaque[$0] ? 1 : 0 }, centroidX(box) { layerR.opaque[$0] ? 1 : 0 })
    let reprOpaque = (centroidX(box) { kL.pixels[$0 * 4 + 1] > 128 ? 1 : 0 }, centroidX(box) { kR.pixels[$0 * 4 + 1] > 128 ? 1 : 0 })
    let trueFlame = (centroidX(box) { layerL.addSum($0) }, centroidX(box) { layerR.addSum($0) })
    // (blue marks the reprojection's holes, magenta: neither flame nor hand)
    func keyFlame(_ k: Picture, _ i: Int) -> Double {
        k.pixels[i * 4 + 1] > 128 || k.pixels[i * 4 + 2] > 128 ? 0 : Double(k.pixels[i * 4])
    }
    let reprFlame = (centroidX(box) { keyFlame(kL, $0) }, centroidX(box) { keyFlame(kR, $0) })
    if let l = trueOpaque.0, let r = trueOpaque.1, let bl = reprOpaque.0, let br = reprOpaque.1 {
        out["opaque"] = ["true": round2(l - r), "hybrid": round2(l - r), "reprojection": round2(bl - br),
                         "pixels": layerL.opaque.indices.filter { i in
                             let x = i % width, y = i / width
                             return layerL.opaque[i] && x >= box.x0 && x < box.x1 && y >= box.y0 && y < box.y1 }.count]
    }
    if let l = trueFlame.0, let r = trueFlame.1, let bl = reprFlame.0, let br = reprFlame.1 {
        out["flame"] = ["true": round2(l - r), "hybrid": round2(l - r), "reprojection": round2(bl - br)]
    }
    // block matching on the pictures themselves
    var core = Box(x0: width, y0: height, x1: 0, y1: 0)
    for lay in [layerL, layerR] {
        for y in box.y0..<box.y1 {
            for x in box.x0..<box.x1 where lay.addSum(y * width + x) > 120 {
                core.x0 = min(core.x0, x); core.y0 = min(core.y0, y); core.x1 = max(core.x1, x + 1); core.y1 = max(core.y1, y + 1)
            }
        }
    }
    if core.x1 > core.x0 {
        core = Box(x0: max(0, core.x0 - 16), y0: max(0, core.y0 - 16), x1: min(width, core.x1 + 16), y1: min(height, core.y1 + 16))
        func match(_ a: Picture, _ b: Picture) -> Double? {
            var lum: [Double] = []
            for y in core.y0..<core.y1 { for x in core.x0..<core.x1 { lum.append(a.luma(y * width + x)) } }
            lum.sort()
            let floor = lum[lum.count * 3 / 4]
            return blockMatch(a, b, core) { max(0, a.luma($0) - floor) }
        }
        var m: [String: Any] = ["box": [core.x0, core.y0, core.x1, core.y1]]
        if let v = match(L, R) { m["true"] = round2(v) }
        if let v = match(bL, bR) { m["reprojection"] = round2(v) }
        if let v = match(cL, cR) { m["hybrid"] = round2(v) }
        out["flame_block_match"] = m
    }
    return out
}

var cropMetrics: [String: Any] = [:]
for (name, rect) in crops.sorted(by: { $0.key < $1.key }) {
    let box = boxOf(rect)
    cropMetrics[name] = [
        "rect": rect,
        "mad_vs_true": [
            "L": ["reprojection": round2(mad(bL, L, box)), "hybrid": round2(mad(cL, L, box)),
                  "hybrid_over_C": round2(mad(c2L, L, box))],
            "R": ["reprojection": round2(mad(bR, R, box)), "hybrid": round2(mad(cR, R, box)),
                  "hybrid_over_C": round2(mad(c2R, R, box))],
        ],
        "disparity_px": objectDisparity(boxOf(rect, padX: 120)),
    ]
}
metrics["crops"] = cropMetrics

// Checks. The centre's own layer (C against W) marks where the centre eye's hands are.
var centreHands = [Bool](repeating: false, count: width * height)
for i in 0..<(width * height) {
    for c in 0..<3 where abs(Double(C.pixels[i * 4 + c]) - Double(W.pixels[i * 4 + c])) > noise { centreHands[i] = true }
}
metrics["checks"] = [
    "FL_vs_L_in_opaque_layer": round2(mad(FL, L, whole) { layerL.opaque[$0] }),
    "FR_vs_R_in_opaque_layer": round2(mad(FR, R, whole) { layerR.opaque[$0] }),
    "FL_vs_C_outside_hands": round2(mad(FL, C, whole) { !layerL.any($0) && !centreHands[$0] }),
    "FR_vs_C_outside_hands": round2(mad(FR, C, whole) { !layerR.any($0) && !centreHands[$0] }),
    "W_vs_C_outside_centre_hands": round2(mad(W, C, whole) { !centreHands[$0] }),
    "opaque_pixels": [layerL.opaque.filter { $0 }.count, layerR.opaque.filter { $0 }.count],
    "additive_pixels": [(0..<(width * height)).filter { layerL.addSum($0) > 0 }.count,
                        (0..<(width * height)).filter { layerR.addSum($0) > 0 }.count],
]

let json = try! JSONSerialization.data(withJSONObject: metrics, options: [.prettyPrinted, .sortedKeys])
FileManager.default.createFile(atPath: outDir + "/metrics.json", contents: json)
print(String(data: json, encoding: .utf8)!)
