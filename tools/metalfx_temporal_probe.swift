// The MetalFX temporal scaler the immersive compositor can use (LB_IMMERSIVE_TEMPORAL, docs/handoff/
// temporal-upscale.md), tried on this Mac's GPU. Written on Linux and never compiled there: expect to fix it.
//
//   xcrun swiftc -O tools/metalfx_temporal_probe.swift -o /tmp/temporal_probe
//   /tmp/temporal_probe apple/Sources/App/ImmersiveScreen.swift
//   /tmp/temporal_probe apple/Sources/App/ImmersiveScreen.swift frames <dump dir> <first> <last> <out dir> [factor] [jx,jy,mx,my]
//
// Without `frames` it answers what could not be answered without a Mac:
//   1. Do the compositor's Metal shaders compile, the motion pass among them?
//   2. Which way round does MetalFX want a frame's shift (jitterOffset) and its motion vectors? A made-up scene
//      (a tiled floor, posts, a wall; fine lines that a small render can't hold) is rendered small with the same
//      shifts the game uses, put through the compositor's own motion pass and the scaler under each choice of
//      signs, and the result compared with the same view rendered at the full size with 16 rays a pixel. The
//      right signs give the highest PSNR, first with the camera still (only the shift matters), then moving.
//   3. What the motion pass and the scaler cost on this GPU.
//   4. Which texture formats the scaler takes (last, in case an unsupported one stops the program).
// With `frames` it runs the scaler over frames the game dumped (LB_SHOT=<first>-<last> LB_CAMLOG=1 with the jitter
// on; shot-<frame>.bmp, depth-<frame>.f32 and log.txt in <dump dir>) and writes temporal-<frame>.bmp for each and
// spatial-<last>.bmp (the MetalFX spatial scaler on the last frame alone) to <out dir>: the real picture, to
// look at and to compare with a frame the game rendered at the full size. Game imagery: keep it out of the
// repository.
import Foundation
import Metal
import MetalFX
import simd

setvbuf(stdout, nil, _IOLBF, 0)
let arguments = CommandLine.arguments
guard arguments.count > 1 else {
    print("usage: temporal_probe <ImmersiveScreen.swift> [frames <dump dir> <first> <last> <out dir> [factor] [jx,jy,mx,my]]")
    exit(2)
}
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no Metal device") }
print("device: \(device.name); MetalFX temporal supported: \(MTLFXTemporalScalerDescriptor.supportsDevice(device))")

// The compositor's shaders, straight from its source: the motion pass tested here is the one that ships.
let compositorSource = try String(contentsOfFile: arguments[1], encoding: .utf8)
let pieces = compositorSource.components(separatedBy: "\"\"\"")
guard pieces.count >= 3 else { fatalError("no shader string in \(arguments[1])") }
let compositor: any MTLLibrary
do {
    compositor = try device.makeLibrary(source: pieces[1], options: nil)
} catch {
    print("compositor shaders FAILED to compile: \(error)")
    exit(1)
}
print("compositor shaders compile: \(compositor.functionNames.sorted())")

/// Must match TemporalUniforms in ImmersiveScreen.swift.
struct TemporalUniforms {
    var forward, up, right, frustum: SIMD4<Float>
    var beforeForward, beforeUp, beforeRight, beforeFrustum: SIMD4<Float>
    var moved: SIMD4<Float>
    var nearFar: SIMD2<Float>
    var jitter: SIMD2<Float>
    var size: SIMD2<Float>
    var decode: UInt32
    var still: UInt32
}

/// A camera as the game reports it: Z up, frustum tangents left, right, top, bottom.
struct View {
    var forward, up, right, position: SIMD3<Float>
    var frustum: SIMD4<Float>
    var near, far: Float
}

func texture(_ format: MTLPixelFormat, _ width: Int, _ height: Int, _ usage: MTLTextureUsage, shared: Bool = false) -> any MTLTexture {
    let wanted = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: false)
    wanted.usage = usage
    wanted.storageMode = shared ? .shared : .private
    guard let made = device.makeTexture(descriptor: wanted) else { fatalError("no \(width)x\(height) texture of format \(format.rawValue)") }
    return made
}

func pipeline(_ library: any MTLLibrary, _ vertex: String, _ fragment: String, _ colors: [MTLPixelFormat],
              depth: MTLPixelFormat = .invalid) -> any MTLRenderPipelineState {
    guard let vertexFunction = library.makeFunction(name: vertex), let fragmentFunction = library.makeFunction(name: fragment) else {
        fatalError("no \(vertex) or \(fragment) in the shaders")
    }
    let wanted = MTLRenderPipelineDescriptor()
    wanted.vertexFunction = vertexFunction
    wanted.fragmentFunction = fragmentFunction
    for (index, format) in colors.enumerated() { wanted.colorAttachments[index].pixelFormat = format }
    wanted.depthAttachmentPixelFormat = depth
    do {
        return try device.makeRenderPipelineState(descriptor: wanted)
    } catch {
        fatalError("no pipeline for \(vertex)/\(fragment): \(error)")
    }
}

let motionPipeline = pipeline(compositor, "temporal_vertex", "temporal_fragment", [.rgba16Float, .rg16Float], depth: .depth32Float)
let alwaysDepth: any MTLDepthStencilState = {
    let wanted = MTLDepthStencilDescriptor()
    wanted.depthCompareFunction = .always
    wanted.isDepthWriteEnabled = true
    return device.makeDepthStencilState(descriptor: wanted)!
}()

/// The scaler with the textures and the pass that feed it, as ImmersiveScreen.swift sets them up.
final class Upscaler {
    let scaler: any MTLFXTemporalScaler
    let color, motion, depth, output: any MTLTexture
    let width, height: Int

    init?(width: Int, height: Int, outWidth: Int, outHeight: Int, autoExposure: Bool = false) {
        let wanted = MTLFXTemporalScalerDescriptor()
        wanted.inputWidth = width
        wanted.inputHeight = height
        wanted.outputWidth = outWidth
        wanted.outputHeight = outHeight
        wanted.colorTextureFormat = .rgba16Float
        wanted.depthTextureFormat = .depth32Float
        wanted.motionTextureFormat = .rg16Float
        wanted.outputTextureFormat = .rgba16Float
        wanted.isAutoExposureEnabled = autoExposure
        guard let made = wanted.makeTemporalScaler(device: device) else { return nil }
        scaler = made
        color = texture(.rgba16Float, width, height, made.colorTextureUsage.union([.renderTarget, .shaderRead]))
        motion = texture(.rg16Float, width, height, made.motionTextureUsage.union([.renderTarget, .shaderRead]))
        depth = texture(.depth32Float, width, height, made.depthTextureUsage.union([.renderTarget, .shaderRead]))
        output = texture(.rgba16Float, outWidth, outHeight, made.outputTextureUsage.union(.shaderRead), shared: true)
        self.width = width
        self.height = height
    }

    /// One game frame: the compositor's motion pass, then the scaler. `image` reads as linear light (an sRGB
    /// format), `sceneDepth` is R32Float D3D depth. `before` nil: nothing to compare with. Returns the GPU ms.
    func frame(image: any MTLTexture, sceneDepth: any MTLTexture, view: View, before: View?, jitter: SIMD2<Float>,
               jitterSign: SIMD2<Float>, motionSign: SIMD2<Float>, reset: Bool, noMotion: Bool = false) -> Double {
        let from = before ?? view
        var uniforms = TemporalUniforms(forward: SIMD4(view.forward, 0), up: SIMD4(view.up, 0), right: SIMD4(view.right, 0),
                                        frustum: view.frustum,
                                        beforeForward: SIMD4(from.forward, 0), beforeUp: SIMD4(from.up, 0),
                                        beforeRight: SIMD4(from.right, 0), beforeFrustum: from.frustum,
                                        moved: SIMD4(view.position - from.position, 0),
                                        nearFar: SIMD2(view.near, view.far), jitter: jitter,
                                        size: SIMD2(Float(width), Float(height)), decode: 0,
                                        still: before == nil || noMotion ? 1 : 0)
        let commands = queue.makeCommandBuffer()!
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = color
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[1].texture = motion
        pass.colorAttachments[1].loadAction = .dontCare
        pass.colorAttachments[1].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .dontCare
        pass.depthAttachment.storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(motionPipeline)
        encoder.setDepthStencilState(alwaysDepth)
        encoder.setCullMode(.none)
        encoder.setFragmentBytes(&uniforms, length: MemoryLayout<TemporalUniforms>.stride, index: 0)
        encoder.setFragmentTexture(image, index: 0)
        encoder.setFragmentTexture(sceneDepth, index: 1)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        scaler.colorTexture = color
        scaler.depthTexture = depth
        scaler.motionTexture = motion
        scaler.outputTexture = output
        scaler.inputContentWidth = width
        scaler.inputContentHeight = height
        scaler.jitterOffsetX = jitterSign.x * jitter.x
        scaler.jitterOffsetY = jitterSign.y * jitter.y
        scaler.motionVectorScaleX = motionSign.x
        scaler.motionVectorScaleY = motionSign.y
        scaler.isDepthReversed = false
        scaler.preExposure = 1
        scaler.reset = reset
        scaler.encode(commandBuffer: commands)
        commands.commit()
        commands.waitUntilCompleted()
        if let error = commands.error { print("command buffer failed: \(error)") }
        return (commands.gpuEndTime - commands.gpuStartTime) * 1000
    }
}

// MARK: reading results back

func encoded(_ linear: Float) -> Float {
    let c = min(max(linear, 0), 1)
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055
}

/// sRGB-encoded values from 0 to 1, red green blue per pixel, rows from the top.
func pixels(linear16 texture: any MTLTexture) -> [Float] {
    let width = texture.width, height = texture.height
    var raw = [Float16](repeating: 0, count: width * height * 4)
    raw.withUnsafeMutableBytes {
        texture.getBytes($0.baseAddress!, bytesPerRow: width * 8, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
    }
    var out = [Float](repeating: 0, count: width * height * 3)
    for pixel in 0..<(width * height) {
        for channel in 0..<3 { out[pixel * 3 + channel] = encoded(Float(raw[pixel * 4 + channel])) }
    }
    return out
}

func pixels(bgra8 texture: any MTLTexture) -> [Float] {
    let width = texture.width, height = texture.height
    var raw = [UInt8](repeating: 0, count: width * height * 4)
    raw.withUnsafeMutableBytes {
        texture.getBytes($0.baseAddress!, bytesPerRow: width * 4, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
    }
    var out = [Float](repeating: 0, count: width * height * 3)
    for pixel in 0..<(width * height) {
        for channel in 0..<3 { out[pixel * 3 + channel] = Float(raw[pixel * 4 + 2 - channel]) / 255 }
    }
    return out
}

func psnr(_ a: [Float], _ b: [Float]) -> Double {
    var sum = 0.0
    for index in 0..<min(a.count, b.count) {
        let difference = Double(a[index] - b[index])
        sum += difference * difference
    }
    let mean = sum / Double(max(min(a.count, b.count), 1))
    return mean > 0 ? 10 * log10(1 / mean) : 99
}

/// The game's jitter sequence (runtime/jitter.c jitter_sequence): Halton 2,3 from index 1, minus a half.
func jitterPosition(_ index: Int, phases: Int) -> SIMD2<Float> {
    func halton(_ index: Int, _ base: Int) -> Float {
        var result: Float = 0, fraction: Float = 1, i = index
        while i > 0 {
            fraction /= Float(base)
            result += fraction * Float(i % base)
            i /= base
        }
        return result
    }
    guard phases > 0 else { return .zero }
    let i = index % phases + 1
    return SIMD2(halton(i, 2) - 0.5, halton(i, 3) - 0.5)
}

func summary(_ times: [Double]) -> String {
    guard !times.isEmpty else { return "no frames" }
    let sorted = times.sorted()
    return String(format: "GPU ms mean %.3f, median %.3f, p95 %.3f over %d frames", sorted.reduce(0, +) / Double(sorted.count),
                  sorted[sorted.count / 2], sorted[sorted.count * 95 / 100], sorted.count)
}

let signChoices: [SIMD2<Float>] = [SIMD2(1, 1), SIMD2(1, -1), SIMD2(-1, 1), SIMD2(-1, -1)]
func name(_ signs: SIMD2<Float>) -> String { String(format: "%+.0f %+.0f", signs.x, signs.y) }

// MARK: frames the game dumped

if arguments.count > 2 {
    guard arguments[2] == "frames", arguments.count >= 7, let first = Int(arguments[4]), let last = Int(arguments[5]) else {
        print("usage: temporal_probe <ImmersiveScreen.swift> frames <dump dir> <first> <last> <out dir> [factor] [jx,jy,mx,my]")
        exit(2)
    }
    let dumps = URL(fileURLWithPath: arguments[3]), out = URL(fileURLWithPath: arguments[6])
    let factor = arguments.count > 7 ? Float(arguments[7]) ?? 2 : 2
    var signs: [Float] = [-1, -1, 1, 1]
    if arguments.count > 8 {
        let given = arguments[8].split(separator: ",").compactMap { Float($0) }
        if given.count == 4 { signs = given }
    }
    try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

    // "cam <frame>: rot [9 numbers, row-major; columns forward up right] pos x y z frustum l . r . t . b . near .
    // far . fov ./. jitter x y" (runtime/d3d9_bridge.cpp camera_log).
    var cameras: [Int: (view: View, jitter: SIMD2<Float>)] = [:]
    let log = String(decoding: try Data(contentsOf: dumps.appendingPathComponent("log.txt")), as: UTF8.self)
    for line in log.split(separator: "\n") where line.hasPrefix("cam ") {
        let n = line.split(separator: " ").compactMap { Float($0.trimmingCharacters(in: CharacterSet(charactersIn: "[]|:"))) }
        guard n.count >= 19 else { continue }
        let view = View(forward: SIMD3(n[1], n[4], n[7]), up: SIMD3(n[2], n[5], n[8]), right: SIMD3(n[3], n[6], n[9]),
                        position: SIMD3(n[10], n[11], n[12]), frustum: SIMD4(n[13], n[14], n[15], n[16]), near: n[17], far: n[18])
        cameras[Int(n[0])] = (view, n.count >= 21 ? SIMD2(n[19], n[20]) : .zero)
    }
    print("\(cameras.count) cameras in the log")

    /// A 24-bit bottom-up BMP as LB_SHOT writes it, as a texture that reads in linear light.
    func shot(_ frame: Int) -> (any MTLTexture)? {
        let url = dumps.appendingPathComponent(String(format: "shot-%05d.bmp", frame))
        guard let data = try? Data(contentsOf: url), data.count > 54 else { return nil }
        let width = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 18, as: Int32.self) })
        let height = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 22, as: Int32.self) })
        let bits = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 28, as: UInt16.self) })
        let row = (width * 3 + 3) & ~3
        guard bits == 24, width > 0, height > 0, data.count >= 54 + row * height else { return nil }
        var bgra = [UInt8](repeating: 255, count: width * height * 4)
        data.withUnsafeBytes { raw in
            for y in 0..<height {
                let source = 54 + (height - 1 - y) * row
                for x in 0..<width {
                    for channel in 0..<3 { bgra[(y * width + x) * 4 + channel] = raw[source + x * 3 + channel] }
                }
            }
        }
        let made = texture(.bgra8Unorm_srgb, width, height, [.shaderRead, .pixelFormatView], shared: true)
        made.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: bgra, bytesPerRow: width * 4)
        return made
    }
    /// Width and height as two uint32, then rows of float32.
    func sceneDepth(_ frame: Int) -> (any MTLTexture)? {
        let url = dumps.appendingPathComponent(String(format: "depth-%05d.f32", frame))
        guard let data = try? Data(contentsOf: url), data.count > 8 else { return nil }
        let width = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) })
        let height = Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: UInt32.self) })
        guard width > 0, height > 0, data.count >= 8 + width * height * 4 else { return nil }
        let made = texture(.r32Float, width, height, .shaderRead, shared: true)
        data.withUnsafeBytes {
            made.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: $0.baseAddress! + 8,
                         bytesPerRow: width * 4)
        }
        return made
    }
    func write(_ rgb: [Float], _ width: Int, _ height: Int, _ name: String) {
        let row = (width * 3 + 3) & ~3
        var bytes = [UInt8](repeating: 0, count: 54 + row * height)
        func put(_ at: Int, _ value: Int) { for i in 0..<4 { bytes[at + i] = UInt8((value >> (8 * i)) & 255) } }
        bytes[0] = 0x42
        bytes[1] = 0x4D
        put(2, bytes.count); put(10, 54); put(14, 40); put(18, width); put(22, height)
        bytes[26] = 1
        bytes[28] = 24
        put(34, row * height)
        for y in 0..<height {
            let target = 54 + (height - 1 - y) * row
            for x in 0..<width {
                for channel in 0..<3 {
                    let value = rgb[(y * width + x) * 3 + 2 - channel] * 255
                    bytes[target + x * 3 + channel] = UInt8(min(max(value.rounded(), 0), 255))
                }
            }
        }
        do { try Data(bytes).write(to: out.appendingPathComponent(name)) } catch { print("can't write \(name): \(error)") }
    }

    var upscaler: Upscaler?
    var before: View?
    var times: [Double] = []
    var lastShot: (any MTLTexture)?
    for frame in first...last {
        guard let image = shot(frame), let depth = sceneDepth(frame), let camera = cameras[frame],
              depth.width == image.width, depth.height == image.height else {
            print("frame \(frame): no shot, depth or camera; starting afresh after it")
            before = nil
            continue
        }
        if upscaler == nil {
            let outWidth = Int((Float(image.width) * factor).rounded()), outHeight = Int((Float(image.height) * factor).rounded())
            upscaler = Upscaler(width: image.width, height: image.height, outWidth: outWidth, outHeight: outHeight)
            guard upscaler != nil else { fatalError("no temporal scaler for \(image.width)x\(image.height) -> \(outWidth)x\(outHeight)") }
            print("temporal \(image.width)x\(image.height) -> \(outWidth)x\(outHeight), signs jitter \(signs[0]) \(signs[1]) motion \(signs[2]) \(signs[3])")
        }
        guard let upscaler, image.width == upscaler.width, image.height == upscaler.height else {
            print("frame \(frame): another size; skipped")
            continue
        }
        times.append(upscaler.frame(image: image, sceneDepth: depth, view: camera.view, before: before, jitter: camera.jitter,
                                    jitterSign: SIMD2(signs[0], signs[1]), motionSign: SIMD2(signs[2], signs[3]),
                                    reset: before == nil))
        before = camera.view
        lastShot = image
        write(pixels(linear16: upscaler.output), upscaler.output.width, upscaler.output.height, String(format: "temporal-%05d.bmp", frame))
    }
    print("motion pass and scaler: \(summary(Array(times.dropFirst(min(4, times.count)))))")

    // The yardstick: the spatial scaler of LB_IMMERSIVE_UPSCALE on the last frame alone.
    if let lastShot, let upscaler, let plain = lastShot.makeTextureView(pixelFormat: .bgra8Unorm) {
        let wanted = MTLFXSpatialScalerDescriptor()
        wanted.inputWidth = lastShot.width
        wanted.inputHeight = lastShot.height
        wanted.outputWidth = upscaler.output.width
        wanted.outputHeight = upscaler.output.height
        wanted.colorTextureFormat = .bgra8Unorm
        wanted.outputTextureFormat = .bgra8Unorm
        wanted.colorProcessingMode = .perceptual
        if let spatial = wanted.makeSpatialScaler(device: device), let commands = queue.makeCommandBuffer() {
            let target = texture(.bgra8Unorm, wanted.outputWidth, wanted.outputHeight, spatial.outputTextureUsage.union(.shaderRead), shared: true)
            spatial.colorTexture = plain
            spatial.outputTexture = target
            spatial.inputContentWidth = lastShot.width
            spatial.inputContentHeight = lastShot.height
            spatial.encode(commandBuffer: commands)
            commands.commit()
            commands.waitUntilCompleted()
            write(pixels(bgra8: target), target.width, target.height, String(format: "spatial-%05d.bmp", last))
        } else {
            print("no spatial scaler for the comparison")
        }
    }
    print("wrote \(out.path)")
    exit(0)
}

// MARK: the made-up scene

/// Must match SceneUniforms below.
struct SceneUniforms {
    var forward, up, right, position, frustum: SIMD4<Float>
    var nearFar, jitter, size: SIMD2<Float>
    var samples: UInt32
    var pad: UInt32 = 0
}

let sceneSource = """
#include <metal_stdlib>
using namespace metal;
struct SceneUniforms {
    float4 forward; float4 up; float4 right; float4 position; float4 frustum;
    float2 nearFar; float2 jitter; float2 size; uint samples; uint pad;
};
struct SceneVertex { float4 position [[position]]; };
struct SceneOut { float4 color [[color(0)]]; float4 depth [[color(1)]]; };
vertex SceneVertex scene_vertex(uint id [[vertex_id]]) {
    SceneVertex out;
    out.position = float4(float2((id << 1) & 2, id & 2) * 2 - 1, 0, 1);
    return out;
}
// What the ray through `pixel` (pixels from the top left) shows: colour in linear light, and how far along the
// camera's forward axis it is (0 for the sky). Z is up. A floor at z = 0 tiled in 24-unit squares with thin
// lines, a row of striped posts at y = 700, a brick wall at y = 2400.
static float4 shade(float2 pixel, constant SceneUniforms &u) {
    float2 uv = pixel / u.size;
    float3 ray = u.forward.xyz + u.right.xyz * mix(u.frustum.x, u.frustum.y, uv.x) + u.up.xyz * mix(u.frustum.z, u.frustum.w, uv.y);
    float3 eye = u.position.xyz;
    float nearest = 1e30;
    float3 color = mix(float3(0.55, 0.65, 0.8), float3(0.15, 0.3, 0.6), saturate(ray.z * 2));
    if (ray.z < 0) {
        float t = -eye.z / ray.z;
        float3 p = eye + ray * t;
        float tile = abs(fmod(floor(p.x / 24) + floor(p.y / 24), 2.0));
        float3 c = mix(float3(0.12, 0.2, 0.1), float3(0.5, 0.45, 0.3), tile);
        if (fract(p.x / 7) > 0.9 || fract(p.y / 11) > 0.92) c = float3(0.9, 0.9, 0.85);
        nearest = t;
        color = mix(float3(0.3, 0.3, 0.25), c, saturate(900 / t));
    }
    if (ray.y > 0) {
        float t = (700 - eye.y) / ray.y;
        float3 p = eye + ray * t;
        if (t > 0 && t < nearest && p.z > 0 && p.z < 260 && fract(p.x / 300) < 0.1) {
            nearest = t;
            color = fract(p.z / 9) < 0.5 ? float3(0.8, 0.15, 0.1) : float3(0.95, 0.9, 0.8);
        }
        t = (2400 - eye.y) / ray.y;
        p = eye + ray * t;
        if (t > 0 && t < nearest && p.z > 0 && p.z < 700) {
            nearest = t;
            color = (fract(p.x / 40) < 0.08 || fract(p.z / 20) < 0.12) ? float3(0.75, 0.75, 0.7) : float3(0.45, 0.2, 0.15);
        }
    }
    return float4(color, nearest < 1e29 ? nearest : 0);
}
fragment SceneOut scene_fragment(SceneVertex in [[stage_in]], constant SceneUniforms &u [[buffer(0)]]) {
    uint n = max(u.samples, 1u);
    float3 sum = float3(0);
    float along = 0;
    for (uint j = 0; j < n; j++) {
        for (uint i = 0; i < n; i++) {
            float2 offset = (float2(i, j) + 0.5) / float(n) - 0.5;
            float4 seen = shade(in.position.xy - u.jitter + offset, u);
            sum += seen.rgb;
            if (i == n / 2 && j == n / 2) along = seen.a;
        }
    }
    SceneOut out;
    out.color = float4(sum / float(n * n), 1);
    // D3D depth, 0 at the near plane to 1 at the far one; the sky is at 1.
    float d = along > 0 ? u.nearFar.y * (along - u.nearFar.x) / (along * (u.nearFar.y - u.nearFar.x)) : 1.0;
    out.depth = float4(d);
    return out;
}
"""
let sceneLibrary: any MTLLibrary
do {
    sceneLibrary = try device.makeLibrary(source: sceneSource, options: nil)
} catch {
    fatalError("the probe's own scene shader does not compile: \(error)")
}
let scenePipeline = pipeline(sceneLibrary, "scene_vertex", "scene_fragment", [.bgra8Unorm_srgb, .r32Float])

/// Renders the scene: `jitter` moves the picture that many pixels right and down, as the game's does.
func renderScene(view: View, jitter: SIMD2<Float>, samples: UInt32, color: any MTLTexture, depth: any MTLTexture) {
    var uniforms = SceneUniforms(forward: SIMD4(view.forward, 0), up: SIMD4(view.up, 0), right: SIMD4(view.right, 0),
                                 position: SIMD4(view.position, 0), frustum: view.frustum,
                                 nearFar: SIMD2(view.near, view.far), jitter: jitter,
                                 size: SIMD2(Float(color.width), Float(color.height)), samples: samples)
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = color
    pass.colorAttachments[0].loadAction = .dontCare
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[1].texture = depth
    pass.colorAttachments[1].loadAction = .dontCare
    pass.colorAttachments[1].storeAction = .store
    let commands = queue.makeCommandBuffer()!
    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
    encoder.setRenderPipelineState(scenePipeline)
    encoder.setCullMode(.none)
    encoder.setFragmentBytes(&uniforms, length: MemoryLayout<SceneUniforms>.stride, index: 0)
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding()
    commands.commit()
    commands.waitUntilCompleted()
}

/// Standing at eye height looking north and a little down; when moving, walking forward, drifting right and
/// turning right, about as fast as a walk at 60 frames a second.
func camera(_ frame: Int, moving: Bool) -> View {
    let step: Float = moving ? Float(frame) : 0
    let yaw = 0.004 * step, pitch: Float = -0.06
    let forward = SIMD3<Float>(sin(yaw) * cos(pitch), cos(yaw) * cos(pitch), sin(pitch))
    let right = SIMD3<Float>(cos(yaw), -sin(yaw), 0)
    return View(forward: forward, up: cross(right, forward), right: right, position: SIMD3(2 * step, 6 * step, 130),
                frustum: SIMD4(-0.849427, 0.849427, 0.477803, -0.477803), near: 15, far: 353840)
}

struct Trial {
    var jitterSign = SIMD2<Float>(-1, -1)
    var motionSign = SIMD2<Float>(1, 1)
    var moving = false
    var noHistory = false
    var noMotion = false
    var autoExposure = false
}

/// PSNR of the scaler's last frame against the same view at the output size with 16 rays a pixel, and the GPU
/// times of the motion pass and scaler.
func run(_ trial: Trial, width: Int = 640, height: Int = 360, factor: Int = 2, frames: Int = 48) -> (psnr: Double, times: [Double])? {
    guard let upscaler = Upscaler(width: width, height: height, outWidth: width * factor, outHeight: height * factor,
                                  autoExposure: trial.autoExposure) else { return nil }
    let image = texture(.bgra8Unorm_srgb, width, height, [.renderTarget, .shaderRead])
    let depth = texture(.r32Float, width, height, [.renderTarget, .shaderRead])
    var before: View?
    var times: [Double] = []
    for frame in 0..<frames {
        let view = camera(frame, moving: trial.moving)
        let jitter = jitterPosition(frame, phases: 8 * factor * factor)
        renderScene(view: view, jitter: jitter, samples: 1, color: image, depth: depth)
        let time = upscaler.frame(image: image, sceneDepth: depth, view: view, before: before, jitter: jitter,
                                  jitterSign: trial.jitterSign, motionSign: trial.motionSign,
                                  reset: frame == 0 || trial.noHistory, noMotion: trial.noMotion)
        if frame >= 8 { times.append(time) }
        before = view
    }
    let reference = texture(.bgra8Unorm_srgb, width * factor, height * factor, [.renderTarget, .shaderRead], shared: true)
    let unused = texture(.r32Float, width * factor, height * factor, [.renderTarget, .shaderRead])
    renderScene(view: camera(frames - 1, moving: trial.moving), jitter: .zero, samples: 4, color: reference, depth: unused)
    return (psnr(pixels(linear16: upscaler.output), pixels(bgra8: reference)), times)
}

func report(_ label: String, _ trial: Trial) -> Double {
    guard let result = run(trial) else {
        print("  \(label): no scaler")
        return -1
    }
    print(String(format: "  %@: %.2f dB", label, result.psnr))
    return result.psnr
}

print("\nPSNR against the same view rendered at the output size with 16 rays a pixel; 640x360 -> 1280x720, 48 frames. Higher is better.")
print("camera still (only the shift matters):")
var best = Trial()
var bestScore = -1.0
for signs in signChoices {
    let score = report("jitter signs \(name(signs))", Trial(jitterSign: signs))
    if score > bestScore { bestScore = score; best.jitterSign = signs }
}
let single = report("no history (every frame starts afresh)", Trial(jitterSign: best.jitterSign, noHistory: true))
print("best jitter signs: \(name(best.jitterSign))\(bestScore - single < 1 ? "  (NOT CLEAR: under 1 dB better than no history)" : "")")

print("camera moving, with those jitter signs:")
bestScore = -1
for signs in signChoices {
    let score = report("motion signs \(name(signs))", Trial(jitterSign: best.jitterSign, motionSign: signs, moving: true))
    if score > bestScore { bestScore = score; best.motionSign = signs }
}
let unmoved = report("no motion vectors", Trial(jitterSign: best.jitterSign, moving: true, noMotion: true))
_ = report("no history (every frame starts afresh)", Trial(jitterSign: best.jitterSign, moving: true, noHistory: true))
print("best motion signs: \(name(best.motionSign))\(bestScore - unmoved < 1 ? "  (NOT CLEAR: under 1 dB better than no motion vectors)" : "")")
_ = report("the best, with MetalFX's auto exposure", Trial(jitterSign: best.jitterSign, motionSign: best.motionSign, moving: true, autoExposure: true))
print("""
ImmersiveScreen.swift readTemporalConventions defaults to jitter -1 -1, motion +1 +1. If the best above differ,
change the defaults there (or set LB_IMMERSIVE_TEMPORAL_SIGNS=\(Int(best.jitterSign.x)),\(Int(best.jitterSign.y)),\(Int(best.motionSign.x)),\(Int(best.motionSign.y))).
""")

print("cost of the motion pass and the scaler together, camera moving:")
for (width, height, factor) in [(1280, 720, 2), (960, 540, 2), (1920, 1080, 2), (640, 360, 3)] {
    var trial = best
    trial.moving = true
    if let result = run(trial, width: width, height: height, factor: factor, frames: 160) {
        print("  \(width)x\(height) -> \(width * factor)x\(height * factor): \(summary(result.times))")
    } else {
        print("  \(width)x\(height) -> \(width * factor)x\(height * factor): no scaler")
    }
}

print("formats the scaler can be made with at 640x360 -> 1280x720 (an unsupported one may stop the program here):")
for color in [MTLPixelFormat.rgba16Float, .bgra8Unorm_srgb, .bgra8Unorm, .rgb10a2Unorm] {
    for depth in [MTLPixelFormat.depth32Float, .r32Float] {
        print("  colour and output \(color.rawValue), depth \(depth.rawValue), motion rg16Float: ", terminator: "")
        let wanted = MTLFXTemporalScalerDescriptor()
        wanted.inputWidth = 640
        wanted.inputHeight = 360
        wanted.outputWidth = 1280
        wanted.outputHeight = 720
        wanted.colorTextureFormat = color
        wanted.depthTextureFormat = depth
        wanted.motionTextureFormat = .rg16Float
        wanted.outputTextureFormat = color
        print(wanted.makeTemporalScaler(device: device) != nil ? "yes" : "no")
    }
}
