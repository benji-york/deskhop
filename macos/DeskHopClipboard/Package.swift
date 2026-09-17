// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "DeskHopClipboard", platforms: [.macOS(.v13)],
    products: [.executable(name: "DeskHopClipboard", targets: ["DeskHopClipboard"])],
    targets: [
        .target(name: "ClipboardCore"),
        .executableTarget(name: "DeskHopClipboard", dependencies: ["ClipboardCore"]),
        // An offline executable harness also works with Command Line Tools;
        // XCTest requires the full Xcode installation on macOS.
        .executableTarget(name: "ClipboardCoreTests", dependencies: ["ClipboardCore"], path: "Tests/ClipboardCoreTests")
    ]
)
