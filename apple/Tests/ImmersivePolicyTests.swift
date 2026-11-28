import XCTest

final class ImmersivePolicyTests: XCTestCase {
    func testLauncherIsGhostedOnlyWhileImmersivePlayIsShown() {
        XCTAssertTrue(ImmersivePolicy.ghostsLauncher(spaceRunning: true, paused: false))
        // A game paused inside the space: the window with Resume must be visible.
        XCTAssertFalse(ImmersivePolicy.ghostsLauncher(spaceRunning: true, paused: true))
        XCTAssertFalse(ImmersivePolicy.ghostsLauncher(spaceRunning: false, paused: false))
        XCTAssertFalse(ImmersivePolicy.ghostsLauncher(spaceRunning: false, paused: true))
    }

    func testReturnIsOfferedForAnyGameOutsideTheSpace() {
        XCTAssertTrue(ImmersivePolicy.offersReturn(gameActive: true, spaceRunning: false))
        XCTAssertFalse(ImmersivePolicy.offersReturn(gameActive: true, spaceRunning: true))
        // Before a game starts, Play Immersive is the way in.
        XCTAssertFalse(ImmersivePolicy.offersReturn(gameActive: false, spaceRunning: false))
        XCTAssertFalse(ImmersivePolicy.offersReturn(gameActive: false, spaceRunning: true))
    }

    func testLeavingTheSpacePausesARunningGame() {
        XCTAssertTrue(ImmersivePolicy.pausesOnLeave(gameRunning: true, unattended: false, launcherPresent: true))
        XCTAssertFalse(ImmersivePolicy.pausesOnLeave(gameRunning: false, unattended: false, launcherPresent: true))
    }

    func testLeavingNeverPausesWhereNobodyCouldResume() {
        // A bench or simulator run has no one to press Resume.
        XCTAssertFalse(ImmersivePolicy.pausesOnLeave(gameRunning: true, unattended: true, launcherPresent: true))
        // LB_GAME_SCENE=1 closed the launcher window, the only place Resume is drawn.
        XCTAssertFalse(ImmersivePolicy.pausesOnLeave(gameRunning: true, unattended: false, launcherPresent: false))
    }

    /// The sequence the fix is for: play immersive, leave, come back, lose the controller, resume.
    func testASessionThatLeavesAndReturns() {
        var spaceRunning = true, paused = false
        XCTAssertTrue(ImmersivePolicy.ghostsLauncher(spaceRunning: spaceRunning, paused: paused))
        XCTAssertFalse(ImmersivePolicy.offersReturn(gameActive: true, spaceRunning: spaceRunning))

        spaceRunning = false  // Digital Crown
        paused = ImmersivePolicy.pausesOnLeave(gameRunning: true, unattended: false, launcherPresent: true)
        XCTAssertTrue(paused)
        XCTAssertFalse(ImmersivePolicy.ghostsLauncher(spaceRunning: spaceRunning, paused: paused))
        XCTAssertTrue(ImmersivePolicy.offersReturn(gameActive: true, spaceRunning: spaceRunning))

        spaceRunning = true  // Return to Immersive, which also resumes
        paused = false
        XCTAssertTrue(ImmersivePolicy.ghostsLauncher(spaceRunning: spaceRunning, paused: paused))

        paused = true  // controller battery dies inside the space
        XCTAssertFalse(ImmersivePolicy.ghostsLauncher(spaceRunning: spaceRunning, paused: paused))
        XCTAssertFalse(ImmersivePolicy.offersReturn(gameActive: true, spaceRunning: spaceRunning))
    }
}
