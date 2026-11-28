import XCTest

/// The interface layer's placement (LB_HUD_SCALE, LB_HUD_OFFSET) for the frame the headset is given: 1600 by 1100
/// at LB_FOV=119.8, 124 by 104 degrees. The compositor asks for tangents 1.881 by 1.280; the runtime sets the
/// game's field of view from that and the window's aspect, so the game's frustum is 1.881 by 1.881 x 1100 / 1600 =
/// 1.293.
final class HUDLayerPlacementTests: XCTestCase {
    private let frame = HUDLayerPlacement.Tangents(left: -1.881, right: 1.881, top: 1.293, bottom: -1.293)
    /// What both eyes of the headset see: each eye reaches tangent 1.0 toward the nose, 1.0 up and 1.192 down.
    private let bothEyes = HUDLayerPlacement.Tangents(left: -1.0, right: 1.0, top: 1.0, bottom: -1.192)
    /// Each eye on its own: 60 degrees outward, 45 inward.
    private let leftEye = HUDLayerPlacement.Tangents(left: -1.732, right: 1.0, top: 1.0, bottom: -1.192)
    private let rightEye = HUDLayerPlacement.Tangents(left: -1.0, right: 1.732, top: 1.0, bottom: -1.192)
    private let width: Float = 1600, height: Float = 1100

    /// The direction of a pixel of the HUD picture, in degrees right and up.
    private func direction(_ px: Float, _ py: Float, _ placement: HUDLayerPlacement) -> (right: Float, up: Float) {
        HUDLayerPlacement.direction(x: px / width, y: py / height, in: placement.rect(frame: frame, bothEyes: bothEyes))
    }

    /// The game's frustum for a field of view set by number (LB_FOV, at the 4:3 reference) and a window's aspect.
    private func frame(fov: Float, aspect: Float) -> HUDLayerPlacement.Tangents {
        let y = 0.75 * HUDLayerPlacement.tangent(fov / 2), x = y * aspect
        return HUDLayerPlacement.Tangents(left: -x, right: x, top: y, bottom: -y)
    }

    func testDefaults() {
        let placement = HUDLayerPlacement(scaleSetting: nil, offsetSetting: nil)
        XCTAssertNil(placement.scale)
        XCTAssertEqual(placement.offsetRight, 0)
        XCTAssertEqual(placement.offsetUp, 0)
        XCTAssertEqual(placement, HUDLayerPlacement())
        // On the frame the HUD was tuned on, the default is scale 0.28.
        XCTAssertEqual(placement.scale(for: frame), 0.28, accuracy: 1e-4)
    }

    func testTheDefaultIsAnAngleWhateverTheFieldOfView() {
        // LB_FOV=100 at 1600 by 1100, and the game's own 65 degrees at 16:9 (LB_IMMERSIVE_FOLLOW=1).
        let fov100 = frame(fov: 100, aspect: 1600 / 1100), fov65 = frame(fov: 65, aspect: 16 / 9)
        XCTAssertEqual(fov100.right, 1.300, accuracy: 0.001)
        XCTAssertEqual(fov65.right, 0.849, accuracy: 0.001)
        XCTAssertEqual(HUDLayerPlacement().scale(for: fov100), 0.405, accuracy: 0.001)
        XCTAssertEqual(HUDLayerPlacement().scale(for: fov65), 0.620, accuracy: 0.001)
        for field in [frame, fov100, fov65] {
            let rect = HUDLayerPlacement().rect(frame: field)
            XCTAssertEqual(2 * HUDLayerPlacement.degrees(rect.halfWidth), HUDLayerPlacement.defaultWidth, accuracy: 0.01)
        }
        // The same picture, so the height follows the frame's aspect: 39.8 degrees at 1600 by 1100.
        XCTAssertEqual(2 * HUDLayerPlacement.degrees(HUDLayerPlacement().rect(frame: fov100).halfHeight), 39.8, accuracy: 0.05)
        // LB_HUD_SCALE is a fraction of whatever the frame is.
        XCTAssertEqual(HUDLayerPlacement(scale: 0.28).rect(frame: fov100).halfWidth, 0.28 * fov100.halfWidth, accuracy: 1e-6)
        // A frame narrower than the default angle is shown whole; one past 158 degrees gets the smallest scale.
        XCTAssertEqual(HUDLayerPlacement().scale(for: frame(fov: 30, aspect: 1)), 1)
        let huge = HUDLayerPlacement.Tangents(left: -6, right: 6, top: 4, bottom: -4)
        XCTAssertEqual(HUDLayerPlacement().scale(for: huge), 0.1)
    }

    func testFullScaleIsTheGamesOwnLayout() {
        // What LB_HUD_LAYER=1 did before: the picture over the whole frustum, both eyes' area ignored (it is larger).
        let rect = HUDLayerPlacement(scale: 1).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(rect, HUDLayerPlacement.Rect(centreX: 0, centreY: 0, halfWidth: 1.881, halfHeight: 1.293))
        XCTAssertEqual(HUDLayerPlacement.degrees(rect.right), 62.0, accuracy: 0.05)
        XCTAssertEqual(HUDLayerPlacement.degrees(rect.top), 52.3, accuracy: 0.05)
    }

    func testDefaultLayerSpans56By40DegreesInsideWhatBothEyesSee() {
        let rect = HUDLayerPlacement().rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(rect.halfWidth, 0.527, accuracy: 0.001)
        XCTAssertEqual(rect.halfHeight, 0.362, accuracy: 0.001)
        XCTAssertEqual(2 * HUDLayerPlacement.degrees(rect.halfWidth), 55.55, accuracy: 0.01)
        XCTAssertEqual(2 * HUDLayerPlacement.degrees(rect.halfHeight), 39.8, accuracy: 0.05)
        XCTAssertGreaterThanOrEqual(rect.left, bothEyes.left)
        XCTAssertLessThanOrEqual(rect.right, bothEyes.right)
        XCTAssertLessThanOrEqual(rect.top, bothEyes.top)
        XCTAssertGreaterThanOrEqual(rect.bottom, bothEyes.bottom)
        // Nothing had to move it there.
        XCTAssertEqual(rect, HUDLayerPlacement().rect(frame: frame))
    }

    func testTheReticleStaysOnTheAimPointAtAnyScale() {
        // Every scale from 0.1 to 1 in steps of 0.001, through the game's frustum and the compositor's request,
        // with what both eyes see: nothing but an offset moves the centre. Both eyes see more downward (1.192) than
        // upward (1.0), so a layer between 1.0 and 1.096 high each way fits but can't be centred inside.
        for field in [frame, HUDLayerPlacement.Tangents(left: -1.881, right: 1.881, top: 1.280, bottom: -1.280)] {
            for step in 100...1000 {
                let rect = HUDLayerPlacement(scale: Float(step) / 1000).rect(frame: field, bothEyes: bothEyes)
                XCTAssertEqual(rect.centreX, 0, "scale \(Float(step) / 1000)")
                XCTAssertEqual(rect.centreY, 0, "scale \(Float(step) / 1000)")
            }
        }
        // The middle pixel of the picture is on the aim point.
        for scale: Float in [0.1, 0.28, 0.5, 0.82, 1] {
            let centre = direction(800, 550, HUDLayerPlacement(scale: scale))
            XCTAssertEqual(centre.right, 0, accuracy: 1e-4)
            XCTAssertEqual(centre.up, 0, accuracy: 1e-4)
        }
        // Also through a frustum that is not symmetric: the centre of the picture is the centre of the frustum.
        let skewed = HUDLayerPlacement.Tangents(left: -1.5, right: 1.9, top: 1.1, bottom: -1.3)
        let full = HUDLayerPlacement(scale: 1).rect(frame: skewed), small = HUDLayerPlacement(scale: 0.28).rect(frame: skewed)
        XCTAssertEqual(small.centreX, full.centreX, accuracy: 1e-6)
        XCTAssertEqual(small.centreY, full.centreY, accuracy: 1e-6)
        XCTAssertEqual(small.halfWidth, full.halfWidth * 0.28, accuracy: 1e-6)
    }

    func testAtScale082TheLayerStaysOnTheAimPoint() {
        // 1.060 each way: fits in both eyes' 2.192 of height, but centred it reaches past the 1.0 an eye sees up.
        let centred = HUDLayerPlacement(scale: 0.82).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(centred.halfHeight, 1.0603, accuracy: 0.0005)
        XCTAssertEqual(centred.centreY, 0)
        XCTAssertEqual(centred.centreX, 0)
        XCTAssertEqual(centred.top, 1.0603, accuracy: 0.0005)
        // An offset up can't take it further out; one down moves it, only until the bottom meets what the eyes see.
        let up = HUDLayerPlacement(scale: 0.82, offsetUp: 10).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(up.centreY, 0)
        let down = HUDLayerPlacement(scale: 0.82, offsetUp: -10).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(down.centreY, -0.1317, accuracy: 0.0005)
        XCTAssertEqual(down.bottom, -1.192, accuracy: 1e-5)
        let littleDown = HUDLayerPlacement(scale: 0.82, offsetUp: -3).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(littleDown.centreY, -HUDLayerPlacement.tangent(3), accuracy: 1e-6)
    }

    /// Where the HUD lands, before (scale 1) and after (the default). The game fits its 1280 by 720 stage into the
    /// frame whole (GFx ShowAll): 1.25 frame pixels per stage pixel, rows 100 to 1000. A clip anchored to an edge
    /// sits either at the stage's edge or, if the movie anchors to the visible area, at the frame's; both are
    /// checked. The stage positions themselves (compass 45 down, bars 40 up, side bars centred 190 in) are estimates.
    func testWhereTheHUDLandsBeforeAndAfter() {
        let before = HUDLayerPlacement(scale: 1), after = HUDLayerPlacement()
        let k: Float = 1.25, stageTop: Float = 100, stageBottom: Float = 1000

        // Compass, top centre. An eye sees 45 degrees up.
        XCTAssertEqual(direction(800, stageTop + 45 * k, before).up, 42.8, accuracy: 0.1)
        XCTAssertEqual(direction(800, 45 * k, before).up, 49.3, accuracy: 0.1)
        XCTAssertEqual(direction(800, stageTop + 45 * k, after).up, 14.5, accuracy: 0.1)
        XCTAssertEqual(direction(800, 45 * k, after).up, 18.0, accuracy: 0.1)

        // Health, bottom centre. An eye sees 50 degrees down.
        XCTAssertEqual(direction(800, stageBottom - 40 * k, before).up, -43.2, accuracy: 0.1)
        XCTAssertEqual(direction(800, height - 40 * k, before).up, -49.6, accuracy: 0.1)
        XCTAssertEqual(direction(800, stageBottom - 40 * k, after).up, -14.8, accuracy: 0.1)
        XCTAssertEqual(direction(800, height - 40 * k, after).up, -18.2, accuracy: 0.1)

        // Magicka and stamina: centres 190 stage pixels in from the sides, outer ends 65. Both eyes see 45 degrees
        // to a side, one eye 60.
        XCTAssertEqual(direction(190 * k, 900, before).right, -52.9, accuracy: 0.1)
        XCTAssertEqual(direction(65 * k, 900, before).right, -59.4, accuracy: 0.1)
        XCTAssertEqual(direction(width - 190 * k, 900, before).right, 52.9, accuracy: 0.1)
        XCTAssertEqual(direction(190 * k, 900, after).right, -20.3, accuracy: 0.1)
        XCTAssertEqual(direction(65 * k, 900, after).right, -25.3, accuracy: 0.1)

        // The corners of the picture.
        XCTAssertEqual(direction(0, 0, before).right, -62.0, accuracy: 0.05)
        XCTAssertEqual(direction(0, 0, before).up, 52.3, accuracy: 0.05)
        XCTAssertEqual(direction(0, 0, after).right, -27.8, accuracy: 0.05)
        XCTAssertEqual(direction(1600, 1100, after).up, -19.9, accuracy: 0.05)
    }

    func testTextDensity() {
        let before = HUDLayerPlacement(scale: 1).rect(frame: frame), after = HUDLayerPlacement().rect(frame: frame)
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: before.halfWidth), 7.42, accuracy: 0.01)
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: after.halfWidth), 26.5, accuracy: 0.05)
        // Rows are as dense as columns: the frame's pixels are square.
        XCTAssertEqual(HUDLayerPlacement.pixelsPerDegree(pixels: height, halfWidth: after.halfHeight),
                       HUDLayerPlacement.pixelsPerDegree(pixels: width, halfWidth: after.halfWidth), accuracy: 0.01)
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
        XCTAssertEqual(HUDLayerPlacement.degrees(low.bottom), -28.3, accuracy: 0.1)

        // 45 degrees up would put the top at tangent 1.36, past the 1.0 an eye sees: cut short with the top at 1.0.
        let high = HUDLayerPlacement(offsetUp: 45).rect(frame: frame, bothEyes: bothEyes)
        XCTAssertEqual(high.top, 1.0, accuracy: 1e-5)
        XCTAssertEqual(high.halfHeight, 0.362, accuracy: 0.0005)
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
        XCTAssertEqual(HUDLayerPlacement.Tangents.shared([leftEye, rightEye]), bothEyes)
        XCTAssertEqual(HUDLayerPlacement.Tangents.shared([leftEye]), leftEye)
        XCTAssertNil(HUDLayerPlacement.Tangents.shared([]))
        let apart = HUDLayerPlacement.Tangents(left: 2, right: 3, top: 1, bottom: -1)
        XCTAssertNil(HUDLayerPlacement.Tangents.shared([leftEye, apart]))
    }

    func testWhereTheEyesAreNarrowsWhatBothSeeUpClose() {
        // Each eye 32 mm from the point between them. At 2 m that takes 0.016 off each side; at 0.3 m, 0.107.
        func both(at distance: Float, half: Float = 0.032) -> HUDLayerPlacement.Tangents? {
            HUDLayerPlacement.Tangents.shared([leftEye.seen(fromEyeRight: -half, up: 0, distance: distance),
                                               rightEye.seen(fromEyeRight: half, up: 0, distance: distance)])
        }
        let far = both(at: 2), near = both(at: 0.3)
        XCTAssertEqual(far?.left ?? 0, -0.984, accuracy: 1e-5)
        XCTAssertEqual(far?.right ?? 0, 0.984, accuracy: 1e-5)
        XCTAssertEqual(near?.left ?? 0, -0.8933, accuracy: 1e-4)
        XCTAssertEqual(near?.right ?? 0, 0.8933, accuracy: 1e-4)
        XCTAssertEqual(near?.top, 1.0)
        XCTAssertEqual(near?.bottom, -1.192)
        // So at 0.3 m an offset to the side stops sooner: the right edge where the left eye's view ends.
        let right = HUDLayerPlacement(offsetRight: 40).rect(frame: frame, bothEyes: near)
        XCTAssertEqual(right.right, 0.8933, accuracy: 1e-4)
        // An eye above the point between them sees the plane's points that much higher.
        let raised = leftEye.seen(fromEyeRight: 0, up: 0.01, distance: 0.5)
        XCTAssertEqual(raised.top, 1.02, accuracy: 1e-5)
        XCTAssertEqual(raised.bottom, -1.172, accuracy: 1e-5)
        XCTAssertEqual(leftEye.seen(fromEyeRight: 1, up: 1, distance: 0), leftEye)
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
        XCTAssertEqual(HUDLayerPlacement(scale: .nan), HUDLayerPlacement())
    }

    func testSettingsThatCantBeReadAreListed() {
        let decimalComma = HUDLayerPlacement.parse(scaleSetting: "0,28", offsetSetting: "-10,")
        XCTAssertEqual(decimalComma.placement, HUDLayerPlacement())
        XCTAssertEqual(decimalComma.unread, ["LB_HUD_SCALE=0,28", "LB_HUD_OFFSET=-10,"])
        XCTAssertEqual(HUDLayerPlacement.parse(scaleSetting: "nan", offsetSetting: "1,2,3").unread,
                       ["LB_HUD_SCALE=nan", "LB_HUD_OFFSET=1,2,3"])
        // Unset, empty, and numbers out of range (held to the limits) are not listed.
        XCTAssertEqual(HUDLayerPlacement.parse(scaleSetting: nil, offsetSetting: nil).unread, [])
        XCTAssertEqual(HUDLayerPlacement.parse(scaleSetting: "", offsetSetting: " ").unread, [])
        XCTAssertEqual(HUDLayerPlacement.parse(scaleSetting: "3", offsetSetting: "-10").unread, [])
    }

    func testTheLogLineSaysWhereTheLayerIs() {
        let line = HUDLayerPlacement().summary(frame: frame, bothEyes: bothEyes, pixelWidth: 1600, distance: 2)
        XCTAssertEqual(line, "scale 0.28 (55.5 degrees across by default), offset 0.0 right 0.0 up, 2.00 m ahead:"
                           + " left -27.8 right 27.8 top 19.9 bottom -19.9 degrees of the game's -62.0 62.0 52.3 -52.3,"
                           + " 26.5 px/degree at its centre")
        XCTAssertTrue(HUDLayerPlacement(scale: 0.4).summary(frame: frame, bothEyes: bothEyes, pixelWidth: 1600, distance: 2)
            .hasPrefix("scale 0.40, offset"))
        XCTAssertTrue(HUDLayerPlacement(offsetUp: 45).summary(frame: frame, bothEyes: bothEyes, pixelWidth: 1600, distance: 2)
            .hasSuffix(", offset cut short to stay where both eyes see"))
        XCTAssertFalse(HUDLayerPlacement(scale: 0.82).summary(frame: frame, bothEyes: bothEyes, pixelWidth: 1600, distance: 2)
            .contains("cut short"))
    }
}
