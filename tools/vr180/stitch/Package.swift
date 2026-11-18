// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "stitch",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "StitchKit", path: "Sources/StitchKit"),
        .executableTarget(name: "stitch", dependencies: ["StitchKit"], path: "Sources/stitch"),
        .executableTarget(name: "stitch-synth", dependencies: ["StitchKit"], path: "Sources/stitch-synth"),
    ]
)
