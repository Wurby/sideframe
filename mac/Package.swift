// swift-tools-version:5.9
import PackageDescription
let package = Package(
    name: "mac-capture",
    platforms: [.macOS(.v13)],
    targets: [.executableTarget(name: "mac-capture", path: "Sources/mac-capture")]
)
