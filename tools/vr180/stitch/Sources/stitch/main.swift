import Foundation
import Metal
import StitchKit

let usage = """
usage:
  stitch --dir IN_DIR --block B --out OUT_DIR [options]
  stitch --dir IN_DIR --out OUT_DIR --watch [--idle-exit SECONDS] [options]

options:
  --size W              output width/height (default 5760)
  --delete-inputs       delete the block's 35 input files after all outputs are written and verified
  --filter F            lanczos3 (default) | lanczos2 | cubic (Catmull-Rom)
  --antiring A          0..1, clamp resampling overshoot to the local texel range (default 1)
  --depth-rows O        top (default: row 0 = top of image, as the game writes it) | bottom
  --depth-tol X         relative z agreement for interpolating depth texels (default 0.02)
  --dist-tol X          relative agreement for blending distance across faces (default 0.03)
  --sky-dist X          value written to dist for sky (d >= 1); default inf
  --max-scale X         cap on the minification filter scale (default 4)
  --poll SECONDS        watch-mode poll interval (default 0.5)
  --settle SECONDS      watch-mode: files must be unchanged for this long (default 1.0)
"""

struct Options {
    var inDir = ""
    var outDir = ""
    var block: String?
    var W = 5760
    var deleteInputs = false
    var watch = false
    var idleExit: Double? = nil
    var filterType: UInt32 = 0
    var filterRadius: Float = 3
    var antiring: Float = 1
    var depthRowsBottomUp = false
    var depthTol: Float = 0.02
    var distTol: Float = 0.03
    var skyDist: Float = .infinity
    var maxScale: Float = 4
    var poll = 0.5
    var settle = 1.0
}

func parseArgs() -> Options {
    var o = Options()
    var args = Array(CommandLine.arguments.dropFirst())
    func next(_ name: String) -> String {
        guard !args.isEmpty else { log("missing value for \(name)\n\(usage)"); exit(2) }
        return args.removeFirst()
    }
    func num(_ name: String) -> Double {
        let s = next(name)
        if s == "inf" { return .infinity }
        guard let v = Double(s) else { log("bad number for \(name): \(s)"); exit(2) }
        return v
    }
    while !args.isEmpty {
        let a = args.removeFirst()
        switch a {
        case "--dir": o.inDir = next(a)
        case "--out": o.outDir = next(a)
        case "--block":
            let s = next(a)
            guard let b = Int(s) else { log("bad block \(s)"); exit(2) }
            o.block = blockString(b)
        case "--size": o.W = Int(num(a))
        case "--delete-inputs": o.deleteInputs = true
        case "--watch": o.watch = true
        case "--idle-exit": o.idleExit = num(a)
        case "--filter":
            switch next(a) {
            case "lanczos3": o.filterType = 0; o.filterRadius = 3
            case "lanczos2": o.filterType = 0; o.filterRadius = 2
            case "cubic", "catmull-rom": o.filterType = 1; o.filterRadius = 2
            default: log("unknown filter"); exit(2)
            }
        case "--antiring": o.antiring = Float(num(a))
        case "--depth-rows":
            switch next(a) {
            case "bottom": o.depthRowsBottomUp = true
            case "top": o.depthRowsBottomUp = false
            default: log("--depth-rows must be top or bottom"); exit(2)
            }
        case "--depth-tol": o.depthTol = Float(num(a))
        case "--dist-tol": o.distTol = Float(num(a))
        case "--sky-dist": o.skyDist = Float(num(a))
        case "--max-scale": o.maxScale = Float(num(a))
        case "--poll": o.poll = num(a)
        case "--settle": o.settle = num(a)
        case "-h", "--help": print(usage); exit(0)
        default: log("unknown argument \(a)\n\(usage)"); exit(2)
        }
    }
    if o.inDir.isEmpty || o.outDir.isEmpty || (!o.watch && o.block == nil) || o.W < 16 {
        log(usage); exit(2)
    }
    return o
}

// MARK: - Block processing

struct BlockInputs {
    var S: Int
    var sidecars: [String: [Sidecar]]   // rig -> 5 sidecars
}

func loadSidecars(_ inDir: URL, _ b: String) throws -> BlockInputs {
    var sc: [String: [Sidecar]] = [:]
    var S: Int? = nil
    for rig in rigNames {
        var list: [Sidecar] = []
        for k in 0..<faceCount {
            let url = inDir.appendingPathComponent(sidecarName(b, rig, k))
            let s = try Sidecar.load(url)
            if let r = s.values["rig"]?.first, r != rig { throw StitchError("\(url.lastPathComponent): rig \(r) != \(rig)") }
            if let f = s.int("face"), f != k { throw StitchError("\(url.lastPathComponent): face \(f) != \(k)") }
            guard let t = s.double("tan"), t > 1 else { throw StitchError("\(url.lastPathComponent): missing/invalid tan") }
            _ = t
            if rig == "C" { guard let n = s.double("near"), n > 0 else { throw StitchError("\(url.lastPathComponent): missing near") } }
            if let (w, h) = s.size {
                guard w == h else { throw StitchError("\(url.lastPathComponent): non-square face \(w)x\(h)") }
                if let s0 = S, s0 != w { throw StitchError("inconsistent face sizes \(s0) vs \(w)") }
                S = w
            }
            list.append(s)
        }
        sc[rig] = list
    }
    if S == nil {   // fall back to the BMP header
        let d = try readFile(inDir.appendingPathComponent(shotName(b, "C", 0)))
        S = try d.withUnsafeBytes { try parseBMPHeader($0).width }
    }
    return BlockInputs(S: S!, sidecars: sc)
}

final class Timer {
    var marks: [(String, Double)] = []
    let t0 = now()
    func mark(_ s: String) { marks.append((s, now() - t0)) }
    var summary: String { marks.map { String(format: "%@ %.2fs", $0.0, $0.1) }.joined(separator: ", ") }
}

/// Correlation between depth discontinuities and colour edges for both row orders, as a sanity check.
/// Returns (corrAsConfigured, corrFlipped).
func depthOrderScore(rgba: UnsafePointer<UInt8>, depth: UnsafePointer<Float>, S: Int, bottomUp: Bool) -> (Double, Double) {
    let n = 128, step = S / n
    guard step >= 1 else { return (0, 0) }
    var lum = [Double](repeating: 0, count: n * n)
    var lz = [Double](repeating: 0, count: n * n)
    for y in 0..<n {
        for x in 0..<n {
            let px = x * step + step / 2, py = y * step + step / 2
            let p = rgba + (py * S + px) * 4
            lum[y * n + x] = 0.3 * Double(p[0]) + 0.6 * Double(p[1]) + 0.1 * Double(p[2])
            let d = Double(depth[py * S + px])   // stored row py (as in file)
            lz[y * n + x] = log(1.0 / max(1.0 - d, 1e-7))
        }
    }
    func corr(flip: Bool) -> Double {
        var a: [Double] = [], b: [Double] = []
        for y in 1..<(n - 1) {
            for x in 1..<(n - 1) {
                let gl = abs(lum[y * n + x + 1] - lum[y * n + x - 1]) + abs(lum[(y + 1) * n + x] - lum[(y - 1) * n + x])
                let yy = flip ? (n - 1 - y) : y
                let gz = abs(lz[yy * n + x + 1] - lz[yy * n + x - 1]) + abs(lz[(yy + 1) * n + x] - lz[(yy - 1) * n + x])
                a.append(gl); b.append(gz)
            }
        }
        let ma = a.reduce(0, +) / Double(a.count), mb = b.reduce(0, +) / Double(b.count)
        var sab = 0.0, saa = 0.0, sbb = 0.0
        for i in 0..<a.count { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb) }
        return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0
    }
    // Depth file row r maps to image row (bottomUp ? S-1-r : r). Sampling at image row y means file row flip.
    // Our grid samples file row py; image row = bottomUp ? flipped : same.
    let asConfigured = corr(flip: bottomUp)
    let flipped = corr(flip: !bottomUp)
    return (asConfigured, flipped)
}

struct BlockResult {
    var outputs: [URL]
}

func processBlock(_ o: Options, gpu: GPUStitcher, block b: String) throws -> BlockResult {
    let tm = Timer()
    let inDir = URL(fileURLWithPath: o.inDir), outDir = URL(fileURLWithPath: o.outDir)
    try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)
    let inputs = try loadSidecars(inDir, b)
    // STITCH_BENCH_UPSCALE=N (testing aid only): upscale the inputs N times in memory, to time large S
    // without needing the disk space for large input files.
    let up = max(1, Int(ProcessInfo.processInfo.environment["STITCH_BENCH_UPSCALE"] ?? "1") ?? 1)
    let S0 = inputs.S, S = S0 * up, W = o.W
    let marginPx = Float(ceil(o.filterRadius * 2.5) + 2)
    let uLim = 1 - 2 * marginPx / Float(S)

    var faceTexHolder: MTLTexture? = gpu.makeFaceTexture(S: S)
    let encodeQueue = DispatchQueue(label: "encode", attributes: .concurrent)
    let group = DispatchGroup()
    let lock = NSLock()
    var errors: [String] = []
    var colorBuffers: [String: MTLBuffer] = [:]
    var distBuffer: MTLBuffer? = nil
    var outputs: [URL] = []
    var depthScores: [(Double, Double)] = []

    for rig in rigNames {
        let isC = rig == "C"
        let sc = inputs.sidecars[rig]!
        // Decode + upload the five colour faces (parallel decode).
        var keepRGBA: [UnsafeMutablePointer<UInt8>?] = Array(repeating: nil, count: faceCount)
        var decodeErr: String? = nil
        DispatchQueue.concurrentPerform(iterations: faceCount) { k in
            do {
                let url = inDir.appendingPathComponent(shotName(b, rig, k))
                let data = try readFile(url)
                var buf = UnsafeMutablePointer<UInt8>.allocate(capacity: S0 * S0 * 4)
                let info = try decodeBMP(data, into: buf)
                guard info.width == S0, info.height == S0 else {
                    buf.deallocate()
                    throw StitchError("\(url.lastPathComponent): \(info.width)x\(info.height), expected \(S0)x\(S0)")
                }
                if up > 1 { let big = benchUpscaleRGBA(buf, S0, up); buf.deallocate(); buf = big }
                lock.lock()
                faceTexHolder!.replace(region: MTLRegionMake2D(0, 0, S, S), mipmapLevel: 0, slice: k,
                                withBytes: buf, bytesPerRow: S * 4, bytesPerImage: S * S * 4)
                lock.unlock()
                if isC { keepRGBA[k] = buf } else { buf.deallocate() }
            } catch {
                lock.lock(); decodeErr = "\(error)"; lock.unlock()
            }
        }
        defer { for p in keepRGBA { p?.deallocate() } }
        if let e = decodeErr { throw StitchError(e) }

        var depthTex: MTLTexture? = nil
        if isC {
            let dt = gpu.makeDepthTexture(S: S)
            var depthErr: String? = nil
            DispatchQueue.concurrentPerform(iterations: faceCount) { k in
                do {
                    let url = inDir.appendingPathComponent(depthName(b, k))
                    let data = try readFile(url)
                    guard let (w, h) = f32Header(data), w == S0, h == S0, data.count == 8 + S0 * S0 * 4 else {
                        throw StitchError("\(url.lastPathComponent): bad header/size")
                    }
                    data.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
                        let src = (p.baseAddress! + 8).assumingMemoryBound(to: Float.self)
                        let tmp = UnsafeMutablePointer<Float>.allocate(capacity: S * S)
                        defer { tmp.deallocate() }
                        for r in 0..<S {
                            let r0 = r / up
                            let srcRow = o.depthRowsBottomUp ? (S0 - 1 - r0) : r0
                            if up == 1 { memcpy(tmp + r * S, src + srcRow * S, S * 4); continue }
                            for c in 0..<S { tmp[r * S + c] = src[srcRow * S0 + c / up] }
                        }
                        // Sanity check of row order against the colour image.
                        if up == 1, let rgba = keepRGBA[k] {
                            let sc = depthOrderScore(rgba: rgba, depth: src, S: S, bottomUp: o.depthRowsBottomUp)
                            lock.lock(); depthScores.append(sc); lock.unlock()
                        }
                        lock.lock()
                        dt.replace(region: MTLRegionMake2D(0, 0, S, S), mipmapLevel: 0, slice: k,
                                   withBytes: tmp, bytesPerRow: S * 4, bytesPerImage: S * S * 4)
                        lock.unlock()
                    }
                } catch {
                    lock.lock(); depthErr = "\(error)"; lock.unlock()
                }
            }
            if let e = depthErr { throw StitchError(e) }
            depthTex = dt
        }
        tm.mark("load \(rig)")

        var p = Params()
        p.W = UInt32(W); p.S = UInt32(S)
        p.hasDepth = isC ? 1 : 0
        p.filterRadius = o.filterRadius; p.filterType = o.filterType
        p.maxScale = o.maxScale; p.antiring = o.antiring
        p.depthTol = o.depthTol; p.distTol = o.distTol; p.skyDist = o.skyDist; p.skyDepth = 1
        var t5 = [Float](repeating: 1, count: 5), tE5 = t5, n5 = t5, u5 = t5
        for k in 0..<faceCount {
            let t = Float(sc[k].double("tan")!)
            t5[k] = t; u5[k] = uLim; tE5[k] = t * uLim
            guard tE5[k] > 1.003 else { throw StitchError("face overlap too small: tan \(t) at S=\(S)") }
            n5[k] = Float(isC ? sc[k].double("near")! : 15)
        }
        p.t = (t5[0], t5[1], t5[2], t5[3], t5[4]); p.tE = (tE5[0], tE5[1], tE5[2], tE5[3], tE5[4])
        p.near = (n5[0], n5[1], n5[2], n5[3], n5[4]); p.uLim = (u5[0], u5[1], u5[2], u5[3], u5[4])

        let outColor = gpu.device.makeBuffer(length: W * W * 4, options: .storageModeShared)!
        let outDist = isC ? gpu.device.makeBuffer(length: W * W * 4, options: .storageModeShared)! : nil
        try gpu.run(faces: faceTexHolder!, depth: depthTex, params: p, outColor: outColor, outDist: outDist)
        tm.mark("gpu \(rig)")
        colorBuffers[rig] = outColor
        distBuffer = outDist ?? distBuffer

        let pngURL = outDir.appendingPathComponent("eqr-\(b)-\(rig).png")
        outputs.append(pngURL)
        group.enter()
        encodeQueue.async {
            do {
                let png = try encodePNG(rgba: outColor.contents(), width: W, height: W)
                try png.withUnsafeBytes { try writeAtomically($0, to: pngURL) }
            } catch {
                lock.lock(); errors.append("\(pngURL.lastPathComponent): \(error)"); lock.unlock()
            }
            group.leave()
        }
    }

    faceTexHolder = nil   // free GPU input memory before waiting on the encoders
    if let db = distBuffer {
        let distURL = outDir.appendingPathComponent("dist-\(b)-C.f32")
        outputs.append(distURL)
        var hdr: (UInt32, UInt32) = (UInt32(W).littleEndian, UInt32(W).littleEndian)
        try withUnsafeBytes(of: &hdr) { h in
            try writeAtomically([h, UnsafeRawBufferPointer(start: db.contents(), count: W * W * 4)], to: distURL)
        }
    }
    group.wait()
    tm.mark("written")
    if !errors.isEmpty { throw StitchError(errors.joined(separator: "; ")) }

    if !depthScores.isEmpty {
        let a = depthScores.map { $0.0 }.reduce(0, +) / Double(depthScores.count)
        let f = depthScores.map { $0.1 }.reduce(0, +) / Double(depthScores.count)
        tm.mark(String(format: "depth/colour edge corr %.2f (flipped %.2f)", a, f))
        if f > a + 0.05 {
            log(String(format: "WARNING block %@: depth row order looks wrong (edge correlation %.3f as configured vs %.3f flipped); check --depth-rows", b, a, f))
            if o.deleteInputs && f > a + 0.1 {
                throw StitchError("depth row order mismatch; outputs written but inputs NOT deleted")
            }
        }
    }

    if ProcessInfo.processInfo.environment["STITCH_BENCH_VERIFY"] == "1" && !o.deleteInputs {   // testing aid
        try verifyOutputs(b: b, outDir: outDir, W: W, colorBuffers: colorBuffers, distBuffer: distBuffer)
        tm.mark("verified (bench)")
    }
    if o.deleteInputs {
        try verifyOutputs(b: b, outDir: outDir, W: W, colorBuffers: colorBuffers, distBuffer: distBuffer)
        tm.mark("verified")
        for name in allInputNames(b) {
            let url = inDir.appendingPathComponent(name)
            if unlink(url.path) != 0 && errno != ENOENT { log("warning: could not delete \(url.path) (errno \(errno))") }
        }
        tm.mark("deleted")
    }
    log("block \(b): S=\(S) W=\(W) — \(tm.summary)")
    return BlockResult(outputs: outputs)
}

/// Bilinear upscale of RGBA8 by an integer factor (benchmark aid).
func benchUpscaleRGBA(_ src: UnsafeMutablePointer<UInt8>, _ S0: Int, _ up: Int) -> UnsafeMutablePointer<UInt8> {
    let S = S0 * up
    let dst = UnsafeMutablePointer<UInt8>.allocate(capacity: S * S * 4)
    DispatchQueue.concurrentPerform(iterations: S) { y in
        let fy = max(0, min(Double(S0 - 1), (Double(y) + 0.5) / Double(up) - 0.5))
        let y0 = Int(fy), y1 = min(y0 + 1, S0 - 1), wy = fy - Double(y0)
        for x in 0..<S {
            let fx = max(0, min(Double(S0 - 1), (Double(x) + 0.5) / Double(up) - 0.5))
            let x0 = Int(fx), x1 = min(x0 + 1, S0 - 1), wx = fx - Double(x0)
            for ch in 0..<4 {
                let a = Double(src[(y0 * S0 + x0) * 4 + ch]) * (1 - wx) + Double(src[(y0 * S0 + x1) * 4 + ch]) * wx
                let b = Double(src[(y1 * S0 + x0) * 4 + ch]) * (1 - wx) + Double(src[(y1 * S0 + x1) * 4 + ch]) * wx
                dst[(y * S + x) * 4 + ch] = UInt8((a * (1 - wy) + b * wy).rounded())
            }
        }
    }
    return dst
}

/// Re-read every output from disk and compare it bit-exactly with what was computed.
func verifyOutputs(b: String, outDir: URL, W: Int, colorBuffers: [String: MTLBuffer], distBuffer: MTLBuffer?) throws {
    let lock = NSLock()
    var errors: [String] = []
    let rigs = rigNames
    for i in 0..<(rigs.count + 1) {   // sequential: keeps peak memory down (one decoded image at a time)
        do {
            if i < rigs.count {
                let rig = rigs[i]
                let url = outDir.appendingPathComponent("eqr-\(b)-\(rig).png")
                let ref = colorBuffers[rig]!.contents().assumingMemoryBound(to: UInt8.self)
                guard let bad = try countImageMismatches(url, ref: ref, width: W, height: W) else {
                    throw StitchError("\(url.lastPathComponent): wrong size")
                }
                if bad > 0 { throw StitchError("\(url.lastPathComponent): \(bad) pixels differ on read-back") }
            } else if let db = distBuffer {
                let url = outDir.appendingPathComponent("dist-\(b)-C.f32")
                let data = try readFile(url)
                guard let (w, h) = f32Header(data), w == W, h == W, data.count == 8 + W * W * 4 else {
                    throw StitchError("\(url.lastPathComponent): bad header/size")
                }
                let same = data.withUnsafeBytes { memcmp($0.baseAddress! + 8, db.contents(), W * W * 4) == 0 }
                if !same { throw StitchError("\(url.lastPathComponent): content differs on read-back") }
            } else {
                throw StitchError("no distance output")
            }
        } catch {
            lock.lock(); errors.append("\(error)"); lock.unlock()
        }
    }
    if !errors.isEmpty { throw StitchError("verification failed, inputs kept: " + errors.joined(separator: "; ")) }
}

// MARK: - Watch mode

/// True when every input of block b exists, is structurally complete, and has not changed for `settle` seconds.
func blockReady(_ inDir: URL, _ b: String, settle: Double) -> Bool {
    let tNow = now()
    for name in allInputNames(b) {
        let url = inDir.appendingPathComponent(name)
        guard let size = fileSize(url), let mt = fileMTime(url) else { return false }
        if tNow - mt < settle { return false }
        if name.hasSuffix(".bmp") {
            guard let fh = FileHandle(forReadingAtPath: url.path) else { return false }
            let hdr = (try? fh.read(upToCount: 128)) ?? Data()
            try? fh.close()
            guard let info = try? hdr.withUnsafeBytes({ try parseBMPHeader($0) }), size >= info.expectedFileSize else { return false }
        } else if name.hasSuffix(".f32") {
            guard let (w, h) = f32HeaderOfFile(url), w > 0, h > 0, size == 8 + w * h * 4 else { return false }
        } else {
            guard let s = try? Sidecar.load(url), s.double("tan") != nil, s.size != nil,
                  !name.contains("-C") || s.double("near") != nil else { return false }
        }
    }
    return true
}

func outputsUpToDate(_ inDir: URL, _ outDir: URL, _ b: String) -> Bool {
    let outs = rigNames.map { "eqr-\(b)-\($0).png" } + ["dist-\(b)-C.f32"]
    var oldestOut = Double.infinity
    for n in outs {
        guard let m = fileMTime(outDir.appendingPathComponent(n)) else { return false }
        oldestOut = min(oldestOut, m)
    }
    for n in allInputNames(b) {
        if let m = fileMTime(inDir.appendingPathComponent(n)), m > oldestOut { return false }
    }
    return true
}

func watch(_ o: Options, gpu: GPUStitcher) {
    let inDir = URL(fileURLWithPath: o.inDir), outDir = URL(fileURLWithPath: o.outDir)
    let regex = try! NSRegularExpression(pattern: "^(?:shot|depth|stereo)-([0-9]+)-")
    var done = Set<String>()
    var lastActivity = now()
    var lastListing: [String] = []
    log("watching \(o.inDir)" + (o.idleExit.map { " (idle exit after \(Int($0)) s)" } ?? ""))
    while true {
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: o.inDir)) ?? []).sorted()
        if names != lastListing { lastListing = names; lastActivity = now() }
        var blocks = Set<String>()
        for n in names {
            if let m = regex.firstMatch(in: n, range: NSRange(n.startIndex..., in: n)), let r = Range(m.range(at: 1), in: n) {
                blocks.insert(String(n[r]))
            }
        }
        var processed = false
        for b in blocks.sorted() where !done.contains(b) {
            guard blockReady(inDir, b, settle: o.settle) else { continue }
            if !o.deleteInputs && outputsUpToDate(inDir, outDir, b) {
                done.insert(b); continue
            }
            do {
                _ = try processBlock(o, gpu: gpu, block: b)
            } catch {
                log("ERROR block \(b): \(error)")
            }
            done.insert(b)
            processed = true
            lastActivity = now()
        }
        if !processed {
            if let idle = o.idleExit, now() - lastActivity >= idle {
                log("no new blocks for \(Int(idle)) s, exiting")
                return
            }
            usleep(useconds_t(o.poll * 1e6))
        }
    }
}

// MARK: - Main

let opts = parseArgs()
do {
    let gpu = try GPUStitcher()
    if opts.watch {
        watch(opts, gpu: gpu)
    } else {
        _ = try processBlock(opts, gpu: gpu, block: opts.block!)
    }
} catch {
    log("error: \(error)")
    exit(1)
}
