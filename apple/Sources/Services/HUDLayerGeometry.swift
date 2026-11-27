import Foundation

/// Where the interface layer (LB_HUD_LAYER=1) sits in the view while the game is played in the immersive space.
///
/// The game lays its HUD out for a screen: compass against the top edge, the three bars against the bottom edge
/// and the corners. Stretched over the whole frame that follows the head (about 124 by 104 degrees), those edges
/// are 45 to 60 degrees off centre, at or past what an eye sees through the lens, and a letter is degrees tall.
/// So the layer is drawn smaller: the same picture, scaled about the centre of the game's frustum. The reticle is
/// at that centre, so it stays on the aim point at any scale.
///
/// All extents are tangents: metres on a plane one metre ahead of the game's camera, x to the right, y up.
/// No simd and no platform calls, so the tests run anywhere.
struct HUDLayerPlacement: Equatable {
    /// LB_HUD_SCALE when unset: the frame's 124 by 104 degrees becomes about 56 by 40.
    static let defaultScale: Float = 0.28
    static let scaleRange: ClosedRange<Float> = 0.1...1
    /// LB_HUD_OFFSET is held to this many degrees each way.
    static let offsetLimit: Float = 45

    /// LB_HUD_SCALE: the layer's size as a fraction of the game's field of view. 1 is the game's own layout.
    var scale: Float
    /// LB_HUD_OFFSET=right,up in degrees: where the layer's centre goes, from the centre of the game's frustum.
    /// Anything but 0,0 takes the reticle off the aim point by as much.
    var offsetRight: Float
    var offsetUp: Float

    init(scale: Float = HUDLayerPlacement.defaultScale, offsetRight: Float = 0, offsetUp: Float = 0) {
        self.scale = Self.usable(scale) ? min(max(scale, Self.scaleRange.lowerBound), Self.scaleRange.upperBound)
                                        : Self.defaultScale
        self.offsetRight = Self.usable(offsetRight) ? min(max(offsetRight, -Self.offsetLimit), Self.offsetLimit) : 0
        self.offsetUp = Self.usable(offsetUp) ? min(max(offsetUp, -Self.offsetLimit), Self.offsetLimit) : 0
    }

    /// From the settings' text (nil when unset). Text that isn't a number counts as unset; a lone number for the
    /// offset is degrees up.
    init(scaleSetting: String?, offsetSetting: String?) {
        let scale = scaleSetting.flatMap { Float($0.trimmingCharacters(in: .whitespaces)) }
        var right: Float = 0, up: Float = 0
        if let offsetSetting {
            let parts = offsetSetting.split(separator: ",", omittingEmptySubsequences: false)
                .map { Float($0.trimmingCharacters(in: .whitespaces)) }
            if parts.count == 2, let x = parts[0], let y = parts[1] { right = x; up = y }
            if parts.count == 1, let y = parts[0] { up = y }
        }
        self.init(scale: scale ?? Self.defaultScale, offsetRight: right, offsetUp: up)
    }

    private static func usable(_ value: Float) -> Bool { !value.isNaN && !value.isInfinite }

    /// A view's reach each way from its axis. Left and bottom are negative.
    struct Tangents: Equatable {
        var left: Float, right: Float, top: Float, bottom: Float
        var centreX: Float { (left + right) / 2 }
        var centreY: Float { (top + bottom) / 2 }
        var halfWidth: Float { (right - left) / 2 }
        var halfHeight: Float { (top - bottom) / 2 }

        /// What every one of the views sees, taking them as looking the same way from the same place (the eyes
        /// are 64 mm apart, under a degree at the layer's 2 m). Nil when there are none or they share nothing.
        static func shared(_ views: [Tangents]) -> Tangents? {
            guard var all = views.first else { return nil }
            for view in views.dropFirst() {
                all.left = max(all.left, view.left)
                all.right = min(all.right, view.right)
                all.top = min(all.top, view.top)
                all.bottom = max(all.bottom, view.bottom)
            }
            return all.left < all.right && all.bottom < all.top ? all : nil
        }
    }

    /// The layer on the plane one metre ahead: its centre and half size. Multiply by the layer's distance.
    struct Rect: Equatable {
        var centreX: Float, centreY: Float, halfWidth: Float, halfHeight: Float
        var left: Float { centreX - halfWidth }
        var right: Float { centreX + halfWidth }
        var top: Float { centreY + halfHeight }
        var bottom: Float { centreY - halfHeight }
    }

    /// The layer for a frame rendered through `frame`. With `bothEyes` (what both eyes see, about the same axis),
    /// a layer that fits inside it is moved the least that puts it wholly inside, each axis on its own: an offset
    /// can't push part of the HUD to where one eye, or neither, sees it. A layer larger than that on an axis is
    /// left where it was asked for on that axis (scale 1 is the game's own layout, edges and all).
    func rect(frame: Tangents, bothEyes: Tangents? = nil) -> Rect {
        var rect = Rect(centreX: frame.centreX + Self.tangent(offsetRight), centreY: frame.centreY + Self.tangent(offsetUp),
                        halfWidth: frame.halfWidth * scale, halfHeight: frame.halfHeight * scale)
        if let bothEyes {
            if rect.halfWidth <= bothEyes.halfWidth {
                rect.centreX = min(max(rect.centreX, bothEyes.left + rect.halfWidth), bothEyes.right - rect.halfWidth)
            }
            if rect.halfHeight <= bothEyes.halfHeight {
                rect.centreY = min(max(rect.centreY, bothEyes.bottom + rect.halfHeight), bothEyes.top - rect.halfHeight)
            }
        }
        return rect
    }

    /// Where a point of the HUD picture is seen, in degrees right and up of the frame's axis. x and y are
    /// fractions of the picture: 0,0 its top left corner, 1,1 its bottom right.
    static func direction(x: Float, y: Float, in rect: Rect) -> (right: Float, up: Float) {
        (degrees(rect.left + x * 2 * rect.halfWidth), degrees(rect.top - y * 2 * rect.halfHeight))
    }

    /// Picture pixels per degree across the layer at its centre, for a picture `pixels` wide shown `halfWidth`
    /// (a tangent) to each side. Toward the edges a degree holds more (by 1 + tangent squared).
    static func pixelsPerDegree(pixels: Float, halfWidth: Float) -> Float {
        halfWidth > 0 ? pixels / (2 * halfWidth) * .pi / 180 : 0
    }

    static func degrees(_ tangent: Float) -> Float { atan(tangent) * 180 / .pi }
    static func tangent(_ degrees: Float) -> Float { tan(degrees * .pi / 180) }

    /// One line for the log: what the layer covers and how dense the picture is.
    func summary(frame: Tangents, bothEyes: Tangents?, pixelWidth: Int, distance: Float) -> String {
        let r = rect(frame: frame, bothEyes: bothEyes)
        func d(_ tangent: Float) -> String { String(format: "%.1f", Self.degrees(tangent)) }
        let moved = r != rect(frame: frame) ? ", moved to stay where both eyes see" : ""
        return String(format: "scale %.2f, offset %.1f right %.1f up, %.2f m ahead: ", scale, offsetRight, offsetUp, distance)
            + "left \(d(r.left)) right \(d(r.right)) top \(d(r.top)) bottom \(d(r.bottom)) degrees"
            + " of the game's \(d(frame.left)) \(d(frame.right)) \(d(frame.top)) \(d(frame.bottom))"
            + String(format: ", %.1f px/degree at its centre",
                     Self.pixelsPerDegree(pixels: Float(pixelWidth), halfWidth: r.halfWidth))
            + moved
    }
}
