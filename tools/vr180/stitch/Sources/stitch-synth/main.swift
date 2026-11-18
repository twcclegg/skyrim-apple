import Foundation
import StitchKit

// Synthetic test: ray-trace a known scene into the five faces of each rig (with the game's file
// formats), then compare the stitcher's output against the same scene ray-traced directly in
// half-equirect.
//
//   stitch-synth render --out DIR --block B --size S [--samples N] [--tan T] [--depth-rows bottom|top] [--gain-jitter G]
//   stitch-synth check  --in DIR --block B --stitched OUT_DIR [--samples N] [--diff PNG]

typealias V3 = SIMD3<Double>
@inline(__always) func dot(_ a: V3, _ b: V3) -> Double { (a * b).sum() }
@inline(__always) func len(_ a: V3) -> Double { dot(a, a).squareRoot() }

let near = 15.0
let far = 353840.0
let eyeX: [String: Double] = ["L": -2.24, "R": 2.24, "C": 0]

struct Sphere { var c: V3; var r: Double; var a: V3; var b: V3 }
struct VCyl { var x: Double; var z: Double; var r: Double; var y0: Double; var y1: Double; var col: V3 }   // vertical, along y
struct XCyl { var y: Double; var z: Double; var r: Double; var x0: Double; var x1: Double; var col: V3 }  // along x

let groundY = -100.0
let spheres: [Sphere] = [
    Sphere(c: [0, 0, 400], r: 80, a: [0.8, 0.1, 0.1], b: [0.9, 0.9, 0.9]),        // front centre
    Sphere(c: [300, 30, 300], r: 60, a: [0.1, 0.6, 0.1], b: [0.9, 0.9, 0.2]),      // front/right seam
    Sphere(c: [-250, 250, 250], r: 70, a: [0.1, 0.2, 0.8], b: [0.9, 0.6, 0.1]),    // front/left/up corner
    Sphere(c: [0, 350, 350], r: 60, a: [0.7, 0.2, 0.7], b: [0.1, 0.1, 0.1]),       // front/up seam
    Sphere(c: [-500, -20, 0], r: 100, a: [0.9, 0.5, 0.1], b: [0.2, 0.1, 0.05]),    // left face centre
    Sphere(c: [60, -40, 90], r: 25, a: [0.95, 0.95, 0.95], b: [0.05, 0.05, 0.05]), // near object
    Sphere(c: [0, 600, 0], r: 80, a: [0.2, 0.8, 0.8], b: [0.8, 0.2, 0.2]),         // zenith
    Sphere(c: [0, -70, 110], r: 30, a: [0.9, 0.9, 0.1], b: [0.1, 0.4, 0.9]),       // front/down seam
    Sphere(c: [200, -60, -60], r: 40, a: [0.5, 0.9, 0.5], b: [0.4, 0.1, 0.4]),     // right/down
]
let vcyls: [VCyl] = [
    VCyl(x: 212, z: 212, r: 6, y0: groundY, y1: 900, col: [0.95, 0.85, 0.2]),    // az +45 (front/right seam)
    VCyl(x: -160, z: 160, r: 5, y0: groundY, y1: 900, col: [0.2, 0.85, 0.95]),   // az -45 (front/left seam)
]
let xcyls: [XCyl] = [
    XCyl(y: 180, z: 180, r: 5, x0: -2000, x1: 2000, col: [0.95, 0.3, 0.3]),      // el +45 (front/up seam)
    XCyl(y: -60, z: 60, r: 3, x0: -2000, x1: 2000, col: [0.3, 0.95, 0.3]),       // el -45 (front/down seam)
]

/// Returns (distance along the normalized direction, or nil for sky; linear colour).
func trace(_ o: V3, _ dIn: V3) -> (Double?, V3) { let r = traceID(o, dIn); return (r.0, r.1) }

func traceID(_ o: V3, _ dIn: V3) -> (Double?, V3, Int) {
    let d = dIn / len(dIn)
    var best = Double.infinity
    var col = V3(0, 0, 0)
    var oid = -1
    for (si, s) in spheres.enumerated() {
        let oc = o - s.c
        let b = dot(oc, d), c = dot(oc, oc) - s.r * s.r
        let disc = b * b - c
        if disc < 0 { continue }
        let tHit = -b - disc.squareRoot()
        if tHit > 1e-6 && tHit < best {
            best = tHit; oid = si
            let p = (o + d * tHit - s.c) / s.r
            let lat = asin(max(-1, min(1, p.y))), lon = atan2(p.x, p.z)
            let iu = Int(floor((lon + .pi) / (2 * .pi) * 12)), iv = Int(floor((lat + .pi / 2) / .pi * 6))
            let shade = 0.55 + 0.45 * max(0, dot(p, V3(0.3, 0.8, -0.5) / len(V3(0.3, 0.8, -0.5))))
            col = ((iu + iv) & 1 == 0 ? s.a : s.b) * shade
        }
    }
    for (ci, c) in vcyls.enumerated() {
        let ox = o.x - c.x, oz = o.z - c.z
        let A = d.x * d.x + d.z * d.z, B = ox * d.x + oz * d.z, C = ox * ox + oz * oz - c.r * c.r
        let disc = B * B - A * C
        if A < 1e-12 || disc < 0 { continue }
        let tHit = (-B - disc.squareRoot()) / A
        let y = o.y + d.y * tHit
        if tHit > 1e-6 && tHit < best && y >= c.y0 && y <= c.y1 {
            best = tHit; oid = 100 + ci
            let band = Int(floor(y / 40)) & 1 == 0 ? 1.0 : 0.35
            col = c.col * band
        }
    }
    for (ci, c) in xcyls.enumerated() {
        let oy = o.y - c.y, oz = o.z - c.z
        let A = d.y * d.y + d.z * d.z, B = oy * d.y + oz * d.z, C = oy * oy + oz * oz - c.r * c.r
        let disc = B * B - A * C
        if A < 1e-12 || disc < 0 { continue }
        let tHit = (-B - disc.squareRoot()) / A
        let x = o.x + d.x * tHit
        if tHit > 1e-6 && tHit < best && x >= c.x0 && x <= c.x1 {
            best = tHit; oid = 200 + ci
            let band = Int(floor(x / 40)) & 1 == 0 ? 1.0 : 0.35
            col = c.col * band
        }
    }
    if d.y < -1e-9 {
        let tHit = (groundY - o.y) / d.y
        if tHit > 0 && tHit < best && tHit < 4000 {   // ground disc of radius ~4000
            best = tHit; oid = 300
            let p = o + d * tHit
            let ck = (Int(floor(p.x / 50)) + Int(floor(p.z / 50))) & 1 == 0
            let fine = (Int(floor(p.x / 10)) + Int(floor(p.z / 10))) & 1 == 0 ? 1.0 : 0.85
            col = (ck ? V3(0.45, 0.42, 0.35) : V3(0.12, 0.14, 0.10)) * fine
        }
    }
    if best.isFinite { return (best, col, oid) }
    // Sky: gradient with 10-degree azimuth/elevation grid lines (thin, so seams are visible).
    let el = asin(max(-1, min(1, d.y))) * 180 / .pi
    let az = atan2(d.x, d.z) * 180 / .pi
    var sky = V3(0.25, 0.45, 0.85) * (0.6 + 0.4 * max(0, el) / 90) + V3(0.3, 0.25, 0.1) * (1 - max(0, el) / 90)
    let gAz = abs(az / 10 - (az / 10).rounded()) * 10, gEl = abs(el / 10 - (el / 10).rounded()) * 10
    if min(gAz * cos(el * .pi / 180), gEl) < 0.15 { sky = V3(1, 1, 1) }
    return (nil, sky, -1)
}

func toSrgb8(_ c: Double) -> UInt8 { UInt8(max(0, min(255, (linearToSrgb(c) * 255).rounded()))) }

// MARK: - args

var args = Array(CommandLine.arguments.dropFirst())
guard !args.isEmpty else { print("usage: stitch-synth render|check ..."); exit(2) }
let cmd = args.removeFirst()
var kv: [String: String] = [:]
while !args.isEmpty {
    let k = args.removeFirst()
    kv[k] = args.isEmpty ? "" : args.removeFirst()
}
func arg(_ k: String, _ def: String) -> String { kv[k] ?? def }

let block = blockString(Int(arg("--block", "1"))!)
let samples = Int(arg("--samples", "4"))!

// MARK: - render

func renderFaces() throws {
    let out = URL(fileURLWithPath: arg("--out", "."))
    try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
    let S = Int(arg("--size", "1024"))!
    let t = Double(arg("--tan", "1.1"))!
    let bottomUp = arg("--depth-rows", "top") == "bottom"
    let jitter = Double(arg("--gain-jitter", "0"))!
    let gains = [0.0, 1.0, -1.0, 0.6, -0.6].map { 1 + $0 * jitter }
    let rigs = (kv["--rigs"] ?? "LRC").map { String($0) }
    for rig in rigs {
        let eye = V3(eyeX[rig]!, 0, 0)
        for k in 0..<faceCount {
            let fb = faceBases[k]
            var rgba = [UInt8](repeating: 255, count: S * S * 4)
            var depth = [Float](repeating: 1, count: S * S)
            rgba.withUnsafeMutableBufferPointer { px in
                depth.withUnsafeMutableBufferPointer { dp in
                    DispatchQueue.concurrentPerform(iterations: S) { j in
                        for i in 0..<S {
                            var acc = V3(0, 0, 0)
                            for sj in 0..<samples {
                                for si in 0..<samples {
                                    let u = 2 * (Double(i) + (Double(si) + 0.5) / Double(samples)) / Double(S) - 1
                                    let v = 1 - 2 * (Double(j) + (Double(sj) + 0.5) / Double(samples)) / Double(S)
                                    acc += trace(eye, fb.fwd + fb.right * (u * t) + fb.up * (v * t)).1
                                }
                            }
                            acc = acc / Double(samples * samples) * gains[k]
                            let o = (j * S + i) * 4
                            px[o] = toSrgb8(acc.x); px[o + 1] = toSrgb8(acc.y); px[o + 2] = toSrgb8(acc.z)
                            if rig == "C" {
                                let u = 2 * (Double(i) + 0.5) / Double(S) - 1, v = 1 - 2 * (Double(j) + 0.5) / Double(S)
                                let D = fb.fwd + fb.right * (u * t) + fb.up * (v * t)
                                if let s = trace(eye, D).0 {
                                    let z = s / len(D)   // view depth along fwd
                                    dp[j * S + i] = Float(1 - near / z)
                                }
                            }
                        }
                    }
                }
            }
            let bmp = rgba.withUnsafeBufferPointer { encodeBMP24(rgba: $0.baseAddress!, width: S, height: S) }
            try bmp.write(to: out.appendingPathComponent(shotName(block, rig, k)))
            if rig == "C" {
                var rows = [Float](repeating: 0, count: S * S)
                for r in 0..<S {
                    let src = bottomUp ? S - 1 - r : r
                    for i in 0..<S { rows[r * S + i] = depth[src * S + i] }
                }
                let d = rows.withUnsafeBufferPointer { encodeF32($0.baseAddress!, width: S, height: S) }
                try d.write(to: out.appendingPathComponent(depthName(block, k)))
            }
            var sc = Sidecar()
            sc.values = ["rig": [rig], "face": ["\(k)"], "tan": ["\(t)"], "near": ["\(near)"], "far": ["\(far)"],
                         "eye_x": ["\(eyeX[rig]!)"], "width": ["\(S)"], "height": ["\(S)"], "block": [block]]
            try sc.text().write(to: out.appendingPathComponent(sidecarName(block, rig, k)), atomically: true, encoding: .utf8)
        }
        log("rendered rig \(rig)")
    }
}

// MARK: - check

/// Covering faces of a direction (|a|,|b| <= t): >1 means the direction lies in an overlap band.
func covering(_ d: V3, t: Double) -> [Int] {
    var r: [Int] = []
    for k in 0..<faceCount {
        let fb = faceBases[k]
        let fz = dot(d, fb.fwd)
        if fz <= 0 { continue }
        if abs(dot(d, fb.right) / fz) <= t && abs(dot(d, fb.up) / fz) <= t { r.append(k) }
    }
    return r
}

func check() throws {
    let inDir = URL(fileURLWithPath: arg("--in", "."))
    let stDir = URL(fileURLWithPath: arg("--stitched", "."))
    let t = try Sidecar.load(inDir.appendingPathComponent(sidecarName(block, "C", 0))).double("tan") ?? 1.1
    var report: [String] = []
    var pass = true
    let rigs = (kv["--rigs"] ?? "LRC").map { String($0) }
    var W = 0
    var faceLabel: [Int] = []   // per pixel: -1 interior, else pair code
    let pairNames = ["front/left", "front/right", "front/up", "front/down", "left/up", "left/down", "right/up", "right/down"]
    func pairIndex(_ a: Int, _ b: Int) -> Int {
        let p = [(0, 1), (0, 2), (0, 3), (0, 4), (1, 3), (1, 4), (2, 3), (2, 4)]
        return p.firstIndex { $0 == (min(a, b), max(a, b)) } ?? -2
    }
    for rig in rigs {
        let img = try decodeImageFile(stDir.appendingPathComponent("eqr-\(block)-\(rig).png"))
        W = img.width
        if faceLabel.isEmpty {
            faceLabel = [Int](repeating: -1, count: W * W)
            faceLabel.withUnsafeMutableBufferPointer { fl in
                DispatchQueue.concurrentPerform(iterations: W) { r in
                    for c in 0..<W {
                        let cov = covering(eqrDirection(cx: Double(c) + 0.5, ry: Double(r) + 0.5, W: Double(W)), t: t)
                        fl[r * W + c] = cov.count < 2 ? -1 : (cov.count == 2 ? pairIndex(cov[0], cov[1]) : -3)
                    }
                }
            }
        }
        // Reference colour, box-supersampled over each output pixel.
        let eye = V3(eyeX[rig]!, 0, 0)
        var ref = [Float](repeating: 0, count: W * W * 3)
        var refDist = [Double](repeating: 0, count: rig == "C" ? W * W : 0)
        ref.withUnsafeMutableBufferPointer { rp in
            refDist.withUnsafeMutableBufferPointer { dp in
                DispatchQueue.concurrentPerform(iterations: W) { r in
                    for c in 0..<W {
                        var acc = V3(0, 0, 0)
                        for sj in 0..<samples {
                            for si in 0..<samples {
                                let d = eqrDirection(cx: Double(c) + (Double(si) + 0.5) / Double(samples),
                                                     ry: Double(r) + (Double(sj) + 0.5) / Double(samples), W: Double(W))
                                acc += trace(eye, d).1
                            }
                        }
                        acc /= Double(samples * samples)
                        let o = (r * W + c) * 3
                        rp[o] = Float(linearToSrgb(acc.x) * 255); rp[o + 1] = Float(linearToSrgb(acc.y) * 255); rp[o + 2] = Float(linearToSrgb(acc.z) * 255)
                        if rig == "C" {
                            dp[r * W + c] = trace(eye, eqrDirection(cx: Double(c) + 0.5, ry: Double(r) + 0.5, W: Double(W))).0 ?? .infinity
                        }
                    }
                }
            }
        }
        // Colour error and sub-pixel registration (Lucas-Kanade on luma) per region.
        var sumAbs = [Double](repeating: 0, count: 10), cnt = [Double](repeating: 0, count: 10), sse = 0.0
        var G = [[Double]](repeating: [0, 0, 0, 0, 0], count: 10)   // gxx gxy gyy gxe gye
        func luma(_ a: UnsafePointer<Float>, _ i: Int) -> Double { 0.3 * Double(a[3 * i]) + 0.6 * Double(a[3 * i + 1]) + 0.1 * Double(a[3 * i + 2]) }
        ref.withUnsafeBufferPointer { rp in
            let rb = rp.baseAddress!
            for r in 1..<(W - 1) {
                for c in 1..<(W - 1) {
                    let i = r * W + c
                    let lab = faceLabel[i]
                    let region = lab == -1 ? 0 : (lab >= 0 ? 1 + lab : 9)
                    var e = 0.0
                    for ch in 0..<3 {
                        let diff = Double(img.rgba[4 * i + ch]) - Double(rb[3 * i + ch])
                        sumAbs[region] += abs(diff); sse += diff * diff
                        e += diff * (ch == 0 ? 0.3 : ch == 1 ? 0.6 : 0.1)
                    }
                    cnt[region] += 3
                    let gx = (luma(rb, i + 1) - luma(rb, i - 1)) / 2, gy = (luma(rb, i + W) - luma(rb, i - W)) / 2
                    G[region][0] += gx * gx; G[region][1] += gx * gy; G[region][2] += gy * gy
                    G[region][3] += gx * e; G[region][4] += gy * e
                }
            }
        }
        func shift(_ g: [Double]) -> (Double, Double) {
            let det = g[0] * g[2] - g[1] * g[1]
            guard det > 1e-9 else { return (.nan, .nan) }
            return ((g[2] * g[3] - g[1] * g[4]) / det, (g[0] * g[4] - g[1] * g[3]) / det)
        }
        let mse = sse / cnt.reduce(0, +)
        let psnr = 10 * log10(255 * 255 / mse)
        var allSeam = [Double](repeating: 0, count: 5)
        var seamAbs = 0.0, seamCnt = 0.0
        for reg in 1..<10 { for q in 0..<5 { allSeam[q] += G[reg][q] }; seamAbs += sumAbs[reg]; seamCnt += cnt[reg] }
        let (ix, iy) = shift(G[0]), (sxs, sys) = shift(allSeam)
        report.append(String(format: "rig %@: PSNR %.2f dB; MAE interior %.3f, overlap bands %.3f (8-bit levels); registration vs truth interior (%.3f, %.3f) px, overlap bands (%.3f, %.3f) px",
                             rig, psnr, sumAbs[0] / cnt[0], seamAbs / seamCnt, ix, iy, sxs, sys))
        if rig == "C" {
            var seamDetail: [String] = []
            for p in 0..<8 where cnt[1 + p] > 0 {
                let (dx, dy) = shift(G[1 + p])
                seamDetail.append(String(format: "%@ MAE %.2f shift (%.3f,%.3f)", pairNames[p], sumAbs[1 + p] / cnt[1 + p], dx, dy))
                if abs(dx) > 0.1 || abs(dy) > 0.1 { pass = false }
            }
            report.append("  per seam: " + seamDetail.joined(separator: "; "))
        }
        if abs(sxs) > 0.1 || abs(sys) > 0.1 || abs(ix) > 0.1 || abs(iy) > 0.1 { pass = false }

        if let diffPath = kv["--diff"], rig == "C" {
            var dimg = [UInt8](repeating: 255, count: W * W * 4)
            ref.withUnsafeBufferPointer { rp in
                for i in 0..<(W * W) {
                    for ch in 0..<3 {
                        let diff = abs(Double(img.rgba[4 * i + ch]) - Double(rp[3 * i + ch]))
                        dimg[4 * i + ch] = UInt8(min(255, diff * 8))
                    }
                    if faceLabel[i] != -1 && Int(dimg[4 * i]) + Int(dimg[4 * i + 1]) < 20 { dimg[4 * i + 2] = max(dimg[4 * i + 2], 40) }
                }
            }
            let png = try dimg.withUnsafeBytes { try encodePNG(rgba: $0.baseAddress!, width: W, height: W) }
            try png.write(to: URL(fileURLWithPath: diffPath))
        }

        if rig == "C" {
            let dd = try readFile(stDir.appendingPathComponent("dist-\(block)-C.f32"))
            guard let (dw, dh) = f32Header(dd), dw == W, dh == W else { throw StitchError("dist header mismatch") }
            var smooth = 0, within1 = 0, within01 = 0, skyOK = 0, skyN = 0, edgeN = 0, edgeBad = 0
            var maxErr = 0.0, sumErr = 0.0
            var worst: [(Int, Int, Double, Double)] = []
            var edgeGt1 = 0, edgeGt5 = 0, edgeGt20 = 0, skyMix = 0
            var edgeExamples: [(Int, Int, [Double], Double)] = []
            dd.withUnsafeBytes { p in
                let st = (p.baseAddress! + 8).assumingMemoryBound(to: Float.self)
                for r in 1..<(W - 1) {
                    for c in 1..<(W - 1) {
                        let i = r * W + c
                        let rd = refDist[i]
                        let sd = Double(st[i])
                        var disc = false
                        var nb: [Double] = []
                        for dy in -1...1 { for dx in -1...1 {
                            let q = refDist[i + dy * W + dx]
                            nb.append(q)
                            if q.isInfinite != rd.isInfinite || (rd.isFinite && abs(q - rd) > 0.02 * rd) { disc = true }
                        } }
                        if !disc {
                            if rd.isInfinite { skyN += 1; if sd.isInfinite { skyOK += 1 } else { worst.append((r, c, rd, sd)) }; continue }
                            smooth += 1
                            let e = abs(sd - rd) / rd
                            if e < 0.01 { within1 += 1 } else { worst.append((r, c, rd, sd)) }
                            if e < 0.001 { within01 += 1 }
                            if e.isFinite { maxErr = max(maxErr, e); sumErr += e } else { skyMix += 1 }
                        } else if r >= 2 && c >= 2 && r < W - 2 && c < W - 2 {
                            // At a discontinuity the value must be the true depth of SOME surface seen within
                            // +-1 output pixel: for some object id, sd lies within [min*0.99, max*1.01] of that
                            // object's true distances sampled densely over the neighbourhood. A value blended
                            // between a silhouette and its background fails this.
                            edgeN += 1
                            func surfaceOK(_ n: Int) -> Bool {
                                var ranges: [Int: (Double, Double)] = [:]
                                for sj in 0..<n { for si in 0..<n {
                                    let d = eqrDirection(cx: Double(c) + 0.5 + 2.0 * (Double(si) / Double(n - 1) - 0.5),
                                                         ry: Double(r) + 0.5 + 2.0 * (Double(sj) / Double(n - 1) - 0.5), W: Double(W))
                                    let h = traceID(V3(0, 0, 0), d)
                                    let v = h.0 ?? .infinity
                                    if let old = ranges[h.2] { ranges[h.2] = (min(old.0, v), max(old.1, v)) } else { ranges[h.2] = (v, v) }
                                } }
                                return ranges.values.contains { rg in
                                    sd.isInfinite ? rg.1.isInfinite : (sd >= rg.0 * 0.99 && sd <= rg.1 * 1.01)
                                }
                            }
                            let ok = surfaceOK(9) || surfaceOK(49)
                            if !ok {
                                edgeBad += 1
                                // How far is it from the nearest true depth in the 5x5 neighbourhood?
                                var m = Double.infinity
                                for dy in -2...2 { for dx in -2...2 {
                                    let q = refDist[i + dy * W + dx]
                                    if q.isFinite && sd.isFinite { m = min(m, abs(sd - q) / q) }
                                } }
                                if m > 0.20 { edgeGt20 += 1 } else if m > 0.05 { edgeGt5 += 1 } else { edgeGt1 += 1 }
                                if m > 0.05 && edgeExamples.count < 6 { edgeExamples.append((r, c, nb, sd)) }
                            }
                        }
                    }
                }
            }
            for w in worst.prefix(8) { log(String(format: "  smooth-pixel miss r=%d c=%d true %.3f stitched %.3f", w.0, w.1, w.2, w.3)) }
            for e in edgeExamples { log("  edge miss r=\(e.0) c=\(e.1) stitched \(e.3) neighbours \(e.2.map { String(format: "%.1f", $0) })") }
            report.append(String(format: "dist: smooth pixels %d: %.4f%% within 1%%, %.4f%% within 0.1%%, mean rel err %.5f%%, max %.4f%% (excluding %d object/sky swaps); sky %d/%d correct; discontinuity pixels %d, %d (%.3f%%) with an in-between depth",
                                 smooth, 100 * Double(within1) / Double(smooth), 100 * Double(within01) / Double(smooth),
                                 100 * sumErr / Double(smooth), 100 * maxErr, skyMix, skyOK, skyN, edgeN, edgeBad, 100 * Double(edgeBad) / Double(max(edgeN, 1))))
            report.append("  of those, nearest true depth is 1-5% away: \(edgeGt1), 5-20%: \(edgeGt5), >20%: \(edgeGt20)")
            if Double(within1) / Double(smooth) < 0.999 || Double(skyOK) < 0.9999 * Double(skyN) || Double(edgeBad) > 0.0005 * Double(edgeN) { pass = false }
        }
    }
    print(report.joined(separator: "\n"))
    print(pass ? "RESULT: PASS" : "RESULT: FAIL")
}

/// Cross-face consistency on real data: in each overlap band, the same direction seen by two faces
/// must give the same distance (validates bases, tan, row order and z linearization) and similar colour.
func checkReal() throws {
    let inDir = URL(fileURLWithPath: arg("--in", "."))
    let flip = arg("--depth-rows", "top") == "bottom"
    var S = 0
    var depth: [[Float]] = [], rgba: [[UInt8]] = [], tan: [Double] = [], nearZ: [Double] = []
    for k in 0..<faceCount {
        let sc = try Sidecar.load(inDir.appendingPathComponent(sidecarName(block, "C", k)))
        tan.append(sc.double("tan")!); nearZ.append(sc.double("near")!)
        let d = try readFile(inDir.appendingPathComponent(depthName(block, k)))
        let (w, _) = f32Header(d)!
        S = w
        var arr = [Float](repeating: 0, count: S * S)
        d.withUnsafeBytes { p in
            let f = (p.baseAddress! + 8).assumingMemoryBound(to: Float.self)
            for r in 0..<S { for c in 0..<S { arr[r * S + c] = f[(flip ? S - 1 - r : r) * S + c] } }
        }
        depth.append(arr)
        let bmp = try readFile(inDir.appendingPathComponent(shotName(block, "C", k)))
        var px = [UInt8](repeating: 0, count: S * S * 4)
        _ = try px.withUnsafeMutableBufferPointer { try decodeBMP(bmp, into: $0.baseAddress!) }
        rgba.append(px)
    }
    func look(_ k: Int, _ d: V3) -> (Double, V3)? {
        let fb = faceBases[k]
        let fz = dot(d, fb.fwd)
        guard fz > 0 else { return nil }
        let a = dot(d, fb.right) / fz, b = dot(d, fb.up) / fz
        let u = a / tan[k], v = b / tan[k]
        guard abs(u) < 0.98, abs(v) < 0.98 else { return nil }
        let i = Int(((u + 1) * 0.5 * Double(S)).rounded(.down)), j = Int(((1 - v) * 0.5 * Double(S)).rounded(.down))
        let dd = Double(depth[k][j * S + i])
        let dist = dd >= 1 ? Double.infinity : nearZ[k] / (1 - dd) * (1 + a * a + b * b).squareRoot()
        let p = (j * S + i) * 4
        let col = V3(srgbToLinear(Double(rgba[k][p]) / 255), srgbToLinear(Double(rgba[k][p + 1]) / 255), srgbToLinear(Double(rgba[k][p + 2]) / 255))
        return (dist, col)
    }
    let pairs = [(0, 1), (0, 2), (0, 3), (0, 4), (1, 3), (1, 4), (2, 3), (2, 4)]
    let names = ["front", "left", "right", "up", "down"]
    for (fa, fb) in pairs {
        var rel: [Double] = [], skyAgree = 0, skyN = 0
        var ca = V3(0, 0, 0), cb = V3(0, 0, 0)
        let n = 400
        for y in 0..<n { for x in 0..<n {
            let d0 = eqrDirection(cx: (Double(x) + 0.5) / Double(n) * 2000, ry: (Double(y) + 0.5) / Double(n) * 2000, W: 2000)
            guard let A = look(fa, d0), let B = look(fb, d0) else { continue }
            ca += A.1; cb += B.1
            if A.0.isInfinite || B.0.isInfinite { skyN += 1; if A.0.isInfinite && B.0.isInfinite { skyAgree += 1 }; continue }
            rel.append(abs(A.0 - B.0) / min(A.0, B.0))
        } }
        rel.sort()
        guard !rel.isEmpty else { continue }
        let within1 = Double(rel.filter { $0 < 0.01 }.count) / Double(rel.count)
        print(String(format: "%@/%@: %d samples, dist rel diff median %.4f%%, p90 %.3f%%, %.1f%% within 1%%; sky agree %d/%d; mean linear colour ratio %.3f %.3f %.3f",
                     names[fa], names[fb], rel.count, 100 * rel[rel.count / 2], 100 * rel[rel.count * 9 / 10], 100 * within1, skyAgree, skyN,
                     ca.x / cb.x, ca.y / cb.y, ca.z / cb.z))
    }
}

do {
    switch cmd {
    case "render": try renderFaces()
    case "check": try check()
    case "check-real": try checkReal()
    case "viz-dist":
        // viz-dist --in dist.f32 --out viz.png : log-scale grey (near = white), sky = blue
        let d = try readFile(URL(fileURLWithPath: arg("--in", "")))
        guard let (w, h) = f32Header(d) else { throw StitchError("bad f32") }
        var img = [UInt8](repeating: 255, count: w * h * 4)
        d.withUnsafeBytes { p in
            let f = (p.baseAddress! + 8).assumingMemoryBound(to: Float.self)
            for i in 0..<(w * h) {
                let v = Double(f[i])
                if !v.isFinite { img[4 * i] = 40; img[4 * i + 1] = 60; img[4 * i + 2] = 160; continue }
                let g = UInt8(max(0, min(255, 255 * (1 - (log(max(v, 15)) - log(15)) / (log(40000) - log(15))))))
                img[4 * i] = g; img[4 * i + 1] = g; img[4 * i + 2] = g
            }
        }
        let png = try img.withUnsafeBytes { try encodePNG(rgba: $0.baseAddress!, width: w, height: h) }
        try png.write(to: URL(fileURLWithPath: arg("--out", "viz.png")))
    default: print("unknown command \(cmd)"); exit(2)
    }
} catch {
    log("error: \(error)")
    exit(1)
}
