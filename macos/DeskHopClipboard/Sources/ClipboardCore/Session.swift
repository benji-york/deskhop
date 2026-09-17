import Foundation
import Darwin

public protocol TextReader { func read(deadline: TimeInterval, tick: () throws -> Void) throws -> TextResult }
public final class NativeTextReader: TextReader {
    private let executable: URL
    private let arguments: [String]
    public init(executable: URL) { self.executable = executable; self.arguments = ["--read-current"] }
    // The test harness uses an explicit fixture executable; never a pasteboard.
    package init(fixtureExecutable: URL, arguments: [String]) { executable = fixtureExecutable; self.arguments = arguments }
    public func read(deadline: TimeInterval, tick: () throws -> Void) throws -> TextResult {
        let raw = Bytes(capacity: 1028); defer { raw.clear() }
        let process = Process(), pipe = Pipe()
        process.executableURL = executable; process.arguments = arguments
        process.standardInput = FileHandle.nullDevice; process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice
        do { try process.run() } catch { return TextResult(status: .unavailable) }
        pipe.fileHandleForWriting.closeFile()
        defer {
            if process.isRunning { _ = kill(process.processIdentifier, SIGKILL) }
            process.waitUntilExit(); pipe.fileHandleForReading.closeFile()
        }
        let fd = pipe.fileHandleForReading.fileDescriptor
        guard fcntl(fd, F_SETFL, O_NONBLOCK) == 0 else { return TextResult(status: .unavailable) }
        let end = min(deadline, uptime() + 2)
        var eof = false
        while !eof || process.isRunning {
            try tick()
            if uptime() >= end { return TextResult(status: .unavailable) }
            if eof { Thread.sleep(forTimeInterval: 0.01); continue }
            var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let result = poll(&p, 1, 25)
            if result < 0 && errno == EINTR { continue }
            if result < 0 || p.revents & Int16(POLLERR | POLLNVAL) != 0 { return TextResult(status: .unavailable) }
            if p.revents & Int16(POLLIN | POLLHUP) == 0 { continue }
            let n = Darwin.read(fd, raw.writeView(raw.count).baseAddress!, raw.capacity - raw.count)
            if n < 0 && (errno == EINTR || errno == EAGAIN) { continue }
            if n < 0 { return TextResult(status: .unavailable) }
            if n == 0 { eof = true }
            raw.count += max(0, n)
            if raw.count == raw.capacity { return TextResult(status: .unavailable) }
        }
        process.waitUntilExit()
        guard process.terminationStatus == 0, raw.count >= 3, let status = TextStatus(rawValue: raw[0]) else { return TextResult(status: .unavailable) }
        let length = Int(raw.get(1, width: 2))
        guard length <= 1024 && raw.count == length + 3 && (status == .ok || length == 0) else { return TextResult(status: .unavailable) }
        let bytes = Bytes(capacity: 1024); bytes.count = length
        bytes.copy(from: raw.readView(3, length))
        if status == .ok && classify(bytes) != .ok { bytes.clear(); return TextResult(status: .unavailable) }
        return TextResult(status: status, bytes: bytes)
    }
}
public final class Session {
    private let transport: Transport, reader: TextReader, cancellation: Cancellation
    private let boot: UInt64, helper: UInt64, now: () -> TimeInterval
    private var targetBoot: UInt64?, lastNonce: UInt64 = 0, nextPing: TimeInterval = 0
    public init(transport: Transport, reader: TextReader, cancellation: Cancellation, boot: UInt64, helper: UInt64, now: @escaping () -> TimeInterval = uptime) {
        self.transport = transport; self.reader = reader; self.cancellation = cancellation
        self.boot = boot; self.helper = helper; self.now = now
    }
    public func heartbeat() throws {
        try cancellation.check()
        if now() >= nextPing {
            let b = Wire.frame(5); defer { b.clear() }; b.put(helper, at: 8)
            try transport.write(b.readView(), deadline: now() + 0.5)
            nextPing = now() + 0.25
        }
    }
    public func handle(_ frame: Bytes) throws {
        try cancellation.check()
        let started = now(), binding = try Wire.request(frame)
        try require(binding.sourceBoot == boot && binding.helper == helper)
        if let previous = targetBoot, previous != binding.targetBoot { throw HelperError.peerRestarted }
        try require(binding.nonce > lastNonce)
        targetBoot = binding.targetBoot; lastNonce = binding.nonce
        let result = try reader.read(deadline: started + 2.5, tick: heartbeat)
        defer { result.bytes.clear() }
        try cancellation.check()
        if now() >= started + 2.5 { return }
        try Wire.respond(binding, result: result) { frame in
            try self.heartbeat()
            try require(self.now() < started + 2.5, .timeout)
            try self.transport.write(frame.readView(), deadline: min(started + 2.5, self.now() + 0.5))
        }
    }
    public func run() throws {
        let frame = Bytes(capacity: 64); defer { frame.clear() }
        var partialSince: TimeInterval?
        while true {
            try heartbeat()
            if let start = partialSince { try require(now() - start < 1) }
            let n = try transport.read(into: frame.writeView(frame.count), deadline: now() + 0.05)
            if n > 0 {
                if let start = partialSince { try require(now() - start < 1) }
                if frame.count == 0 { partialSince = now() }
                frame.count += n
                if frame.count == 64 { try handle(frame); frame.clear(); partialSince = nil }
            }
        }
    }
}
