// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "MagiTrackEngine",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
    ],
    products: [
        .library(name: "MagiTrackCore",    targets: ["MagiTrackCore"]),
        .library(name: "MagiTrackFile",    targets: ["MagiTrackFile"]),
        .library(name: "MagiTrackWire",    targets: ["MagiTrackWire"]),
        .library(name: "MagiTrackNet",     targets: ["MagiTrackNet"]),
        .library(name: "MagiTrackPainter", targets: ["MagiTrackPainter"]),
    ],
    targets: [
        .target(name: "MagiTrackCore"),
        .target(name: "MagiTrackFile",    dependencies: ["MagiTrackCore"]),
        .target(name: "MagiTrackWire",    dependencies: ["MagiTrackCore"]),
        .target(name: "MagiTrackNet",     dependencies: ["MagiTrackCore", "MagiTrackWire"]),
        .target(name: "MagiTrackPainter", dependencies: ["MagiTrackCore"]),

        .testTarget(name: "MagiTrackCoreTests",    dependencies: ["MagiTrackCore"]),
        .testTarget(name: "MagiTrackFileTests",    dependencies: ["MagiTrackCore", "MagiTrackFile"]),
        .testTarget(name: "MagiTrackWireTests",    dependencies: ["MagiTrackCore", "MagiTrackWire"]),
        .testTarget(name: "MagiTrackNetTests",     dependencies: ["MagiTrackCore", "MagiTrackWire", "MagiTrackNet"]),
        .testTarget(name: "MagiTrackPainterTests", dependencies: ["MagiTrackCore", "MagiTrackPainter"]),
    ]
)
