import Foundation
import ClipboardCore

func bytes(_ array: [UInt8]) -> Bytes {
    let b = Bytes(capacity: max(1, array.count)); b.count = array.count
    array.withUnsafeBytes { b.copy(from: $0) }; return b
}
func request(_ values: [UInt64] = [1, 2, 3, 4, 5]) -> Bytes {
    let b = Wire.frame(1); for i in 0..<5 { b.put(values[i], at: 8 + i * 8) }; return b
}
func status(_ uid: String = "0123456789ABCDEF", build: String = "0.115", peer: Bool = true) -> String {
    var lines = ["status", "BEGIN status"]
    for role in (peer ? ["A", "B"] : ["A"]) {
        lines += ["board=\(role)", "board_id=\(role == "A" ? uid : "FEDCBA9876543210")", "build=\(build)",
                  "image_crc_at_boot=deadbeef", "boot_session=\(role == "A" ? "0000000000000001" : "0000000000000002")", "uptime_ms=1000", "",
                  "board=\(role) core=0 checkpoints=1000 age_ms=1", "board=\(role) core=1 checkpoints=1001 age_ms=1",
                  "board=\(role) update seen=0 source=none phase=idle received=0 total=262144 progress_age_ms=unavailable target=unknown attempt=0"]
        if role == "B" { lines += ["board=B observation boot=first_seen progress=baseline update=not_observed"] }
        lines.append("")
    }
    lines += ["peer=\(peer ? "ok" : "timeout_or_unsupported")", "verification=available", "END status", "deskhop> "]
    return lines.joined(separator: "\r\n")
}
let config = Configuration(port: "/dev/cu.FIXTURE", boardID: "0123456789ABCDEF", build: "0.115")
final class FakeTransport: Transport {
    var input: [UInt8] = [], writes: [[UInt8]] = [], closes = 0, readLimit = 64
    var failOpcode: UInt8?, whenRead: (() -> Void)?, onWrite: (([UInt8]) -> Void)?
    func read(into b: UnsafeMutableRawBufferPointer, deadline: TimeInterval) throws -> Int {
        whenRead?(); let n = min(b.count, min(readLimit, input.count))
        for i in 0..<n { b[i] = input[i] }; input.removeFirst(n); return n
    }
    func write(_ b: UnsafeRawBufferPointer, deadline: TimeInterval) throws {
        if b.count == 64 && b[4] == failOpcode { throw HelperError.io }
        let copy = Array(b); writes.append(copy); onWrite?(copy)
    }
    func close() { closes += 1 }
}
final class FixtureReader: TextReader {
    var calls = 0, texts: [[UInt8]] = [Array("fixture".utf8)], afterRead: (() throws -> Void)?
    var returned: Bytes?
    func read(deadline: TimeInterval, tick: () throws -> Void) throws -> TextResult {
        calls += 1; try tick(); try afterRead?()
        let b = bytes(texts[min(calls - 1, texts.count - 1)]); returned = b
        return TextResult(status: classify(b), bytes: b)
    }
}
final class CoreTests {
    func testAllBytesAndExactBounds() throws {
        for n in 0...255 {
            let b = bytes([UInt8(n)])
            XCTAssertEqual(classify(b), (n == 9 || n == 10 || (32...126).contains(n)) ? .ok : .unsupported)
        }
        XCTAssertEqual(classify(bytes([])), .empty)
        XCTAssertEqual(classify(bytes(Array(repeating: 65, count: 1024))), .ok)
        XCTAssertEqual(classify(bytes(Array(repeating: 65, count: 1025))), .oversize)
        XCTAssertEqual(Wire.crc(bytes(Array("123456789".utf8))), 0xcbf43926)
    }
    func testOwnedBuffersClearAllCapacity() {
        let b = bytes(Array(repeating: 0xa5, count: 1024)); b.count = 3; b.clear()
        XCTAssertEqual(b.count, 0); XCTAssertTrue((0..<1024).allSatisfy { b[$0] == 0 })
    }
    func testCanonicalFramesAndBinding() throws {
        let r = request(); XCTAssertEqual(try Wire.request(r).values, [1, 2, 3, 4, 5])
        for i in Array(0..<8) + Array(48..<64) {
            let b = request(); b[i] ^= 0x80; XCTAssertThrowsError(try Wire.request(b))
        }
        for i in 0..<5 { let b = request(); b.put(0, at: 8 + i * 8); XCTAssertThrowsError(try Wire.request(b)) }
        let h = Wire.frame(0); h.put(1, at: 8); h.put(3, at: 16)
        try Wire.hello(h, boot: 1, helper: 3); XCTAssertThrowsError(try Wire.hello(h, boot: 2, helper: 3))
    }
    func testResponseFramesGoldenCRCAndWholeText() throws {
        let b = bytes(Array("123456789".utf8)); var output: [[UInt8]] = []
        try Wire.respond(Binding([1, 2, 3, 4, 5]), result: TextResult(status: .ok, bytes: b)) { output.append(Array($0.readView())) }
        XCTAssertEqual(output.count, 3); XCTAssertEqual(output.map { $0[4] }, [2, 3, 4])
        XCTAssertEqual(Array(output[0][48..<55]), [0, 9, 0, 0x26, 0x39, 0xf4, 0xcb])
        XCTAssertEqual(Array(output[1][19..<28]), Array("123456789".utf8))
        XCTAssertTrue(output.allSatisfy { $0.count == 64 }); XCTAssertEqual(b.count, 0)
        for s in TextStatus.allCases where s != .ok {
            output = []; try Wire.respond(Binding([1,2,3,4,5]), result: TextResult(status: s)) { output.append(Array($0.readView())) }
            XCTAssertEqual(output.count, 1); XCTAssertEqual(output[0][48], s.rawValue)
            XCTAssertTrue(output[0][49..<64].allSatisfy { $0 == 0 })
        }
        let full = bytes(Array(repeating: 65, count: 1024)); output = []
        try Wire.respond(Binding([1,2,3,4,5]), result: TextResult(status: .ok, bytes: full)) { output.append(Array($0.readView())) }
        XCTAssertEqual(output.count, 28)
        let decoded = output.filter { $0[4] == 3 }.flatMap { Array($0[19..<(19 + Int($0[18]))]) }
        XCTAssertEqual(decoded, Array(repeating: 65, count: 1024))
    }
    func testFailureAndUnsupportedNeverPartiallyRespond() throws {
        let bad = bytes([65, 0, 66]); var sends = 0
        XCTAssertThrowsError(try Wire.respond(Binding([1,2,3,4,5]), result: TextResult(status: .ok, bytes: bad)) { _ in sends += 1 })
        XCTAssertEqual(sends, 0); XCTAssertEqual(bad.count, 0)
        let good = bytes(Array(repeating: 65, count: 1024)); var captured: Bytes?
        XCTAssertThrowsError(try Wire.respond(Binding([1,2,3,4,5]), result: TextResult(status: .ok, bytes: good)) { b in captured = b; throw HelperError.io })
        XCTAssertEqual(good.count, 0); XCTAssertEqual(captured?.count, 0)
    }
    func testSessionFreshCurrentReadsReplayAndAllIdentityFields() throws {
        let t = FakeTransport(), reader = FixtureReader(), token = Cancellation()
        reader.texts = [Array("first".utf8), Array("second".utf8)]
        let s = Session(transport: t, reader: reader, cancellation: token, boot: 1, helper: 3)
        for b in [request([99,2,3,4,5]), request([1,2,99,4,5])] { XCTAssertThrowsError(try s.handle(b)) }
        XCTAssertEqual(reader.calls, 0)
        try s.handle(request()); XCTAssertEqual(reader.calls, 1); XCTAssertEqual(reader.returned?.count, 0)
        XCTAssertThrowsError(try s.handle(request())); XCTAssertEqual(reader.calls, 1)
        try s.handle(request([1,2,3,5,5])); XCTAssertEqual(reader.calls, 2)
        XCTAssertThrowsError(try s.handle(request([1,99,3,6,5]))) { XCTAssertEqual($0 as? HelperError, .peerRestarted) }
        XCTAssertEqual(reader.calls, 2)
        let data = t.writes.filter { $0[4] == 3 }.map { Array($0[19..<(19 + Int($0[18]))]) }
        XCTAssertEqual(data, [Array("first".utf8), Array("second".utf8)])
    }
    func testTimeoutCancellationAndWriteFailureWipeNoEnd() throws {
        for kind in 0..<3 {
            let t = FakeTransport(), reader = FixtureReader(), token = Cancellation(); var clock = 10.0
            let s = Session(transport: t, reader: reader, cancellation: token, boot: 1, helper: 3, now: { clock })
            if kind == 0 { reader.afterRead = { clock += 3 } }
            if kind == 1 { reader.afterRead = { token.cancel() } }
            if kind == 2 { t.failOpcode = 3 }
            if kind == 0 { try s.handle(request()) } else { XCTAssertThrowsError(try s.handle(request())) }
            XCTAssertEqual(reader.returned?.count, 0); XCTAssertFalse(t.writes.contains { $0[4] == 4 })
        }
    }
    func testPartialFrameExpiryAndIdleNoClipboardRead() throws {
        let t = FakeTransport(), reader = FixtureReader(), token = Cancellation(); var clock = 1.0
        t.input = Array(request().readView()).prefix(17).map { $0 }
        t.whenRead = { clock += 0.3 }
        let s = Session(transport: t, reader: reader, cancellation: token, boot: 1, helper: 3, now: { clock })
        XCTAssertThrowsError(try s.run()); XCTAssertEqual(reader.calls, 0)
        // The final bytes arrive after the absolute partial-frame deadline.
        let late = FakeTransport(); var reads = 0; clock = 1.0
        late.input = Array(request().readView()); late.readLimit = 32
        late.whenRead = { reads += 1; clock += reads == 1 ? 0.1 : 1.1 }
        XCTAssertThrowsError(try Session(transport: late, reader: reader, cancellation: token, boot: 1, helper: 3, now: { clock }).run())
        XCTAssertEqual(reader.calls, 0)
        let idle = FakeTransport(); var iterations = 0
        idle.whenRead = { iterations += 1; clock += 0.05; if iterations == 10 { token.cancel() } }
        XCTAssertThrowsError(try Session(transport: idle, reader: reader, cancellation: token, boot: 1, helper: 3, now: { clock }).run())
        XCTAssertEqual(reader.calls, 0)
    }
    func testIdentityStrictGrammarAndConfiguration() throws {
        XCTAssertEqual(try Identity.localBoot(bytes(Array(status().utf8)), configuration: config), 1)
        let reversed = status().replacingOccurrences(of: "board=A", with: "board=X")
            .replacingOccurrences(of: "board=B", with: "board=A")
            .replacingOccurrences(of: "board=X", with: "board=B")
        XCTAssertEqual(try Identity.localBoot(bytes(Array(reversed.utf8)), configuration: config), 1)
        XCTAssertEqual(try Identity.localBoot(bytes(Array(status(peer: false).utf8)), configuration: config), 1)
        for broken in [status("FFFFFFFFFFFFFFFF"), status(build: "0.113"), status().replacingOccurrences(of: "board=A\r\n", with: "board=B\r\n"),
                       status().replacingOccurrences(of: "phase=idle", with: "phase=receiving"), status() + "extra",
                       status().replacingOccurrences(of: "peer=ok", with: "peer=busy"), status().replacingOccurrences(of: "boot_session=0000000000000001", with: "boot_session=0000000000000000"),
                       status().replacingOccurrences(of: "uptime_ms=1000", with: "uptime_ms=18446744073709551616"), status().replacingOccurrences(of: "source=none", with: "source=usb"),
                       status().replacingOccurrences(of: "board_id=FEDCBA9876543210", with: "board_id=0123456789ABCDEF")] {
            XCTAssertThrowsError(try Identity.localBoot(bytes(Array(broken.utf8)), configuration: config))
        }
        for c in [Configuration(port: "/tmp/file", boardID: config.boardID), Configuration(port: config.port, boardID: "ABC"), Configuration(port: config.port, boardID: config.boardID, build: "0.114"), Configuration(port: config.port, boardID: config.boardID, build: "0.115\nbootloader A")] { XCTAssertThrowsError(try c.validate()) }
    }
    func testHandshakeFragmentedExactAndNoReadOrDisruptiveCommand() throws {
        let t = FakeTransport(); t.readLimit = 1
        let h = Wire.frame(0); h.put(1, at: 8); h.put(3, at: 16)
        t.input = Array(("\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> " + status() + "clipboard 0000000000000003\r\n").utf8) + Array(h.readView())
        XCTAssertEqual(try Handshake.open(on: t, configuration: config, helper: 3), 1)
        XCTAssertEqual(t.writes.map { String(decoding: $0, as: UTF8.self) }, ["status\n", "clipboard 0000000000000003\n"])
        let wrong = FakeTransport(); wrong.input = Array(("\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> " + status("FFFFFFFFFFFFFFFF")).utf8)
        XCTAssertThrowsError(try Handshake.open(on: wrong, configuration: config, helper: 3))
        XCTAssertEqual(wrong.writes.count, 1)
    }
    func testLoginIsOnlyChangedByExplicitPolicyAction() throws {
        final class FakeLogin: LoginService {
            var state: LoginState = .disabled; var registrations = 0, removals = 0
            func register() { registrations += 1; state = .approvalRequired }
            func unregister() { removals += 1; state = .disabled }
        }
        let service = FakeLogin(); _ = service.state; XCTAssertEqual(service.registrations, 0)
        try LoginPolicy.userSelected(true, service: service); XCTAssertEqual(service.registrations, 1)
        try LoginPolicy.userSelected(true, service: service); XCTAssertEqual(service.registrations, 1)
        try LoginPolicy.userSelected(false, service: service); XCTAssertEqual(service.removals, 1)
        try LoginPolicy.userSelected(false, service: service); XCTAssertEqual(service.removals, 1)
    }
    func testNativeReaderBoundedPositiveMalformedOversizeTimeoutAndCancellation() throws {
        var ticks = 0
        let good = NativeTextReader(fixtureExecutable: URL(fileURLWithPath: "/usr/bin/printf"), arguments: ["\\000\\003\\000abc"])
        let result = try good.read(deadline: uptime() + 3) { ticks += 1 }
        XCTAssertEqual(result.status, .ok); XCTAssertEqual(Array(result.bytes.readView()), Array("abc".utf8)); result.bytes.clear()
        let malformed = NativeTextReader(fixtureExecutable: URL(fileURLWithPath: "/usr/bin/printf"), arguments: ["bad"])
        XCTAssertEqual(try malformed.read(deadline: uptime() + 3, tick: {}).status, .unavailable)
        let huge = NativeTextReader(fixtureExecutable: URL(fileURLWithPath: "/bin/sh"), arguments: ["-c", "head -c 1000000 /dev/zero"])
        XCTAssertEqual(try huge.read(deadline: uptime() + 3, tick: {}).status, .unavailable)
        let slow = NativeTextReader(fixtureExecutable: URL(fileURLWithPath: "/bin/sleep"), arguments: ["30"])
        let start = uptime()
        XCTAssertEqual(try slow.read(deadline: uptime() + 0.15) { ticks += 1 }.status, .unavailable)
        XCTAssertLessThan(uptime() - start, 1); XCTAssertGreaterThan(ticks, 2)
        XCTAssertThrowsError(try slow.read(deadline: uptime() + 3) { throw HelperError.cancelled })
    }
}
