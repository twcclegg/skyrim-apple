import Foundation
import Metal
import simd

// MARK: - Synthetic scene

struct SynthU {
    var W: UInt32, H: UInt32, spp: UInt32, view: UInt32
    var eyeX: Float, skyDist: Float, pad0: Float = 0, pad1: Float = 0
}

struct SynthOutput {
    var image: RGBA8
    var dist: [Float]?
    var truth: [SIMD4<Float>]?
}

/// view 0: the C rig (colour + distance), view 1: an eye at x = eyeX (colour + per-pixel truth).
func synthView(gpu: GPU, width: Int, height: Int, eyeX: Float, view: Int, spp: Int, skyDist: Float = 1e9) -> SynthOutput {
    let n = width * height
    let rgba = gpu.device.makeBuffer(length: n * 4, options: .storageModeShared)!
    let dist = gpu.device.makeBuffer(length: view == 0 ? n * 4 : 16, options: .storageModeShared)!
    let truth = gpu.device.makeBuffer(length: view == 1 ? n * 16 : 16, options: .storageModeShared)!
    let commands = gpu.queue.makeCommandBuffer()!
    let encoder = commands.makeComputeCommandEncoder()!
    encoder.setComputePipelineState(gpu.synthPipeline)
    var u = SynthU(W: UInt32(width), H: UInt32(height), spp: UInt32(spp), view: UInt32(view), eyeX: eyeX, skyDist: skyDist)
    encoder.setBytes(&u, length: MemoryLayout<SynthU>.stride, index: 0)
    encoder.setBuffer(rgba, offset: 0, index: 1)
    encoder.setBuffer(dist, offset: 0, index: 2)
    encoder.setBuffer(truth, offset: 0, index: 3)
    encoder.dispatchThreads(MTLSize(width: width, height: height, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 8, depth: 1))
    encoder.endEncoding()
    commands.commit()
    commands.waitUntilCompleted()
    if let error = commands.error { fail("GPU: \(error)") }
    let image = RGBA8(width: width, height: height, pixels: Array(UnsafeBufferPointer(start: rgba.contents().bindMemory(to: UInt8.self, capacity: n * 4), count: n * 4)))
    var out = SynthOutput(image: image)
    if view == 0 { out.dist = Array(UnsafeBufferPointer(start: dist.contents().bindMemory(to: Float.self, capacity: n), count: n)) }
    if view == 1 { out.truth = Array(UnsafeBufferPointer(start: truth.contents().bindMemory(to: SIMD4<Float>.self, capacity: n), count: n)) }
    return out
}

// MARK: - Metrics

func direction(_ u: Double, _ v: Double, _ W: Int, _ H: Int) -> SIMD3<Double> {
    let az = Double.pi * (u / Double(W) - 0.5), el = Double.pi * (0.5 - v / Double(H))
    return SIMD3(cos(el) * sin(az), sin(el), cos(el) * cos(az))
}

func angle(_ a: SIMD3<Double>, _ b: SIMD3<Double>) -> Double {
    // atan2 form: accurate for tiny angles.
    let c = simd_cross(a, b)
    return atan2(simd_length(c), simd_dot(a, b))
}

/// PSNR over RGB of 8-bit pixels where mask is true (all pixels without a mask).
func psnr(_ a: RGBA8, _ b: RGBA8, mask: [Bool]? = nil) -> (psnr: Double, count: Int) {
    var sum = 0.0, count = 0
    for i in 0..<(a.width * a.height) where mask?[i] ?? true {
        for c in 0..<3 {
            let d = Double(a.pixels[i * 4 + c]) - Double(b.pixels[i * 4 + c])
            sum += d * d
        }
        count += 1
    }
    guard count > 0 else { return (.nan, 0) }
    let mse = sum / Double(count * 3)
    return (mse == 0 ? .infinity : 10 * log10(255 * 255 / mse), count)
}

func stats(_ values: inout [Double]) -> String {
    guard !values.isEmpty else { return "n/a" }
    values.sort()
    func q(_ p: Double) -> Double { values[min(values.count - 1, Int(p * Double(values.count)))] }
    let mean = values.reduce(0, +) / Double(values.count)
    let over1 = Double(values.filter { $0 > 1 }.count) / Double(values.count)
    let over05 = Double(values.filter { $0 > 0.5 }.count) / Double(values.count)
    return String(format: "mean %.3f, median %.3f, p99 %.3f, p99.9 %.3f, max %.2f px; >0.5 px %.3f%%, >1 px %.4f%% (n=%d)",
                  mean, q(0.5), q(0.99), q(0.999), values.last!, 100 * over05, 100 * over1, values.count)
}

/// Compares one reprojected eye with the ray-cast truth for that eye.
func evaluateEye(name: String, ours: EyeResult, truthImage: RGBA8, truth: [SIMD4<Float>], centre: RGBA8, ss: Int, outDir: String, block: String) {
    let W = truthImage.width, H = truthImage.height, n = W * H
    let visible = truth.map { $0.z > 0.5 }
    let occluded = visible.map { !$0 }
    let all = psnr(ours.image, truthImage)
    let vis = psnr(ours.image, truthImage, mask: visible)
    let occ = psnr(ours.image, truthImage, mask: occluded)
    let base = psnr(centre, truthImage)
    print("  \(name) eye")
    print(String(format: "    PSNR vs ray-cast truth: all %.2f dB, seen from C %.2f dB, disoccluded (not seen from C, %.3f%% of pixels) %.2f dB",
                 all.psnr, vis.psnr, 100 * Double(occ.count) / Double(n), occ.psnr))
    // Interior: the 5x5 neighbourhood in the truth is one object, all seen from C (no depth edge nearby).
    var interiorMask = [Bool](repeating: false, count: n)
    for y in 2..<(H - 2) {
        for x in 2..<(W - 2) {
            let i = y * W + x
            guard visible[i] else { continue }
            var ok = true
            for dy in -2...2 where ok { for dx in -2...2 where ok {
                let j = i + dy * W + dx
                if !visible[j] || truth[j].w != truth[i].w { ok = false }
            } }
            interiorMask[i] = ok
        }
    }
    let edgeMask = (0..<n).map { visible[$0] && !interiorMask[$0] }
    let inner = psnr(ours.image, truthImage, mask: interiorMask), edge = psnr(ours.image, truthImage, mask: edgeMask)
    print(String(format: "    seen from C, split: away from depth edges %.2f dB, within 2 px of a depth edge (%.3f%% of pixels) %.2f dB",
                 inner.psnr, 100 * Double(edge.count) / Double(n), edge.psnr))
    print(String(format: "    baseline, centre image as-is: %.2f dB", base.psnr))
    print(String(format: "    hole fraction (samples): stretched-background fill %.3f%%, nothing drawn (backplate) %.4f%%; truly disoccluded %.3f%%",
                 100 * ours.fillFraction, 100 * ours.holeFraction, 100 * Double(occ.count) / Double(n)))
    guard let dump = ours.dump, ss == 1 else {
        print("    (disparity check needs --ss 1)")
        return
    }
    // Per-pixel geometry check: the source position the reprojection used against the analytic one (the point
    // the eye's centre ray hits, projected into C), as an angle in pixels. Pixels seen from C only.
    var errAll: [Double] = [], errInterior: [Double] = [], disparity: [Double] = [], baseErr: [Double] = []
    var fillOnVisible = 0, surfaceOnOccluded = 0
    let pixelsPerRadian = Double(W) / Double.pi
    for y in 0..<H {
        for x in 0..<W {
            let i = y * W + x
            let t = truth[i], o = dump[i]
            let flag = Int(o.z.rounded())
            if !visible[i] { if flag == 1 { surfaceOnOccluded += 1 }; continue }
            if flag != 1 { fillOnVisible += 1; continue }
            let dt = direction(Double(t.x), Double(t.y), W, H)
            let e = angle(direction(Double(o.x), Double(o.y), W, H), dt) * pixelsPerRadian
            errAll.append(e)
            var interior = true
            for dy in -1...1 { for dx in -1...1 {
                let xx = min(max(x + dx, 0), W - 1), yy = min(max(y + dy, 0), H - 1)
                let j = yy * W + xx
                if !visible[j] || truth[j].w != t.w { interior = false }
            } }
            if interior { errInterior.append(e); if e > 0.4, ProcessInfo.processInfo.environment["REPROJECT_DEBUG"] != nil { print(String(format: "      interior err %.2f at %d,%d id %.0f flag %d uv %.2f,%.2f true %.2f,%.2f", e, x, y, t.w, flag, o.x, o.y, t.x, t.y)) } }
            if t.w >= 1 && t.w <= 3 {   // sphere, box, rod: the near objects
                let d = angle(direction(Double(x) + 0.5, Double(y) + 0.5, W, H), dt) * pixelsPerRadian
                disparity.append(d)
                baseErr.append(e)
                if e > 1, ProcessInfo.processInfo.environment["REPROJECT_DEBUG"] == "near" { print(String(format: "      near err %.2f at %d,%d id %.0f uv %.2f,%.2f true %.2f,%.2f", e, x, y, t.w, o.x, o.y, t.x, t.y)) }
            }
        }
    }
    var dsorted = disparity
    print("    true disparity on the near objects (centre view -> this eye): " + stats(&dsorted))
    print("    source-position error, all surface pixels seen from C: " + stats(&errAll))
    print("    source-position error, away from depth edges:          " + stats(&errInterior))
    print("    source-position error, near objects only:              " + stats(&baseErr))
    print(String(format: "    pixels seen from C but shown as fill: %.4f%%; disoccluded pixels shown as surface: %.4f%%",
                 100 * Double(fillOnVisible) / Double(n), 100 * Double(surfaceOnOccluded) / Double(n)))

    // Pictures for inspection: |difference| x4, and a flag map (grey = surface, green = fill, red = backplate,
    // blue tint = truly disoccluded).
    var diff = RGBA8(width: W, height: H, pixels: [UInt8](repeating: 255, count: n * 4))
    var flags = diff
    for i in 0..<n {
        for c in 0..<3 {
            let d = abs(Int(ours.image.pixels[i * 4 + c]) - Int(truthImage.pixels[i * 4 + c]))
            diff.pixels[i * 4 + c] = UInt8(min(255, d * 4))
        }
        let flag = Int(dump[i].z.rounded())
        var c: (Int, Int, Int) = flag == 1 ? (90, 90, 90) : (flag == 2 ? (40, 200, 40) : (230, 30, 30))
        if !visible[i] { c.2 = 255 }
        flags.pixels[i * 4] = UInt8(c.0); flags.pixels[i * 4 + 1] = UInt8(c.1); flags.pixels[i * 4 + 2] = UInt8(c.2)
    }
    writePNG(diff, (outDir as NSString).appendingPathComponent("diff-\(block)-\(name.prefix(1)).png"))
    writePNG(flags, (outDir as NSString).appendingPathComponent("flags-\(block)-\(name.prefix(1)).png"))
}

func selftest(gpu: GPU, size: Int, outDir: String, params: Params, spp: Int) {
    let block = "S"
    let W = size, H = size
    let t0 = now()
    let c = synthView(gpu: gpu, width: W, height: H, eyeX: 0, view: 0, spp: spp)
    let l = synthView(gpu: gpu, width: W, height: H, eyeX: -params.eyeUnits, view: 1, spp: spp)
    let r = synthView(gpu: gpu, width: W, height: H, eyeX: params.eyeUnits, view: 1, spp: spp)
    writePNG(c.image, (outDir as NSString).appendingPathComponent("eqr-\(block)-C.png"))
    writeDist(c.dist!, width: W, height: H, (outDir as NSString).appendingPathComponent("dist-\(block)-C.f32"))
    writePNG(l.image, (outDir as NSString).appendingPathComponent("true-\(block)-L.png"))
    writePNG(r.image, (outDir as NSString).appendingPathComponent("true-\(block)-R.png"))
    print(String(format: "synthetic scene %dx%d, %d x %d samples per pixel, made in %.2f s", W, H, spp, spp, now() - t0))
    print(String(format: "params: eye %.3f units, tear %.3f, tear-px %.2f, skirt %d, refine %d, ss %d, fill-skip %.2f, filter %@",
                 params.eyeUnits, params.tear, params.tearPx, params.skirt ? 1 : 0, params.refine, params.ss, params.fillSkip, ["bilinear", "catmull-rom", "lanczos-3"][params.filter]) + (params.mirror ? ", mirror fill" : ", smear fill"))
    let report = processBlock(gpu: gpu, inDir: outDir, block: block, outDir: outDir, params: params, dump: true)
    evaluateEye(name: "left", ours: report.left, truthImage: l.image, truth: l.truth!, centre: c.image, ss: params.ss, outDir: outDir, block: block)
    evaluateEye(name: "right", ours: report.right, truthImage: r.image, truth: r.truth!, centre: c.image, ss: params.ss, outDir: outDir, block: block)
}

func writeSynthInput(gpu: GPU, size: Int, outDir: String, block: String, spp: Int) {
    let t0 = now()
    let c = synthView(gpu: gpu, width: size, height: size, eyeX: 0, view: 0, spp: spp)
    writePNG(c.image, (outDir as NSString).appendingPathComponent("eqr-\(block)-C.png"))
    writeDist(c.dist!, width: size, height: size, (outDir as NSString).appendingPathComponent("dist-\(block)-C.f32"))
    print(String(format: "wrote synthetic eqr-%@-C.png and dist-%@-C.f32 (%dx%d) in %.2f s", block, block, size, size, now() - t0))
}

func writeSynthTruth(gpu: GPU, size: Int, outDir: String, block: String, spp: Int, eyeUnits: Float) {
    for (name, x) in [("L", -eyeUnits), ("R", eyeUnits)] {
        let e = synthView(gpu: gpu, width: size, height: size, eyeX: x, view: 1, spp: spp)
        writePNG(e.image, (outDir as NSString).appendingPathComponent("true-\(block)-\(name).png"))
    }
}

func compareImages(_ a: String, _ b: String) {
    let x = loadPNG(a), y = loadPNG(b)
    guard x.width == y.width, x.height == y.height else { fail("sizes differ: \(x.width)x\(x.height) vs \(y.width)x\(y.height)") }
    let p = psnr(x, y)
    var sum = 0.0
    for i in 0..<(x.width * x.height) { for c in 0..<3 { sum += abs(Double(x.pixels[i * 4 + c]) - Double(y.pixels[i * 4 + c])) } }
    print(String(format: "PSNR %.2f dB, mean |diff| %.3f (8-bit) over %d pixels", p.psnr, sum / Double(x.width * x.height * 3), p.count))
}

// MARK: - Real data: reprojection against true per-eye renders

/// mask dilated by r pixels (square neighbourhood), separable.
func dilate(_ mask: [Bool], _ w: Int, _ h: Int, _ r: Int) -> [Bool] {
    guard r > 0 else { return mask }
    let rows = UnsafeMutableBufferPointer<Bool>.allocate(capacity: w * h)
    let out = UnsafeMutableBufferPointer<Bool>.allocate(capacity: w * h)
    defer { rows.deallocate(); out.deallocate() }
    // One pass per line: distance to the nearest set element before and after.
    func line(_ count: Int, _ get: (Int) -> Bool, _ set: (Int, Bool) -> Void) {
        var before = [Int](repeating: 0, count: count)
        var last = -1_000_000
        for i in 0..<count { if get(i) { last = i }; before[i] = i - last }
        last = 1_000_000
        for i in stride(from: count - 1, through: 0, by: -1) {
            if get(i) { last = i }
            set(i, min(before[i], last - i) <= r)
        }
    }
    mask.withUnsafeBufferPointer { m in
        DispatchQueue.concurrentPerform(iterations: h) { y in line(w, { m[y * w + $0] }, { rows[y * w + $0] = $1 }) }
    }
    DispatchQueue.concurrentPerform(iterations: w) { x in line(h, { rows[$0 * w + x] }, { out[$0 * w + x] = $1 }) }
    return Array(out)
}

func evaluateReal(inDir: String, outDir: String, block: String, near: Float, margin: Int) {
    let path = { (d: String, n: String) in (d as NSString).appendingPathComponent(n) }
    let dist = loadDist(path(inDir, "dist-\(block)-C.f32"))
    let w = dist.width, h = dist.height, n = w * h
    var hist = [0, 0, 0, 0, 0, 0]
    let edges: [Float] = [70, 140, 350, 700, 7000]
    for v in dist.values { if !v.isFinite { hist[5] += 1 } else { hist[edges.firstIndex { v < $0 } ?? 4 + (v >= 7000 ? 1 : 0)] += 1 } }
    let pct = hist.map { 100 * Double($0) / Double(n) }
    print(String(format: "block %@: distance <1 m %.2f%%, 1-2 m %.2f%%, 2-5 m %.2f%%, 5-10 m %.2f%%, 10-100 m %.2f%%, >=100 m or sky %.2f%%",
                 block, pct[0], pct[1], pct[2], pct[3], pct[4], pct[5]))
    let nearMask = dilate(dist.values.map { $0.isFinite && $0 > 0 && $0 < near }, w, h, margin)
    let farMask = nearMask.map { !$0 }
    let centre = loadPNG(path(inDir, "eqr-\(block)-C.png"))
    print(String(format: "  near region: distance < %.0f units (%.1f m), grown by %d px: %.2f%% of pixels", near, near / 70, margin,
                 100 * Double(nearMask.filter { $0 }.count) / Double(n)))
    for eye in ["L", "R"] {
        let truth = loadPNG(path(inDir, "eqr-\(block)-\(eye).png"))
        let ours = loadPNG(path(outDir, "reproj-\(block)-\(eye).png"))
        let a = psnr(ours, truth), an = psnr(ours, truth, mask: nearMask), af = psnr(ours, truth, mask: farMask)
        let b = psnr(centre, truth), bn = psnr(centre, truth, mask: nearMask), bf = psnr(centre, truth, mask: farMask)
        print(String(format: "  %@: reprojection PSNR all %.2f dB, near %.2f dB, rest %.2f dB | centre image as-is: all %.2f, near %.2f, rest %.2f dB",
                     eye, a.psnr, an.psnr, af.psnr, b.psnr, bn.psnr, bf.psnr))
    }
}

// MARK: - Residual shift by block matching

/// Luminance at half resolution (2x2 box), linear-ish 8-bit scale.
func halfLuma(_ im: RGBA8) -> (w: Int, h: Int, v: [Float]) {
    let w = im.width / 2, h = im.height / 2
    var out = [Float](repeating: 0, count: w * h)
    out.withUnsafeMutableBufferPointer { o in
        im.pixels.withUnsafeBufferPointer { p in
            DispatchQueue.concurrentPerform(iterations: h) { y in
                for x in 0..<w {
                    var s: Float = 0
                    for dy in 0..<2 { for dx in 0..<2 {
                        let i = ((2 * y + dy) * im.width + 2 * x + dx) * 4
                        s += 0.2126 * Float(p[i]) + 0.7152 * Float(p[i + 1]) + 0.0722 * Float(p[i + 2])
                    } }
                    o[y * w + x] = s / 4
                }
            }
        }
    }
    return (w, h, out)
}

struct TileShift { var dx: Float, dy: Float, ncc: Float, std: Float }

/// For each tile of a (half-res), the shift of b that matches it best (NCC), searched over +-sx, +-sy half-res px,
/// with a parabolic sub-pixel fit in x.
func tileShifts(_ a: (w: Int, h: Int, v: [Float]), _ b: (w: Int, h: Int, v: [Float]), tile: Int, sx: Int, sy: Int) -> [TileShift?] {
    let w = a.w, h = a.h, tx = w / tile, ty = h / tile
    var result = [TileShift?](repeating: nil, count: tx * ty)
    result.withUnsafeMutableBufferPointer { res in
        DispatchQueue.concurrentPerform(iterations: tx * ty) { t in
            let x0 = (t % tx) * tile, y0 = (t / tx) * tile
            guard x0 - sx >= 0, x0 + tile + sx <= w, y0 - sy >= 0, y0 + tile + sy <= h else { return }
            var ma: Float = 0
            for y in 0..<tile { for x in 0..<tile { ma += a.v[(y0 + y) * w + x0 + x] } }
            ma /= Float(tile * tile)
            var va: Float = 0
            for y in 0..<tile { for x in 0..<tile { let d = a.v[(y0 + y) * w + x0 + x] - ma; va += d * d } }
            let std = (va / Float(tile * tile)).squareRoot()
            guard std > 8 else { return }
            var best: Float = -2, bx = 0, by = 0
            var row = [Float](repeating: -2, count: 2 * sx + 1)
            var rowAtBest = row
            for oy in -sy...sy {
                for ox in -sx...sx {
                    var mb: Float = 0, sab: Float = 0, sbb: Float = 0
                    for y in 0..<tile { for x in 0..<tile { mb += b.v[(y0 + y + oy) * w + x0 + x + ox] } }
                    mb /= Float(tile * tile)
                    for y in 0..<tile { for x in 0..<tile {
                        let da = a.v[(y0 + y) * w + x0 + x] - ma, db = b.v[(y0 + y + oy) * w + x0 + x + ox] - mb
                        sab += da * db; sbb += db * db
                    } }
                    let ncc = sab / max((va * sbb).squareRoot(), 1e-6)
                    row[ox + sx] = ncc
                    if ncc > best { best = ncc; bx = ox; by = oy }
                }
                if by == oy { rowAtBest = row }
            }
            var fx = Float(bx)
            let i = bx + sx
            if i > 0, i < 2 * sx {
                let l = rowAtBest[i - 1], c = rowAtBest[i], r = rowAtBest[i + 1], den = l - 2 * c + r
                if den < 0 { fx += 0.5 * (l - r) / den }
            }
            res[t] = TileShift(dx: fx, dy: Float(by), ncc: best, std: std)
        }
    }
    return result
}

func shiftReport(inDir: String, outDir: String, block: String, near: Float) {
    let path = { (d: String, n: String) in (d as NSString).appendingPathComponent(n) }
    let dist = loadDist(path(inDir, "dist-\(block)-C.f32"))
    let centre = halfLuma(loadPNG(path(inDir, "eqr-\(block)-C.png")))
    let tile = 32, sx = 72, sy = 4
    print("block \(block): residual shift by block matching (\(tile * 2) px tiles at full res, shifts in full-res px; textured non-sky tiles with NCC > 0.9)")
    for eye in ["L", "R"] {
        let truth = halfLuma(loadPNG(path(inDir, "eqr-\(block)-\(eye).png")))
        let ours = halfLuma(loadPNG(path(outDir, "reproj-\(block)-\(eye).png")))
        let s1 = tileShifts(truth, ours, tile: tile, sx: sx, sy: sy)
        let s0 = tileShifts(truth, centre, tile: tile, sx: sx, sy: sy)
        let tx = truth.w / tile, ty = truth.h / tile
        // Camera-locked tiles: the truth has (almost) not moved from the centre view, but the depth gives a
        // parallax of several pixels (our reprojection moved it). In these captures that is first-person
        // geometry (arms, weapon, spell), which moves with each eye's camera.
        var locked = [Bool](repeating: false, count: tx * ty)
        for t in 0..<s1.count {
            guard let a = s1[t], let b = s0[t], b.ncc > 0.9 else { continue }
            if abs(b.dx) * 2 < 1.5, abs(b.dx - a.dx) * 2 > 6 { locked[t] = true }
        }
        var lockedPixels = [Bool](repeating: false, count: centre.w * 2 * centre.h * 2)
        let W = centre.w * 2
        for t in 0..<locked.count where locked[t] {
            let x0 = (t % tx) * tile * 2, y0 = (t / tx) * tile * 2
            for y in max(0, y0 - tile * 2)..<min(centre.h * 2, y0 + tile * 4) { for x in max(0, x0 - tile * 2)..<min(W, x0 + tile * 4) { lockedPixels[y * W + x] = true } }
        }
        let keep = lockedPixels.map { !$0 }
        let oursFull = loadPNG(path(outDir, "reproj-\(block)-\(eye).png")), truthFull = loadPNG(path(inDir, "eqr-\(block)-\(eye).png"))
        let nearMask = dilate(dist.values.map { $0.isFinite && $0 > 0 && $0 < near }, dist.width, dist.height, 16)
        let p1 = psnr(oursFull, truthFull, mask: keep)
        let p2 = psnr(oursFull, truthFull, mask: (0..<keep.count).map { keep[$0] && nearMask[$0] })
        print(String(format: "  %@: %d camera-locked tiles (truth unmoved, depth parallax > 6 px); without them (+1 tile margin, %.1f%% of pixels left out): reprojection PSNR all %.2f dB, near %.2f dB",
                     eye, locked.filter { $0 }.count, 100 * Double(lockedPixels.filter { $0 }.count) / Double(keep.count), p1.psnr, p2.psnr))
        for (label, wantNear) in [("near (median distance < \(Int(near)) units)", true), ("farther", false)] {
            var r: [Double] = [], c: [Double] = [], ry: [Double] = [], ratio: [Double] = []
            var count = 0, total = 0
            for t in 0..<s1.count {
                let x0 = (t % tx) * tile * 2, y0 = (t / tx) * tile * 2
                var ds: [Float] = []
                for y in stride(from: y0, to: y0 + tile * 2, by: 4) { for x in stride(from: x0, to: x0 + tile * 2, by: 4) { ds.append(dist.values[y * dist.width + x]) } }
                ds.sort()
                let med = ds[ds.count / 2]
                guard med.isFinite, (med < near) == wantNear else { continue }   // sky: featureless, no parallax
                total += 1
                guard let a = s1[t], let b = s0[t], a.ncc > 0.9, b.ncc > 0.9, !locked[t] else { continue }
                count += 1
                if ProcessInfo.processInfo.environment["REPROJECT_DEBUG"] == "tiles", abs(a.dx) * 2 > 4 { print(String(format: "      tile %d,%d med %.0f true %.1f resid %.1f,%.1f ncc %.2f/%.2f std %.1f", x0, y0, med, b.dx * 2, a.dx * 2, a.dy * 2, a.ncc, b.ncc, a.std)) }
                r.append(Double(abs(a.dx)) * 2); ry.append(Double(abs(a.dy)) * 2); c.append(Double(abs(b.dx)) * 2)
                if abs(b.dx) > 3, abs(b.dx) < Float(sx - 1) { ratio.append(1 - Double(a.dx / b.dx)) }
            }
            print(String(format: "  %@ %@: %d of %d tiles matched", eye, label, count, total))
            print("    centre image vs truth |dx| (true disparity): " + stats(&c))
            print("    reprojection vs truth |dx| (residual):        " + stats(&r))
            print("    reprojection vs truth |dy| (residual):        " + stats(&ry))
            ratio.sort()
            if !ratio.isEmpty {
                func q(_ p: Double) -> Double { ratio[min(ratio.count - 1, Int(p * Double(ratio.count)))] }
                print(String(format: "    reprojected disparity / true disparity (tiles with > 6 px): p10 %.3f, median %.3f, p90 %.3f (n=%d)",
                             q(0.1), q(0.5), q(0.9), ratio.count))
            }
        }
    }
}
