import AppKit
import SwiftUI
import ServiceManagement
import ClipboardCore
import Darwin

// No crash dump may include the on-demand response. This precedes app startup.
var coreLimit = rlimit(rlim_cur: 0, rlim_max: 0)
guard setrlimit(RLIMIT_CORE, &coreLimit) == 0 else { exit(2) }
let fixtureMode = CommandLine.arguments.contains("--ui-fixture")

final class SystemLogin: LoginService {
    var state: LoginState {
        switch SMAppService.mainApp.status {
        case .enabled: return .enabled
        case .requiresApproval: return .approvalRequired
        case .notRegistered: return .disabled
        case .notFound: return .unavailable
        @unknown default: return .unavailable
        }
    }
    func register() throws { try SMAppService.mainApp.register() }
    func unregister() throws { try SMAppService.mainApp.unregister() }
}
final class PreviewLogin: LoginService {
    var state: LoginState = .disabled
    func register() { state = .enabled }
    func unregister() { state = .disabled }
}
final class AppModel: ObservableObject {
    @Published var port = ""
    @Published var boardID = ""
    @Published var build = "0.115"
    @Published var state: ConnectionState = .paused
    @Published var loginState: LoginState = .disabled
    @Published var notice = ""
    var changed: (() -> Void)?
    private let login: LoginService = fixtureMode ? PreviewLogin() : SystemLogin()
    private var controller: ConnectionController?
    private let defaults: UserDefaults? = fixtureMode ? nil : .standard
    var running: Bool { fixtureMode ? state == .connected : controller?.running == true }
    var status: String { state.label }
    init() {
        if fixtureMode {
            port = "/dev/cu.usbmodem-DEMO"; boardID = "0123456789ABCDEF"
            notice = "Fixture preview — no serial, clipboard or login-item access."
        } else {
            port = defaults?.string(forKey: "port") ?? ""
            boardID = defaults?.string(forKey: "boardID") ?? ""
            build = defaults?.string(forKey: "build") ?? "0.115"
            let reader = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/DeskHopClipboardReader")
            controller = ConnectionController(reader: NativeTextReader(executable: reader))
            controller?.onState = { [weak self] in self?.state = $0; self?.changed?() }
        }
        refreshLogin()
    }
    func autoStartIfRequested() {
        if defaults?.bool(forKey: "resumeOnLaunch") == true { connect() }
    }
    func connect() {
        do { try Configuration(port: port, boardID: boardID, build: build).validate() }
        catch { state = .failed(.configuration); changed?(); return }
        if fixtureMode { state = .connected; changed?(); return }
        defaults?.set(port, forKey: "port"); defaults?.set(boardID.uppercased(), forKey: "boardID"); defaults?.set(build, forKey: "build")
        defaults?.set(true, forKey: "resumeOnLaunch")
        controller?.start(Configuration(port: port, boardID: boardID, build: build))
    }
    func pause() {
        if fixtureMode { state = .paused; changed?(); return }
        defaults?.set(false, forKey: "resumeOnLaunch"); controller?.stop()
    }
    func quit(_ done: @escaping () -> Void) {
        if fixtureMode { done() } else { controller?.stop(completion: done) }
    }
    func refreshLogin() { loginState = login.state; changed?() }
    func setLogin(_ enabled: Bool) {
        // Keep login registration tied to the app's permanent user-selected
        // Applications location, never a build folder or mounted installer.
        let path = Bundle.main.bundleURL.standardizedFileURL.path
        let userApps = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications").path + "/"
        if enabled && !fixtureMode && !path.hasPrefix("/Applications/") && !path.hasPrefix(userApps) {
            notice = "Move DeskHop Clipboard to Applications, then enable Launch at login."; return
        }
        do {
            try LoginPolicy.userSelected(enabled, service: login)
            notice = fixtureMode ? "Fixture preview — login setting is simulated." : ""
        } catch { notice = "macOS could not change the login item. Review Login Items in System Settings." }
        refreshLogin()
    }
    func openLoginSettings() { if !fixtureMode { SMAppService.openSystemSettingsLoginItems() } }
}
struct SettingsView: View {
    @ObservedObject var model: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack(spacing: 12) {
                Image(systemName: "keyboard.badge.ellipsis").font(.system(size: 30)).foregroundStyle(.blue)
                VStack(alignment: .leading, spacing: 3) {
                    Text("DeskHop Clipboard").font(.title2.bold())
                    Text("Share this Mac’s text with the other Mac").foregroundStyle(.secondary)
                }
            }
            Label(model.status, systemImage: model.state == .connected ? "checkmark.circle.fill" : "pause.circle")
                .font(.callout).foregroundStyle(model.state == .connected ? Color.green : Color.secondary)
                .accessibilityIdentifier("connection-status")
            GroupBox("Connect to this Mac’s DeskHop") {
                Grid(alignment: .leading, verticalSpacing: 10) {
                    GridRow { Text("Serial port"); TextField("/dev/cu.usbmodem…", text: $model.port).accessibilityIdentifier("serial-port") }
                    GridRow { Text("Local board ID"); TextField("16 hexadecimal characters", text: $model.boardID).accessibilityIdentifier("board-id") }
                    GridRow { Text("Firmware version"); TextField("0.115", text: $model.build).accessibilityIdentifier("firmware-version") }
                }.textFieldStyle(.roundedBorder).padding(8).disabled(model.running)
            }
            HStack {
                Button(model.running ? "Pause & release serial port" : "Connect / Resume") {
                    if model.running { model.pause() } else { model.connect() }
                }.keyboardShortcut(.defaultAction).accessibilityIdentifier("pause-resume")
                Spacer()
                Toggle("Launch at login", isOn: Binding(get: { model.loginState == .enabled || model.loginState == .approvalRequired }, set: model.setLogin))
                    .toggleStyle(.checkbox).accessibilityIdentifier("launch-at-login")
            }
            if model.loginState == .approvalRequired {
                HStack { Text("Approval required in System Settings."); Button("Open Login Items", action: model.openLoginSettings) }.font(.callout)
            }
            if model.loginState == .unavailable { Text("Login startup is unavailable for this bundle.").font(.callout).foregroundStyle(.secondary) }
            Divider()
            Text("Copy on this Mac, switch to the other Mac, then press and release the DeskHop clipboard shortcut. The opposite Mac supplies text only while its helper is connected. Up to 1024 bytes of U.S. ASCII text, tabs and newlines.")
                .font(.callout).fixedSize(horizontal: false, vertical: true)
            Text("Pause before using diagnostics, configuration or the updater. No clipboard history is stored. This app never displays clipboard contents.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if !model.notice.isEmpty { Text(model.notice).font(.callout).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true) }
        }.padding(24).frame(width: 540).onAppear { model.refreshLogin() }
    }
}
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let model = AppModel()
    var item: NSStatusItem!, window: NSWindow?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = NSImage(systemSymbolName: "keyboard", accessibilityDescription: "DeskHop Clipboard")
        model.changed = { [weak self] in self?.refreshMenu() }
        refreshMenu()
        if fixtureMode || model.port.isEmpty { settings() }
        model.autoStartIfRequested()
    }
    func refreshMenu() {
        guard item != nil else { return }
        let menu = item.menu ?? NSMenu(); menu.removeAllItems(); menu.delegate = self
        let status = NSMenuItem(title: model.status, action: nil, keyEquivalent: ""); status.isEnabled = false; menu.addItem(status)
        menu.addItem(.separator())
        for (title, action, key) in [(model.running ? "Pause & release serial port" : "Connect / Resume", #selector(toggle), ""), ("Settings…", #selector(settings), ",")] {
            let entry = NSMenuItem(title: title, action: action, keyEquivalent: key); entry.target = self; menu.addItem(entry)
        }
        let login = NSMenuItem(title: "Launch at login", action: #selector(toggleLogin), keyEquivalent: ""); login.target = self
        login.state = model.loginState == .enabled ? .on : (model.loginState == .approvalRequired ? .mixed : .off); menu.addItem(login)
        if model.loginState == .approvalRequired {
            let approval = NSMenuItem(title: "Review Login Items…", action: #selector(loginSettings), keyEquivalent: ""); approval.target = self; menu.addItem(approval)
        }
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit DeskHop Clipboard", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"); menu.addItem(quit)
        item.menu = menu; item.button?.toolTip = "DeskHop Clipboard — \(model.status)"
    }
    func menuWillOpen(_ menu: NSMenu) { model.refreshLogin() }
    @objc func toggle() { if model.running { model.pause() } else if model.port.isEmpty { settings() } else { model.connect() } }
    @objc func toggleLogin() { model.setLogin(!(model.loginState == .enabled || model.loginState == .approvalRequired)); if !model.notice.isEmpty { settings() } }
    @objc func loginSettings() { model.openLoginSettings() }
    @objc func settings() {
        if window == nil {
            let host = NSHostingController(rootView: SettingsView(model: model))
            let w = NSWindow(contentViewController: host); w.title = "DeskHop Clipboard"
            w.styleMask = [.titled, .closable, .miniaturizable]; w.isReleasedWhenClosed = false
            w.center(); window = w
        }
        NSApp.activate(ignoringOtherApps: true); window?.makeKeyAndOrderFront(nil)
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool { settings(); return true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if !model.running || fixtureMode { return .terminateNow }
        model.quit { sender.reply(toApplicationShouldTerminate: true) }; return .terminateLater
    }
}
if CommandLine.arguments.contains("--bundle-check") {
    // Offline packaging check: no NSApplication, preferences, serial, login API
    // or pasteboard is instantiated on this path.
    guard Bundle.main.bundleIdentifier == "org.deskhop.clipboard",
          FileManager.default.isExecutableFile(atPath: Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/DeskHopClipboardReader").path) else { exit(1) }
    print("DeskHop Clipboard bundle check passed"); exit(0)
}
let app = NSApplication.shared
let delegate = AppDelegate(); app.delegate = delegate; app.run()
