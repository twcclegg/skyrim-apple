import SwiftUI
#if os(visionOS) && SKYRIM_NATIVE_RUNTIME
import CompositorServices
#endif

@main
struct SkyrimLauncherApp: App {
    @StateObject private var model = LauncherModel()

    var body: some Scene {
        #if os(macOS)
        Window("Skyrim", id: "library") {
            LibraryView(model: model)
                .frame(minWidth: 600, minHeight: 680)
                .preferredColorScheme(.dark)
        }
        .defaultSize(width: 960, height: 820)
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(after: .appSettings) {
                Button("Check Game Files", action: model.refresh)
                    .keyboardShortcut("r")
                    .disabled(model.isBusy || model.isGameActive)
            }
        }
        #else
        WindowGroup("Skyrim", id: LibraryView.windowID) {
            LibraryView(model: model)
                .environment(\.skyrimGameActive, model.isGameActive)
                #if os(visionOS)
                .frame(minWidth: 640, minHeight: 700)
                #endif
                #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
                // Tells SDL which window scene is the game's, before an ImmersiveSpace adds a second one.
                .background(GameSceneReporter())
                .modifier(ImmersiveGhost())
                #endif
                .preferredColorScheme(.dark)
        }
        #if os(visionOS)
        // A conventional flat window. Immersive spaces stay out until windowed play is accepted.
        .defaultSize(width: 900, height: 840)
        .windowResizability(.contentMinSize)
        // Every launch opens the launcher, whatever scenes the last run left open: a run that ended inside the
        // game window (LB_GAME_SCENE=1) or the immersive space would otherwise come back as only that, with no
        // launcher to start the game, which also leaves bench autoplay with nothing to press.
        .defaultLaunchBehavior(.presented)
        #endif
        #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
        // The glass comes from ImmersiveGhost, so it can go away while immersive play runs.
        .windowStyle(.plain)
        #endif
        #if os(visionOS) && SKYRIM_NATIVE_RUNTIME
        // LB_GAME_SCENE=1: the game's own window, also the trackpad's pointer surface (GameSurface.swift).
        WindowGroup("Skyrim", id: GameSurfaceView.windowID) {
            GameSurfaceView(model: model)
        }
        .windowStyle(.plain)
        // Only Play opens it; a relaunch never brings back an empty game window.
        .defaultLaunchBehavior(.suppressed)
        .restorationBehavior(.disabled)
        .defaultSize(width: 2400, height: 1300)
        ImmersiveSpace(id: ImmersiveScreen.spaceID) {
            CompositorLayer(configuration: ImmersiveScreenConfiguration()) { layer in
                // Which spatial events (trackpad clicks, pinches) reach the layer: counted for the log
                // (docs/VISIONOS_POINTER_PLAN.md step 1).
                layer.onSpatialEvent = { events in
                    for event in events {
                        SpatialEventLog.note(kind: "\(event.kind)", phase: "\(event.phase)")
                        // Clicks with the pointer off every window: an indirect pinch is a left click
                        // (deduplicated against GCMouse in the runtime).
                        guard event.kind == .indirectPinch else { continue }
                        switch event.phase {
                        case .active: skyrim_spatial_click(1)
                        case .ended, .cancelled: skyrim_spatial_click(0)
                        default: break
                        }
                    }
                }
                ImmersiveCompositor(layer: layer).start()
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        // Only Play opens it: a relaunch after a run that was killed in the space must not come back inside it,
        // where Play Immersive's openImmersiveSpace then fails because the space is already open.
        .restorationBehavior(.disabled)
        // No Home indicator to catch the pointer or the eyes when looking up mid-game (Minecraft port, observed).
        .persistentSystemOverlays(.hidden)
        #endif
        #endif
    }
}

#if os(visionOS) && SKYRIM_NATIVE_RUNTIME
/// While the immersive layer runs, the launcher window (which also holds SDL's game window) turns nearly invisible
/// and loses its glass, so it doesn't cover the immersive picture; it stays in place as the trackpad's pointer
/// surface (docs/VISIONOS_POINTER_PLAN.md).
private struct ImmersiveGhost: ViewModifier {
    @State private var immersive = false
    func body(content: Content) -> some View {
        content
            .opacity(immersive ? 0.02 : 1)
            .glassBackgroundEffect(displayMode: immersive ? .never : .always)
            .onReceive(NotificationCenter.default.publisher(for: .skyrimImmersiveRunning)) { note in
                immersive = (note.object as? Bool) ?? false
            }
    }
}
#endif
