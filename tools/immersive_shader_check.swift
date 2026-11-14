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
// Writes out_prefix-sbs.png (left | right, 1280x720 each) and prints the share of pixels the mesh left undrawn.
// --tangent: the game camera's right frustum tangent (tan of the horizontal half-angle), as camera[] carries it;
// 0.849 is the default 65-degree 4:3 field of view at 16:9.
import AppKit
import Foundation
import Metal
import simd

// Must match QuadUniforms in ImmersiveScreen.swift.
struct QuadUniforms {
    var viewProjection: simd_float4x4
    var model: simd_float4x4
    var halfSize: SIMD2<Float>
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
    var eyeRefine: SIMD4<Float>
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
    if argument == "--head" {
        options["head"] = "1"
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
pipelineDescription.colorAttachments[0].pixelFormat = .bgra8Unorm
pipelineDescription.depthAttachmentPixelFormat = .depth32Float
let pipeline: any MTLRenderPipelineState
do { pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescription) } catch { fail("no pipeline: \(error)") }
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

func uniforms(side: Float) -> QuadUniforms {
    let eye = SIMD3<Float>(viewerX + side * 0.032 * separationScale, 0, eyeBack)
    var view = matrix_identity_float4x4
    view.columns.3 = SIMD4(-eye, 1)
    // Right-up-back, reversed depth with the far plane at infinity (the layer's convention).
    let projection = simd_float4x4(SIMD4(1 / cameraTangents.x, 0, 0, 0), SIMD4(0, 1 / cameraTangents.y, 0, 0),
                                   SIMD4(0, 0, 0, -1), SIMD4(0, 0, 0.05, 0))
    return QuadUniforms(viewProjection: projection * view, model: matrix_identity_float4x4, halfSize: halfSize, decode: 0, hasDepth: 1,
                        grid: SIMD2(UInt32(width) / gridCells + 1, UInt32(height) / gridCells + 1), eyeBack: eyeBack,
                        nearUnits: number("near", 15), unitsPerMeter: number("units", 70),
                        depthScale: head ? number("head-scale", 1) : number("scale", 0.15),
                        maxBehind: head ? 300 : number("max-behind", 30), backplate: 0, tear: number("tear", 0.08),
                        trueDepth: head ? 1 : 0, eyeRefine: SIMD4(eye, refine))
}

func target(_ format: MTLPixelFormat, _ w: Int, _ h: Int) -> any MTLTexture {
    let description = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: w, height: h, mipmapped: false)
    description.usage = [.renderTarget, .shaderRead]
    description.storageMode = format == .depth32Float ? .private : .shared
    return device.makeTexture(descriptor: description)!
}

/// Draws both eyes into textures of the given size and returns them with the GPU time of the command buffer.
func draw(_ w: Int, _ h: Int) -> (eyes: [any MTLTexture], gpuSeconds: Double) {
    let commands = queue.makeCommandBuffer()!
    var eyes: [any MTLTexture] = []
    for side: Float in [-1, 1] {
        let colour = target(.bgra8Unorm, w, h)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 1, alpha: 0)   // alpha 0: nothing drawn
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = target(.depth32Float, w, h)
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 0
        pass.depthAttachment.storeAction = .dontCare
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
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: Int((values.grid.x - 1) * (values.grid.y - 1) * 6))
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

let size = (options["time-size"] ?? "1888x1792").split(separator: "x").compactMap { Int($0) }
guard size.count == 2 else { fail("--time-size takes WIDTHxHEIGHT") }
_ = draw(size[0], size[1])
let times = (0..<30).map { _ in draw(size[0], size[1]).gpuSeconds * 1000 }.sorted()
print(String(format: "%@, refine %.0f: gaps in the mesh %.2f%% of the picture; GPU for both eyes at %dx%d on %@: median %.2f ms (%.2f-%.2f)",
             head ? "head" : "window", refine, 100 * Double(undrawn) / Double(outWidth * outHeight * 2), size[0], size[1],
             device.name, times[times.count / 2], times.first!, times.last!))
