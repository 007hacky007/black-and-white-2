// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "BW2Builder",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "BuilderCore"),
        .executableTarget(name: "BW2Builder", dependencies: ["BuilderCore"]),
        .testTarget(name: "BuilderCoreTests", dependencies: ["BuilderCore"]),
    ]
)
