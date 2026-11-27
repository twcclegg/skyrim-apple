import XCTest

/// The interface layer's placement (LB_HUD_SCALE, LB_HUD_OFFSET) for the frame the headset is given: 1600 by 1100
/// at LB_FOV=119.8, tangents 1.881 by 1.280, which is 124 by 104 degrees.
final class HUDLayerGeometryTests: XCTestCase {
    private let frame = HUDLayerPlacement.Tangents(left: -1.881, right: 1.881, top: 1.280, bottom: -1.280)
    /// What both eyes of the headset see: each eye reaches tangent 1.0 toward the nose, 1.0 up and 1.192 down.
    private let bothEyes = HUDLayerPlacement.Tangents(left: -1.0, right: 1.0, top: 1.0, bottom: -1.192)
    private let width: Float = 1600, height: Float = 1100

    /// The direction of a pixel of the HUD picture, in degrees right and up.
    private func direction(_ px: Float, _ py: Float, _ placement: HUDLayerPlacement) -> (right: Float, up: Float) {
        HUDLayerPlacement.direction(x: px / width, y: py / height, in: placement.rect(frame: frame, bothEyes: bothEyes))
    }

    func testDefaultsAreTheReportsValues() {
        let placement = HUDLayerPlacement(scaleSetting: nil, offsetSetting: nil)
        XCTAssertEqual(placement.scale, 0.28)
        XCTAssertEqual(placement.offsetRight, 0)
        XCTAssertEqual(placement.offsetUp, 0)
        XCTAssertEqual(placement, HUDLayerPlacement())
    }

    func testFullScaleIsTheGamesOwnLayout() {
        // What LB_HUD_LAYER=1 did before: the picture over the whole frustum, both eyes' area ignored (it is larger).
        let rect = HUDLayerPlacement(scale: 1).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(rect, HUDLayerPlacement.Rect(centreX: 0, centreY: 0, halfWidth: 1.881, halfHeight: 1.280))
        XCTAssertEqual(HUDLayerPlacement.degrees(rect.right), 62.0, accuracy: 0.05)
        XCTAssertEqual(HUDLayerPlacement.degrees(rect.top), 52.0, accuracy: 0.05)
    }

    func testDefaultLayerSpans56By40DegreesInsideWhatBothEyesSee() {
        let rect = HUDLayerPlacement().rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(rect.halfWidth, 0.527, accuracy: 0.001)
        XCTAssertEqual(rect.halfHeight, 0.358, accuracy: 0.001)
        XCTAssertEqual(2 * HUDLayerPlacement.degrees(rect.halfWidth), 55.5, accuracy: 0.1)
        XCTAssertEqual(2 * HUDLayerPlacement.degrees(rect.halfHeight), 39.4, accuracy: 0.1)
        XCTAssertGreaterThanOrEqual(rect.left, bothEyes.left)
        XCTAssertLessThanOrEqual(rect.right, bothEyes.right)
        XCTAssertLessThanOrEqual(rect.top, bothEyes.top)
        XCTAssertGreaterThanOrEqual(rect.bottom, bothEyes.bottom)
        // Nothing had to move it there.
        XCTAssertEqual(rect, HUDLayerPlacement().rect(frame: frame))
    }

    func testTheReticleStaysOnTheAimPointAtAnyScale() {
        for scale: Float in [0.1, 0.28, 0.5, 1] {
            let centre = direction(800, 550, HUDLayerPlacement(scale: scale))
            XCTAssertEqual(centre.right, 0, accuracy: 1e-4)
            XCTAssertEqual(centre.up, 0, accuracy: 1e-4)
        }
        // Also through a frustum that is not symmetric: the centre of the picture is the centre of the frustum.
        let skewed = HUDLayerPlacement.Tangents(left: -1.5, right: 1.9, top: 1.1, bottom: -1.3)
        let full = HUDLayerPlacement(scale: 1).rect(frame: skewed), small = HUDLayerPlacement().rect(frame: skewed)
        XCTAssertEqual(small.centreX, full.centreX, accuracy: 1e-6)
        XCTAssertEqual(small.centreY, full.centreY, accuracy: 1e-6)
        XCTAssertEqual(small.halfWidth, full.halfWidth * 0.28, accuracy: 1e-6)
    }

    /// Where the HUD lands, before (scale 1) and after (0.28). The game fits its 1280 by 720 stage into the frame
    /// whole (GFx ShowAll): 1.25 frame pixels per stage pixel, rows 100 to 1000. A clip anchored to an edge sits
    /// either at the stage's edge or, if the movie anchors to the visible area, at the frame's; both are checked.
    /// The stage positions themselves (compass 45 down, bars 40 up, side bars centred 190 in) are estimates.
    func testWhereTheHUDLandsBeforeAndAfter() {
        let before = HUDLayerPlacement(scale: 1), after = HUDLayerPlacement()
        let k: Float = 1.25, stageTop: Float = 100, stageBottom: Float = 1000

        // Compass, top centre. An eye sees 45 degrees up.
        XCTAssertEqual(direction(800, stageTop + 45 * k, before).up, 42.5, accuracy: 0.1)
        XCTAssertEqual(direction(800, 45 * k, before).up, 49.0, accuracy: 0.1)
        XCTAssertEqual(direction(800, stageTop + 45 * k, after).up, 14.4, accuracy: 0.1)
        XCTAssertEqual(direction(800, 45 * k, after).up, 17.8, accuracy: 0.1)

        // Health, bottom centre. An eye sees 50 degrees down.
        XCTAssertEqual(direction(800, stageBottom - 40 * k, before).up, -43.0, accuracy: 0.1)
        XCTAssertEqual(direction(800, height - 40 * k, before).up, -49.3, accuracy: 0.1)
        XCTAssertEqual(direction(800, stageBottom - 40 * k, after).up, -14.6, accuracy: 0.1)
        XCTAssertEqual(direction(800, height - 40 * k, after).up, -18.0, accuracy: 0.1)

        // Magicka and stamina: centres 190 stage pixels in from the sides, outer ends 65. Both eyes see 45 degrees
        // to a side, one eye 60.
        XCTAssertEqual(direction(190 * k, 900, before).right, -52.9, accuracy: 0.1)
        XCTAssertEqual(direction(65 * k, 900, before).right, -59.4, accuracy: 0.1)
        XCTAssertEqual(direction(width - 190 * k, 900, before).right, 52.9, accuracy: 0.1)
        XCTAssertEqual(direction(190 * k, 900, after).right, -20.3, accuracy: 0.1)
        XCTAssertEqual(direction(65 * k, 900, after).right, -25.3, accuracy: 0.1)

        // The corners of the picture.
        XCTAssertEqual(direction(0, 0, before).right, -62.0, accuracy: 0.05)
        XCTAssertEqual(direction(0, 0, before).up, 52.0, accuracy: 0.05)
        XCTAssertEqual(direction(0, 0, after).right, -27.8, accuracy: 0.05)
        XCTAssertEqual(direction(1600, 1100, after).up, -19.7, accuracy: 0.05)
    }

    func testTextDensity() {
        let before = HUDLayerPlacement(scale: 1).rect(frame: frame), after = HUDLayerPlacement().rect(frame: frame)
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: before.halfWidth), 7.42, accuracy: 0.01)
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: after.halfWidth), 26.5, accuracy: 0.05)
        // Rows are as dense as columns: the frame's pixels are square.
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: height, halfWidth: after.halfHeight), 26.8, accuracy: 0.05)
        // A 20-pixel line of stage text is 25 frame pixels: 3.4 degrees tall before, 0.94 after.
        XCTAssertEqual(25 / HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: before.halfWidth), 3.37, accuracy: 0.01)
        XCTAssertEqual(25 / HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: after.halfWidth), 0.94, accuracy: 0.01)
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: 0), 0)
    }

    func testAnOffsetMovesTheLayerAndIsKeptWhereBothEyesSee() {
        // 10 degrees down: the whole layer moves, and still fits.
        let low = HUDLayerPlacement(offsetUp: -10).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(low.centreY, -0.1763, accuracy: 0.0005)
        XCTAssertEqual(low.centreX, 0)
        XCTAssertEqual(HUDLayerPlacement.degrees(low.bottom), -28.1, accuracy: 0.1)

        // 45 degrees up would put the top at tangent 1.36, past the 1.0 an eye sees: stopped with the top at 1.0.
        let high = HUDLayerPlacement(offsetUp: 45).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(high.top, 1.0, accuracy: 1e-5)
        XCTAssertEqual(high.halfHeight, 0.3584, accuracy: 0.0005)
        // Down has more room (1.192).
        let lowest = HUDLayerPlacement(offsetUp: -45).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(lowest.bottom, -1.192, accuracy: 1e-5)
        // To a side: stopped where the far eye's view ends.
        let right = HUDLayerPlacement(offsetRight: 40).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(right.right, 1.0, accuracy: 1e-5)
        XCTAssertEqual(right.centreY, 0)
        // Without the eyes' extents the offset is taken as given.
        XCTAssertEqual(HUDLayerPlacement(offsetRight: 40).rect(frame: frame).centreX, 0.8391, accuracy: 0.0005)
        // A layer wider than both eyes' area is left where it was asked for on that axis, and still held on the other.
        let wide = HUDLayerPlacement(scale: 0.6, offsetRight: 10, offsetUp: 45).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(wide.centreX, 0.1763, accuracy: 0.0005)
        XCTAssertEqual(wide.top, 1.0, accuracy: 1e-5)
    }

    func testWhatBothEyesSee() {
        // The headset's eyes: each sees 60 degrees outward, 45 inward.
        let left = HUDLayerPlacement.Tangents(left: -1.732, right: 1.0, top: 1.0, bottom: -1.192)
        let right = HUDLayerPlacement.Tangents(left: -1.0, right: 1.732, top: 1.0, bottom: -1.192)
        XCTAssertEqual(HUDLayerPlacement.Tangents.shared([left, right]), bothEyes)
        XCTAssertEqual(HUDLayerPlacement.Tangents.shared([left]), left)
        XCTAssertNil(HUDLayerPlacement.Tangents.shared([]))
        let apart = HUDLayerPlacement.Tangents(left: 2, right: 3, top: 1, bottom: -1)
        XCTAssertNil(HUDLayerPlacement.Tangents.shared([left, apart]))
    }

    func testSettingsText() {
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "0.4", offsetSetting: "5,-8"),
                       HUDLayerPlacement(scale: 0.4, offsetRight: 5, offsetUp: -8))
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: " 1 ", offsetSetting: " 0 , -12.5 "),
                       HUDLayerPlacement(scale: 1, offsetRight: 0, offsetUp: -12.5))
        // A lone number is degrees up.
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: nil, offsetSetting: "-10"), HUDLayerPlacement(offsetUp: -10))
        // Out of range: held to the limits.
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "3", offsetSetting: "90,-90"),
                       HUDLayerPlacement(scale: 1, offsetRight: 45, offsetUp: -45))
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "0", offsetSetting: nil).scale, 0.1)
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "-1", offsetSetting: nil).scale, 0.1)
        // Not numbers: as if unset.
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "small", offsetSetting: "left"), HUDLayerPlacement())
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "", offsetSetting: "1,2,3"), HUDLayerPlacement())
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: "nan", offsetSetting: "inf,nan"), HUDLayerPlacement())
        XCTAssertEqual(HUDLayerPlacement(scaleSetting: nil, offsetSetting: "4,"), HUDLayerPlacement())
    }

    func testTheLogLineSaysWhereTheLayerIs() {
        let line = HUDLayerPlacement().summary(frame: frame, bothEyes: bothEyes, pixelWidth: 1600, distance: 2)
        XCTAssertEqual(line, "scale 0.28, offset 0.0 right 0.0 up, 2.00 m ahead: left -27.8 right 27.8 top 19.7 bottom -19.7 degrees"
                           + " of the game's -62.0 62.0 52.0 -52.0, 26.5 px/degree at its centre")
        XCTAssertTrue(HUDLayerPlacement(offsetUp: 45).summary(frame: frame, bothEyes: bothEyes, pixelWidth: 1600, distance: 2)
            .hasSuffix(", moved to stay where both eyes see"))
    }
}
