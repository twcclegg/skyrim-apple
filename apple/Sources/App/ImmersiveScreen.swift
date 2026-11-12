#if os(visionOS) && SKYRIM_NATIVE_RUNTIME
import ARKit
import CompositorServices
import Foundation
import Metal
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

final class GPUTimeTally: @unchecked Sendable {
    private let lock = NSLock()
    private var total = 0.0, count = 0
    func add(_ seconds: Double) {
        guard seconds > 0, seconds < 1 else { return }
        lock.lock(); total += seconds; count += 1; lock.unlock()
    }
    func take() -> (total: Double, count: Int) {
        lock.lock(); defer { total = 0; count = 0; lock.unlock() }
        return (total, count)
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

private struct QuadUniforms {
    var viewProjection: simd_float4x4
    var model: simd_float4x4
    var halfSize: SIMD2<Float>
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
    /// xyz: this view's eye in the screen's own space. w: per-pixel depth search steps in the fragment shader
    /// (LB_IMMERSIVE_REFINE); 0 = off.
    var eyeRefine: SIMD4<Float>
}

/// Runs on its own thread for as long as the layer lives: presents at the display's rate, showing the newest finished
/// game frame and repeating the last one when the game hasn't produced another.
final class ImmersiveCompositor: @unchecked Sendable {
    private static let shaders = """
    #include <metal_stdlib>
    using namespace metal;
    struct Uniforms {
        float4x4 viewProjection; float4x4 model; float2 halfSize; uint decode; uint hasDepth;
        uint2 grid; float eyeBack; float nearUnits; float unitsPerMeter; float depthScale; float maxBehind; float backplate;
        float tear; float trueDepth; float4 eyeRefine;
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
    // The game pixel's view depth in game meters.
    float behind_screen_raw(float2 uv, constant Uniforms &u, texture2d<float> depth, bool dilate) {
        constexpr sampler nearest(address::clamp_to_edge, filter::nearest);
        float d = depth.sample(nearest, uv, level(0)).r;
        if (dilate) {
            // The nearest depth around the point (half a grid cell each way), so a thin foreground edge keeps its
            // depth instead of a vertex landing beside it in the background.
            float2 cell = 0.5 / float2(u.grid - 1);
            d = min(d, depth.sample(nearest, uv + float2(-cell.x, 0), level(0)).r);
            d = min(d, depth.sample(nearest, uv + float2(cell.x, 0), level(0)).r);
            d = min(d, depth.sample(nearest, uv + float2(0, -cell.y), level(0)).r);
            d = min(d, depth.sample(nearest, uv + float2(0, cell.y), level(0)).r);
        }
        // D3D depth with a far plane much farther than the near one: view distance ~ near / (1 - d).
        return (u.nearUnits / max(1.0 - d, 1e-6)) / u.unitsPerMeter;
    }
    float behind_screen(float2 uv, constant Uniforms &u, texture2d<float> depth, bool dilate) {
        return min(u.depthScale * behind_screen_raw(uv, u, depth, dilate), u.maxBehind);
    }
    // Distance from the eye of the game pixel at uv. Window mode: the screen distance plus how far behind it.
    // True-depth mode: its view depth in meters, along the ray (view depth z sits at z / cos = |ray| * z / eyeBack).
    float radial(float2 uv, float toScreen, constant Uniforms &u, texture2d<float> depth, bool dilate) {
        if (u.trueDepth == 0) return toScreen + behind_screen(uv, u, depth, dilate);
        return toScreen * min(behind_screen_raw(uv, u, depth, dilate) * u.depthScale, u.maxBehind) / u.eyeBack;
    }
    float3 screen_point(float2 uv, constant Uniforms &u) {
        return float3((uv.x * 2 - 1) * u.halfSize.x, (1 - uv.y * 2) * u.halfSize.y, 0);
    }
    // Fades the push to nothing over the outer 4% so the picture's outline stays a clean rectangle. Applied after the
    // tear test, so the fade's own slope doesn't count as a depth edge.
    float edge_fade(float2 uv) {
        float2 edge = min(uv, 1.0 - uv);
        return smoothstep(0.0, 0.04, min(edge.x, edge.y));
    }
    // Two triangles per grid cell, unindexed, so each vertex knows its whole triangle. With depth, each vertex moves
    // along the ray from the viewer's starting eye through its point on the screen, by how far that pixel is in the
    // game. A triangle spanning a depth jump (an object's edge against what is behind it) is dropped instead of
    // stretched into streaks (see below). The backplate path (u.backplate) is no longer drawn.
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
        Out out;
        out.uv = uv;
        float3 ray = onScreen - eye;
        float toScreen = length(ray);
        if (u.backplate != 0) {
            float far = u.trueDepth != 0 ? toScreen * u.maxBehind / u.eyeBack - toScreen : u.maxBehind * edge_fade(uv);
            p = eye + ray * (1.0 + far / toScreen);
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
        out.local = p;
        out.position = u.viewProjection * u.model * float4(p, 1);
        return out;
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
        for (int i = 0; i < int(u.eyeRefine.w); i++) {
            before = uv;
            rBefore = pixel_radial(uv, u, depth);
            float3 fromOrigin = eye + d * (-ad + sqrt(max(ad * ad - aa + rBefore * rBefore, 0.0))) - origin;
            float3 on = fromOrigin * (u.eyeBack / max(-fromOrigin.z, 1e-6));
            uv = saturate(float2((on.x / u.halfSize.x + 1) * 0.5, (1 - on.y / u.halfSize.y) * 0.5));
        }
        // A ray that keeps jumping between a near and a far surface sees neither's own pixel: it looks past the
        // near surface's edge at something the game's camera did not see. Show the far one there.
        return rBefore > pixel_radial(uv, u, depth) * (1.0 + u.tear) ? before : uv;
    }
    fragment float4 screen_fragment(Out in [[stage_in]], texture2d<float> image [[texture(0)]],
                                    texture2d<float> depth [[texture(1)]], constant Uniforms &u [[buffer(0)]]) {
        constexpr sampler linear_clamp(address::clamp_to_edge, filter::linear);
        float2 uv = u.eyeRefine.w > 0 && u.hasDepth != 0 ? refined_uv(in, u, depth) : in.uv;
        float3 c = image.sample(linear_clamp, uv).rgb;
        if (u.decode) c = select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
        return float4(c, 1);
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
    private let session = ARKitSession()
    private let tracking = WorldTrackingProvider()
    private var screen: simd_float4x4?
    private var lastCommands: (any MTLCommandBuffer)?
    private var checkedDevice = false
    private var presented = 0, newFrames = 0, loggedAt = CACurrentMediaTime()
    /// The compositor's own GPU time per frame (its command buffers' GPU start to end), for the log.
    private let gpuTime = GPUTimeTally()
    /// Grid size: one vertex every 4 game pixels.
    private static let gridColumns: UInt32 = 321, gridRows: UInt32 = 181
    private var depthSettings: (scale: Float, maxBehind: Float, near: Float, units: Float, tear: Float, on: Bool)?
    private var loggedDepth = false
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
    /// Distance of the plane the frame's grid is laid on before depth moves it (any value works in true depth).
    private static let viewPlane: Float = 2.0

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

    private func run() {
        do {
            try makePipeline()
        } catch {
            log("no pipeline: \(error)")
            return
        }
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

    private func finish() {
        skyrim_sdl_set_window_ghost(0)
        DispatchQueue.main.async { NotificationCenter.default.post(name: .skyrimImmersiveRunning, object: false) }
        if headMode { skyrim_head_set(0, 0, 0) }
        lastCommands?.waitUntilCompleted()
        skyrim_immersive_stop()
        session.stop()
        log("layer ended")
    }

    /// The screen, placed once: straight ahead of where the wearer is looking, level, at eye height.
    private func place(head: simd_float4x4) -> simd_float4x4 {
        if let screen { return screen }
        var forward = -SIMD3<Float>(head.columns.2.x, 0, head.columns.2.z)
        forward = length(forward) > 0.001 ? normalize(forward) : SIMD3<Float>(0, 0, -1)
        let back = -forward, up = SIMD3<Float>(0, 1, 0), right = normalize(cross(up, back))
        let center = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z) + forward * Self.distance
        let placed = simd_float4x4(SIMD4<Float>(right, 0), SIMD4<Float>(up, 0), SIMD4<Float>(back, 0), SIMD4<Float>(center, 1))
        screen = placed
        log("screen placed \(Self.distance) m ahead")
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
    private func framePose(camera c: [Float]) -> simd_float4x4? {
        var valid: Int32 = 0
        let offset = skyrim_head_offset(&valid)
        guard valid != 0, let startYaw = headStartYaw else { return nil }
        // Game camera forward is the rotation's first column; Skyrim heading is clockwise from +Y (north).
        let gameYaw = atan2(c[0], c[3]), gamePitch = asin(max(-1, min(1, c[6])))
        let yaw = startYaw + (gameYaw - offset)
        let forward = SIMD3<Float>(sin(yaw) * cos(gamePitch), sin(gamePitch), -cos(yaw) * cos(gamePitch))
        let back = -forward
        let right = normalize(cross(SIMD3<Float>(0, 1, 0), back))
        let up = cross(back, right)
        let center = headStartPosition + forward * Self.viewPlane
        return simd_float4x4(SIMD4<Float>(right, 0), SIMD4<Float>(up, 0), SIMD4<Float>(back, 0), SIMD4<Float>(center, 1))
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
        if depthTexture != nil, !loggedDepth {
            loggedDepth = true
            log("game depth \(depthTexture!.width)x\(depthTexture!.height) arriving")
        }
        if let texture, !checkedDevice {
            checkedDevice = true
            log("game texture \(texture.width)x\(texture.height), same Metal device as the layer: \(texture.device === device)")
        }

        var publishedHead = false
        for drawable in drawables {
            let shownAt = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime)
            let seconds = Double(shownAt.components.seconds) + Double(shownAt.components.attoseconds) * 1e-18
            let anchor = tracking.queryDeviceAnchor(atTimestamp: seconds)
            drawable.deviceAnchor = anchor
            let head = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4
            var model = anchor != nil ? place(head: head) : screen
            var halfSize: SIMD2<Float>? = nil
            if headMode, anchor != nil {
                let angles = headAngles(head)
                if !publishedHead { skyrim_head_set(angles.yaw, angles.pitch, 1); publishedHead = true }
                if game.has_camera != 0 {
                    let c = withUnsafeBytes(of: game.camera) { Array($0.bindMemory(to: Float.self)) }
                    if let pose = framePose(camera: c) {
                        model = pose
                        // Frustum tangents right and top: the frame covers exactly the angles it was rendered for.
                        halfSize = SIMD2(Self.viewPlane * c[13], Self.viewPlane * c[14])
                    }
                }
                // Menus and loading come without a camera: show them on a screen placed ahead of where the head
                // looks as they appear, not where the session started.
                if halfSize == nil, lastTracked { screen = nil; model = place(head: head) }
                lastTracked = halfSize != nil
            }
            // The point between the eyes, in the device's space: the eye separation is scaled about it.
            let eyesCentre = drawable.views.reduce(SIMD4<Float>(0, 0, 0, 0)) { $0 + $1.transform.columns.3 } / Float(max(drawable.views.count, 1))
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
                guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { continue }
                encoder.setViewport(view.textureMap.viewport)
                if let texture, let model, let pipeline, let depth {
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
                    var uniforms = QuadUniforms(viewProjection: drawable.computeProjection(convention: .rightUpBack, viewIndex: index) * viewMatrix,
                                                model: model,
                                                halfSize: halfSize ?? SIMD2(Self.width / 2, Self.width / 2 / aspect),
                                                decode: game.needs_srgb_decode != 0 ? 1 : 0,
                                                hasDepth: useDepth ? 1 : 0,
                                                grid: SIMD2(Self.gridColumns, Self.gridRows),
                                                eyeBack: eyeBack, nearUnits: settings.near, unitsPerMeter: settings.units,
                                                depthScale: tracked ? headDepthScale : settings.scale,
                                                maxBehind: tracked ? 300 : settings.maxBehind, backplate: 0,
                                                tear: settings.tear, trueDepth: tracked && useDepth ? 1 : 0,
                                                eyeRefine: SIMD4(simd_make_float3(model.inverse * originFromEye.columns.3), useDepth ? refineSteps : 0))
                    encoder.setRenderPipelineState(pipeline)
                    encoder.setDepthStencilState(depth)
                    encoder.setCullMode(.none)
                    encoder.setVertexBytes(&uniforms, length: MemoryLayout<QuadUniforms>.stride, index: 0)
                    encoder.setFragmentBytes(&uniforms, length: MemoryLayout<QuadUniforms>.stride, index: 0)
                    encoder.setFragmentTexture(texture, index: 0)
                    encoder.setVertexTexture(useDepth ? depthTexture : texture, index: 1)
                    encoder.setFragmentTexture(useDepth ? depthTexture : texture, index: 1)
                    let vertexCount = Int((Self.gridColumns - 1) * (Self.gridRows - 1) * 6)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertexCount)
                }
                encoder.endEncoding()
            }
            drawable.encodePresent(commandBuffer: commands)
        }
        commands.addCompletedHandler { [gpuTime] buffer in
            gpuTime.add(buffer.gpuEndTime - buffer.gpuStartTime)
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
            if gpu.count > 0 { log(String(format: "compositor gpu %.2f ms per frame", gpu.total * 1000 / Double(gpu.count))) }
            presented = 0
            newFrames = 0
            loggedAt = now
        }
    }
}
#endif
