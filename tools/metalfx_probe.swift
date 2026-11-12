// GPU cost of the MetalFX spatial upscale the immersive compositor can use (LB_IMMERSIVE_UPSCALE), measured on
// this Mac's GPU, and a compile check of the compositor's Metal shaders.
//
//   xcrun swiftc -O tools/metalfx_probe.swift -o /tmp/metalfx_probe
//   /tmp/metalfx_probe [apple/Sources/App/ImmersiveScreen.swift]
//
// Each case scales a noise image `frames` times, one command buffer per frame, and reports the GPU time of
// those command buffers (gpuEndTime - gpuStartTime). It says nothing about a headset's GPU or about how the
// upscale shares the GPU with the game there.
import Foundation
import Metal
import MetalFX

guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no Metal device") }
print("device: \(device.name); MetalFX spatial supported: \(MTLFXSpatialScalerDescriptor.supportsDevice(device))")

if CommandLine.arguments.count > 1 {
    let source = try String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
    let parts = source.components(separatedBy: "\"\"\"")
    guard parts.count >= 3 else { fatalError("no shader string in \(CommandLine.arguments[1])") }
    do {
        let library = try device.makeLibrary(source: parts[1], options: nil)
        print("compositor shaders compile: \(library.functionNames.sorted())")
    } catch {
        print("compositor shaders FAILED to compile: \(error)")
        exit(1)
    }
}

func noise(_ width: Int, _ height: Int, _ format: MTLPixelFormat) -> any MTLTexture {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: false)
    d.usage = [.shaderRead, .renderTarget]
    let texture = device.makeTexture(descriptor: d)!
    var bytes = [UInt8](repeating: 0, count: width * height * 4)
    var state: UInt32 = 12345
    for i in 0..<bytes.count { state = state &* 1664525 &+ 1013904223; bytes[i] = UInt8(truncatingIfNeeded: state >> 24) }
    texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: bytes, bytesPerRow: width * 4)
    return texture
}

func measure(_ inW: Int, _ inH: Int, _ outW: Int, _ outH: Int, format: MTLPixelFormat, frames: Int = 300) {
    let d = MTLFXSpatialScalerDescriptor()
    d.inputWidth = inW; d.inputHeight = inH; d.outputWidth = outW; d.outputHeight = outH
    d.colorTextureFormat = format; d.outputTextureFormat = format
    d.colorProcessingMode = .perceptual
    guard let scaler = d.makeSpatialScaler(device: device) else { print("\(inW)x\(inH) -> \(outW)x\(outH): no scaler"); return }
    let o = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: outW, height: outH, mipmapped: false)
    o.usage = scaler.outputTextureUsage.union(.shaderRead)
    o.storageMode = .private
    let input = noise(inW, inH, format), output = device.makeTexture(descriptor: o)!
    scaler.colorTexture = input
    scaler.outputTexture = output
    scaler.inputContentWidth = inW
    scaler.inputContentHeight = inH
    var times: [Double] = []
    for frame in 0..<(frames + 20) {
        let commands = queue.makeCommandBuffer()!
        scaler.encode(commandBuffer: commands)
        commands.commit()
        commands.waitUntilCompleted()
        if frame >= 20 { times.append((commands.gpuEndTime - commands.gpuStartTime) * 1000) }
    }
    times.sort()
    let mean = times.reduce(0, +) / Double(times.count)
    print(String(format: "%dx%d -> %dx%d (%@): GPU ms mean %.3f, median %.3f, p95 %.3f, max %.3f over %d frames",
                 inW, inH, outW, outH, format == .bgra8Unorm ? "bgra8" : "format \(format.rawValue)", mean,
                 times[times.count / 2], times[times.count * 95 / 100], times.last!, times.count))
}

for _ in 0..<3 {
    measure(1280, 720, 1920, 1080, format: .bgra8Unorm)
    measure(1280, 720, 2560, 1440, format: .bgra8Unorm)
    measure(1280, 720, 3840, 2160, format: .bgra8Unorm)
    measure(960, 540, 1920, 1080, format: .bgra8Unorm)
    measure(1920, 1080, 3840, 2160, format: .bgra8Unorm)
}
