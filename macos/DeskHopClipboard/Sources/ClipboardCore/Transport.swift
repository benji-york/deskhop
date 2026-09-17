import Foundation
import Darwin

public protocol Transport: AnyObject {
    func read(into buffer: UnsafeMutableRawBufferPointer, deadline: TimeInterval) throws -> Int
    func write(_ buffer: UnsafeRawBufferPointer, deadline: TimeInterval) throws
    func close()
}
public final class SerialTransport: Transport {
    private var fd: Int32 = -1
    private var original: termios?
    private let cancellation: Cancellation
    // Offline tests supply an owned Unix socket endpoint, never a serial port.
    package init(fixtureFD: Int32, cancellation: Cancellation) {
        self.fd = fixtureFD; self.cancellation = cancellation
        _ = fcntl(fd, F_SETFL, O_NONBLOCK)
    }
    public init(configuration: Configuration, cancellation: Cancellation) throws {
        self.cancellation = cancellation
        try configuration.validate(); try cancellation.check()
        fd = Darwin.open(configuration.port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW)
        do {
            try require(fd >= 0, .io)
            var info = stat(); try require(fstat(fd, &info) == 0 && (info.st_mode & S_IFMT) == S_IFCHR, .io)
            try require(ioctl(fd, TIOCEXCL) == 0, .io)
            var attrs = termios(); try require(tcgetattr(fd, &attrs) == 0, .io); original = attrs
            cfmakeraw(&attrs); attrs.c_cflag |= tcflag_t(CLOCAL | CREAD)
            attrs.c_cflag &= ~tcflag_t(CCTS_OFLOW | CRTS_IFLOW)
            cfsetispeed(&attrs, speed_t(B115200)); cfsetospeed(&attrs, speed_t(B115200))
            withUnsafeMutableBytes(of: &attrs.c_cc) { p in p[Int(VMIN)] = 0; p[Int(VTIME)] = 0 }
            try require(tcsetattr(fd, TCSANOW, &attrs) == 0, .io)
            try dtr(false)
            let until = uptime() + 0.15
            while uptime() < until { try cancellation.check(); Thread.sleep(forTimeInterval: 0.01) }
            try require(tcflush(fd, TCIFLUSH) == 0, .io); try dtr(true)
        } catch { close(); throw error }
    }
    deinit { close() }
    private func dtr(_ value: Bool) throws {
        var mask = Int32(TIOCM_DTR)
        try require(ioctl(fd, value ? TIOCMBIS : TIOCMBIC, &mask) == 0, .io)
    }
    public func close() {
        guard fd >= 0 else { return }
        try? dtr(false)
        if var attrs = original { _ = tcsetattr(fd, TCSANOW, &attrs) }
        _ = ioctl(fd, TIOCNXCL); _ = Darwin.close(fd); fd = -1; original = nil
    }
    private func ready(_ events: Int16, deadline: TimeInterval) throws -> Bool {
        while uptime() < deadline {
            try cancellation.check()
            var p = pollfd(fd: fd, events: events, revents: 0)
            let ms = Int32(max(1, min(50, ceil((deadline - uptime()) * 1000))))
            let n = poll(&p, 1, ms)
            if n < 0 && errno == EINTR { continue }
            try require(n >= 0 && p.revents & Int16(POLLERR | POLLHUP | POLLNVAL) == 0, .io)
            if p.revents & events != 0 { return true }
        }
        return false
    }
    public func read(into buffer: UnsafeMutableRawBufferPointer, deadline: TimeInterval) throws -> Int {
        try require(buffer.count > 0)
        while try ready(Int16(POLLIN), deadline: deadline) {
            let n = Darwin.read(fd, buffer.baseAddress!, buffer.count)
            if n < 0 && (errno == EINTR || errno == EAGAIN) { continue }
            try require(n >= 0, .io)
            if n > 0 { return n }
            Thread.sleep(forTimeInterval: 0.005)
        }
        return 0
    }
    public func write(_ buffer: UnsafeRawBufferPointer, deadline: TimeInterval) throws {
        var offset = 0
        while offset < buffer.count {
            try require(try ready(Int16(POLLOUT), deadline: deadline), .timeout)
            let n = Darwin.write(fd, buffer.baseAddress!.advanced(by: offset), buffer.count - offset)
            if n < 0 && (errno == EINTR || errno == EAGAIN) { continue }
            try require(n > 0, .io); offset += n
        }
    }
}
public enum Handshake {
    static let prompt = "deskhop> "
    static func send(_ text: String, on transport: Transport, deadline: TimeInterval) throws {
        try Array(text.utf8).withUnsafeBytes { try transport.write($0, deadline: deadline) }
    }
    static func until(_ marker: String, limit: Int, on transport: Transport, deadline: TimeInterval) throws -> Bytes {
        let b = Bytes(capacity: limit), end = Array(marker.utf8)
        while b.count < limit {
            let n = try transport.read(into: b.writeView(b.count, 1), deadline: deadline)
            try require(n == 1, .timeout); b.count += 1
            if b.count >= end.count && end.indices.allSatisfy({ b[b.count - end.count + $0] == end[$0] }) { return b }
        }
        throw HelperError.protocolViolation
    }
    public static func open(on transport: Transport, configuration: Configuration, helper: UInt64, now: () -> TimeInterval = uptime) throws -> UInt64 {
        let greeting = try until(prompt, limit: 128, on: transport, deadline: now() + 6)
        defer { greeting.clear() }
        try require(String(decoding: greeting.readView(), as: UTF8.self) == "\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> ")
        try send("status\n", on: transport, deadline: now() + 2)
        let status = try until(prompt, limit: 32768, on: transport, deadline: now() + 6)
        defer { status.clear() }
        let boot = try Identity.localBoot(status, configuration: configuration)
        try require(helper != 0)
        let command = String(format: "clipboard %016llx", helper)
        try send(command + "\n", on: transport, deadline: now() + 2)
        let echo = try until("\r\n", limit: 128, on: transport, deadline: now() + 2)
        defer { echo.clear() }
        try require(String(decoding: echo.readView(), as: UTF8.self) == command + "\r\n")
        let hello = Bytes(capacity: 64); defer { hello.clear() }
        let deadline = now() + 2
        while hello.count < 64 {
            let n = try transport.read(into: hello.writeView(hello.count), deadline: deadline)
            try require(n > 0, .timeout); hello.count += n
        }
        try Wire.hello(hello, boot: boot, helper: helper)
        return boot
    }
}
