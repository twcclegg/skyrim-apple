import Foundation

/// What the launcher does around the immersive space once a game exists: when its window goes nearly invisible,
/// when it offers the way back in, and when leaving the space pauses the game. Pure decisions, so the tests cover
/// them without a headset.
enum ImmersivePolicy {
    /// The pause reason shown after the immersive layer went away under a running game.
    static let leftReason = "You left the immersive view"

    /// The launcher window is nearly invisible only while the immersive picture is being played: the layer is
    /// drawing and the game is not paused. A paused game shows the window in full, so Resume can be seen and
    /// pressed. While it is ghosted the window still sits in place as the trackpad's pointer surface.
    static func ghostsLauncher(spaceRunning: Bool, paused: Bool) -> Bool {
        spaceRunning && !paused
    }

    /// "Return to Immersive" is offered whenever a game exists (starting, running or paused) and the immersive
    /// layer is not drawing it.
    static func offersReturn(gameActive: Bool, spaceRunning: Bool) -> Bool {
        gameActive && !spaceRunning
    }

    /// Whether the game pauses when the immersive layer ends under it (Digital Crown, the system closing the
    /// space). It does when someone is there to resume it and the launcher window is there to resume it from:
    /// not on an unattended test run, which has nobody to press Resume, and not with the launcher window closed
    /// (LB_GAME_SCENE=1), where a pause would have no visible way out.
    static func pausesOnLeave(gameRunning: Bool, unattended: Bool, launcherPresent: Bool) -> Bool {
        gameRunning && !unattended && launcherPresent
    }
}
