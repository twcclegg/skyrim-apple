#if (os(iOS) || os(visionOS)) && SKYRIM_NATIVE_RUNTIME
import CoreFoundation
import Foundation
#if os(visionOS)
import UIKit
#endif

/// The native Skyrim runtime linked into physical iPad and Vision Pro builds (ipad/build.py): the original
/// Windows game's code translated ahead of time to ARM64, run in this process with no JIT.
enum NativeRuntime {
    static var isLinked: Bool { skyrim_native_runtime_linked() == 1 }

    /// The installation the runtime plays: the Skyrim folder in this app's own Documents.
    static var gameDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Skyrim", isDirectory: true)
    }

    /// Exact-hash check of the modules and master files the runtime was built from; reads a few
    /// hundred megabytes, so call it off the main thread. Returns the problem, or nil.
    nonisolated static func validate() -> String? {
        var error = [CChar](repeating: 0, count: 1024)
        return skyrim_native_validate(&error, error.count) == 1 ? nil : text(error)
    }

    /// Runs the game on the main thread until it ends. It starts from a run-loop block rather
    /// than a dispatch block, so the main queue (and with it the launcher's state, input monitor
    /// and notifications) keeps being serviced while SDL turns the run loop inside the game.
    @MainActor
    static func start(saveRoot: URL, started: @escaping @MainActor () -> Void,
                      finished: @escaping @MainActor (String?) -> Void) {
        let main = CFRunLoopGetMain()
        CFRunLoopPerformBlock(main, CFRunLoopMode.defaultMode.rawValue) {
            MainActor.assumeIsolated {
                started()
                var error = [CChar](repeating: 0, count: 1024)
                let succeeded = saveRoot.withUnsafeFileSystemRepresentation { path in
                    guard let path else { return false }
                    return skyrim_native_run(path, &error, error.count) == 1
                }
                finished(succeeded ? nil : text(error))
            }
        }
        CFRunLoopWakeUp(main)
    }

    #if os(visionOS)
    /// Vision Pro: reshapes the app's window to the game's 16:9 (or back to the launcher's shape)
    /// and waits briefly for it to land, since SDL sizes the game from the window scene at start.
    @MainActor
    static func shapeWindow(forGame game: Bool) async {
        guard let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.session.role == .windowApplication })
        else { return }
        let size = game ? CGSize(width: 1280, height: 720) : CGSize(width: 900, height: 840)
        scene.requestGeometryUpdate(.Vision(size: size, resizingRestrictions: game ? .uniform : .freeform))
        for _ in 0..<20 {
            let bounds = scene.coordinateSpace.bounds
            if bounds.height > 0, abs(bounds.width / bounds.height - size.width / size.height) < 0.01 { return }
            try? await Task.sleep(for: .milliseconds(50))
        }
    }
    #endif

    /// Play Immersive, before the game starts: the game gets the comfort defaults for immersive play through LB_INI
    /// (runtime/comfort_ini.h; LB_COMFORT_INI=0 in launch-env.json opts out). The game reads its settings once, at
    /// start, so a game started in the window keeps the game's own behaviour even if it later moves into the space.
    static func markImmersiveLaunch() { skyrim_comfort_ini_immersive(1) }

    /// Holds the game's frames and hides its window, showing the launcher, until resumed.
    static func setPaused(_ paused: Bool) { skyrim_native_set_paused(paused ? 1 : 0) }

    /// Ends the game, and with it this app, at its next frame.
    static func requestQuit() { skyrim_native_request_quit() }

    /// The runtime's identity (build inputs and dependency hashes) as compiled in.
    static var identity: Any? {
        let json = String(cString: skyrim_native_identity())
        return try? JSONSerialization.jsonObject(with: Data(json.utf8))
    }

    /// Writes the launcher's runtime and input state to Documents/SkyrimNative/status.json, so a
    /// missing-controller gate can be told apart from a runtime or file problem without Play.
    static func writeStatus(_ status: [String: Any]) {
        let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SkyrimNative", isDirectory: true)
        guard JSONSerialization.isValidJSONObject(status),
              let data = try? JSONSerialization.data(withJSONObject: status, options: [.prettyPrinted, .sortedKeys]),
              (try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)) != nil
        else { return }
        try? data.write(to: folder.appendingPathComponent("status.json"), options: .atomic)
    }

    private static func text(_ buffer: [CChar]) -> String {
        String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
}
#endif
