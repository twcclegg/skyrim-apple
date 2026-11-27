#if os(visionOS) && SKYRIM_NATIVE_RUNTIME
import ARKit
import CompositorServices
import Foundation
import Metal
#if canImport(MetalFX)
import MetalFX  // not in the visionOS Simulator SDK
#endif
import QuartzCore
import simd
import SwiftUI

/// The flat game image on a world-locked screen in a Full Space. Mono: both eyes see the same picture.
enum ImmersiveScreen {
    static let spaceID = "skyrim.immersive.screen"
}

struct ImmersiveScreenConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        configuration.layout = .dedicated
        // LB_FOVEATION=1: the system's eye-tracked foveation (rasterization rate maps on each eye's pass). Read
        // from launch-env.json directly: the layer is configured before the runtime applies that file.
        configuration.isFoveationEnabled = Self.foveation && capabilities.supportsFoveation
    }

    static var foveation: Bool {
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SkyrimNative/launch-env.json")
        guard let data = try? Data(contentsOf: url),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return false }
        return (json["LB_FOVEATION"] as? String) == "1"
    }
}

/// Spatial events seen on the immersive layer, by kind and phase; logged by the compositor and cleared.
extension Notification.Name {
    /// Posted on the main queue with the immersive layer's start (object true) and end (false).
    static let skyrimImmersiveRunning = Notification.Name("SkyrimImmersiveRunning")
}

/// Compositor GPU time per frame, tallied on the command buffer completion queue and read by the 5 s log. The
/// p95 comes from a fixed histogram (0.25 ms bins up to 20 ms, the last bin holding everything above), so it is
/// accurate to a bin and nothing is allocated per frame.
final class GPUTimeTally: @unchecked Sendable {
    private static let binSeconds = 0.00025
    private static let binCount = 80
    private let lock = NSLock()
    private var total = 0.0, maximum = 0.0, count = 0
    private var bins = [Int](repeating: 0, count: GPUTimeTally.binCount)
    func add(_ seconds: Double) {
        guard seconds > 0, seconds < 1 else { return }
        let bin = min(Int(seconds / Self.binSeconds), Self.binCount - 1)
        lock.lock()
        total += seconds; count += 1; bins[bin] += 1
        if seconds > maximum { maximum = seconds }
        lock.unlock()
    }
    func take() -> (total: Double, count: Int, p95: Double, max: Double) {
        lock.lock(); defer {
            total = 0; maximum = 0; count = 0
            for index in bins.indices { bins[index] = 0 }
            lock.unlock()
        }
        var p95 = 0.0
        if count > 0 {
            let target = (count * 95 + 99) / 100
            var seen = 0
            for (index, n) in bins.enumerated() {
                seen += n
                if seen >= target {
                    p95 = index == Self.binCount - 1 ? maximum : min(Double(index + 1) * Self.binSeconds, maximum)
                    break
                }
            }
        }
        return (total, count, p95, maximum)
    }
}

/// LB_IMMERSIVE_PASS_TIME=1: the GPU time of the eyes' passes, vertex and fragment stages apart, from timestamps
/// taken at the stage boundaries of each pass (a counter sample buffer). Each frame takes the next of `frames`
/// slots of `perFrame` samples, four a pass; the completion handler reads them back and tallies the frame's sum.
/// Timestamps are GPU ticks: converted to seconds by comparing two GPU timestamp readings with the wall clock.
final class PassTimer: @unchecked Sendable {
    private static let frames = 8, perFrame = 16
    private let buffer: any MTLCounterSampleBuffer
    private let device: any MTLDevice
    private var slot = 0, used = 0
    private let lock = NSLock()
    private var vertex = 0.0, fragment = 0.0, vertexMax = 0.0, fragmentMax = 0.0, count = 0, misses = 0
    private var clockStart: (gpu: UInt64, wall: Double)

    /// nil when the GPU can't take timestamps at stage boundaries.
    init?(device: any MTLDevice) {
        guard device.supportsCounterSampling(.atStageBoundary),
              let set = device.counterSets?.first(where: { $0.name == MTLCommonCounterSet.timestamp.rawValue }) else { return nil }
        let description = MTLCounterSampleBufferDescriptor()
        description.counterSet = set
        description.storageMode = .shared
        description.sampleCount = Self.frames * Self.perFrame
        guard let buffer = try? device.makeCounterSampleBuffer(descriptor: description) else { return nil }
        self.buffer = buffer
        self.device = device
        clockStart = (device.sampleTimestamps().gpu, CACurrentMediaTime())
    }

    /// Starts a frame's samples; returns its slot for `finish`.
    func beginFrame() -> Int {
        slot = (slot + 1) % Self.frames
        used = 0
        return slot
    }

    /// Has the pass write its four timestamps, while the frame's slot has room. Returns the passes so far.
    func attach(_ pass: MTLRenderPassDescriptor) -> Int {
        guard used + 4 <= Self.perFrame, let attachment = pass.sampleBufferAttachments[0] else { return used / 4 }
        let first = slot * Self.perFrame + used
        attachment.sampleBuffer = buffer
        attachment.startOfVertexSampleIndex = first
        attachment.endOfVertexSampleIndex = first + 1
        attachment.startOfFragmentSampleIndex = first + 2
        attachment.endOfFragmentSampleIndex = first + 3
        used += 4
        return used / 4
    }

    /// From the command buffer's completion handler: reads the frame's timestamps and tallies its stage times.
    func finish(slot: Int, passes: Int) {
        guard passes > 0 else { return }
        let first = slot * Self.perFrame
        guard let data = try? buffer.resolveCounterRange(first..<(first + passes * 4)) else {
            lock.lock(); misses += 1; lock.unlock()
            return
        }
        var vertexTicks: UInt64 = 0, fragmentTicks: UInt64 = 0, bad = false
        data.withUnsafeBytes { raw in
            let stamps = raw.bindMemory(to: MTLCounterResultTimestamp.self)
            guard stamps.count >= passes * 4 else { bad = true; return }
            for pass in 0..<passes {
                let t = (0..<4).map { stamps[pass * 4 + $0].timestamp }
                if t.contains(where: { $0 == 0 || $0 == UInt64.max }) || t[1] < t[0] || t[3] < t[2] { bad = true; return }
                vertexTicks += t[1] - t[0]
                fragmentTicks += t[3] - t[2]
            }
        }
        lock.lock()
        if bad {
            misses += 1
        } else {
            let v = Double(vertexTicks), f = Double(fragmentTicks)
            vertex += v; fragment += f; count += 1
            vertexMax = max(vertexMax, v); fragmentMax = max(fragmentMax, f)
        }
        lock.unlock()
    }

    /// Mean and largest stage times per frame since the last call, in seconds; frames whose samples failed.
    func take() -> (vertex: Double, fragment: Double, vertexMax: Double, fragmentMax: Double, frames: Int, misses: Int) {
        let now = (gpu: device.sampleTimestamps().gpu, wall: CACurrentMediaTime())
        let ticks = now.gpu > clockStart.gpu ? Double(now.gpu - clockStart.gpu) : 0
        let secondsPerTick = ticks > 0 ? (now.wall - clockStart.wall) / ticks : 0
        clockStart = now
        lock.lock(); defer {
            vertex = 0; fragment = 0; vertexMax = 0; fragmentMax = 0; count = 0; misses = 0
            lock.unlock()
        }
        let n = Double(max(count, 1))
        return (vertex / n * secondsPerTick, fragment / n * secondsPerTick, vertexMax * secondsPerTick,
                fragmentMax * secondsPerTick, count, misses)
    }
}

enum SpatialEventLog {
    nonisolated(unsafe) private static var counts: [String: Int] = [:]
    private static let lock = NSLock()
    static func note(kind: String, phase: String) {
        lock.lock(); counts["\(kind)/\(phase)", default: 0] += 1; lock.unlock()
    }
    static func take() -> String? {
        lock.lock(); defer { counts = [:]; lock.unlock() }
        guard !counts.isEmpty else { return nil }
        return counts.sorted { $0.key < $1.key }.map { "\($0.key) \($0.value)" }.joined(separator: ", ")
    }
}

private struct HudUniforms {
    var mvp: simd_float4x4
    var rect: SIMD4<Float>
    var z: Float
}

private struct QuadUniforms {
    var viewProjection: simd_float4x4
    var model: simd_float4x4
    /// Where the frame goes without the player's movement since it was rendered (see heldWithin).
    var heldModel: simd_float4x4
    var halfSize: SIMD2<Float>
    /// The centre of the frame's rectangle on its plane (non-zero for an eye's asymmetric frustum).
    var screenCentre: SIMD2<Float>
    var decode: UInt32
    var hasDepth: UInt32
    /// Grid columns and rows (vertices per row, rows).
    var grid: SIMD2<UInt32>
    /// Where the game camera sits behind the screen, in screen units (meters), so the screen spans its view.
    var eyeBack: Float
    /// Game near plane in game units, and game units per meter.
    var nearUnits: Float
    var unitsPerMeter: Float
    /// Meters behind the screen per game meter (the screen is a window onto a scaled-down scene); 0 = flat.
    var depthScale: Float
    /// The farthest anything is placed behind the screen (meters); the sky and far scenery sit here.
    var maxBehind: Float
    /// 1: draw the image pushed to the farthest depth, behind the mesh, to fill where torn triangles were dropped.
    var backplate: Float
    /// A triangle whose far corner is more than (1 + tear) times as far from the eye as its near one is dropped
    /// rather than stretched.
    var tear: Float
    /// 0: the screen is a window (depth behind it, scaled). 1: head-tracked view, each pixel at its true distance
    /// (depthScale meters per game meter, up to maxBehind meters) from the eye, which is eyeBack in front.
    var trueDepth: Float
    /// Strength of the sharpening pass on the game image (LB_IMMERSIVE_SHARPEN, 0 = off).
    var sharpen: Float
    /// xyz: this view's eye in the screen's own space. w: per-pixel depth search steps in the fragment shader
    /// (LB_IMMERSIVE_REFINE); 0 = off.
    var eyeRefine: SIMD4<Float>
    /// Following the player between game frames (LB_IMMERSIVE_MOTION): whatever is nearer the eye than this many
    /// meters travels with the camera (the player's own hands and weapon) and is placed by heldModel, not model;
    /// the change is spread over the next half as far again. 0: everything is placed by model.
    var heldWithin: Float
    /// The per-pixel search stops as soon as a step moves the game pixel shown by less than this many game pixels
    /// without leaving the depth texel it read (LB_IMMERSIVE_REFINE_EXIT); 0 = every pixel runs every step.
    var refineExit: Float
}

/// Runs on its own thread for as long as the layer lives: presents at the display's rate, showing the newest finished
/// game frame and repeating the last one when the game hasn't produced another.
final class ImmersiveCompositor: @unchecked Sendable {
    private static let shaders = """
    #include <metal_stdlib>
    using namespace metal;
    struct Uniforms {
        float4x4 viewProjection; float4x4 model; float4x4 heldModel; float2 halfSize; float2 screenCentre; uint decode; uint hasDepth;
        uint2 grid; float eyeBack; float nearUnits; float unitsPerMeter; float depthScale; float maxBehind; float backplate;
        float tear; float trueDepth; float sharpen; float4 eyeRefine; float heldWithin; float refineExit;
    };
    struct Out { float4 position [[position]]; float2 uv; float3 local; };
    // A grid over the game image. With depth, each vertex moves along the ray from the game's camera (eyeBack in
    // front of the screen's centre, on the viewer's side) through its point on the screen, by how far that pixel is
    // in the game: the screen
    // becomes a window into the scene and each eye sees it from its own place.
    // How far along its ray (as a multiple of the ray to the screen) the game pixel at uv is pushed.
    // How far behind the screen (meters, along the viewer's ray) the game pixel at uv sits. The screen is a window:
    // everything is behind it, at depthScale meters per game meter, up to maxBehind. A consistent geometry (unlike a
    // relief pushed out of the screen), so the eyes converge where the picture is.
    // The game's depth (as D3D wrote it) of the game pixel at uv.
    float nearest_depth(float2 uv, uint2 grid, texture2d<float> depth, bool dilate) {
        constexpr sampler nearest(address::clamp_to_edge, filter::nearest);
        float d = depth.sample(nearest, uv, level(0)).r;
        if (dilate) {
            // The nearest depth around the point (half a grid cell each way), so a thin foreground edge keeps its
            // depth instead of a vertex landing beside it in the background.
            float2 cell = 0.5 / float2(grid - 1);
            d = min(d, depth.sample(nearest, uv + float2(-cell.x, 0), level(0)).r);
            d = min(d, depth.sample(nearest, uv + float2(cell.x, 0), level(0)).r);
            d = min(d, depth.sample(nearest, uv + float2(0, -cell.y), level(0)).r);
            d = min(d, depth.sample(nearest, uv + float2(0, cell.y), level(0)).r);
        }
        return d;
    }
    // The view depth in game meters of the D3D depth d.
    float depth_meters(float d, constant Uniforms &u) {
        // D3D depth with a far plane much farther than the near one: view distance ~ near / (1 - d).
        return (u.nearUnits / max(1.0 - d, 1e-6)) / u.unitsPerMeter;
    }
    // The game pixel's view depth in game meters.
    float behind_screen_raw(float2 uv, constant Uniforms &u, texture2d<float> depth, bool dilate) {
        return depth_meters(nearest_depth(uv, u.grid, depth, dilate), u);
    }
    // Distance from the eye of a game pixel whose view depth is meters, on a ray that is toScreen long to the screen.
    // Window mode: the screen distance plus how far behind it. True-depth mode: its view depth in meters, along
    // the ray (view depth z sits at z / cos = |ray| * z / eyeBack).
    float radial_at(float meters, float toScreen, constant Uniforms &u) {
        if (u.trueDepth == 0) return toScreen + min(u.depthScale * meters, u.maxBehind);
        return toScreen * min(meters * u.depthScale, u.maxBehind) / u.eyeBack;
    }
    // Distance from the eye of the game pixel at uv.
    float radial(float2 uv, float toScreen, constant Uniforms &u, texture2d<float> depth, bool dilate) {
        return radial_at(behind_screen_raw(uv, u, depth, dilate), toScreen, u);
    }
    float3 screen_point(float2 uv, constant Uniforms &u) {
        return float3((uv.x * 2 - 1) * u.halfSize.x + u.screenCentre.x, (1 - uv.y * 2) * u.halfSize.y + u.screenCentre.y, 0);
    }
    // Fades the push to nothing over the outer 4% so the picture's outline stays a clean rectangle. Applied after the
    // tear test, so the fade's own slope doesn't count as a depth edge.
    float edge_fade(float2 uv) {
        float2 edge = min(uv, 1.0 - uv);
        return smoothstep(0.0, 0.04, min(edge.x, edge.y));
    }
    // Where the mesh's point p (in the frame's own space), showing the game image at uv, goes on the display.
    Out place_vertex(float3 p, float2 uv, constant Uniforms &u) {
        Out out;
        out.uv = uv;
        out.local = p;
        float4 placed = u.model * float4(p, 1);
        if (u.heldWithin > 0) {
            // The world is shifted against the player's movement since the frame was rendered. The player's own
            // hands and weapon moved with the camera, so they stay where the frame has them.
            float world = smoothstep(u.heldWithin, u.heldWithin * 1.5, length(p - float3(0, 0, u.eyeBack)));
            placed = mix(u.heldModel * float4(p, 1), placed, world);
        }
        out.position = u.viewProjection * placed;
        return out;
    }
    // Two triangles per grid cell, unindexed, so each vertex knows its whole triangle. With depth, each vertex moves
    // along the ray from the viewer's starting eye through its point on the screen, by how far that pixel is in the
    // game. A triangle spanning a depth jump (an object's edge against what is behind it) is dropped instead of
    // stretched into streaks (see below). The backplate (u.backplate) is drawn first, blurred, behind the mesh.
    vertex Out screen_vertex(uint id [[vertex_id]], constant Uniforms &u [[buffer(0)]],
                             texture2d<float> depth [[texture(1)]]) {
        const uint2 corners[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 0), uint2(1, 1), uint2(0, 1) };
        uint cellIndex = id / 6, corner = id % 6, first = corner < 3 ? 0 : 3;
        uint2 cell = uint2(cellIndex % (u.grid.x - 1), cellIndex / (u.grid.x - 1));
        float2 step = 1.0 / float2(u.grid - 1);
        float2 uv = float2(cell + corners[corner]) * step;
        float3 onScreen = screen_point(uv, u);
        float3 eye = float3(0, 0, u.eyeBack);
        float3 p = onScreen;
        float3 ray = onScreen - eye;
        float toScreen = length(ray);
        if (u.backplate != 0) {
            float far = u.trueDepth != 0 ? toScreen * u.maxBehind / u.eyeBack - toScreen : u.maxBehind * edge_fade(uv);
            // 0.2% beyond the far limit, so the mesh where it reaches that limit (the sky) wins the depth test.
            p = eye + ray * ((1.0 + far / toScreen) * 1.002);
        } else if (u.hasDepth) {
            // Distances from the eye of the triangle's corners; a triangle whose far corner is more than (1 + tear)
            // times as far as its near one spans an object's edge.
            float2 c0 = float2(cell + corners[first]) * step, c1 = float2(cell + corners[first + 1]) * step,
                   c2 = float2(cell + corners[first + 2]) * step;
            float r0 = radial(c0, length(screen_point(c0, u) - eye), u, depth, true);
            float r1 = radial(c1, length(screen_point(c1, u) - eye), u, depth, true);
            float r2 = radial(c2, length(screen_point(c2, u) - eye), u, depth, true);
            float r = corner == first ? r0 : (corner == first + 1 ? r1 : r2);
            // A triangle spanning a depth edge is laid flat at its farthest corner's distance instead of being
            // stretched across the gap: the gap shows at most one cell of the edge at the background's depth. (A
            // far backplate behind dropped triangles showed whole second copies of near objects - "two heads" -
            // once the head moved away from where the rays start.)
            // With the per-pixel search (eyeRefine.w) the mesh stays whole instead: a flat triangle leaves a gap
            // beside the near surface, as wide as the parallax between them, and the search settles each pixel there.
            float rFar = max(r0, max(r1, r2));
            if (u.eyeRefine.w == 0 && rFar > min(r0, min(r1, r2)) * (1.0 + u.tear)) r = rFar;
            float fade = u.trueDepth != 0 ? 1.0 : edge_fade(uv);
            p = eye + ray * (1.0 + (r - toScreen) * fade / toScreen);
        }
        return place_vertex(p, uv, u);
    }
    // LB_IMMERSIVE_SEED. What screen_vertex takes from the depth texture changes only when the game hands over a new
    // frame, yet it is fetched again for every corner of every triangle, for each eye, at every refresh: 15 depth
    // fetches a corner, six corners a cell. With the switch, seed_fragment runs once per game frame instead, into a
    // texture with one texel per grid vertex: that vertex's depth exactly as screen_vertex finds it. It holds the
    // depth as D3D wrote it, so nothing that changes between refreshes is baked in. screen_vertex_seeded then
    // draws the same triangles in the same order through an index buffer, each vertex shaded once with one read.
    // Only for the mesh kept whole (the per-pixel search on): screen_vertex's flattening of triangles across a
    // depth edge needs the whole triangle at each corner.
    struct SeedOut { float4 position [[position]]; };
    vertex SeedOut seed_vertex(uint id [[vertex_id]]) {
        // One triangle over the whole target.
        SeedOut out;
        out.position = float4(float(id & 1) * 4 - 1, float(id >> 1) * 4 - 1, 0, 1);
        return out;
    }
    fragment float4 seed_fragment(SeedOut in [[stage_in]], constant uint2 &grid [[buffer(0)]],
                                  texture2d<float> depth [[texture(1)]]) {
        // The texel's grid vertex and its uv, as screen_vertex works them out.
        float2 step = 1.0 / float2(grid - 1);
        float2 uv = float2(uint2(in.position.xy)) * step;
        return float4(nearest_depth(uv, grid, depth, true), 0, 0, 1);
    }
    vertex Out screen_vertex_seeded(uint id [[vertex_id]], constant Uniforms &u [[buffer(0)]],
                                    texture2d<float> seed [[texture(1)]]) {
        uint2 at = uint2(id % u.grid.x, id / u.grid.x);
        float2 step = 1.0 / float2(u.grid - 1);
        float2 uv = float2(at) * step;
        float3 eye = float3(0, 0, u.eyeBack);
        float3 ray = screen_point(uv, u) - eye;
        float toScreen = length(ray);
        float r = radial_at(depth_meters(seed.read(at).r, u), toScreen, u);
        float fade = u.trueDepth != 0 ? 1.0 : edge_fade(uv);
        return place_vertex(eye + ray * (1.0 + (r - toScreen) * fade / toScreen), uv, u);
    }
    // Distance from the rays' origin of the one game pixel at uv, as the mesh would place it with a vertex there.
    float pixel_radial(float2 uv, constant Uniforms &u, texture2d<float> depth) {
        float toScreen = length(screen_point(uv, u) - float3(0, 0, u.eyeBack));
        float r = radial(uv, toScreen, u, depth, false);
        return u.trueDepth != 0 ? r : toScreen + (r - toScreen) * edge_fade(uv);
    }
    // The per-pixel depth search (LB_IMMERSIVE_REFINE steps). The mesh has a vertex every few game pixels, so on
    // its own it gives an object's outline, and anything thinner than a cell, the wrong depth. Starting from the
    // mesh, each step takes the depth of the game pixel the eye's ray is currently thought to show, moves to where
    // the ray reaches that distance, and looks up which game pixel is there. On a surface this settles at once.
    float2 refined_uv(Out in, constant Uniforms &u, texture2d<float> depth) {
        float3 origin = float3(0, 0, u.eyeBack), eye = u.eyeRefine.xyz;
        float3 a = eye - origin, d = normalize(in.local - eye);
        float ad = dot(a, d), aa = dot(a, a), rBefore = 0;
        float2 uv = in.uv, before = uv;
        float2 texels = float2(depth.get_width(), depth.get_height());
        for (int i = 0; i < int(u.eyeRefine.w); i++) {
            before = uv;
            rBefore = pixel_radial(uv, u, depth);
            float3 fromOrigin = eye + d * (-ad + sqrt(max(ad * ad - aa + rBefore * rBefore, 0.0))) - origin;
            float3 on = fromOrigin * (u.eyeBack / max(-fromOrigin.z, 1e-6));
            uv = saturate(float2(((on.x - u.screenCentre.x) / u.halfSize.x + 1) * 0.5,
                                 (1 - (on.y - u.screenCentre.y) / u.halfSize.y) * 0.5));
            if (u.refineExit > 0) {
                // Settled (LB_IMMERSIVE_REFINE_EXIT): the step moved less than refineExit game pixels and stayed on
                // the depth texel it read, so another step would read the same depth again and move less still,
                // and the test below would find no jump. Most pixels (those on a surface) leave here after one
                // depth fetch. The distance alone is not enough: a short step that crosses onto the far side of
                // a depth edge is one the next step would move by many pixels.
                float2 moved = (uv - before) * texels;
                if (dot(moved, moved) < u.refineExit * u.refineExit && all(floor(uv * texels) == floor(before * texels)))
                    return uv;
            }
        }
        // A ray that keeps jumping between a near and a far surface sees neither's own pixel: it looks past the
        // near surface's edge at something the game's camera did not see. Show the far one there.
        return rBefore > pixel_radial(uv, u, depth) * (1.0 + u.tear) ? before : uv;
    }
    fragment float4 screen_fragment(Out in [[stage_in]], texture2d<float> image [[texture(0)]],
                                    texture2d<float> depth [[texture(1)]], constant Uniforms &u [[buffer(0)]]) {
        constexpr sampler linear_clamp(address::clamp_to_edge, filter::linear);
        if (u.backplate != 0) {
            // The fill behind the mesh, seen only through the gaps at depth edges: a wide blur of the picture, so a
            // gap shows the colours around it instead of black, and no sharp second copy of a near object.
            float3 sum = float3(0);
            for (int i = 0; i < 12; i++) {
                float a = float(i) * 0.5235988, rr = (i & 1) ? 0.035 : 0.018;
                sum += image.sample(linear_clamp, saturate(in.uv + rr * float2(cos(a) * 0.5625, sin(a)))).rgb;
            }
            float3 b = sum / 12.0;
            if (u.decode) b = select(pow((b + 0.055) / 1.055, 2.4), b / 12.92, b <= 0.04045);
            return float4(b, 1);
        }
        float2 uv = u.eyeRefine.w > 0 && u.hasDepth != 0 ? refined_uv(in, u, depth) : in.uv;
        float3 c = image.sample(linear_clamp, uv).rgb;
        if (u.sharpen > 0) {
            // Unsharp mask over the four neighbouring texels, limited to their range so edges don't ring.
            float2 t = 1.0 / float2(image.get_width(), image.get_height());
            float3 n = image.sample(linear_clamp, uv + float2(0, -t.y)).rgb;
            float3 s = image.sample(linear_clamp, uv + float2(0, t.y)).rgb;
            float3 w = image.sample(linear_clamp, uv + float2(-t.x, 0)).rgb;
            float3 e = image.sample(linear_clamp, uv + float2(t.x, 0)).rgb;
            float3 lo = min(c, min(min(n, s), min(w, e))), hi = max(c, max(max(n, s), max(w, e)));
            c = clamp(c + u.sharpen * (4.0 * c - n - s - w - e), lo, hi);
        }
        if (u.decode) c = select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
        return float4(c, 1);
    }
    // LB_HUD_LAYER: the interface as a flat quad (rect: centre xy, half size zw, on the plane z of the frame's space).
    struct HudUniforms { float4x4 mvp; float4 rect; float z; };
    struct HudOut { float4 position [[position]]; float2 uv; };
    vertex HudOut hud_vertex(uint id [[vertex_id]], constant HudUniforms &h [[buffer(0)]]) {
        float2 uv = float2(id & 1, id >> 1);
        HudOut o;
        o.uv = uv;
        o.position = h.mvp * float4(h.rect.x + (uv.x * 2 - 1) * h.rect.z, h.rect.y + (1 - uv.y * 2) * h.rect.w, h.z, 1);
        return o;
    }
    // The game blended the interface onto transparent black: colour times coverage a, and alpha a * a (the same
    // blend applied to alpha). Premultiplied colour over the scene with coverage sqrt(alpha).
    fragment float4 hud_fragment(HudOut in [[stage_in]], texture2d<float> hud [[texture(0)]]) {
        constexpr sampler linear_clamp(filter::linear, address::clamp_to_edge);
        float4 c = hud.sample(linear_clamp, in.uv);
        return float4(c.rgb, sqrt(saturate(c.a)));
    }
    """

    /// Distance to the screen and its width, in meters.
    private static let distance: Float = 3.0
    private static let width: Float = 2.8

    private let layer: LayerRenderer
    private let device: any MTLDevice
    private let queue: any MTLCommandQueue
    private var pipeline: (any MTLRenderPipelineState)?
    private var depth: (any MTLDepthStencilState)?
    private var hudPipeline: (any MTLRenderPipelineState)?
    private var hudDepth: (any MTLDepthStencilState)?
    /// LB_HUD_DISTANCE: metres ahead of the frame's camera at which the interface layer (LB_HUD_LAYER) is shown.
    private lazy var hudDistance: Float = getenv("LB_HUD_DISTANCE").flatMap { Float(String(cString: $0)) }.map { max(0.3, $0) } ?? 2.0
    /// LB_HUD_SCALE (0.28) and LB_HUD_OFFSET (degrees right,up; 0,0): the interface layer's size as a fraction of the
    /// game's field of view and where its centre goes, while a frame is placed by its camera. The game lays the HUD
    /// out against the edges of a frame 124 by 104 degrees across; at 1 it is shown there (HUDLayerPlacement).
    private lazy var hudPlacement = HUDLayerPlacement(scaleSetting: getenv("LB_HUD_SCALE").map { String(cString: $0) },
                                                      offsetSetting: getenv("LB_HUD_OFFSET").map { String(cString: $0) })
    private var hudLogged: [Float] = []
    private let session = ARKitSession()
    private let tracking = WorldTrackingProvider()
    private var screen: simd_float4x4?
    private var lastCommands: (any MTLCommandBuffer)?
    private var checkedDevice = false
    private var presented = 0, newFrames = 0, loggedAt = CACurrentMediaTime()
    /// Eye views: the angle between the direction the left eye's picture was rendered looking and where that eye
    /// looks when it is shown (degrees): what the compositor still has to correct (plans/eye-views.md 2.3).
    private var residualSum: Double = 0, residualMax: Double = 0, residualCount = 0
    /// The compositor's own GPU time per frame (its command buffers' GPU start to end), for the log.
    private let gpuTime = GPUTimeTally()
    /// Grid size: one vertex every 4 game pixels.
    private static let gridColumns: UInt32 = 321, gridRows: UInt32 = 181
    /// LB_IMMERSIVE_GRID=<columns>x<rows> (for example 161x91): the depth mesh's size instead of 321x181. Fewer
    /// vertices cost less in the vertex stage; the per-pixel search (LB_IMMERSIVE_REFINE) settles each pixel's depth
    /// either way, but at a coarser grid a thin object's outline can come out a little different (see
    /// docs/perf/IMMERSIVE_OPTIONS.md). Read when the first game frame is drawn. Unset or unreadable: 321x181.
    private lazy var grid: SIMD2<UInt32> = {
        let fallback = SIMD2(Self.gridColumns, Self.gridRows)
        guard let raw = getenv("LB_IMMERSIVE_GRID") else { return fallback }
        let parts = String(cString: raw).split(separator: "x").compactMap { UInt32($0) }
        guard parts.count == 2 else {
            log("mesh grid: LB_IMMERSIVE_GRID not <columns>x<rows>; \(fallback.x)x\(fallback.y)")
            return fallback
        }
        let size = SIMD2(max(9, min(parts[0], 1281)), max(9, min(parts[1], 721)))
        log("mesh grid \(size.x)x\(size.y)")
        return size
    }()
    private var depthSettings: (scale: Float, maxBehind: Float, near: Float, units: Float, tear: Float, on: Bool)?
    private var loggedDepth = false
    private var loggedStereo = false
    /// LB_STEREO_EYE_UNITS: half the game's eye separation in game units (the game's left and right cameras).
    private lazy var stereoEyeUnits: Float = getenv("LB_STEREO_EYE_UNITS").flatMap { Float(String(cString: $0)) }.map { abs($0) } ?? 2.24
    /// Head tracking (LB_IMMERSIVE_HEAD=1, docs/HEAD_TRACKING.md): the head steers the game camera and each frame
    /// is drawn where its camera pointed, at true depth. The head's starting heading and position.
    /// Read once the game is running: launch-env.json is applied when the game starts, after this layer opens.
    private var headModeSetting: Bool?
    private var headMode: Bool {
        if let headModeSetting { return headModeSetting }
        guard checkedDevice else { return false }  // the game is presenting, so it has started
        let on = getenv("LB_IMMERSIVE_HEAD").map { String(cString: $0) == "1" } ?? false
        headModeSetting = on
        log("head tracking \(on ? "on" : "off")")
        return on
    }
    private var headStartYaw: Float?
    private var lastTracked = false
    private var headStartPosition = SIMD3<Float>(0, 0, 0)
    /// LB_EYE_VIEWS=1 (with LB_IMMERSIVE_HEAD=1 and LB_STEREO_MULTIVIEW=1): the game renders each eye's picture
    /// through that eye's own frustum and from its own place (skyrim_stereo_set_eyes), so each display pixel shows
    /// its own game pixel when the head hasn't moved since. The frusta are clipped to a window W (tangents, the
    /// same for both eyes): LB_EYE_WINDOW=<x>x<y>, for example 0.85x0.6; unset, the game's own frustum less
    /// LB_EYE_MARGIN degrees (default 2) on every side, so the eyes never look past what the game drew. The game's
    /// frustum (LB_FOV and LB_WINDOW's aspect) must cover W plus the margin.
    private var eyeViewsSetting: Bool?
    private var eyeViews: Bool {
        if let eyeViewsSetting { return eyeViewsSetting }
        guard checkedDevice else { return false }
        let on = getenv("LB_EYE_VIEWS").map { String(cString: $0) == "1" } ?? false
        eyeViewsSetting = on
        if on { log("eye views on: the game renders each eye's own frustum") }
        return on
    }
    private lazy var eyeWindow: SIMD2<Float>? = {
        guard let raw = getenv("LB_EYE_WINDOW") else { return nil }
        let parts = String(cString: raw).split(separator: "x").compactMap { Float($0) }
        return parts.count == 2 && parts[0] > 0 && parts[1] > 0 ? SIMD2(parts[0], parts[1]) : nil
    }()
    private lazy var eyeMarginDegrees: Float = getenv("LB_EYE_MARGIN").flatMap { Float(String(cString: $0)) } ?? 2
    /// What was last given to skyrim_stereo_set_eyes, so it is only called when the eyes or the window change.
    private var givenEyes: [Float] = []
    /// The game goes back to its own frustum (tracking lost, a menu without a camera, the layer ending).
    private func dropEyes() {
        guard !givenEyes.isEmpty else { return }
        givenEyes = []
        skyrim_stereo_clear_eyes()
        log("eyes dropped: the game renders its own frustum")
    }

    /// Gives the game the eyes of this drawable's first two views: their tangents clipped to the window and their
    /// poses relative to the device, whose starting place the frame's rays start from (the game camera follows the
    /// head), with the eye separation scaled about the point between them.
    private func giveEyes(_ drawable: LayerRenderer.Drawable, camera c: [Float], unitsPerMetre: Float) {
        guard drawable.views.count >= 2 else { return }
        // The game's field of view follows the eyes (LB_FOV unset or "eyes"): ask for a frustum that holds both eyes'
        // whole frusta plus the margin, so the window below clips nothing once the game has widened.
        let margin = eyeMarginDegrees * .pi / 180
        var needX: Float = 0, needY: Float = 0
        for view in drawable.views.prefix(2) {
            let t = view.tangents  // positive extents: left, right, top, bottom
            needX = max(needX, tan(atan(max(abs(t[0]), abs(t[1]))) + margin))
            needY = max(needY, tan(atan(max(abs(t[2]), abs(t[3]))) + margin))
        }
        skyrim_eye_fov_request(needX, needY)
        var window: (l: Float, r: Float, t: Float, b: Float)
        if let eyeWindow {
            window = (-eyeWindow.x, eyeWindow.x, eyeWindow.y, -eyeWindow.y)
        } else {
            // The game frustum's angles less the margin on each side.
            let m = eyeMarginDegrees * .pi / 180
            func inset(_ tangent: Float) -> Float { tan(atan(tangent) - m * (tangent < 0 ? -1 : 1)) }
            window = (inset(c[12]), inset(c[13]), inset(c[14]), inset(c[15]))
        }
        let centre = (drawable.views[0].transform.columns.3 + drawable.views[1].transform.columns.3) / 2
        var tangents: [Float] = [], poses: [Float] = []
        for view in drawable.views.prefix(2) {
            // visionOS gives each tangent as a positive extent from the view axis.
            let t = view.tangents
            tangents += [max(-abs(t[0]), window.l), min(abs(t[1]), window.r), min(abs(t[2]), window.t), max(-abs(t[3]), window.b)]
            var pose = view.transform
            pose.columns.3 = centre + (pose.columns.3 - centre) * eyeSeparationScale
            for column in 0..<4 { for row in 0..<4 { poses.append(pose[column][row]) } }
        }
        let key = tangents + poses + [unitsPerMetre]
        if key.count == givenEyes.count, zip(key, givenEyes).allSatisfy({ abs($0 - $1) < 1e-5 }) { return }
        givenEyes = key
        skyrim_stereo_set_eyes(tangents, poses, unitsPerMetre)
        func f(_ v: Float) -> String { String(format: "%.3f", v) }
        log("eyes: tangents L \(tangents[0..<4].map(f)) R \(tangents[4..<8].map(f)); raw L \([0, 1, 2, 3].map { f(drawable.views[0].tangents[$0]) }) R \([0, 1, 2, 3].map { f(drawable.views[1].tangents[$0]) }); eye x \(f(poses[12])) \(f(poses[28])) m, \(f(unitsPerMetre)) units/m")
    }

    /// Distance of the plane the frame's grid is laid on before depth moves it (any value works in true depth).
    private static let viewPlane: Float = 2.0

    /// LB_IMMERSIVE_UPSCALE=<factor> (1.25 to 3, for example 2): the game image goes through a MetalFX spatial
    /// upscale to factor times its size before it is drawn, so the game can render small (LB_WINDOW) and still
    /// be shown sharp. Unset, 0 or 1: off. LB_IMMERSIVE_SHARPEN=<0 to 1>: sharpening when the image is drawn,
    /// with or without the upscale; off by default. Both are read once the game is presenting.
    private var imageSettings: (upscale: Float, sharpen: Float)?
    private func readImageSettings() -> (upscale: Float, sharpen: Float) {
        if let imageSettings { return imageSettings }
        func number(_ name: String) -> Float { getenv(name).flatMap { Float(String(cString: $0)) } ?? 0 }
        var upscale = number("LB_IMMERSIVE_UPSCALE")
        upscale = upscale > 1 ? min(max(upscale, 1.25), 3) : 0
        let settings = (upscale: upscale, sharpen: min(max(number("LB_IMMERSIVE_SHARPEN"), 0), 1))
        imageSettings = settings
        log("image: upscale \(settings.upscale > 0 ? "x\(settings.upscale)" : "off"), sharpen \(settings.sharpen)")
        return settings
    }
    #if canImport(MetalFX)
    private var scaler: (any MTLFXSpatialScaler)?
    #endif
    private var upscaled: (any MTLTexture)?
    private var upscaledFrom: (width: Int, height: Int, format: MTLPixelFormat)?
    private var upscaleUnavailable = false

    /// The game image after the MetalFX upscale, encoded into `commands` when the frame is new; nil when the
    /// upscale is off or can't be made (logged once), and the caller draws the game image as it is.
    private func upscale(_ texture: any MTLTexture, isNew: Bool, encoded: Bool, factor: Float,
                         commands: any MTLCommandBuffer) -> (any MTLTexture)? {
        guard factor > 1, !upscaleUnavailable else { return nil }
        #if canImport(MetalFX)
        var fresh = false
        if scaler == nil || upscaled == nil || upscaledFrom?.width != texture.width
            || upscaledFrom?.height != texture.height || upscaledFrom?.format != texture.pixelFormat {
            scaler = nil
            upscaled = nil
            guard MTLFXSpatialScalerDescriptor.supportsDevice(device) else {
                upscaleUnavailable = true
                log("upscale: MetalFX spatial scaling is not supported on this device; drawing the game image as it is")
                return nil
            }
            let description = MTLFXSpatialScalerDescriptor()
            description.inputWidth = texture.width
            description.inputHeight = texture.height
            description.outputWidth = Int((Float(texture.width) * factor).rounded())
            description.outputHeight = Int((Float(texture.height) * factor).rounded())
            description.colorTextureFormat = texture.pixelFormat
            description.outputTextureFormat = texture.pixelFormat
            // Values the shader still has to decode from sRGB are perceptual; an sRGB texture reads as linear.
            description.colorProcessingMode = encoded ? .perceptual : .linear
            guard let made = description.makeSpatialScaler(device: device) else {
                upscaleUnavailable = true
                log("upscale: no MetalFX scaler for \(texture.width)x\(texture.height) format \(texture.pixelFormat.rawValue); drawing the game image as it is")
                return nil
            }
            let target = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: texture.pixelFormat, width: description.outputWidth,
                                                                  height: description.outputHeight, mipmapped: false)
            target.usage = made.outputTextureUsage.union(.shaderRead)
            target.storageMode = .private
            guard let output = device.makeTexture(descriptor: target) else {
                upscaleUnavailable = true
                log("upscale: no \(description.outputWidth)x\(description.outputHeight) texture; drawing the game image as it is")
                return nil
            }
            scaler = made
            upscaled = output
            upscaledFrom = (texture.width, texture.height, texture.pixelFormat)
            fresh = true
            log("upscale: MetalFX spatial \(texture.width)x\(texture.height) -> \(output.width)x\(output.height), format \(texture.pixelFormat.rawValue), \(encoded ? "perceptual" : "linear")")
        }
        guard let scaler, let upscaled else { return nil }
        if isNew || fresh {
            scaler.colorTexture = texture
            scaler.outputTexture = upscaled
            scaler.inputContentWidth = texture.width
            scaler.inputContentHeight = texture.height
            scaler.encode(commandBuffer: commands)
        }
        return upscaled
        #else
        upscaleUnavailable = true
        log("upscale: MetalFX is not in this SDK (visionOS Simulator); drawing the game image as it is")
        return nil
        #endif
    }

    /// LB_IMMERSIVE_SEED=1, with the per-pixel search on (LB_IMMERSIVE_REFINE): the depth each mesh vertex needs is
    /// worked out once per game frame, into a small texture (one texel per grid vertex), and the mesh is drawn
    /// indexed from it, instead of every vertex of every triangle fetching it again for each eye at every refresh
    /// (see seed_fragment in the shaders). The picture is meant to be the same to the bit. Unset: off. Read when
    /// the first game frame with depth is drawn.
    private lazy var seedMesh: Bool = {
        let on = getenv("LB_IMMERSIVE_SEED").map { String(cString: $0) == "1" } ?? false
        if on { log("mesh seed on: depth for the mesh once per game frame, indexed mesh") }
        return on
    }()
    private var library: (any MTLLibrary)?
    private var seedPipeline: (any MTLRenderPipelineState)?
    private var seededPipeline: (any MTLRenderPipelineState)?
    private var seedUnavailable = false
    /// The grid's triangles in screen_vertex's order: per cell, corners (0,0) (1,0) (0,1) and (1,0) (1,1) (0,1).
    private var gridIndices: (buffer: any MTLBuffer, count: Int, type: MTLIndexType, grid: SIMD2<UInt32>)?
    /// Counts the game frames taken from the bridge. The bridge hands out the same few depth textures again and
    /// again, so a texture alone does not say which frame a seed was made from.
    private var frameGeneration = 0
    /// The seeds: [0] from the frame's depth, [1] from the right eye's own depth (stereo frames).
    private var seeds: [(texture: any MTLTexture, source: ObjectIdentifier, generation: Int, grid: SIMD2<UInt32>)?] = [nil, nil]

    /// The two pipelines and the index buffer LB_IMMERSIVE_SEED draws with, made when it is first used.
    private func prepareSeed(grid: SIMD2<UInt32>) -> Bool {
        if seedPipeline != nil, seededPipeline != nil, let gridIndices, gridIndices.grid == grid { return true }
        guard let library else { return false }
        do {
            let fill = MTLRenderPipelineDescriptor()
            fill.vertexFunction = library.makeFunction(name: "seed_vertex")
            fill.fragmentFunction = library.makeFunction(name: "seed_fragment")
            fill.colorAttachments[0].pixelFormat = .r32Float
            seedPipeline = try device.makeRenderPipelineState(descriptor: fill)
            let mesh = MTLRenderPipelineDescriptor()
            mesh.vertexFunction = library.makeFunction(name: "screen_vertex_seeded")
            mesh.fragmentFunction = library.makeFunction(name: "screen_fragment")
            mesh.colorAttachments[0].pixelFormat = layer.configuration.colorFormat
            mesh.depthAttachmentPixelFormat = layer.configuration.depthFormat
            seededPipeline = try device.makeRenderPipelineState(descriptor: mesh)
        } catch {
            log("mesh seed: no pipeline: \(error)")
            return false
        }
        let columns = Int(grid.x), rows = Int(grid.y)
        let corners = [(0, 0), (1, 0), (0, 1), (1, 0), (1, 1), (0, 1)]
        var indices: [UInt32] = []
        indices.reserveCapacity((columns - 1) * (rows - 1) * 6)
        for y in 0..<(rows - 1) {
            for x in 0..<(columns - 1) {
                for corner in corners { indices.append(UInt32((y + corner.1) * columns + x + corner.0)) }
            }
        }
        // 16-bit indices while every vertex has one below 0xFFFF (321x181 has 58,101 vertices).
        let short = columns * rows < 0xFFFF
        let buffer: (any MTLBuffer)?
        if short {
            buffer = indices.map { UInt16($0) }.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count) }
        } else {
            buffer = indices.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count) }
        }
        guard let buffer else {
            log("mesh seed: no index buffer")
            return false
        }
        gridIndices = (buffer: buffer, count: indices.count, type: short ? MTLIndexType.uint16 : MTLIndexType.uint32, grid: grid)
        return true
    }

    /// The seed made from depthTexture for the game frame now shown, filled in `commands` first if seeds[slot]
    /// holds another frame's. nil (logged once) when it can't be made: the mesh is then drawn as without the switch.
    private func seed(_ slot: Int, from depthTexture: any MTLTexture, grid: SIMD2<UInt32>,
                      commands: any MTLCommandBuffer) -> (any MTLTexture)? {
        guard !seedUnavailable else { return nil }
        let source = ObjectIdentifier(depthTexture)
        if let have = seeds[slot], have.source == source, have.generation == frameGeneration, have.grid == grid {
            return have.texture
        }
        var target: (any MTLTexture)?
        if prepareSeed(grid: grid), let seedPipeline {
            target = seeds[slot].flatMap { $0.grid == grid ? $0.texture : nil }
            if target == nil {
                let description = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: Int(grid.x),
                                                                           height: Int(grid.y), mipmapped: false)
                description.usage = [.renderTarget, .shaderRead]
                description.storageMode = .private
                target = device.makeTexture(descriptor: description)
            }
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            if target != nil, let encoder = commands.makeRenderCommandEncoder(descriptor: pass) {
                var size = grid
                encoder.setRenderPipelineState(seedPipeline)
                encoder.setFragmentBytes(&size, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 0)
                encoder.setFragmentTexture(depthTexture, index: 1)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                encoder.endEncoding()
            } else {
                target = nil
            }
        }
        guard let target else {
            seedUnavailable = true
            seeds = [nil, nil]
            log("mesh seed: unavailable; drawing the mesh without it")
            return nil
        }
        seeds[slot] = (texture: target, source: source, generation: frameGeneration, grid: grid)
        return target
    }

    init(layer: LayerRenderer) {
        self.layer = layer
        device = layer.device
        queue = layer.device.makeCommandQueue()!
    }

    func start() {
        let thread = Thread { [self] in run() }
        thread.name = "Skyrim immersive compositor"
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    private func log(_ text: String) { fputs("immersive: \(text)\n", stderr) }

    private func makePipeline() throws {
        let library = try device.makeLibrary(source: Self.shaders, options: nil)
        self.library = library
        let description = MTLRenderPipelineDescriptor()
        description.vertexFunction = library.makeFunction(name: "screen_vertex")
        description.fragmentFunction = library.makeFunction(name: "screen_fragment")
        description.colorAttachments[0].pixelFormat = layer.configuration.colorFormat
        description.depthAttachmentPixelFormat = layer.configuration.depthFormat
        pipeline = try device.makeRenderPipelineState(descriptor: description)
        let stencil = MTLDepthStencilDescriptor()
        stencil.depthCompareFunction = .greater
        stencil.isDepthWriteEnabled = true
        depth = device.makeDepthStencilState(descriptor: stencil)
        let hud = MTLRenderPipelineDescriptor()
        hud.vertexFunction = library.makeFunction(name: "hud_vertex")
        hud.fragmentFunction = library.makeFunction(name: "hud_fragment")
        hud.colorAttachments[0].pixelFormat = layer.configuration.colorFormat
        hud.colorAttachments[0].isBlendingEnabled = true
        hud.colorAttachments[0].sourceRGBBlendFactor = .one
        hud.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        hud.colorAttachments[0].sourceAlphaBlendFactor = .one
        hud.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        hud.depthAttachmentPixelFormat = layer.configuration.depthFormat
        hudPipeline = try device.makeRenderPipelineState(descriptor: hud)
        let always = MTLDepthStencilDescriptor()
        always.depthCompareFunction = .always
        always.isDepthWriteEnabled = false
        hudDepth = device.makeDepthStencilState(descriptor: always)
    }

    /// Depth tuning from the launch environment (LB_ prefix, Documents/SkyrimNative/launch-env.json), read once the
    /// game is running: LB_IMMERSIVE_DEPTH=0 shows the flat screen; LB_IMMERSIVE_DEPTH_SCALE, _SCREEN_M, _NEAR,
    /// _UNITS_PER_M override the defaults.
    private func readDepthSettings() -> (scale: Float, maxBehind: Float, near: Float, units: Float, tear: Float, on: Bool) {
        if let depthSettings { return depthSettings }
        func number(_ name: String, _ fallback: Float) -> Float {
            guard let raw = getenv(name), let value = Float(String(cString: raw)) else { return fallback }
            return value
        }
        let on = getenv("LB_IMMERSIVE_DEPTH").map { String(cString: $0) != "0" } ?? true
        let settings = (scale: number("LB_IMMERSIVE_DEPTH_SCALE", 0.15), maxBehind: number("LB_IMMERSIVE_DEPTH_MAX_M", 30.0),
                        near: number("LB_IMMERSIVE_DEPTH_NEAR", 15.0), units: number("LB_IMMERSIVE_DEPTH_UNITS_PER_M", 70.0),
                        tear: number("LB_IMMERSIVE_DEPTH_TEAR", 0.08), on: on)
        depthSettings = settings
        log("depth \(on ? "on" : "off"): scale \(settings.scale) m/m, up to \(settings.maxBehind) m behind, near \(settings.near) units, \(settings.units) units/m, tear ratio \(settings.tear)")
        return settings
    }

    /// One compositor at a time owns the game's frames. The space can be closed and opened again while the game
    /// runs ("Return to Immersive"), and each opening makes a new compositor; the new one waits here until the old
    /// one has finished, so the old one's skyrim_immersive_stop() can't land after the new one's start and switch
    /// its frames off. The wait is bounded: an old compositor that never ends must not keep the new one dark.
    private static let lifetime = NSLock()

    private func run() {
        do {
            try makePipeline()
        } catch {
            log("no pipeline: \(error)")
            return
        }
        let alone = Self.lifetime.lock(before: Date(timeIntervalSinceNow: 5))
        defer { if alone { Self.lifetime.unlock() } }
        if !alone { log("the previous compositor has not finished after 5 s; starting anyway") }
        skyrim_immersive_start()
        skyrim_sdl_set_window_ghost(1)
        DispatchQueue.main.async { NotificationCenter.default.post(name: .skyrimImmersiveRunning, object: true) }
        Task { [session, tracking] in
            do { try await session.run([tracking]) } catch { fputs("immersive: no world tracking: \(error)\n", stderr) }
        }
        log("layer \(layer.configuration.colorFormat.rawValue), foveation \(layer.configuration.isFoveationEnabled ? "on" : "off"), started")
        while true {
            switch layer.state {
            case .invalidated:
                finish()
                return
            case .paused:
                layer.waitUntilRunning()
            case .running:
                autoreleasepool { draw() }
            @unknown default:
                finish()
                return
            }
        }
    }

    /// Meters in the headset per game meter in head-tracking mode (LB_IMMERSIVE_HEAD_SCALE; 1 = true scale).
    private lazy var headDepthScale: Float = getenv("LB_IMMERSIVE_HEAD_SCALE").flatMap { Float(String(cString: $0)) } ?? 1.0
    /// With LB_IMMERSIVE_MOTION=1: how near the eye (meters) something must be to count as the player's own hands
    /// and weapon, which travel with the camera (LB_IMMERSIVE_MOTION_NEAR_M; 0 moves everything with the world).
    private lazy var motionNear: Float = getenv("LB_IMMERSIVE_MOTION_NEAR_M").flatMap { Float(String(cString: $0)) } ?? 1.0

    /// Per-pixel depth search steps (LB_IMMERSIVE_REFINE; 0 = off, 2 is enough) and the eye separation used for
    /// drawing as a multiple of the wearer's (LB_IMMERSIVE_IPD_SCALE; above 1 gives stronger depth and a smaller
    /// looking world). Both read when the first game frame is drawn, like the depth settings.
    private lazy var refineSteps: Float = {
        let steps = getenv("LB_IMMERSIVE_REFINE").flatMap { Float(String(cString: $0)) } ?? 0
        log("per-pixel depth search: \(Int(steps)) steps")
        return max(0, min(steps.rounded(), 8))
    }()
    private lazy var eyeSeparationScale: Float = {
        let scale = getenv("LB_IMMERSIVE_IPD_SCALE").flatMap { Float(String(cString: $0)) } ?? 1
        log("eye separation x\(scale)")
        return max(0, min(scale, 4))
    }()

    /// LB_IMMERSIVE_REFINE_EXIT=<game pixels> (for example 0.25): the per-pixel depth search leaves a pixel as soon
    /// as a step moves it by less than this without leaving the depth texel it read, instead of running every
    /// step and the final test on every pixel. Unset or 0: off. tools/compositor_reference.py compares the two.
    private lazy var refineExit: Float = {
        let pixels = min(getenv("LB_IMMERSIVE_REFINE_EXIT").flatMap { Float(String(cString: $0)) } ?? 0, 4)
        guard pixels > 0 else { return 0 }
        log("per-pixel depth search: stops once a step is under \(pixels) game pixels on the same depth texel")
        return pixels
    }()

    /// LB_FRAME_REPEAT=1: the layer shows each compositor frame for two display refreshes
    /// (LayerRenderer.minimumFrameRepeatCount = 1), so the compositor draws at half the display rate: 45 Hz on a
    /// 90 Hz display, every pass half as often. Only 1 is taken: the bridge's display tick (skyrim_display_tick)
    /// paces the game by whole compositor frames, and at 45 Hz a 45 fps game is one tick a frame; at 2 (30 Hz) it
    /// would no longer fit a whole number and fall back to its timer. Read and applied once the game is presenting
    /// (launch-env.json arrives after this layer opens). Unset: the layer's default (no repeat).
    private var frameRepeatSetting: Int?
    private var frameRepeat: Int {
        if let frameRepeatSetting { return frameRepeatSetting }
        guard checkedDevice else { return 0 }
        let raw = getenv("LB_FRAME_REPEAT").map { String(cString: $0) } ?? ""
        let value = raw == "1" ? 1 : 0
        if !raw.isEmpty, value == 0 { log("frame repeat: LB_FRAME_REPEAT=\(raw) not taken (only 1)") }
        if value == 1 {
            layer.minimumFrameRepeatCount = 1
            shownGapSum = 0; shownGapCount = 0  // the check counts only frames after this
            log("frame repeat: each frame shown for 2 refreshes (layer asks for at least \(layer.minimumFrameRepeatCount))")
        }
        frameRepeatSetting = value
        return value
    }
    /// An easing factor tuned per compositor frame at the full display rate, for the rate the compositor runs at:
    /// the same easing per second. Exactly f with no frame repeat.
    private func perFrame(_ f: Float) -> Float {
        let repeats = frameRepeat
        return repeats == 0 ? f : 1 - pow(1 - f, Float(repeats + 1))
    }
    /// Pacing check for LB_FRAME_REPEAT: the time between the presentation times of successive compositor frames,
    /// before the repeat is applied (the display's own period) and after, for the 5 s log.
    private var lastShownAt: Double?
    private var shownGapSum: Double = 0, shownGapCount = 0
    private var displayPeriod: Double?

    /// LB_IMMERSIVE_PASS_TIME=1: the eyes' passes' vertex and fragment stage times in the 5 s log (PassTimer).
    /// Read once the game is presenting.
    private var passTimerSetting: Bool?
    private var passTimer: PassTimer?
    private var passTiming: Bool {
        if let passTimerSetting { return passTimerSetting }
        guard checkedDevice else { return false }
        let wanted = getenv("LB_IMMERSIVE_PASS_TIME").map { String(cString: $0) == "1" } ?? false
        if wanted {
            passTimer = PassTimer(device: device)
            log(passTimer != nil ? "pass time on: vertex and fragment stage timestamps on each eye's pass"
                                 : "pass time: this GPU takes no timestamps at stage boundaries")
        }
        passTimerSetting = wanted && passTimer != nil
        return passTimerSetting!
    }
    /// What the drawables and rate maps last logged looked like, so each is logged when it changes (at most 8 times).
    private var loggedDrawable = "", loggedDrawableCount = 0
    private var loggedRateMaps = "", loggedRateMapCount = 0
    /// Calls to logDrawable; it looks only at every 90th (about once a second), so a steady drawable costs no
    /// string building at the display rate.
    private var drawableLogTick = 0
    /// How far the eye was from where the mesh's rays start (meters), over the drawn eyes: how much the per-pixel
    /// search had to correct, to read next to the cost switches.
    private var eyeOffsetSum: Double = 0, eyeOffsetMax: Double = 0, eyeOffsetCount = 0

    /// Logs the drawable's texture and viewport sizes and its rasterization rate maps, when they change (checked
    /// about once a second).
    private func logDrawable(_ drawable: LayerRenderer.Drawable) {
        drawableLogTick += 1
        guard drawableLogTick % 90 == 1 else { return }
        if loggedDrawableCount < 8 {
            let colour = drawable.colorTextures.map { "\($0.width)x\($0.height)x\($0.arrayLength)" }.joined(separator: " ")
            let depth = drawable.depthTextures.map { "\($0.width)x\($0.height)" }.joined(separator: " ")
            let viewports = drawable.views.map { "\(Int($0.textureMap.viewport.width))x\(Int($0.textureMap.viewport.height))" }.joined(separator: " ")
            let text = "drawable: colour \(colour), depth \(depth), \(drawable.views.count) views, viewports \(viewports)"
            if text != loggedDrawable {
                loggedDrawable = text
                loggedDrawableCount += 1
                log(text)
            }
        }
        let maps = drawable.rasterizationRateMaps
        if !maps.isEmpty, loggedRateMapCount < 8 {
            let text = "rate maps: " + maps.map { map in
                let layers = (0..<map.layerCount).map { layer -> String in
                    let size = map.physicalSize(layer: layer)
                    return "\(size.width)x\(size.height)"
                }.joined(separator: " ")
                return "screen \(map.screenSize.width)x\(map.screenSize.height), physical \(layers)"
            }.joined(separator: "; ")
            if text != loggedRateMaps {
                loggedRateMaps = text
                loggedRateMapCount += 1
                log(text)
            }
        }
    }

    private func finish() {
        skyrim_sdl_set_window_ghost(0)
        DispatchQueue.main.async { NotificationCenter.default.post(name: .skyrimImmersiveRunning, object: false) }
        if headMode { skyrim_head_set(0, 0, 0) }
        dropEyes()
        lastCommands?.waitUntilCompleted()
        skyrim_immersive_stop()
        session.stop()
        log("layer ended")
    }

    /// The screen, placed once: straight ahead of where the wearer is looking, level, at eye height.
    /// LB_IMMERSIVE_FOLLOW: 1 = the screen is locked to the head on every axis, always straight ahead of the eyes
    /// whichever way they face (lying down looking at the ceiling included), eased so small head motion doesn't
    /// shake it; "yaw" = it follows only the heading and stays level. With head tracking on, it applies to the
    /// menu and loading screens only. Read once the game is presenting, as LB_IMMERSIVE_HEAD is.
    private var followSetting: String?
    private var followMode: String {
        if let followSetting { return followSetting }
        guard checkedDevice else { return "" }
        let mode = getenv("LB_IMMERSIVE_FOLLOW").map { String(cString: $0) } ?? ""
        followSetting = mode
        if !mode.isEmpty { log("screen follows the head: \(mode)") }
        return mode
    }
    private var followHead: Bool { followMode == "1" || followMode == "yaw" }
    private var followYaw: Float?
    /// LB_IMMERSIVE_HOLES (default on): the gaps the depth mesh leaves at object edges show a blurred fill at the
    /// farthest depth instead of black; 0 = black.
    private lazy var fillHoles: Bool = getenv("LB_IMMERSIVE_HOLES").map { String(cString: $0) != "0" } ?? true
    private var followRotation: simd_quatf?
    /// `distance`: how far ahead a head-locked screen (LB_IMMERSIVE_FOLLOW=1) sits; viewPlane when it is drawn at the
    /// game's field of view, the window distance otherwise (menus), so its size and ray origin match how it is drawn.
    private func place(head: simd_float4x4, distance: Float = ImmersiveCompositor.distance) -> simd_float4x4 {
        if let screen, !followHead { return screen }
        if followMode == "1" {
            let rotation = simd_quatf(simd_float3x3(SIMD3(head.columns.0.x, head.columns.0.y, head.columns.0.z),
                                                    SIMD3(head.columns.1.x, head.columns.1.y, head.columns.1.z),
                                                    SIMD3(head.columns.2.x, head.columns.2.y, head.columns.2.z)))
            let eased = followRotation.map { simd_slerp($0, rotation, perFrame(0.15)) } ?? rotation
            followRotation = eased
            let m = simd_float3x3(eased)
            let position = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)
            let center = position - m.columns.2 * distance  // the head looks along -z
            let placed = simd_float4x4(SIMD4<Float>(m.columns.0, 0), SIMD4<Float>(m.columns.1, 0),
                                       SIMD4<Float>(m.columns.2, 0), SIMD4<Float>(center, 1))
            if screen == nil { log("screen locked to the head at the game's field of view") }
            screen = placed
            return placed
        }
        let flat = -SIMD3<Float>(head.columns.2.x, 0, head.columns.2.z)
        var forward = length(flat) > 0.001 ? normalize(flat) : SIMD3<Float>(0, 0, -1)
        if followHead {
            // Looking nearly straight up or down the heading is undefined: hold the last one.
            if length(flat) > 0.1 || followYaw == nil {
                let yaw = atan2(forward.x, -forward.z)
                var eased = followYaw ?? yaw
                var turn = yaw - eased
                turn -= 2 * .pi * (turn / (2 * .pi)).rounded()
                eased += turn * perFrame(0.08)  // per compositor frame: about 0.15 s to catch up at 90 Hz
                followYaw = eased
            }
            forward = SIMD3<Float>(sin(followYaw!), 0, -cos(followYaw!))
        }
        let back = -forward, up = SIMD3<Float>(0, 1, 0), right = normalize(cross(up, back))
        let center = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z) + forward * Self.distance
        let placed = simd_float4x4(SIMD4<Float>(right, 0), SIMD4<Float>(up, 0), SIMD4<Float>(back, 0), SIMD4<Float>(center, 1))
        if screen == nil { log("screen placed \(Self.distance) m ahead\(followHead ? ", following the head's heading" : "")") }
        screen = placed
        return placed
    }

    /// The head's heading (clockwise from its starting one) and pitch, in radians.
    private func headAngles(_ head: simd_float4x4) -> (yaw: Float, pitch: Float) {
        let forward = -SIMD3<Float>(head.columns.2.x, head.columns.2.y, head.columns.2.z)
        let yaw = atan2(forward.x, -forward.z)
        if headStartYaw == nil {
            headStartYaw = yaw
            headStartPosition = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)
            log("head tracking: anchored")
        }
        var relative = yaw - headStartYaw!
        relative -= 2 * .pi * (relative / (2 * .pi)).rounded()
        return (relative, asin(max(-1, min(1, forward.y))))
    }

    /// Where a frame goes in head-tracking mode: a plane facing back along the direction its game camera looked,
    /// viewPlane meters from the starting head position, so the eye sits eyeBack = viewPlane in front of it.
    /// turn = the headset heading that game north maps to; shift moves the whole frame (meters).
    private func framePose(camera c: [Float], turn: Float, shift: SIMD3<Float>) -> simd_float4x4 {
        // Game camera forward is the rotation's first column; Skyrim heading is clockwise from +Y (north).
        let gameYaw = atan2(c[0], c[3]), gamePitch = asin(max(-1, min(1, c[6])))
        let yaw = gameYaw + turn
        let forward = SIMD3<Float>(sin(yaw) * cos(gamePitch), sin(gamePitch), -cos(yaw) * cos(gamePitch))
        let back = -forward
        let right = normalize(cross(SIMD3<Float>(0, 1, 0), back))
        let up = cross(back, right)
        let center = headStartPosition + forward * Self.viewPlane + shift
        return simd_float4x4(SIMD4<Float>(right, 0), SIMD4<Float>(up, 0), SIMD4<Float>(back, 0), SIMD4<Float>(center, 1))
    }

    /// The frame's place in head-tracking mode, or nil until steering is anchored. With LB_IMMERSIVE_MOTION=1 it
    /// also follows the player between game frames (runtime/motion_path.h): it turns with the steering offset as
    /// the right stick turns the view, and shifts against the distance the game camera has travelled since it
    /// rendered the frame. held is the place without either, for what travels with the camera; nil when the frame
    /// isn't being moved. metersPerUnit is nil when the frame has no depth: a flat picture can't be walked through.
    private func framePoses(camera c: [Float], shownAt seconds: Double, metersPerUnit: Float?)
        -> (model: simd_float4x4, held: simd_float4x4?)? {
        var valid: Int32 = 0
        let offset = skyrim_head_offset(&valid)
        guard valid != 0, let startYaw = headStartYaw else { return nil }
        let held = framePose(camera: c, turn: startYaw - offset, shift: SIMD3<Float>(0, 0, 0))
        var motion = skyrim_motion()
        guard skyrim_immersive_motion(seconds, c[9], c[10], c[11], &motion) != 0 else { return (model: held, held: nil) }
        let turn = startYaw - (motion.has_offset != 0 ? motion.offset : offset)
        var shift = SIMD3<Float>(0, 0, 0)
        if motion.has_moved != 0, let metersPerUnit {
            // The camera has gone on by this much, so everything in the frame is that much the other way.
            let moved = skyrim_motion_in_headset(motion.moved.0, motion.moved.1, motion.moved.2, turn, metersPerUnit)
            shift = -SIMD3<Float>(moved.x, moved.y, moved.z)
        }
        return (model: framePose(camera: c, turn: turn, shift: shift), held: held)
    }

    private func draw() {
        guard let frame = layer.queryNextFrame() else {
            usleep(1000)
            return
        }
        frame.startUpdate()
        frame.endUpdate()
        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)

        var game = skyrim_immersive_frame()
        let hasImage = skyrim_immersive_acquire(&game) == 1
        skyrim_display_tick()
        // Counted here, where the frame is taken: this refresh may still be given up before anything is drawn.
        if hasImage, game.is_new != 0 { frameGeneration += 1 }
        // An integer so the completion handler can carry it across threads.
        let retiredState = game.retired != 0 ? UInt(bitPattern: game.retired_state) : 0
        let retiredSlot = game.retired_slot
        let giveBack: @Sendable () -> Void = {
            if retiredState != 0 { skyrim_immersive_release(UnsafeMutableRawPointer(bitPattern: retiredState), retiredSlot) }
        }

        guard let commands = queue.makeCommandBuffer() else {
            giveBack()
            return
        }
        frame.startSubmission()
        let drawables = frame.queryDrawables()
        guard !drawables.isEmpty else {
            // The system cancelled this frame; it must not be touched again.
            giveBack()
            return
        }
        var texture: (any MTLTexture)?
        if hasImage, let pointer = game.texture {
            texture = Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue() as? any MTLTexture
        }
        var depthTexture: (any MTLTexture)?
        if hasImage, let pointer = game.depth_texture {
            depthTexture = Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue() as? any MTLTexture
        }
        // Stereo frames (LB_STEREO_PRESENT): the right eye's own picture and depth, rendered by the game.
        var textureEye2: (any MTLTexture)?
        var depthTextureEye2: (any MTLTexture)?
        if hasImage, let pointer = game.texture_eye2 {
            textureEye2 = Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue() as? any MTLTexture
            if let depthPointer = game.depth_texture_eye2 {
                depthTextureEye2 = Unmanaged<AnyObject>.fromOpaque(depthPointer).takeUnretainedValue() as? any MTLTexture
            }
            if !loggedStereo { loggedStereo = true; log("stereo frames: each eye draws its own picture (eye units \(stereoEyeUnits))") }
        }
        if depthTexture != nil, !loggedDepth {
            loggedDepth = true
            log("game depth \(depthTexture!.width)x\(depthTexture!.height) arriving")
        }
        if let texture, !checkedDevice {
            checkedDevice = true
            log("game texture \(texture.width)x\(texture.height), same Metal device as the layer: \(texture.device === device)")
        }

        // The image the screen is drawn from: the game frame, or its MetalFX upscale.
        var image = texture
        var sharpen: Float = 0
        if let texture {
            let imageSettings = readImageSettings()
            sharpen = imageSettings.sharpen
            // The MetalFX upscaler holds one picture; stereo frames are drawn as they are.
            if textureEye2 == nil {
                image = upscale(texture, isNew: game.is_new != 0, encoded: game.needs_srgb_decode != 0,
                                factor: imageSettings.upscale, commands: commands) ?? texture
            }
        }

        // LB_IMMERSIVE_SEED: what the mesh needs from this game frame's depth, worked out here when the frame is
        // new, ahead of the eyes' passes that read it.
        var seedTextures: [(any MTLTexture)?] = [nil, nil]
        if let depthTexture, seedMesh, refineSteps > 0, readDepthSettings().on {
            seedTextures[0] = seed(0, from: depthTexture, grid: grid, commands: commands)
            if let depthTextureEye2 { seedTextures[1] = seed(1, from: depthTextureEye2, grid: grid, commands: commands) }
        }

        if let first = drawables.first {
            let shownAt = LayerRenderer.Clock.Instant.epoch.duration(to: first.frameTiming.presentationTime)
            let seconds = Double(shownAt.components.seconds) + Double(shownAt.components.attoseconds) * 1e-18
            if let lastShownAt, seconds > lastShownAt, seconds - lastShownAt < 0.1 {
                shownGapSum += seconds - lastShownAt
                shownGapCount += 1
            }
            lastShownAt = seconds
        }
        let repeats = frameRepeat
        if repeats == 0, frameRepeatSetting == nil, shownGapCount >= 90 {
            // Before the repeat is applied: the display's period, the yardstick for the check below.
            displayPeriod = shownGapSum / Double(shownGapCount)
        }

        let timer = passTiming ? passTimer : nil
        let timerSlot = timer?.beginFrame() ?? 0
        var timedPasses = 0

        var publishedHead = false
        for drawable in drawables {
            logDrawable(drawable)
            let shownAt = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime)
            let seconds = Double(shownAt.components.seconds) + Double(shownAt.components.attoseconds) * 1e-18
            let anchor = tracking.queryDeviceAnchor(atTimestamp: seconds)
            drawable.deviceAnchor = anchor
            let head = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4
            let atFOV = !headMode && followMode == "1" && game.has_camera != 0
            var model = anchor != nil ? place(head: head, distance: atFOV ? Self.viewPlane : Self.distance) : screen
            var halfSize: SIMD2<Float>? = nil
            var heldModel: simd_float4x4? = nil
            var hudCamera: [Float]? = nil  // LB_HUD_LAYER: the frame's camera when the frame is placed by it
            if atFOV, anchor != nil {
                // Locked to the head and as wide as the game's field of view: the frame fills the angles it was
                // rendered for whichever way the head faces, at true depth.
                let c = withUnsafeBytes(of: game.camera) { Array($0.bindMemory(to: Float.self)) }
                halfSize = SIMD2(Self.viewPlane * c[13], Self.viewPlane * c[14])
                hudCamera = c
            }
            if anchor == nil { dropEyes() }
            if headMode, anchor != nil {
                let angles = headAngles(head)
                if !publishedHead { skyrim_head_set(angles.yaw, angles.pitch, 1); publishedHead = true }
                if game.has_camera != 0 {
                    let c = withUnsafeBytes(of: game.camera) { Array($0.bindMemory(to: Float.self)) }
                    var metersPerUnit: Float? = nil
                    if depthTexture != nil {
                        let settings = readDepthSettings()
                        if settings.on, settings.units > 0 { metersPerUnit = headDepthScale / settings.units }
                    }
                    if let poses = framePoses(camera: c, shownAt: seconds, metersPerUnit: metersPerUnit) {
                        // Only while the frame is placed by the head: the eyes are relative to it.
                        if eyeViews, depthTexture != nil, metersPerUnit != nil {
                            giveEyes(drawable, camera: c, unitsPerMetre: readDepthSettings().units / headDepthScale)
                        }
                        model = poses.model
                        heldModel = poses.held
                        // Frustum tangents right and top: the frame covers exactly the angles it was rendered for.
                        halfSize = SIMD2(Self.viewPlane * c[13], Self.viewPlane * c[14])
                        hudCamera = c
                    }
                }
                // Menus and loading come without a camera: show them on a screen placed ahead of where the head
                // looks as they appear, not where the session started.
                if halfSize == nil, lastTracked { screen = nil; model = place(head: head) }
                if halfSize == nil { dropEyes() }
                lastTracked = halfSize != nil
            }
            // The point between the eyes, in the device's space: the eye separation is scaled about it.
            let eyesCentre = drawable.views.reduce(SIMD4<Float>(0, 0, 0, 0)) { $0 + $1.transform.columns.3 } / Float(max(drawable.views.count, 1))
            let centreDepthTexture = depthTexture
            for (index, view) in drawable.views.enumerated() {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = drawable.colorTextures[view.textureMap.textureIndex]
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = drawable.depthTextures[view.textureMap.textureIndex]
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 0
                pass.depthAttachment.storeAction = .store
                let rateMaps = drawable.rasterizationRateMaps
                if !rateMaps.isEmpty { pass.rasterizationRateMap = rateMaps[min(index, rateMaps.count - 1)] }
                if let timer { timedPasses = timer.attach(pass) }
                guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { continue }
                encoder.setViewport(view.textureMap.viewport)
                if let monoTexture = texture, let monoImage = image, let centreModel = model, let pipeline, let depth {
                    // Stereo: view 1 draws the right eye's picture and depth; each eye's mesh starts its rays from
                    // where the game's camera for that eye was, half the eye separation to its side, so the depth
                    // already in each picture is not added a second time.
                    let stereoEye = textureEye2 != nil && index <= 1
                    let texture = stereoEye && index == 1 ? textureEye2! : monoTexture
                    let image = stereoEye && index == 1 ? textureEye2! : monoImage
                    let depthTexture = stereoEye && index == 1 ? (depthTextureEye2 ?? centreDepthTexture) : centreDepthTexture
                    var eyeFromDevice = view.transform
                    eyeFromDevice.columns.3 = eyesCentre + (eyeFromDevice.columns.3 - eyesCentre) * eyeSeparationScale
                    let originFromEye = head * eyeFromDevice
                    let viewMatrix = originFromEye.inverse
                    let aspect = game.height > 0 ? Float(game.width) / Float(game.height) : 16.0 / 9.0
                    let settings = depthTexture != nil ? readDepthSettings() : (scale: 0, maxBehind: 0, near: 15, units: 70, tear: 0.08, on: false)
                    let useDepth = settings.on && depthTexture != nil
                    // Rays start at the viewer's starting head position, not at where the game's 75-degree camera would
                    // sit: a point moved along a ray from the viewer's own eye looks unchanged from there, so the
                    // picture stays undistorted and depth only shows as parallax between the eyes and with head
                    // movement. From a ray origin closer than the viewer (1.8 m) it bent the screen into a box.
                    let tracked = halfSize != nil
                    let eyeBack = tracked ? Self.viewPlane : Self.distance
                    var model = centreModel
                    var eyeHeldModel = heldModel
                    var rectHalf = halfSize ?? SIMD2(Self.width / 2, Self.width / 2 / aspect)
                    var rectCentre = SIMD2<Float>(0, 0)
                    if stereoEye, tracked, game.eye_views != 0 {
                        // The picture covers this eye's own frustum, seen from where the eye was relative to the
                        // game camera: the rays start there (the plane's origin moved by the eye's pose).
                        let tangents = withUnsafeBytes(of: game.eye_tangents) { Array($0.bindMemory(to: Float.self)) }
                        let poses = withUnsafeBytes(of: game.eye_pose) { Array($0.bindMemory(to: Float.self)) }
                        let t = Array(tangents[(index * 4)..<(index * 4 + 4)])
                        rectHalf = SIMD2((t[1] - t[0]) / 2, (t[2] - t[3]) / 2) * Self.viewPlane
                        rectCentre = SIMD2((t[1] + t[0]) / 2, (t[2] + t[3]) / 2) * Self.viewPlane
                        var pose = matrix_identity_float4x4
                        for column in 0..<4 { for row in 0..<4 { pose[column][row] = poses[index * 16 + column * 4 + row] } }
                        var toOrigin = matrix_identity_float4x4, fromOrigin = matrix_identity_float4x4
                        toOrigin.columns.3 = SIMD4(0, 0, eyeBack, 1)
                        fromOrigin.columns.3 = SIMD4(0, 0, -eyeBack, 1)
                        let eye = toOrigin * pose * fromOrigin
                        model = centreModel * eye
                        eyeHeldModel = heldModel.map { $0 * eye }
                        if index == 0 {
                            let rendered = -simd_normalize(simd_make_float3(model.columns.2))
                            let shown = -simd_normalize(simd_make_float3(originFromEye.columns.2))
                            let degrees = Double(acos(max(-1, min(1, simd_dot(rendered, shown))))) * 180 / .pi
                            residualSum += degrees
                            residualMax = max(residualMax, degrees)
                            residualCount += 1
                        }
                    } else if stereoEye, settings.units > 0 {
                        let side: Float = index == 0 ? -1 : 1
                        let metres = stereoEyeUnits / settings.units * (tracked ? headDepthScale : max(settings.scale, 1e-3))
                        var shift = matrix_identity_float4x4
                        shift.columns.3 = SIMD4<Float>(side * metres, 0, 0, 1)
                        model = centreModel * shift
                        eyeHeldModel = heldModel.map { $0 * shift }
                    }
                    var uniforms = QuadUniforms(viewProjection: drawable.computeProjection(convention: .rightUpBack, viewIndex: index) * viewMatrix,
                                                model: model, heldModel: eyeHeldModel ?? model,
                                                halfSize: rectHalf, screenCentre: rectCentre,
                                                decode: game.needs_srgb_decode != 0 ? 1 : 0,
                                                hasDepth: useDepth ? 1 : 0,
                                                grid: grid,
                                                eyeBack: eyeBack, nearUnits: settings.near, unitsPerMeter: settings.units,
                                                depthScale: tracked ? headDepthScale : settings.scale,
                                                maxBehind: tracked ? 300 : settings.maxBehind, backplate: 0,
                                                tear: settings.tear, trueDepth: tracked && useDepth ? 1 : 0,
                                                sharpen: sharpen,
                                                eyeRefine: SIMD4(simd_make_float3(model.inverse * originFromEye.columns.3), useDepth ? refineSteps : 0),
                                                heldWithin: heldModel != nil && useDepth ? motionNear : 0,
                                                refineExit: useDepth && refineSteps > 0 ? refineExit : 0)
                    if useDepth {
                        let offset = Double(simd_length(simd_make_float3(uniforms.eyeRefine) - SIMD3(0, 0, eyeBack)))
                        eyeOffsetSum += offset; eyeOffsetMax = max(eyeOffsetMax, offset); eyeOffsetCount += 1
                    }
                    encoder.setRenderPipelineState(pipeline)
                    encoder.setDepthStencilState(depth)
                    encoder.setCullMode(.none)
                    encoder.setVertexBytes(&uniforms, length: MemoryLayout<QuadUniforms>.stride, index: 0)
                    encoder.setFragmentBytes(&uniforms, length: MemoryLayout<QuadUniforms>.stride, index: 0)
                    encoder.setFragmentTexture(image, index: 0)
                    encoder.setVertexTexture(useDepth ? depthTexture : texture, index: 1)
                    encoder.setFragmentTexture(useDepth ? depthTexture : texture, index: 1)
                    let vertexCount = Int((grid.x - 1) * (grid.y - 1) * 6)
                    let seedTexture = useDepth ? seedTextures[stereoEye && index == 1 && depthTextureEye2 != nil ? 1 : 0] : nil
                    if let seedTexture, let seededPipeline, let gridIndices, refineSteps > 0 {
                        // LB_IMMERSIVE_SEED: the same triangles, each vertex shaded once, its depth read from the seed.
                        encoder.setRenderPipelineState(seededPipeline)
                        encoder.setVertexTexture(seedTexture, index: 1)
                        encoder.drawIndexedPrimitives(type: .triangle, indexCount: gridIndices.count, indexType: gridIndices.type,
                                                      indexBuffer: gridIndices.buffer, indexBufferOffset: 0)
                    } else {
                        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertexCount)
                    }
                    if useDepth && fillHoles && refineSteps == 0 {
                        // Then a blurred copy at the farthest depth, behind the mesh: it shows only through the
                        // gaps at depth edges, instead of black (LB_IMMERSIVE_HOLES=0: black as before). Drawn
                        // second so the depth test keeps the mesh wherever both lie at the far limit (the sky), and
                        // the blur's taps are only shaded in the gaps. The per-pixel search (LB_IMMERSIVE_REFINE)
                        // leaves no gaps, so then there is nothing to fill.
                        var plate = uniforms
                        plate.backplate = 1
                        encoder.setVertexBytes(&plate, length: MemoryLayout<QuadUniforms>.stride, index: 0)
                        encoder.setFragmentBytes(&plate, length: MemoryLayout<QuadUniforms>.stride, index: 0)
                        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertexCount)
                    }
                }
                if let hudPointer = game.hud_texture, let hudPipeline, let hudDepth, let place = heldModel ?? model,
                   let hud = Unmanaged<AnyObject>.fromOpaque(hudPointer).takeUnretainedValue() as? any MTLTexture {
                    // LB_HUD_LAYER: the interface as a flat layer, never moved by the depth mesh. With a camera, a
                    // plane hudDistance ahead of it, placed as the frame is but without the movement shift: the
                    // game's field of view scaled about its centre (LB_HUD_SCALE; the reticle is at the centre, so
                    // it stays on the aim point) and kept where both eyes see it. Without one (menus), over the
                    // screen the picture is shown on, just in front of it, at its full size.
                    var rect: SIMD4<Float>
                    var z: Float
                    if let c = hudCamera {
                        let d = hudDistance
                        let field = HUDLayerPlacement.Tangents(left: c[12], right: c[13], top: c[14], bottom: c[15])
                        // visionOS gives each tangent as a positive extent from the view axis.
                        let bothEyes = HUDLayerPlacement.Tangents.shared(drawable.views.map {
                            HUDLayerPlacement.Tangents(left: -abs($0.tangents[0]), right: abs($0.tangents[1]),
                                                       top: abs($0.tangents[2]), bottom: -abs($0.tangents[3]))
                        })
                        let placed = hudPlacement.rect(frame: field, bothEyes: bothEyes)
                        rect = SIMD4(d * placed.centreX, d * placed.centreY, d * placed.halfWidth, d * placed.halfHeight)
                        z = Self.viewPlane - d  // the plane's space has the eye at z = viewPlane, looking to -z
                        // Said once, and again when the field of view or the eyes' extents change.
                        let eyes = bothEyes ?? HUDLayerPlacement.Tangents(left: 0, right: 0, top: 0, bottom: 0)
                        let key: [Float] = [field.left, field.right, field.top, field.bottom,
                                            eyes.left, eyes.right, eyes.top, eyes.bottom, Float(hud.width)]
                        if key.count != hudLogged.count || !zip(key, hudLogged).allSatisfy({ abs($0 - $1) < 1e-3 }) {
                            hudLogged = key
                            log("hud layer: " + hudPlacement.summary(frame: field, bothEyes: bothEyes, pixelWidth: hud.width, distance: d))
                        }
                    } else {
                        let aspect = game.height > 0 ? Float(game.width) / Float(game.height) : 16.0 / 9.0
                        let half = halfSize ?? SIMD2(Self.width / 2, Self.width / 2 / aspect)
                        rect = SIMD4(0, 0, half.x, half.y)
                        z = 0.002
                    }
                    var eyeFromDevice = view.transform
                    eyeFromDevice.columns.3 = eyesCentre + (eyeFromDevice.columns.3 - eyesCentre) * eyeSeparationScale
                    let viewProjection = drawable.computeProjection(convention: .rightUpBack, viewIndex: index) * (head * eyeFromDevice).inverse
                    var hudUniforms = HudUniforms(mvp: viewProjection * place, rect: rect, z: z)
                    encoder.setRenderPipelineState(hudPipeline)
                    encoder.setDepthStencilState(hudDepth)
                    encoder.setVertexBytes(&hudUniforms, length: MemoryLayout<HudUniforms>.stride, index: 0)
                    encoder.setFragmentTexture(hud, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            drawable.encodePresent(commandBuffer: commands)
        }
        commands.addCompletedHandler { [gpuTime, timer, timerSlot, timedPasses] buffer in
            gpuTime.add(buffer.gpuEndTime - buffer.gpuStartTime)
            timer?.finish(slot: timerSlot, passes: timedPasses)
            giveBack()
        }
        commands.commit()
        lastCommands = commands
        frame.endSubmission()

        presented += 1
        if game.is_new != 0 { newFrames += 1 }
        let now = CACurrentMediaTime()
        if now - loggedAt >= 5 {
            log(String(format: "%.0f Hz presented, %.0f fps from the game", Double(presented) / (now - loggedAt), Double(newFrames) / (now - loggedAt)))
            if let spatial = SpatialEventLog.take() { log("spatial events: \(spatial)") }
            let gpu = gpuTime.take()
            if gpu.count > 0 {
                log(String(format: "compositor gpu %.2f ms per frame, p95 %.2f ms, max %.2f ms", gpu.total * 1000 / Double(gpu.count), gpu.p95 * 1000, gpu.max * 1000))
            }
            if let passTimer, passTimerSetting == true {
                let t = passTimer.take()
                if t.frames > 0 || t.misses > 0 {
                    log(String(format: "compositor passes: vertex %.2f ms, fragment %.2f ms per frame (mean), max %.2f / %.2f ms, %d frames, %d unreadable",
                               t.vertex * 1000, t.fragment * 1000, t.vertexMax * 1000, t.fragmentMax * 1000, t.frames, t.misses))
                }
            }
            if eyeOffsetCount > 0 {
                log(String(format: "eye from the rays' origin: %.1f mm (mean), max %.1f mm", eyeOffsetSum / Double(eyeOffsetCount) * 1000, eyeOffsetMax * 1000))
                eyeOffsetSum = 0; eyeOffsetMax = 0; eyeOffsetCount = 0
            }
            if repeats > 0, shownGapCount > 0 {
                let gap = shownGapSum / Double(shownGapCount)
                var line = String(format: "frame repeat: frames %.2f ms apart (mean)", gap * 1000)
                if let displayPeriod, displayPeriod > 0 {
                    let ratio = gap / displayPeriod
                    line += String(format: ", %.2fx the display's %.2f ms", ratio, displayPeriod * 1000)
                    if abs(ratio - Double(repeats + 1)) > 0.25 { line += String(format: " (expected %dx: not paced as asked)", repeats + 1) }
                }
                log(line)
            }
            if frameRepeatSetting != nil || displayPeriod != nil { shownGapSum = 0; shownGapCount = 0 }
            if residualCount > 0 {
                log(String(format: "eye views: shown %.2f deg from where the picture was rendered looking (mean), max %.2f",
                           residualSum / Double(residualCount), residualMax))
                residualSum = 0; residualMax = 0; residualCount = 0
            }
            presented = 0
            newFrames = 0
            loggedAt = now
        }
    }
}
#endif
