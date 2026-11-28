import Foundation

/// Where the interface layer (LB_HUD_LAYER=1) sits in the view while the game is played in the immersive space.
///
/// The game lays its HUD out for a screen: compass against the top edge, the three bars against the bottom edge
/// and the corners. Stretched over the whole frame that follows the head (124 by 104 degrees at LB_FOV=119.8, or
/// with eye views), the compass and the health bar are 43 to 49 degrees above and below centre and magicka and
/// stamina 53 to 59 degrees to the sides, at or past what an eye sees through the lens, and a letter is degrees
/// tall. So the layer is drawn smaller: the same picture, scaled about the centre of the game's frustum. The
/// reticle is at that centre, and nothing but an offset moves the layer's centre, so with no offset the reticle
/// stays on the aim point at any scale.
///
/// All extents are tangents: metres on a plane one metre ahead of the game's camera, x to the right, y up.
/// No simd and no platform calls, so the tests run anywhere.
struct HUDLayerPlacement: Equatable {
    /// The layer's width in degrees when LB_HUD_SCALE is unset, whatever the game's field of view: what scale 0.28
    /// gives on the 124 by 104 degree frame the HUD was tuned on (half width 0.28 x tangent 1.881 = tangent 0.527,
    /// 27.77 degrees each side).
    static let defaultWidth: Float = 55.55
    static let scaleRange: ClosedRange<Float> = 0.1...1
    /// LB_HUD_OFFSET is held to this many degrees each way.
    static let offsetLimit: Float = 45

    /// LB_HUD_SCALE: the layer's size as a fraction of the game's field of view, 1 being the game's own layout.
    /// Nil when unset: then the layer is defaultWidth degrees across (`scale(for:)`).
    var scale: Float?
    /// LB_HUD_OFFSET=right,up in degrees: where the layer's centre goes, from the centre of the game's frustum.
    /// Anything but 0,0 takes the reticle off the aim point by as much as it moves the layer (it can be cut short:
    /// see `rect`).
    var offsetRight: Float
    var offsetUp: Float

    init(scale: Float? = nil, offsetRight: Float = 0, offsetUp: Float = 0) {
        self.scale = scale.flatMap { Self.usable($0) ? Self.clamped($0) : nil }
        self.offsetRight = Self.usable(offsetRight) ? min(max(offsetRight, -Self.offsetLimit), Self.offsetLimit) : 0
        self.offsetUp = Self.usable(offsetUp) ? min(max(offsetUp, -Self.offsetLimit), Self.offsetLimit) : 0
    }

    /// From the settings' text (nil when unset). A lone number for the offset is degrees up.
    init(scaleSetting: String?, offsetSetting: String?) {
        self = Self.parse(scaleSetting: scaleSetting, offsetSetting: offsetSetting).placement
    }

    /// From the settings' text (nil when unset): the placement, and each setting that was set but couldn't be read
    /// (`LB_HUD_SCALE=0,28`, say), as `NAME=text`. Those count as unset. Empty text counts as unset and isn't listed.
    static func parse(scaleSetting: String?, offsetSetting: String?) -> (placement: HUDLayerPlacement, unread: [String]) {
        func number<S: StringProtocol>(_ text: S) -> Float? {
            Float(text.trimmingCharacters(in: .whitespaces)).flatMap { usable($0) ? $0 : nil }
        }
        func isBlank(_ text: String?) -> Bool { text?.trimmingCharacters(in: .whitespaces).isEmpty ?? true }
        var unread: [String] = []
        var scale: Float? = nil
        if let scaleSetting, !isBlank(scaleSetting) {
            scale = number(scaleSetting)
            if scale == nil { unread.append("LB_HUD_SCALE=\(scaleSetting)") }
        }
        var right: Float = 0, up: Float = 0
        if let offsetSetting, !isBlank(offsetSetting) {
            let parts = offsetSetting.split(separator: ",", omittingEmptySubsequences: false).map { number($0) }
            if parts.count == 2, let x = parts[0], let y = parts[1] {
                right = x; up = y
            } else if parts.count == 1, let y = parts[0] {
                up = y
            } else {
                unread.append("LB_HUD_OFFSET=\(offsetSetting)")
            }
        }
        return (HUDLayerPlacement(scale: scale, offsetRight: right, offsetUp: up), unread)
    }

    private static func usable(_ value: Float) -> Bool { !value.isNaN && !value.isInfinite }
    private static func clamped(_ scale: Float) -> Float { min(max(scale, scaleRange.lowerBound), scaleRange.upperBound) }

    /// The fraction of `frame` the layer covers: LB_HUD_SCALE when set, else what makes it defaultWidth degrees
    /// across (0.28 at LB_FOV=119.8, 0.41 at LB_FOV=100), within scaleRange.
    func scale(for frame: Tangents) -> Float {
        if let scale { return scale }
        guard frame.halfWidth > 0 else { return 1 }
        return Self.clamped(Self.tangent(Self.defaultWidth / 2) / frame.halfWidth)
    }

    /// A view's reach each way from its axis. Left and bottom are negative.
    struct Tangents: Equatable {
        var left: Float, right: Float, top: Float, bottom: Float
        var centreX: Float { (left + right) / 2 }
        var centreY: Float { (top + bottom) / 2 }
        var halfWidth: Float { (right - left) / 2 }
        var halfHeight: Float { (top - bottom) / 2 }

        /// What this view, from an eye `eyeRight`, `eyeUp` metres from the point between the eyes and looking the
        /// same way, sees of a plane `distance` metres ahead, as tangents from that point. Half the eye separation
        /// is 0.016 at 2 m (0.9 degrees) but 0.107 at 0.3 m (6 degrees).
        func seen(fromEyeRight eyeRight: Float, up eyeUp: Float, distance: Float) -> Tangents {
            guard distance > 0 else { return self }
            let x = eyeRight / distance, y = eyeUp / distance
            return Tangents(left: left + x, right: right + x, top: top + y, bottom: bottom + y)
        }

        /// What every one of the views sees (each as from the same point: see `seen`). Nil when there are none or
        /// they share nothing.
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

    /// The layer for a frame rendered through `frame`. With `bothEyes` (what both eyes see, from the point between
    /// them), on each axis where the layer fits inside it, the offset is cut short where it would take part of the
    /// layer out of it: an offset can't push the HUD to where one eye, or neither, sees it. Only the offset is cut,
    /// never the frustum's centre itself, so with no offset the layer is centred on the frustum (and the reticle on
    /// the aim point) even when a part of it is then out of one eye's view. A layer larger than both eyes' view on an
    /// axis is left where it was asked for on that axis (scale 1 is the game's own layout, edges and all).
    func rect(frame: Tangents, bothEyes: Tangents? = nil) -> Rect {
        let fraction = scale(for: frame)
        let halfWidth = frame.halfWidth * fraction, halfHeight = frame.halfHeight * fraction
        return Rect(centreX: Self.centre(frame.centreX, offset: Self.tangent(offsetRight), half: halfWidth,
                                         low: bothEyes?.left, high: bothEyes?.right),
                    centreY: Self.centre(frame.centreY, offset: Self.tangent(offsetUp), half: halfHeight,
                                         low: bothEyes?.bottom, high: bothEyes?.top),
                    halfWidth: halfWidth, halfHeight: halfHeight)
    }

    /// The layer's centre on one axis: `centre` moved by `offset`, the offset cut short so that a layer `half` each
    /// way that fits in `low...high` doesn't end up further out of it than with no offset at all.
    private static func centre(_ centre: Float, offset: Float, half: Float, low: Float?, high: Float?) -> Float {
        guard let low, let high, 2 * half <= high - low else { return centre + offset }
        // Where the centre may go: the places that keep the layer inside, and anywhere between them and `centre`.
        let lowest = min(low + half, centre), highest = max(high - half, centre)
        return min(max(centre + offset, lowest), highest)
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
        let size = scale == nil ? String(format: "scale %.2f (%.1f degrees across by default)", scale(for: frame), Self.defaultWidth)
                                : String(format: "scale %.2f", scale(for: frame))
        let cut = r != rect(frame: frame) ? ", offset cut short to stay where both eyes see" : ""
        return size + String(format: ", offset %.1f right %.1f up, %.2f m ahead: ", offsetRight, offsetUp, distance)
            + "left \(d(r.left)) right \(d(r.right)) top \(d(r.top)) bottom \(d(r.bottom)) degrees"
            + " of the game's \(d(frame.left)) \(d(frame.right)) \(d(frame.top)) \(d(frame.bottom))"
            + String(format: ", %.1f px/degree at its centre",
                     Self.pixelsPerDegree(pixels: Float(pixelWidth), halfWidth: r.halfWidth))
            + cut
    }
}
