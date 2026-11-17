import CoreGraphics
import Foundation
import ImageIO
import Metal
import UniformTypeIdentifiers

func fail(_ text: String) -> Never {
    FileHandle.standardError.write(("reproject: " + text + "\n").data(using: .utf8)!)
    exit(1)
}

func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) * 1e-9 }

// MARK: - Image and depth files

/// 8-bit RGBA, rows top first. PNG bytes are taken as they are (sRGB), without colour management.
struct RGBA8 {
    var width: Int, height: Int
    var pixels: [UInt8]
}

func loadPNG(_ path: String) -> RGBA8 {
    let url = URL(fileURLWithPath: path) as CFURL
    guard let source = CGImageSourceCreateWithURL(url, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCache: false] as CFDictionary)
    else { fail("cannot read \(path)") }
    let w = image.width, h = image.height
    var pixels = [UInt8](repeating: 255, count: w * h * 4)
    // Raw path for the usual 8-bit RGB / RGBA layouts: no colour conversion at all.
    let alpha = image.alphaInfo
    let order = image.bitmapInfo.intersection(.byteOrderMask)
    if image.bitsPerComponent == 8, (order == [] || order == .byteOrder32Big || order == .byteOrderDefault || order == .byteOrder32Little),
       image.bitsPerPixel == 24 || image.bitsPerPixel == 32,
       let data = image.dataProvider?.data, let bytes = CFDataGetBytePtr(data) {
        let stride = image.bytesPerRow, bpp = image.bitsPerPixel / 8
        let little = order == .byteOrder32Little
        // Offsets of R, G, B in memory.
        var offsets = [0, 1, 2]
        if bpp == 4 {
            let alphaFirst = alpha == .first || alpha == .premultipliedFirst || alpha == .noneSkipFirst
            offsets = alphaFirst ? [1, 2, 3] : [0, 1, 2]
            if little { offsets = offsets.map { 3 - $0 } }
        }
        let premultiplied = alpha == .premultipliedFirst || alpha == .premultipliedLast
        if !premultiplied {
            pixels.withUnsafeMutableBufferPointer { out in
                DispatchQueue.concurrentPerform(iterations: h) { y in
                    let row = bytes + y * stride
                    for x in 0..<w {
                        let s = row + x * bpp, d = (y * w + x) * 4
                        out[d] = s[offsets[0]]; out[d + 1] = s[offsets[1]]; out[d + 2] = s[offsets[2]]
                    }
                }
            }
            return RGBA8(width: w, height: h, pixels: pixels)
        }
    }
    let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
    pixels.withUnsafeMutableBytes { buffer in
        let context = CGContext(data: buffer.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                space: space.model == .rgb ? space : CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        context.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    }
    return RGBA8(width: w, height: h, pixels: pixels)
}

func writePNG(_ image: RGBA8, _ path: String) {
    var pixels = image.pixels
    let provider = CGDataProvider(data: Data(bytesNoCopy: &pixels, count: pixels.count, deallocator: .none) as CFData)!
    guard let cg = CGImage(width: image.width, height: image.height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: image.width * 4,
                           space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
          let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)
    else { fail("cannot write \(path)") }
    CGImageDestinationAddImage(destination, cg, nil)
    if !CGImageDestinationFinalize(destination) { fail("cannot write \(path)") }
    withExtendedLifetime(pixels) {}
}

/// dist-B-C.f32: uint32 W, uint32 H, then float32 rows top first (radial distance from the rig centre).
func loadDist(_ path: String) -> (width: Int, height: Int, values: [Float]) {
    guard let handle = FileHandle(forReadingAtPath: path) else { fail("cannot read \(path)") }
    defer { try? handle.close() }
    guard let header = try? handle.read(upToCount: 8), header.count == 8 else { fail("\(path): short header") }
    let w = Int(header.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) })
    let h = Int(header.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self) })
    guard w > 1, h > 1, w * h < 1 << 31 else { fail("\(path): bad size \(w)x\(h)") }
    var values = [Float](repeating: 0, count: w * h)
    let need = w * h * 4
    let got = values.withUnsafeMutableBytes { buffer -> Int in
        var done = 0
        while done < need {
            guard let chunk = try? handle.read(upToCount: min(need - done, 64 << 20)), !chunk.isEmpty else { break }
            chunk.copyBytes(to: UnsafeMutableRawBufferPointer(rebasing: buffer[done..<done + chunk.count]))
            done += chunk.count
        }
        return done
    }
    guard got == need else { fail("\(path): \(got) bytes of float data, header says \(need)") }
    return (w, h, values)
}

func writeDist(_ values: [Float], width: Int, height: Int, _ path: String) {
    var data = Data(capacity: 8 + values.count * 4)
    var header = [UInt32(width), UInt32(height)]
    data.append(Data(bytes: &header, count: 8))
    values.withUnsafeBytes { data.append(contentsOf: $0) }
    guard FileManager.default.createFile(atPath: path, contents: data) else { fail("cannot write \(path)") }
}

// MARK: - GPU

final class GPU {
    let device: any MTLDevice
    let queue: any MTLCommandQueue
    let library: any MTLLibrary
    lazy var meshPipeline: any MTLRenderPipelineState = {
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = library.makeFunction(name: "mesh_vertex")
        d.fragmentFunction = library.makeFunction(name: "mesh_fragment")
        d.colorAttachments[0].pixelFormat = .rgba32Float
        d.depthAttachmentPixelFormat = .depth32Float
        do { return try device.makeRenderPipelineState(descriptor: d) } catch { fail("mesh pipeline: \(error)") }
    }()
    lazy var depthState: any MTLDepthStencilState = {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .greater
        d.isDepthWriteEnabled = true
        return device.makeDepthStencilState(descriptor: d)!
    }()
    lazy var resolvePipeline = compute("resolve")
    lazy var synthPipeline = compute("synth")

    init() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("no Metal device") }
        self.device = device
        self.queue = queue
        let options = MTLCompileOptions()
        options.mathMode = .safe
        options.mathFloatingPointFunctions = .precise
        do { library = try device.makeLibrary(source: shaderSource, options: options) } catch { fail("shaders: \(error)") }
    }

    func compute(_ name: String) -> any MTLComputePipelineState {
        guard let f = library.makeFunction(name: name) else { fail("no function \(name)") }
        do { return try device.makeComputePipelineState(function: f) } catch { fail("\(name): \(error)") }
    }

    func texture(_ format: MTLPixelFormat, _ w: Int, _ h: Int, _ usage: MTLTextureUsage, shared: Bool) -> any MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: w, height: h, mipmapped: false)
        d.usage = usage
        d.storageMode = shared ? .shared : .private
        guard let t = device.makeTexture(descriptor: d) else { fail("cannot allocate \(w)x\(h) \(format) texture") }
        return t
    }
}

// MARK: - Reprojection

struct Params {
    var eyeUnits: Float = 2.24
    var tear: Float = 0.08
    var refine: Int = 2
    var ss: Int = 1
    var fillSkip: Float = 2
    /// Output pixels a torn triangle must open (or fold) by to count as torn.
    var tearPx: Float = 1
    var skirt = true
    /// Disocclusion fill: false stretches the background edge across the gap, true mirrors the background into it.
    var mirror = false
    /// Source filter: 0 bilinear, 1 Catmull-Rom, 2 Lanczos-3 (all depth-aware).
    var filter = 1
    /// Depth-buffer curve constant: 1 / (distance in game units where the curve is at half range).
    var zc: Float = 1.0 / 700.0
    /// Upper bound on one band's mesh render target, bytes.
    var bandBytes = 192 << 20
}

/// A centre view on the GPU: sRGB colour and inverse radial distance.
final class CentreView {
    let width: Int, height: Int
    let colour: any MTLTexture
    let invr: any MTLTexture
    /// Smallest valid distance in each source row, for bounding how far a row can move vertically.
    let rowMinDist: [Float]
    /// (sin, cos) of each mesh vertex column's azimuth (outermost on the image edge) and row's elevation.
    let colTable: any MTLBuffer, rowTable: any MTLBuffer

    init(gpu: GPU, image: RGBA8, dist: (width: Int, height: Int, values: [Float])) {
        guard image.width == dist.width, image.height == dist.height else {
            fail("colour is \(image.width)x\(image.height) but distance is \(dist.width)x\(dist.height)")
        }
        let width = image.width, height = image.height
        self.width = width; self.height = height
        func table(_ n: Int, edges: Bool, _ angle: (Double) -> Double) -> any MTLBuffer {
            var values = [SIMD2<Float>]()
            for i in 0..<n {
                let x = edges && i == 0 ? 0 : (edges && i == n - 1 ? Double(n) : Double(i) + 0.5)
                let a = angle(x / Double(n))
                values.append(SIMD2(Float(sin(a)), Float(cos(a))))
            }
            return gpu.device.makeBuffer(bytes: values, length: n * 8, options: .storageModeShared)!
        }
        colTable = table(width, edges: true) { Double.pi * ($0 - 0.5) }
        rowTable = table(height, edges: false) { Double.pi * (0.5 - $0) }
        let colour = gpu.texture(.rgba8Unorm_srgb, width, height, .shaderRead, shared: true)
        image.pixels.withUnsafeBytes {
            colour.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: width * 4)
        }
        self.colour = colour
        var inv = [Float](repeating: 0, count: width * height)
        var mins = [Float](repeating: .infinity, count: height)
        let w = width
        inv.withUnsafeMutableBufferPointer { out in
            mins.withUnsafeMutableBufferPointer { m in
                dist.values.withUnsafeBufferPointer { d in
                    DispatchQueue.concurrentPerform(iterations: height) { y in
                        var lo = Float.infinity
                        for x in 0..<w {
                            let v = d[y * w + x]
                            // Non-finite, non-positive or NaN: no surface, as far as the sky.
                            if v.isFinite, v > 0 {
                                let r = max(v, 1e-3)
                                out[y * w + x] = 1 / r
                                lo = min(lo, r)
                            } else {
                                out[y * w + x] = 0
                            }
                        }
                        m[y] = lo
                    }
                }
            }
        }
        rowMinDist = mins
        let invr = gpu.texture(.r32Float, width, height, .shaderRead, shared: true)
        self.invr = invr
        inv.withUnsafeBytes {
            invr.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: width * 4)
        }
    }
}

struct EyeResult {
    var image: RGBA8
    var holeFraction: Double      // samples nothing landed on (backplate)
    var fillFraction: Double      // samples showing stretched background (torn triangles)
    var gpuSeconds: Double
    var dump: [SIMD4<Float>]?     // per pixel: (source u, v, flag, inverse distance) of the first sample
}

struct MeshU {
    var W: UInt32, H: UInt32, rowStart: UInt32, bandStart: UInt32
    var bandRows: UInt32, outW: UInt32, outH: UInt32, skirt: UInt32
    var eyeX: Float, tear: Float, fillSkip: Float, zc: Float
    var tearPx: Float, mirror: Float, pad1: Float = 0, pad2: Float = 0
}

struct ResolveU {
    var W: UInt32, H: UInt32, outW: UInt32, outH: UInt32
    var ss: UInt32, bandStart: UInt32, bandRows: UInt32, refine: UInt32
    var eyeX: Float, tear: Float, zc: Float, pad0: Float = 0
    var dumpUV: UInt32, cubic: UInt32, pad1: UInt32 = 0, pad2: UInt32 = 0
}

func reprojectEye(gpu: GPU, view: CentreView, eyeX: Float, params p: Params, dump: Bool) -> EyeResult {
    let W = view.width, H = view.height, ss = max(1, p.ss)
    let outW = W * ss, outH = H * ss
    var bandRows = max(ss, min(outH, p.bandBytes / (outW * 16)) / ss * ss)
    bandRows = min(bandRows, outH)

    let meshTarget = gpu.texture(.rgba32Float, outW, bandRows, [.renderTarget, .shaderRead], shared: false)
    let depthTarget = gpu.texture(.depth32Float, outW, bandRows, .renderTarget, shared: false)
    let output = gpu.texture(.rgba8Unorm, W, H, [.shaderWrite, .shaderRead], shared: true)
    let stats = gpu.device.makeBuffer(length: 16, options: .storageModeShared)!
    memset(stats.contents(), 0, 16)
    let dumpBuffer = gpu.device.makeBuffer(length: dump ? W * H * 16 : 16, options: .storageModeShared)!

    // How far (radians) each source row can move as seen from the eye: a point at distance r moves by at most
    // asin(e / r) when the viewpoint moves by e.
    let e = abs(eyeX)
    let shift = view.rowMinDist.map { r -> Double in r > e * 1.0001 ? asin(Double(e / r)) : Double.pi }
    let pixelAngle = Double.pi / Double(H)
    func elCentre(_ row: Int) -> Double { Double.pi * (0.5 - (Double(row) + 0.5) / Double(H)) }

    var gpuSeconds = 0.0
    var bandStart = 0
    while bandStart < outH {
        let rows = min(bandRows, outH - bandStart)
        let elHi = Double.pi * (0.5 - Double(bandStart) / Double(outH))
        let elLo = Double.pi * (0.5 - Double(bandStart + rows) / Double(outH))
        var firstCell = -1, lastCell = -1
        for i in 0..<(H - 1) {
            let b = max(shift[i], shift[i + 1]) + 2 * pixelAngle
            if elCentre(i + 1) - b <= elHi && elCentre(i) + b >= elLo {
                if firstCell < 0 { firstCell = i }
                lastCell = i
            }
        }

        let commands = gpu.queue.makeCommandBuffer()!
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = meshTarget
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depthTarget
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 0
        pass.depthAttachment.storeAction = .dontCare
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(gpu.meshPipeline)
        encoder.setDepthStencilState(gpu.depthState)
        encoder.setCullMode(.none)
        encoder.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(outW), height: Double(rows), znear: 0, zfar: 1))
        encoder.setVertexTexture(view.invr, index: 0)
        encoder.setVertexBuffer(view.colTable, offset: 0, index: 1)
        encoder.setVertexBuffer(view.rowTable, offset: 0, index: 2)
        if firstCell >= 0 {
            let chunk = 256
            var row = firstCell
            while row <= lastCell {
                let n = min(chunk, lastCell + 1 - row)
                var u = MeshU(W: UInt32(W), H: UInt32(H), rowStart: UInt32(row), bandStart: UInt32(bandStart), bandRows: UInt32(rows),
                              outW: UInt32(outW), outH: UInt32(outH), skirt: p.skirt ? 1 : 0, eyeX: eyeX, tear: p.tear,
                              fillSkip: p.fillSkip, zc: p.zc, tearPx: p.tearPx, mirror: p.mirror ? 1 : 0)
                encoder.setVertexBytes(&u, length: MemoryLayout<MeshU>.stride, index: 0)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: (W - 1) * n * 18)
                row += n
            }
        }
        encoder.endEncoding()

        let compute = commands.makeComputeCommandEncoder()!
        compute.setComputePipelineState(gpu.resolvePipeline)
        var r = ResolveU(W: UInt32(W), H: UInt32(H), outW: UInt32(outW), outH: UInt32(outH), ss: UInt32(ss), bandStart: UInt32(bandStart),
                         bandRows: UInt32(rows), refine: UInt32(p.refine), eyeX: eyeX, tear: p.tear, zc: p.zc,
                         dumpUV: dump ? 1 : 0, cubic: UInt32(p.filter))
        compute.setBytes(&r, length: MemoryLayout<ResolveU>.stride, index: 0)
        compute.setBuffer(stats, offset: 0, index: 1)
        compute.setBuffer(dumpBuffer, offset: 0, index: 2)
        compute.setTexture(meshTarget, index: 0)
        compute.setTexture(view.colour, index: 1)
        compute.setTexture(view.invr, index: 2)
        compute.setTexture(output, index: 3)
        compute.dispatchThreads(MTLSize(width: W, height: rows / ss, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 8, depth: 1))
        compute.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        if let error = commands.error { fail("GPU: \(error)") }
        gpuSeconds += commands.gpuEndTime - commands.gpuStartTime
        bandStart += rows
    }

    var pixels = [UInt8](repeating: 0, count: W * H * 4)
    output.getBytes(&pixels, bytesPerRow: W * 4, from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0)
    let counts = stats.contents().bindMemory(to: UInt32.self, capacity: 2)
    let samples = Double(outW) * Double(outH)
    var dumpArray: [SIMD4<Float>]?
    if dump {
        let ptr = dumpBuffer.contents().bindMemory(to: SIMD4<Float>.self, capacity: W * H)
        dumpArray = Array(UnsafeBufferPointer(start: ptr, count: W * H))
    }
    return EyeResult(image: RGBA8(width: W, height: H, pixels: pixels), holeFraction: Double(counts[0]) / samples,
                     fillFraction: Double(counts[1]) / samples, gpuSeconds: gpuSeconds, dump: dumpArray)
}

// MARK: - One block

struct BlockReport {
    var left: EyeResult, right: EyeResult
}

/// Reads eqr-B-C.png and dist-B-C.f32 from inDir, writes reproj-B-L.png and reproj-B-R.png to outDir.
@discardableResult
func processBlock(gpu: GPU, inDir: String, block: String, outDir: String, params: Params, dump: Bool = false, quiet: Bool = false) -> BlockReport {
    let t0 = now()
    let colourPath = (inDir as NSString).appendingPathComponent("eqr-\(block)-C.png")
    let distPath = (inDir as NSString).appendingPathComponent("dist-\(block)-C.f32")
    var image: RGBA8?
    var dist: (width: Int, height: Int, values: [Float])?
    DispatchQueue.concurrentPerform(iterations: 2) { i in
        if i == 0 { image = loadPNG(colourPath) } else { dist = loadDist(distPath) }
    }
    let t1 = now()
    let view = CentreView(gpu: gpu, image: image!, dist: dist!)
    image = nil; dist = nil
    let t2 = now()
    let debugHoles = ProcessInfo.processInfo.environment["REPROJECT_DEBUG"] == "holes"
    let left = reprojectEye(gpu: gpu, view: view, eyeX: -params.eyeUnits, params: params, dump: dump || debugHoles)
    if debugHoles, let d = left.dump {
        // Where nothing landed, by 8 x 8 regions of the left eye.
        var grid = [Int](repeating: 0, count: 64)
        for y in 0..<view.height { for x in 0..<view.width where d[y * view.width + x].z < 0.5 { grid[(y * 8 / view.height) * 8 + x * 8 / view.width] += 1 } }
        for r in 0..<8 { print("  holes " + (0..<8).map { String(format: "%7d", grid[r * 8 + $0]) }.joined()) }
    }
    let right = reprojectEye(gpu: gpu, view: view, eyeX: params.eyeUnits, params: params, dump: dump)
    let t3 = now()
    DispatchQueue.concurrentPerform(iterations: 2) { i in
        let eye = i == 0 ? left : right
        writePNG(eye.image, (outDir as NSString).appendingPathComponent("reproj-\(block)-\(i == 0 ? "L" : "R").png"))
    }
    let t4 = now()
    if !quiet {
        print(String(format: "block %@: %dx%d, read %.2f s, prepare %.2f s, reproject %.2f s (GPU %.2f + %.2f s), write %.2f s, total %.2f s",
                     block, view.width, view.height, t1 - t0, t2 - t1, t3 - t2, left.gpuSeconds, right.gpuSeconds, t4 - t3, t4 - t0))
        print(String(format: "  holes (backplate) L %.4f%% R %.4f%%; stretched background (fill) L %.3f%% R %.3f%%",
                     100 * left.holeFraction, 100 * right.holeFraction, 100 * left.fillFraction, 100 * right.fillFraction))
    }
    return BlockReport(left: left, right: right)
}

/// Every block B with both eqr-B-C.png and dist-B-C.f32 in dir.
func blocks(in dir: String) -> [String] {
    let names = (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
    let set = Set(names)
    return names.compactMap { name -> String? in
        guard name.hasPrefix("eqr-"), name.hasSuffix("-C.png") else { return nil }
        let block = String(name.dropFirst(4).dropLast(6))
        return set.contains("dist-\(block)-C.f32") && !block.isEmpty ? block : nil
    }.sorted { $0.localizedStandardCompare($1) == .orderedAscending }
}
