import Foundation
import Darwin

public enum HelperError: Error, Equatable {
    case configuration, identity, protocolViolation, io, timeout, cancelled, peerRestarted
    public var message: String {
        switch self {
        case .configuration: return "Enter A’s serial port, board ID and exact firmware version."
        case .identity: return "Board identity or firmware did not match. Check settings."
        case .protocolViolation: return "Connection rejected an invalid or stale response."
        case .io: return "DeskHop disconnected or its serial port is in use."
        case .timeout: return "DeskHop did not respond in time."
        case .cancelled: return "Paused — serial port released."
        case .peerRestarted: return "Work-side DeskHop restarted. Reconnecting securely."
        }
    }
}
public func require(_ condition: Bool, _ error: HelperError = .protocolViolation) throws {
    if !condition { throw error }
}
public func uptime() -> TimeInterval { ProcessInfo.processInfo.systemUptime }
public final class Cancellation {
    private let lock = NSLock()
    private var stopped = false
    public init() {}
    public func cancel() { lock.lock(); stopped = true; lock.unlock() }
    public var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
    public func check() throws { if isCancelled { throw HelperError.cancelled } }
}

// Unique storage: text is never put in String, UI state, logs or UserDefaults.
// Explicit zeroing covers this owned allocation, not OS/runtime-owned copies.
public final class Bytes {
    public let capacity: Int
    private let pointer: UnsafeMutableRawPointer
    public var count: Int = 0
    public init(capacity: Int) {
        precondition(capacity > 0 && capacity <= 32768)
        self.capacity = capacity
        pointer = .allocate(byteCount: capacity, alignment: 8)
        pointer.initializeMemory(as: UInt8.self, repeating: 0, count: capacity)
    }
    deinit { clear(); pointer.deallocate() }
    public func clear() { _ = memset_s(pointer, capacity, 0, capacity); count = 0 }
    public subscript(_ index: Int) -> UInt8 {
        get { precondition(index >= 0 && index < capacity); return pointer.load(fromByteOffset: index, as: UInt8.self) }
        set { precondition(index >= 0 && index < capacity); pointer.storeBytes(of: newValue, toByteOffset: index, as: UInt8.self) }
    }
    public func readView(_ offset: Int = 0, _ length: Int? = nil) -> UnsafeRawBufferPointer {
        let size = length ?? count
        precondition(offset >= 0 && size >= 0 && offset + size <= capacity)
        return UnsafeRawBufferPointer(start: pointer.advanced(by: offset), count: size)
    }
    public func writeView(_ offset: Int = 0, _ length: Int? = nil) -> UnsafeMutableRawBufferPointer {
        let size = length ?? (capacity - offset)
        precondition(offset >= 0 && size >= 0 && offset + size <= capacity)
        return UnsafeMutableRawBufferPointer(start: pointer.advanced(by: offset), count: size)
    }
    public func put(_ value: UInt64, at offset: Int, width: Int = 8) {
        for i in 0..<width { self[offset + i] = UInt8(truncatingIfNeeded: value >> (i * 8)) }
    }
    public func get(_ offset: Int, width: Int = 8) -> UInt64 {
        (0..<width).reduce(UInt64(0)) { $0 | UInt64(self[offset + $1]) << ($1 * 8) }
    }
    public func copy(from source: UnsafeRawBufferPointer, at offset: Int = 0) {
        precondition(offset >= 0 && source.count + offset <= capacity)
        if source.count > 0 { pointer.advanced(by: offset).copyMemory(from: source.baseAddress!, byteCount: source.count) }
    }
}
public enum TextStatus: UInt8, CaseIterable { case ok, empty, nonText, oversize, unsupported, unavailable }
public func classify(_ bytes: Bytes) -> TextStatus {
    if bytes.count > 1024 { return .oversize }
    if bytes.count == 0 { return .empty }
    for i in 0..<bytes.count where bytes[i] != 9 && bytes[i] != 10 && !(32...126).contains(bytes[i]) { return .unsupported }
    return .ok
}
public struct TextResult {
    public let status: TextStatus
    public let bytes: Bytes
    public init(status: TextStatus, bytes: Bytes = Bytes(capacity: 1024)) { self.status = status; self.bytes = bytes }
}
public struct Binding: Equatable {
    public let values: [UInt64]
    public init(_ values: [UInt64]) throws { try require(values.count == 5 && values.allSatisfy { $0 != 0 }); self.values = values }
    public var sourceBoot: UInt64 { values[0] }
    public var targetBoot: UInt64 { values[1] }
    public var helper: UInt64 { values[2] }
    public var nonce: UInt64 { values[3] }
}
public enum Wire {
    public static func frame(_ opcode: UInt8) -> Bytes {
        let b = Bytes(capacity: 64); b.count = 64
        for (i, c) in Array("DHC2".utf8).enumerated() { b[i] = c }; b[4] = opcode
        return b
    }
    public static func header(_ b: Bytes, opcode: UInt8, used: Int) throws {
        try require(b.count == 64 && b[0] == 68 && b[1] == 72 && b[2] == 67 && b[3] == 50 && b[4] == opcode)
        try require((5..<8).allSatisfy { b[$0] == 0 } && ((8 + used)..<64).allSatisfy { b[$0] == 0 })
    }
    public static func hello(_ b: Bytes, boot: UInt64, helper: UInt64) throws {
        try header(b, opcode: 0, used: 16); try require(b.get(8) == boot && b.get(16) == helper)
    }
    public static func request(_ b: Bytes) throws -> Binding {
        try header(b, opcode: 1, used: 40)
        return try Binding((0..<5).map { b.get(8 + $0 * 8) })
    }
    public static func crc(_ b: Bytes) -> UInt32 {
        var crc: UInt32 = .max
        for i in 0..<b.count {
            crc ^= UInt32(b[i])
            for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) == 1 ? 0xedb88320 : 0) }
        }
        return ~crc
    }
    // Send one owned frame at a time, including on exceptions/cancellation.
    public static func respond(_ binding: Binding, result: TextResult, send: (Bytes) throws -> Void) throws {
        defer { result.bytes.clear() }
        try require(result.status == .ok ? classify(result.bytes) == .ok : result.bytes.count == 0)
        let b = frame(2); defer { b.clear() }
        for i in 0..<5 { b.put(binding.values[i], at: 8 + i * 8) }
        b[48] = result.status.rawValue
        b.put(UInt64(result.bytes.count), at: 49, width: 2)
        b.put(result.status == .ok ? UInt64(crc(result.bytes)) : 0, at: 51, width: 4)
        try send(b); b.clear()
        guard result.status == .ok else { return }
        for offset in stride(from: 0, to: result.bytes.count, by: 40) {
            let data = frame(3); defer { data.clear() }
            let count = min(40, result.bytes.count - offset)
            data.put(binding.nonce, at: 8); data.put(UInt64(offset), at: 16, width: 2); data[18] = UInt8(count)
            data.copy(from: result.bytes.readView(offset, count), at: 19)
            try send(data)
        }
        let end = frame(4); defer { end.clear() }; end.put(binding.nonce, at: 8); try send(end)
    }
}
public struct Configuration: Codable, Equatable {
    public var port: String, boardID: String, build: String
    public init(port: String = "", boardID: String = "", build: String = "0.115") {
        self.port = port; self.boardID = boardID.uppercased(); self.build = build
    }
    public func validate() throws {
        try require(matches(port, #"/dev/cu\.[A-Za-z0-9._-]+"#) && matches(boardID, "[0-9A-F]{16}"), .configuration)
        try require(try version(build) >= 215, .configuration)
    }
}
func matches(_ text: String, _ expression: String) -> Bool {
    text.range(of: "\\A(?:" + expression + ")\\z", options: .regularExpression) != nil
}
func version(_ text: String) throws -> UInt64 {
    try require(matches(text, #"(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,2})"#), .configuration)
    let p = text.split(separator: ".").map { UInt64($0)! }; let n = p[0] * 1000 + p[1] + 100
    try require(n <= 65535, .configuration); return n
}
