// Runs the immersive screen's own Metal shaders on the Mac, on a capture from the Mac build (LB_SHOT writes
// shot-N.bmp and depth-N.f32): the shader source is read out of apple/Sources/App/ImmersiveScreen.swift, compiled
// for this machine's GPU and drawn once per eye into an offscreen picture, as tools/immersive_preview.py does in
// numpy. It checks that the shaders compile and shows what the real rasterizer makes of the mesh; it also times
// the GPU at a per-eye size closer to the headset's.
//
// Build and run from the repository root:
//   swiftc -O tools/immersive_shader_check.swift -o /tmp/immersive_shader_check
//   /tmp/immersive_shader_check shot-N.bmp depth-N.f32 out_prefix [--head] [--refine 2] [--ipd-scale 1] [--viewer-x 0]
//       [--scale 0.15] [--head-scale 1] [--tear 0.08] [--time-size 1888x1792] [--tangent 0.849]
//       [--exit 0.25] [--grid 161x91] [--seed] [--compare-seed] [--runs 30] [--warmup 1] [--stage-time]
// Writes out_prefix-sbs.png (left | right, 1280x720 each) and prints the share of pixels the mesh left undrawn.
// --tangent: the game camera's right frustum tangent (tan of the horizontal half-angle), as camera[] carries it;
// 0.849 is the default 65-degree 4:3 field of view at 16:9.
// The compositor cost switches (docs/perf/IMMERSIVE_OPTIONS.md): --exit is LB_IMMERSIVE_REFINE_EXIT, --grid is
// LB_IMMERSIVE_GRID (default one vertex every 4 game pixels), --seed is LB_IMMERSIVE_SEED (with --refine above 0:
// the seed pass once, then the indexed mesh; the seed pass is timed on its own). --compare-seed draws both ways and
// exits 1 unless the two pictures are identical to the bit. --runs and --warmup: timed draws, and untimed ones
// before them (for steady numbers try --runs 300 --warmup 100). --stage-time also times the vertex and fragment
// stages of each eye's pass apart, with timestamps at the stage boundaries.
// Like the layer, it draws into bgra8Unorm_srgb with the game image read as sRGB, and stores the depth attachment.
import AppKit
import Foundation
import Metal
import simd

// Must match QuadUniforms in ImmersiveScreen.swift.
struct QuadUniforms {
    var viewProjection: simd_float4x4
    var model: simd_float4x4
    var heldModel: simd_float4x4
    var halfSize: SIMD2<Float>
    var screenCentre: SIMD2<Float>
    var decode: UInt32
    var hasDepth: UInt32
    var grid: SIMD2<UInt32>
    var eyeBack: Float
    var nearUnits: Float
    var unitsPerMeter: Float
    var depthScale: Float
    var maxBehind: Float
    var backplate: Float
    var tear: Float
    var trueDepth: Float
    var sharpen: Float
    var eyeRefine: SIMD4<Float>
    var heldWithin: Float
    var refineExit: Float
}

func fail(_ text: String) -> Never {
    FileHandle.standardError.write((text + "\n").data(using: .utf8)!)
    exit(1)
}

var positional: [String] = []
var options: [String: String] = [:]
var arguments = Array(CommandLine.arguments.dropFirst())
while !arguments.isEmpty {
    let argument = arguments.removeFirst()
    if ["--head", "--seed", "--compare-seed", "--stage-time"].contains(argument) {
        options[String(argument.dropFirst(2))] = "1"
    } else if argument.hasPrefix("--") {
        guard !arguments.isEmpty else { fail("\(argument) needs a value") }
        options[String(argument.dropFirst(2))] = arguments.removeFirst()
    } else {
        positional.append(argument)
    }
}
guard positional.count == 3 else { fail("usage: immersive_shader_check shot.bmp depth.f32 out_prefix [options]") }
func number(_ name: String, _ fallback: Float) -> Float { options[name].flatMap { Float($0) } ?? fallback }
let head = options["head"] != nil
let refine = number("refine", 0), separationScale = number("ipd-scale", 1), viewerX = number("viewer-x", 0)
let source = options["source"] ?? "apple/Sources/App/ImmersiveScreen.swift"

guard let swift = try? String(contentsOfFile: source, encoding: .utf8),
      let open = swift.range(of: "static let shaders = \"\"\"\n"),
      let close = swift.range(of: "\n    \"\"\"", range: open.upperBound..<swift.endIndex) else { fail("no shader source in \(source)") }
let shaders = String(swift[open.upperBound..<close.lowerBound])

guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("no Metal device") }
let library: any MTLLibrary
do { library = try device.makeLibrary(source: shaders, options: nil) } catch { fail("shaders do not compile: \(error)") }

// The capture: colour as BGRA, depth as one float per pixel after a width and height header.
guard let bitmap = NSImage(contentsOfFile: positional[0])?.cgImage(forProposedRect: nil, context: nil, hints: nil) else { fail("cannot read \(positional[0])") }
guard let depthFile = FileManager.default.contents(atPath: positional[1]), depthFile.count > 8 else { fail("cannot read \(positional[1])") }
let width = Int(depthFile.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) })
let height = Int(depthFile.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self) })
guard depthFile.count >= 8 + width * height * 4 else { fail("depth file is shorter than its header says") }

var pixels = [UInt8](repeating: 0, count: width * height * 4)
let context = CGContext(data: &pixels, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
context.draw(bitmap, in: CGRect(x: 0, y: 0, width: width, height: height))
let imageDescription = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
imageDescription.pixelFormat = .bgra8Unorm_srgb
let image = device.makeTexture(descriptor: imageDescription)!
image.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: pixels, bytesPerRow: width * 4)
let depthDescription = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: width, height: height, mipmapped: false)
let gameDepth = device.makeTexture(descriptor: depthDescription)!
depthFile.withUnsafeBytes {
    gameDepth.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: $0.baseAddress! + 8, bytesPerRow: width * 4)
}

let pipelineDescription = MTLRenderPipelineDescriptor()
pipelineDescription.vertexFunction = library.makeFunction(name: "screen_vertex")
pipelineDescription.fragmentFunction = library.makeFunction(name: "screen_fragment")
pipelineDescription.colorAttachments[0].pixelFormat = .bgra8Unorm_srgb
pipelineDescription.depthAttachmentPixelFormat = .depth32Float
let pipeline: any MTLRenderPipelineState
do { pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescription) } catch { fail("no pipeline: \(error)") }
// LB_IMMERSIVE_SEED's two pipelines: the seed pass and the indexed mesh.
let seedDescription = MTLRenderPipelineDescriptor()
seedDescription.vertexFunction = library.makeFunction(name: "seed_vertex")
seedDescription.fragmentFunction = library.makeFunction(name: "seed_fragment")
seedDescription.colorAttachments[0].pixelFormat = .r32Float
let seededDescription = MTLRenderPipelineDescriptor()
seededDescription.vertexFunction = library.makeFunction(name: "screen_vertex_seeded")
seededDescription.fragmentFunction = library.makeFunction(name: "screen_fragment")
seededDescription.colorAttachments[0].pixelFormat = .bgra8Unorm_srgb
seededDescription.depthAttachmentPixelFormat = .depth32Float
let seedPipeline: any MTLRenderPipelineState, seededPipeline: any MTLRenderPipelineState
do {
    seedPipeline = try device.makeRenderPipelineState(descriptor: seedDescription)
    seededPipeline = try device.makeRenderPipelineState(descriptor: seededDescription)
} catch { fail("no seed pipeline: \(error)") }
let stencilDescription = MTLDepthStencilDescriptor()
stencilDescription.depthCompareFunction = .greater
stencilDescription.isDepthWriteEnabled = true
let stencil = device.makeDepthStencilState(descriptor: stencilDescription)!

// The same geometry as the renderer: a 2.8 m screen 3 m away, or (--head) the frame at its own angular size on a
// plane 2 m from the rays' origin. The camera looks straight at it from each eye.
let aspect = Float(width) / Float(height)
let tangent = number("tangent", 0.849)
let frustum = SIMD2<Float>(tangent, tangent / aspect)
let eyeBack: Float = head ? 2.0 : 3.0
let halfSize = head ? eyeBack * frustum : SIMD2<Float>(1.4, 1.4 / aspect)
let cameraTangents = head ? frustum : tan(35 * Float.pi / 180) * SIMD2<Float>(1, 1 / aspect)
let gridCells: UInt32 = 4
let gridOption = options["grid"].map { $0.split(separator: "x").compactMap { UInt32($0) } }
if let gridOption, gridOption.count != 2 || gridOption.contains(where: { $0 < 2 }) { fail("--grid takes COLUMNSxROWS") }
let grid = gridOption.map { SIMD2($0[0], $0[1]) } ?? SIMD2(UInt32(width) / gridCells + 1, UInt32(height) / gridCells + 1)
let refineExit = refine > 0 ? number("exit", 0) : 0
let useSeed = options["seed"] != nil && refine > 0, compareSeed = options["compare-seed"] != nil
if (options["seed"] != nil || compareSeed) && refine <= 0 { fail("--seed and --compare-seed need --refine above 0 (as in the app)") }

func uniforms(side: Float) -> QuadUniforms {
    let eye = SIMD3<Float>(viewerX + side * 0.032 * separationScale, 0, eyeBack)
    var view = matrix_identity_float4x4
    view.columns.3 = SIMD4(-eye, 1)
    // Right-up-back, reversed depth with the far plane at infinity (the layer's convention).
    let projection = simd_float4x4(SIMD4(1 / cameraTangents.x, 0, 0, 0), SIMD4(0, 1 / cameraTangents.y, 0, 0),
                                   SIMD4(0, 0, 0, -1), SIMD4(0, 0, 0.05, 0))
    return QuadUniforms(viewProjection: projection * view, model: matrix_identity_float4x4, heldModel: matrix_identity_float4x4,
                        halfSize: halfSize, screenCentre: SIMD2(0, 0), decode: 0, hasDepth: 1,
                        grid: grid, eyeBack: eyeBack,
                        nearUnits: number("near", 15), unitsPerMeter: number("units", 70),
                        depthScale: head ? number("head-scale", 1) : number("scale", 0.15),
                        maxBehind: head ? 300 : number("max-behind", 30), backplate: 0, tear: number("tear", 0.08),
                        trueDepth: head ? 1 : 0, sharpen: 0, eyeRefine: SIMD4(eye, refine), heldWithin: 0, refineExit: refineExit)
}

// The grid's triangles in screen_vertex's order, for the indexed mesh.
let gridIndices: [UInt32] = {
    let corners = [(0, 0), (1, 0), (0, 1), (1, 0), (1, 1), (0, 1)]
    var indices: [UInt32] = []
    for y in 0..<Int(grid.y - 1) {
        for x in 0..<Int(grid.x - 1) {
            for corner in corners { indices.append(UInt32((y + corner.1) * Int(grid.x) + x + corner.0)) }
        }
    }
    return indices
}()
let shortIndices = Int(grid.x * grid.y) < 0xFFFF
let indexBuffer: any MTLBuffer = shortIndices
    ? gridIndices.map { UInt16($0) }.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count)! }
    : gridIndices.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count)! }

/// The seed pass: the depth each grid vertex needs, as seed_fragment works it out. Returns it and its GPU time.
func makeSeed() -> (texture: any MTLTexture, gpuSeconds: Double) {
    let description = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: Int(grid.x), height: Int(grid.y), mipmapped: false)
    description.usage = [.renderTarget, .shaderRead]
    description.storageMode = .private
    let seed = device.makeTexture(descriptor: description)!
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = seed
    pass.colorAttachments[0].loadAction = .dontCare
    pass.colorAttachments[0].storeAction = .store
    let commands = queue.makeCommandBuffer()!
    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
    var size = grid
    encoder.setRenderPipelineState(seedPipeline)
    encoder.setFragmentBytes(&size, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 0)
    encoder.setFragmentTexture(gameDepth, index: 1)
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding()
    commands.commit()
    commands.waitUntilCompleted()
    return (seed, commands.gpuEndTime - commands.gpuStartTime)
}
let seed = useSeed || compareSeed ? makeSeed() : nil

// --stage-time: timestamps at the stage boundaries of each eye's pass (four a pass).
let stageTime = options["stage-time"] != nil
let stageBuffer: (any MTLCounterSampleBuffer)? = {
    guard stageTime else { return nil }
    guard device.supportsCounterSampling(.atStageBoundary),
          let set = device.counterSets?.first(where: { $0.name == MTLCommonCounterSet.timestamp.rawValue }) else {
        fail("--stage-time: this GPU takes no timestamps at stage boundaries")
    }
    let description = MTLCounterSampleBufferDescriptor()
    description.counterSet = set
    description.storageMode = .shared
    description.sampleCount = 8
    do { return try device.makeCounterSampleBuffer(descriptor: description) } catch { fail("--stage-time: \(error)") }
}()
/// The vertex and fragment stage ticks of the last draw, summed over both eyes; nil if unreadable.
func stageTicks() -> (vertex: Double, fragment: Double)? {
    guard let stageBuffer, let data = try? stageBuffer.resolveCounterRange(0..<8) else { return nil }
    return data.withUnsafeBytes { raw -> (vertex: Double, fragment: Double)? in
        let t = raw.bindMemory(to: MTLCounterResultTimestamp.self).map { $0.timestamp }
        guard t.count >= 8, !t.contains(where: { $0 == 0 || $0 == UInt64.max }) else { return nil }
        var vertex = 0.0, fragment = 0.0
        for pass in 0..<2 {
            guard t[pass * 4 + 1] >= t[pass * 4], t[pass * 4 + 3] >= t[pass * 4 + 2] else { return nil }
            vertex += Double(t[pass * 4 + 1] - t[pass * 4]); fragment += Double(t[pass * 4 + 3] - t[pass * 4 + 2])
        }
        return (vertex, fragment)
    }
}

func target(_ format: MTLPixelFormat, _ w: Int, _ h: Int) -> any MTLTexture {
    let description = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: w, height: h, mipmapped: false)
    description.usage = [.renderTarget, .shaderRead]
    description.storageMode = format == .depth32Float ? .private : .shared
    return device.makeTexture(descriptor: description)!
}

/// Draws both eyes into textures of the given size and returns them with the GPU time of the command buffer.
/// seeded: the mesh from the seed, indexed (LB_IMMERSIVE_SEED).
func draw(_ w: Int, _ h: Int, seeded: Bool = useSeed) -> (eyes: [any MTLTexture], gpuSeconds: Double) {
    let commands = queue.makeCommandBuffer()!
    var eyes: [any MTLTexture] = []
    for side: Float in [-1, 1] {
        let colour = target(.bgra8Unorm_srgb, w, h)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 1, alpha: 0)   // alpha 0: nothing drawn
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = target(.depth32Float, w, h)
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 0
        pass.depthAttachment.storeAction = .store
        if let stageBuffer, let attachment = pass.sampleBufferAttachments[0] {
            let first = side < 0 ? 0 : 4
            attachment.sampleBuffer = stageBuffer
            attachment.startOfVertexSampleIndex = first
            attachment.endOfVertexSampleIndex = first + 1
            attachment.startOfFragmentSampleIndex = first + 2
            attachment.endOfFragmentSampleIndex = first + 3
        }
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        var values = uniforms(side: side)
        encoder.setRenderPipelineState(pipeline)
        encoder.setDepthStencilState(stencil)
        encoder.setCullMode(.none)
        encoder.setVertexBytes(&values, length: MemoryLayout<QuadUniforms>.stride, index: 0)
        encoder.setFragmentBytes(&values, length: MemoryLayout<QuadUniforms>.stride, index: 0)
        encoder.setFragmentTexture(image, index: 0)
        encoder.setVertexTexture(gameDepth, index: 1)
        encoder.setFragmentTexture(gameDepth, index: 1)
        if seeded, let seed {
            encoder.setRenderPipelineState(seededPipeline)
            encoder.setVertexTexture(seed.texture, index: 1)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: gridIndices.count, indexType: shortIndices ? .uint16 : .uint32,
                                          indexBuffer: indexBuffer, indexBufferOffset: 0)
        } else {
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: Int((values.grid.x - 1) * (values.grid.y - 1) * 6))
        }
        encoder.endEncoding()
        eyes.append(colour)
    }
    commands.commit()
    commands.waitUntilCompleted()
    return (eyes, commands.gpuEndTime - commands.gpuStartTime)
}

let outWidth = 1280, outHeight = 720
let result = draw(outWidth, outHeight)
var sideBySide = [UInt8](repeating: 0, count: outWidth * 2 * outHeight * 4)
var undrawn = 0
for (index, eye) in result.eyes.enumerated() {
    var one = [UInt8](repeating: 0, count: outWidth * outHeight * 4)
    eye.getBytes(&one, bytesPerRow: outWidth * 4, from: MTLRegionMake2D(0, 0, outWidth, outHeight), mipmapLevel: 0)
    for y in 0..<outHeight {
        // Undrawn pixels with drawn ones on both sides of them in their row are gaps in the mesh.
        var first = -1, last = -1
        for x in 0..<outWidth where one[(y * outWidth + x) * 4 + 3] != 0 { if first < 0 { first = x }; last = x }
        for x in 0..<outWidth {
            let from = (y * outWidth + x) * 4, to = (y * outWidth * 2 + index * outWidth + x) * 4
            if one[from + 3] == 0, x > first, x < last { undrawn += 1 }
            sideBySide[to] = one[from]; sideBySide[to + 1] = one[from + 1]; sideBySide[to + 2] = one[from + 2]; sideBySide[to + 3] = 255
        }
    }
}
let outContext = CGContext(data: &sideBySide, width: outWidth * 2, height: outHeight, bitsPerComponent: 8, bytesPerRow: outWidth * 2 * 4,
                           space: CGColorSpace(name: CGColorSpace.sRGB)!,
                           bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
let png = NSBitmapImageRep(cgImage: outContext.makeImage()!).representation(using: .png, properties: [:])!
try png.write(to: URL(fileURLWithPath: positional[2] + "-sbs.png"))

// --compare-seed: the seeded, indexed mesh against the unindexed one. The arithmetic is the same, but the Metal
// compiler's fast math may round it differently in the two vertex functions, moving a vertex by a fraction of an ulp
// and so the coverage of a few edge pixels. That is fine; a fault (a seed from the wrong texel, a cell the index
// buffer leaves out) shows as many pixels, large differences, or a gap the unindexed mesh doesn't have. So: count
// every differing pixel, how far apart they are (the largest colour difference), how many differ by more than 8/255,
// and the coverage changes each way, and write <out>-seeddiff.png (differing pixels white, both eyes side by side).
// The unindexed mesh shades each shared vertex once per triangle, so it can crack where the indexed mesh can't:
// "drawn only seeded" is not a failure. Exit 1 on more than 0.01% of pixels over 8/255, or any gap only seeded has.
var seedDifferent = -1, seedLarge = 0, seedMax = 0, seedOnlyPlain = 0, seedOnlySeeded = 0
if compareSeed {
    let plain = draw(outWidth, outHeight, seeded: false).eyes, seeded = draw(outWidth, outHeight, seeded: true).eyes
    seedDifferent = 0
    var mask = [UInt8](repeating: 0, count: outWidth * 2 * outHeight * 4)
    for (eyeIndex, (a, b)) in zip(plain, seeded).enumerated() {
        var one = [UInt8](repeating: 0, count: outWidth * outHeight * 4), two = one
        a.getBytes(&one, bytesPerRow: outWidth * 4, from: MTLRegionMake2D(0, 0, outWidth, outHeight), mipmapLevel: 0)
        b.getBytes(&two, bytesPerRow: outWidth * 4, from: MTLRegionMake2D(0, 0, outWidth, outHeight), mipmapLevel: 0)
        for index in 0..<(outWidth * outHeight) where one[index * 4..<index * 4 + 4] != two[index * 4..<index * 4 + 4] {
            seedDifferent += 1
            var largest = 0
            for channel in 0..<3 { largest = max(largest, abs(Int(one[index * 4 + channel]) - Int(two[index * 4 + channel]))) }
            seedMax = max(seedMax, largest)
            if largest > 8 { seedLarge += 1 }
            let drawnPlain = one[index * 4 + 3] != 0, drawnSeeded = two[index * 4 + 3] != 0
            if drawnPlain && !drawnSeeded { seedOnlyPlain += 1 }
            if drawnSeeded && !drawnPlain { seedOnlySeeded += 1 }
            let (y, x) = (index / outWidth, index % outWidth), to = (y * outWidth * 2 + eyeIndex * outWidth + x) * 4
            mask[to] = 255; mask[to + 1] = 255; mask[to + 2] = 255
        }
    }
    for index in 0..<(outWidth * 2 * outHeight) { mask[index * 4 + 3] = 255 }
    let maskContext = CGContext(data: &mask, width: outWidth * 2, height: outHeight, bitsPerComponent: 8, bytesPerRow: outWidth * 2 * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    let maskPNG = NSBitmapImageRep(cgImage: maskContext.makeImage()!).representation(using: .png, properties: [:])!
    try maskPNG.write(to: URL(fileURLWithPath: positional[2] + "-seeddiff.png"))
}

let size = (options["time-size"] ?? "1888x1792").split(separator: "x").compactMap { Int($0) }
guard size.count == 2 else { fail("--time-size takes WIDTHxHEIGHT") }
let runs = max(1, Int(number("runs", 30))), warmup = max(0, Int(number("warmup", 1)))
for _ in 0..<warmup { _ = draw(size[0], size[1]) }
let clockStart = (gpu: stageTime ? device.sampleTimestamps().gpu : 0, wall: ProcessInfo.processInfo.systemUptime)
var stages: [(vertex: Double, fragment: Double)] = []
let times = (0..<runs).map { _ -> Double in
    let seconds = draw(size[0], size[1]).gpuSeconds * 1000
    if let ticks = stageTicks() { stages.append(ticks) }
    return seconds
}.sorted()
print(String(format: "%@, refine %.0f: gaps in the mesh %.2f%% of the picture; GPU for both eyes at %dx%d on %@: median %.2f ms (%.2f-%.2f)",
             head ? "head" : "window", refine, 100 * Double(undrawn) / Double(outWidth * outHeight * 2), size[0], size[1],
             device.name, times[times.count / 2], times.first!, times.last!))
print(String(format: "grid %dx%d, exit %.2f, %@, %d timed draws after %d", grid.x, grid.y, refineExit,
             useSeed ? "seeded indexed mesh" : "unindexed mesh", runs, warmup))
if let seed { print(String(format: "seed pass %.3f ms (once per game frame)", seed.gpuSeconds * 1000)) }
if stageTime {
    let ticks = Double(device.sampleTimestamps().gpu &- clockStart.gpu)
    let msPerTick = ticks > 0 ? (ProcessInfo.processInfo.systemUptime - clockStart.wall) * 1000 / ticks : 0
    if stages.isEmpty {
        print("stage times: no readable timestamps")
    } else {
        let vertex = stages.map { $0.vertex * msPerTick }.sorted(), fragment = stages.map { $0.fragment * msPerTick }.sorted()
        print(String(format: "stages, both eyes: vertex median %.3f ms (%.3f-%.3f), fragment median %.3f ms (%.3f-%.3f), %d of %d draws read",
                     vertex[vertex.count / 2], vertex.first!, vertex.last!, fragment[fragment.count / 2], fragment.first!, fragment.last!,
                     stages.count, runs))
    }
}
if compareSeed {
    let pixels = outWidth * outHeight * 2
    if seedDifferent == 0 {
        print("seeded indexed mesh against unindexed: identical")
    } else {
        print(String(format: "seeded indexed mesh against unindexed: %d pixels differ (%.4f%%), largest colour difference %d/255, "
                     + "%d over 8/255, drawn only unindexed %d, drawn only seeded %d; mask in %@-seeddiff.png",
                     seedDifferent, 100 * Double(seedDifferent) / Double(pixels), seedMax, seedLarge, seedOnlyPlain,
                     seedOnlySeeded, positional[2]))
    }
    if Double(seedLarge) > 0.0001 * Double(pixels) || seedOnlyPlain > 0 { exit(1) }
}
