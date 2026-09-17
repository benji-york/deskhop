import Foundation

public enum ConnectionState: Equatable {
    case paused, connecting, connected, reconnecting, stopping, failed(HelperError)
    public var label: String {
        switch self {
        case .paused: return "Paused — serial port released"
        case .connecting: return "Connecting to this Mac’s DeskHop…"
        case .connected: return "Connected locally — listening for the shortcut"
        case .reconnecting: return "Disconnected — retrying the same device"
        case .stopping: return "Pausing and releasing the serial port…"
        case .failed(let error): return error.message
        }
    }
}
public protocol LoginService {
    var state: LoginState { get }
    func register() throws
    func unregister() throws
}
public enum LoginState: Equatable { case disabled, enabled, approvalRequired, unavailable }
public enum LoginPolicy {
    // Reading/launching the app never calls this. Only a user action may change
    // ServiceManagement state; preference storage is not registration authority.
    public static func userSelected(_ enabled: Bool, service: LoginService) throws {
        if enabled {
            if service.state != .enabled && service.state != .approvalRequired { try service.register() }
        } else if service.state == .enabled || service.state == .approvalRequired { try service.unregister() }
    }
}
public final class ConnectionController {
    public typealias Factory = (Configuration, Cancellation) throws -> Transport
    private let factory: Factory, reader: TextReader
    private let queue = DispatchQueue(label: "org.deskhop.clipboard.connection", qos: .userInitiated)
    // The owner calls public methods on the main thread. Worker owns transport;
    // stop cancels it and waits for its defer to drop DTR before another start.
    private var cancellation: Cancellation?
    private var completion: (() -> Void)?
    public var onState: (ConnectionState) -> Void = { _ in }
    public init(reader: TextReader, factory: @escaping Factory = { try SerialTransport(configuration: $0, cancellation: $1) }) {
        self.reader = reader; self.factory = factory
    }
    public var running: Bool { cancellation != nil }
    public func start(_ configuration: Configuration) {
        precondition(Thread.isMainThread)
        guard cancellation == nil else { return }
        do { try configuration.validate() } catch { onState(.failed(.configuration)); return }
        let token = Cancellation(); cancellation = token; onState(.connecting)
        queue.async {
            var outcome: ConnectionState = .paused
            for attempt in 0...3 {
                do {
                    try token.check()
                    let transport = try self.factory(configuration, token)
                    defer { transport.close() }
                    let helper = UInt64.random(in: 1...UInt64.max)
                    let boot = try Handshake.open(on: transport, configuration: configuration, helper: helper)
                    try token.check()
                    DispatchQueue.main.async { if !token.isCancelled { self.onState(.connected) } }
                    try Session(transport: transport, reader: self.reader, cancellation: token, boot: boot, helper: helper).run()
                } catch {
                    let failure = (error as? HelperError) ?? .io
                    if token.isCancelled || failure == .cancelled { outcome = .paused; break }
                    let retryable = failure == .io || failure == .timeout || failure == .peerRestarted
                    if !retryable || attempt == 3 { outcome = .failed(failure); break }
                    DispatchQueue.main.async { if !token.isCancelled { self.onState(.reconnecting) } }
                    let until = uptime() + 1
                    while !token.isCancelled && uptime() < until { Thread.sleep(forTimeInterval: 0.025) }
                }
            }
            let final = outcome
            DispatchQueue.main.async {
                self.cancellation = nil; self.onState(token.isCancelled ? .paused : final)
                let done = self.completion; self.completion = nil; done?()
            }
        }
    }
    public func stop(completion: (() -> Void)? = nil) {
        precondition(Thread.isMainThread)
        guard let cancellation else { completion?(); return }
        if let completion { self.completion = completion }
        onState(.stopping); cancellation.cancel()
    }
}
