// What Metal really does with the rate map docs/handoff/foveated-render.md wants to put on the game's render
// passes. Apple's documentation leaves several things the design depends on unsaid; this asks the GPU.
//
//   tools/foveation_map.py rates --width 2560 > /tmp/rates.json
//   xcrun swiftc -O tools/ratemap_probe.swift -o /tmp/ratemap_probe
//   /tmp/ratemap_probe /tmp/rates.json [/tmp/lut.json] [--msaa 4]
//
// It prints:
//   - whether the device takes rate maps, the physical size Metal chose against the one the tool estimated,
//     and whether the map is separable (x depends on x only), which the compositor's two 1D tables assume;
//   - the space a fragment's [[position]] is in under a map (physical texels or screen coordinates);
//   - that the interpolated screen position at each physical texel is what screenCoordinates(physical:) says;
//   - the space a scissor rectangle is in under a map (the MoltenVK patch assumes screen);
//   - with --msaa N, whether a multisampled mapped pass with a resolve works at all. Metal's validation stops
//     the process if it does not, so this is a separate run and a crash is the answer.
// With a second path it writes the real map as a table (the screen coordinate of every physical pixel edge),
// which tools/foveation_map.py unwarp --lut reads.
//
// Written on Linux without a Metal SDK: parsed with swiftc -parse only, never compiled or run.
import Foundation
import Metal
import simd

func fail(_ text: String) -> Never {
    FileHandle.standardError.write((text + "\n").data(using: .utf8)!)
    exit(1)
}

var positional: [String] = []
var options: [String: String] = [:]
var arguments = Array(CommandLine.arguments.dropFirst())
while !arguments.isEmpty {
    let argument = arguments.removeFirst()
    if argument.hasPrefix("--") {
        guard !arguments.isEmpty else { fail("\(argument) needs a value") }
        options[String(argument.dropFirst(2))] = arguments.removeFirst()
    } else {
        positional.append(argument)
    }
}
guard positional.count >= 1 else { fail("usage: ratemap_probe rates.json [lut.json] [--msaa 4]") }
let sampleCount = options["msaa"].flatMap { Int($0) } ?? 1

guard let data = FileManager.default.contents(atPath: positional[0]),
      let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
      let screen = json["screen"] as? [Int], screen.count == 2,
      let horizontal = (json["horizontal"] as? [NSNumber])?.map({ $0.floatValue }),
      let vertical = (json["vertical"] as? [NSNumber])?.map({ $0.floatValue })
else { fail("\(positional[0]) is not the output of foveation_map.py rates") }
let estimate = json["physical_estimate"] as? [Int] ?? [0, 0]

guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("no Metal device") }
print("device: \(device.name); rate maps supported: \(device.supportsRasterizationRateMap(layerCount: 1))")
guard device.supportsRasterizationRateMap(layerCount: 1) else { exit(1) }

// The map, made the way runtime/foveation_provider.mm makes it.
let layer = MTLRasterizationRateLayerDescriptor(sampleCount: MTLSizeMake(horizontal.count, vertical.count, 1))
for (i, rate) in horizontal.enumerated() { layer.horizontal[i] = rate }
for (i, rate) in vertical.enumerated() { layer.vertical[i] = rate }
let mapDescriptor = MTLRasterizationRateMapDescriptor()
mapDescriptor.label = "probe"
mapDescriptor.screenSize = MTLSizeMake(screen[0], screen[1], 0)
mapDescriptor.setLayer(layer, at: 0)
guard let map = device.makeRasterizationRateMap(descriptor: mapDescriptor) else { fail("the device refused the map") }
let physical = map.physicalSize(layer: 0)
let granularity = map.physicalGranularity
let cost = Double(physical.width * physical.height) / Double(screen[0] * screen[1])
print("screen \(screen[0])x\(screen[1]); physical \(physical.width)x\(physical.height) "
      + "(tool estimated \(estimate[0])x\(estimate[1])); granularity \(granularity.width)x\(granularity.height); "
      + String(format: "%.3f of the screen's pixels", cost))

func toScreen(_ x: Float, _ y: Float) -> SIMD2<Float> {
    let c = map.screenCoordinates(physicalCoordinates: MTLCoordinate2D(x: x, y: y), layer: 0)
    return SIMD2(c.x, c.y)
}
func toPhysical(_ x: Float, _ y: Float) -> SIMD2<Float> {
    let c = map.physicalCoordinates(screenCoordinates: MTLCoordinate2D(x: x, y: y), layer: 0)
    return SIMD2(c.x, c.y)
}

// The table, and whether the map is separable: x at three heights, y at three columns.
let tableX = (0...physical.width).map { toScreen(Float($0), Float(physical.height) / 2).x }
let tableY = (0...physical.height).map { toScreen(Float(physical.width) / 2, Float($0)).y }
var separableError: Float = 0
for row in [0.5, Float(physical.height) * 0.25, Float(physical.height) - 0.5] {
    for column in stride(from: 0, through: physical.width, by: 16) {
        separableError = max(separableError, abs(toScreen(Float(column), row).x - tableX[column]))
    }
}
for column in [0.5, Float(physical.width) * 0.25, Float(physical.width) - 0.5] {
    for row in stride(from: 0, through: physical.height, by: 16) {
        separableError = max(separableError, abs(toScreen(column, Float(row)).y - tableY[row]))
    }
}
let monotone = zip(tableX, tableX.dropFirst()).allSatisfy { $0 < $1 } && zip(tableY, tableY.dropFirst()).allSatisfy { $0 < $1 }
print(String(format: "separable: largest disagreement %.4f screen px (%@); monotone: %@; centre rate x %.3f, edge rate x %.3f",
             separableError, separableError < 0.01 ? "yes" : "NO", monotone ? "yes" : "NO",
             1 / (tableX[physical.width / 2 + 1] - tableX[physical.width / 2]), 1 / (tableX[1] - tableX[0])))
print(String(format: "table ends: x %.2f..%.2f of %d, y %.2f..%.2f of %d",
             tableX.first!, tableX.last!, screen[0], tableY.first!, tableY.last!, screen[1]))
if positional.count > 1 {
    let table: [String: Any] = ["screen": screen, "physical": [physical.width, physical.height], "x": tableX, "y": tableY]
    let out = try! JSONSerialization.data(withJSONObject: table)
    guard FileManager.default.createFile(atPath: positional[1], contents: out) else { fail("cannot write \(positional[1])") }
    print("wrote \(positional[1])")
}

// One triangle over the whole viewport. The fragment keeps its own [[position]] and an interpolated screen
// position (0..screen size), so the picture says where each physical texel thinks it is.
let source = """
#include <metal_stdlib>
using namespace metal;
struct ProbeOut { float4 position [[position]]; float2 screen; };
vertex ProbeOut probe_vertex(uint id [[vertex_id]], constant float2 &size [[buffer(0)]]) {
    float2 uv = float2((id << 1) & 2, id & 2);
    ProbeOut out;
    out.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    out.screen = uv * size;
    return out;
}
fragment float4 probe_fragment(ProbeOut in [[stage_in]]) { return float4(in.position.xy, in.screen); }
"""
let library: any MTLLibrary
do { library = try device.makeLibrary(source: source, options: nil) } catch { fail("shader: \(error)") }
let pipelineDescriptor = MTLRenderPipelineDescriptor()
pipelineDescriptor.vertexFunction = library.makeFunction(name: "probe_vertex")
pipelineDescriptor.fragmentFunction = library.makeFunction(name: "probe_fragment")
pipelineDescriptor.colorAttachments[0].pixelFormat = .rgba32Float
pipelineDescriptor.rasterSampleCount = sampleCount
let pipeline: any MTLRenderPipelineState
do { pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor) } catch { fail("pipeline: \(error)") }

func target(samples: Int) -> any MTLTexture {
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: physical.width,
                                                     height: physical.height, mipmapped: false)
    d.usage = [.renderTarget, .shaderRead]
    d.storageMode = samples > 1 ? .private : .shared
    if samples > 1 { d.textureType = .type2DMultisample; d.sampleCount = samples }
    return device.makeTexture(descriptor: d)!
}

/// Draws the triangle through the map, with a scissor if given, and returns the picture: 4 floats per texel,
/// -1 where nothing was drawn.
func draw(scissor: MTLScissorRect?) -> [Float] {
    let resolved = target(samples: 1)
    let pass = MTLRenderPassDescriptor()
    pass.rasterizationRateMap = map
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].clearColor = MTLClearColor(red: -1, green: -1, blue: -1, alpha: -1)
    if sampleCount > 1 {
        pass.colorAttachments[0].texture = target(samples: sampleCount)
        pass.colorAttachments[0].resolveTexture = resolved
        pass.colorAttachments[0].storeAction = .multisampleResolve
    } else {
        pass.colorAttachments[0].texture = resolved
        pass.colorAttachments[0].storeAction = .store
    }
    let commands = queue.makeCommandBuffer()!
    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
    encoder.setRenderPipelineState(pipeline)
    // The documented part: with a map, the viewport is in screen coordinates.
    encoder.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(screen[0]), height: Double(screen[1]),
                                    znear: 0, zfar: 1))
    if let scissor { encoder.setScissorRect(scissor) }
    var size = SIMD2<Float>(Float(screen[0]), Float(screen[1]))
    encoder.setVertexBytes(&size, length: MemoryLayout<SIMD2<Float>>.size, index: 0)
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding()
    commands.commit()
    commands.waitUntilCompleted()
    if let error = commands.error { fail("the mapped pass failed: \(error)") }
    var pixels = [Float](repeating: 0, count: physical.width * physical.height * 4)
    resolved.getBytes(&pixels, bytesPerRow: physical.width * 16,
                      from: MTLRegionMake2D(0, 0, physical.width, physical.height), mipmapLevel: 0)
    return pixels
}

// Fragment position and the interpolated screen position, over a grid of texels.
let full = draw(scissor: nil)
var asPhysical: Float = 0, asScreen: Float = 0, varyingError: Float = 0, undrawn = 0, samples = 0
for y in stride(from: 1, to: physical.height - 1, by: 7) {
    for x in stride(from: 1, to: physical.width - 1, by: 7) {
        let i = (y * physical.width + x) * 4
        if full[i] < 0 { undrawn += 1; continue }
        let centre = SIMD2(Float(x) + 0.5, Float(y) + 0.5), expected = toScreen(centre.x, centre.y)
        let position = SIMD2(full[i], full[i + 1]), varying = SIMD2(full[i + 2], full[i + 3])
        asPhysical = max(asPhysical, simd_reduce_max(abs(position - centre)))
        asScreen = max(asScreen, simd_reduce_max(abs(position - expected)))
        varyingError = max(varyingError, simd_reduce_max(abs(varying - expected)))
        samples += 1
    }
}
print("mapped pass\(sampleCount > 1 ? " (\(sampleCount)x MSAA, resolved)" : ""): \(samples) texels checked, \(undrawn) undrawn")
print(String(format: "fragment [[position]]: off by up to %.2f as physical texels, %.2f as screen coordinates -> %@",
             asPhysical, asScreen, asPhysical < 0.6 ? "PHYSICAL" : (asScreen < 1.5 ? "SCREEN" : "NEITHER")))
print(String(format: "interpolated screen position against screenCoordinates(physical:): off by up to %.2f screen px", varyingError))

// Scissor: a rectangle that is legal in either space (inside the physical size), on the top-left quarter.
// Count how far the drawn area reaches along its top row and left column.
let rect = MTLScissorRect(x: 0, y: 0, width: physical.width / 2, height: physical.height / 2)
let clipped = draw(scissor: rect)
var reachX = 0, reachY = 0
while reachX < physical.width && clipped[reachX * 4] >= 0 { reachX += 1 }
while reachY < physical.height && clipped[reachY * physical.width * 4] >= 0 { reachY += 1 }
let ifScreen = toPhysical(Float(rect.width), Float(rect.height))
let scissorVerdict: String
if abs(reachX - rect.width) <= 1 && abs(reachY - rect.height) <= 1 {
    scissorVerdict = "PHYSICAL"
} else if abs(Float(reachX) - ifScreen.x) <= 2 && abs(Float(reachY) - ifScreen.y) <= 2 {
    scissorVerdict = "SCREEN"
} else {
    scissorVerdict = "NEITHER"
}
print(String(format: "scissor %dx%d drew %dx%d texels; physical space would give %dx%d, screen space %.0fx%.0f -> %@",
             rect.width, rect.height, reachX, reachY, rect.width, rect.height, ifScreen.x, ifScreen.y, scissorVerdict))
