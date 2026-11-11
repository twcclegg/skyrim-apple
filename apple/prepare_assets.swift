// Mechanical packaging of the existing original launcher art, without edits.
import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let original = root.appendingPathComponent("Artwork/SkyrimAdjacent-original.png")
let files = FileManager.default
let info: [String: Any] = ["author": "xcode", "version": 1]
guard let input = CGImageSourceCreateWithURL(original as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(input, 0, nil),
      image.width == image.height else { fatalError("Original square artwork is missing") }

func writeJSON(_ object: [String: Any], to path: URL) throws {
    try files.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
    try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys]).write(to: path)
}

func writePNG(size: Int, to path: URL) throws {
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
        bytesPerRow: size * 4, space: space, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    context.interpolationQuality = .high
    context.draw(image, in: CGRect(x: 0, y: 0, width: size, height: size))
    try files.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
    guard let png = context.makeImage(),
          let destination = CGImageDestinationCreateWithURL(path as CFURL, UTType.png.identifier as CFString, 1, nil)
    else { fatalError("Icon packaging failed") }
    CGImageDestinationAddImage(destination, png, nil)
    guard CGImageDestinationFinalize(destination) else { fatalError("Icon write failed") }
}

let common = root.appendingPathComponent("Resources/Common.xcassets")
try writeJSON(["info": info], to: common.appendingPathComponent("Contents.json"))
let cover = common.appendingPathComponent("LauncherArtwork.imageset")
try writeJSON(["info": info, "images": [["filename": "Artwork.png", "idiom": "universal"]]],
              to: cover.appendingPathComponent("Contents.json"))
try writePNG(size: 1024, to: cover.appendingPathComponent("Artwork.png"))

for platform in ["iOS", "Mac"] {
    let catalog = root.appendingPathComponent("Resources/\(platform).xcassets")
    try writeJSON(["info": info], to: catalog.appendingPathComponent("Contents.json"))
    let icon = catalog.appendingPathComponent("AppIcon.appiconset")
    var entries: [[String: String]] = []
    let specifications: [(String, Double, [Int])] = platform == "Mac"
        ? [16, 32, 128, 256, 512].map { ("mac", Double($0), [1, 2]) }
        : [("iphone", 20, [2, 3]), ("iphone", 29, [2, 3]), ("iphone", 40, [2, 3]),
           ("iphone", 60, [2, 3]), ("ipad", 20, [1, 2]), ("ipad", 29, [1, 2]),
           ("ipad", 40, [1, 2]), ("ipad", 76, [1, 2]), ("ipad", 83.5, [2]),
           ("ios-marketing", 1024, [1])]
    for (idiom, points, scales) in specifications {
        for scale in scales {
            let value = points.rounded() == points ? String(Int(points)) : String(points)
            let name = "\(idiom)-\(value)@\(scale)x.png"
            entries.append(["idiom": idiom, "size": "\(value)x\(value)",
                            "scale": "\(scale)x", "filename": name])
            try writePNG(size: Int(points * Double(scale)), to: icon.appendingPathComponent(name))
        }
    }
    try writeJSON(["info": info, "images": entries], to: icon.appendingPathComponent("Contents.json"))
}

let vision = root.appendingPathComponent("Resources/Vision.xcassets")
try writeJSON(["info": info], to: vision.appendingPathComponent("Contents.json"))
let originalStack = root.appendingPathComponent("Artwork/AppIcon.solidimagestack")
let visionStack = vision.appendingPathComponent("AppIcon.solidimagestack")
if !files.fileExists(atPath: visionStack.path) {
    try files.copyItem(at: originalStack, to: visionStack)
}
print("Packaged original silver and gold art for Mac, iPhone, iPad and Vision Pro.")
