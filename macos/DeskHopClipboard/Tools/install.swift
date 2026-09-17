// Source-build installer. No clipboard access, preferences writes or login registration.
import AppKit

struct InstallError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

let files = FileManager.default
let bundleID = "org.deskhop.clipboard"

func run(_ executable: String, _ arguments: [String]) throws {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = arguments
    try process.run()
    process.waitUntilExit()
    guard process.terminationStatus == 0 else {
        throw InstallError("\(executable) failed (\(process.terminationStatus)).")
    }
}

func checkIdentity(_ app: URL) throws {
    let data = try Data(contentsOf: app.appendingPathComponent("Contents/Info.plist"))
    let info = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
    guard info?["CFBundleIdentifier"] as? String == bundleID,
          info?["CFBundleExecutable"] as? String == "DeskHopClipboard" else {
        throw InstallError("Refusing to replace or install an unrelated app: \(app.path)")
    }
}

func verify(_ app: URL) throws {
    try checkIdentity(app)
    guard files.isExecutableFile(atPath: app.appendingPathComponent("Contents/Helpers/DeskHopClipboardReader").path) else {
        throw InstallError("The bundled clipboard reader is missing or not executable.")
    }
    try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
}

func install() throws {
    guard CommandLine.arguments.count == 3 else {
        throw InstallError("Usage: install.swift SOURCE_APP INSTALL_DIRECTORY")
    }
    let source = URL(fileURLWithPath: CommandLine.arguments[1]).standardizedFileURL
    let directory = URL(fileURLWithPath: (CommandLine.arguments[2] as NSString).expandingTildeInPath).standardizedFileURL
    let destination = directory.appendingPathComponent("DeskHop Clipboard.app")
    let sourcePath = source.resolvingSymlinksInPath().path
    let destinationPath = destination.resolvingSymlinksInPath().path
    guard sourcePath != destinationPath,
          !sourcePath.hasPrefix(destinationPath + "/"),
          !destinationPath.hasPrefix(sourcePath + "/") else {
        throw InstallError("Build and installation locations must not overlap.")
    }
    try verify(source)
    if let attributes = try? files.attributesOfItem(atPath: destination.path) {
        guard attributes[.type] as? FileAttributeType == .typeDirectory else {
            throw InstallError("Installation destination must be an app directory, not a file or symlink.")
        }
        try checkIdentity(destination)
    }
    try files.createDirectory(at: directory, withIntermediateDirectories: true)
    let stage = directory.appendingPathComponent(".deskhop-install-\(UUID().uuidString)")
    try files.createDirectory(at: stage, withIntermediateDirectories: false)
    defer { try? files.removeItem(at: stage) }
    let stagedApp = stage.appendingPathComponent(destination.lastPathComponent)
    try files.copyItem(at: source, to: stagedApp)
    try verify(stagedApp)

    // Only manage copies being built/replaced; leave other installations alone.
    let paths = [source, destination].map { $0.resolvingSymlinksInPath() }
    let running = NSRunningApplication.runningApplications(withBundleIdentifier: bundleID).filter {
        guard let url = $0.bundleURL else { return false }
        return paths.contains(url.resolvingSymlinksInPath())
    }
    guard running.count <= 1 else {
        throw InstallError("Multiple copies are running. Quit the extra copies and retry.")
    }
    if let app = running.first {
        print("Quitting the running helper and waiting for serial-port release…")
        guard app.terminate() || app.isTerminated else {
            throw InstallError("Could not quit the helper. Quit it from its menu and retry.")
        }
        let deadline = ProcessInfo.processInfo.systemUptime + 10
        while !app.isTerminated && ProcessInfo.processInfo.systemUptime < deadline {
            RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.05))
        }
        guard app.isTerminated else {
            throw InstallError("The helper did not quit within 10 seconds; the installed app was not replaced.")
        }
    }

    do {
        if files.fileExists(atPath: destination.path) {
            // Foundation's safe-save operation retains the old app if replacement fails.
            _ = try files.replaceItemAt(destination, withItemAt: stagedApp)
        } else {
            try files.moveItem(at: stagedApp, to: destination)
        }
    } catch {
        if let previous = running.first?.bundleURL, files.fileExists(atPath: previous.path) {
            try? run("/usr/bin/open", ["-g", previous.path])
        }
        throw error
    }
    try verify(destination)
    print("Installed \(destination.path)")
    if !running.isEmpty {
        try run("/usr/bin/open", ["-g", destination.path])
        print("Restarted the helper using its saved connection settings.")
    } else {
        print("Open DeskHop Clipboard from Applications to configure or connect it.")
    }
    print("Connection preferences and Launch at login were not changed.")
}

do { try install() }
catch {
    FileHandle.standardError.write(Data("Install failed: \(error)\n".utf8))
    exit(1)
}
