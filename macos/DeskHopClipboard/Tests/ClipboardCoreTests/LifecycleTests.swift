import Foundation
import Darwin
import ClipboardCore

extension CoreTests {
    func testPOSIXTransportWithOnlySocketFixtures() throws {
        var pair: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair), 0)
        let token = Cancellation(), transport = SerialTransport(fixtureFD: pair[0], cancellation: token)
        defer { transport.close(); Darwin.close(pair[1]) }
        let incoming = Array("fixture".utf8)
        XCTAssertEqual(incoming.withUnsafeBytes { Darwin.write(pair[1], $0.baseAddress!, $0.count) }, incoming.count)
        let b = Bytes(capacity: 64)
        let n = try transport.read(into: b.writeView(0, 3), deadline: uptime() + 1)
        XCTAssertEqual(n, 3); XCTAssertEqual(Array(b.readView(0, 3)), Array("fix".utf8))
        try incoming.withUnsafeBytes { try transport.write($0, deadline: uptime() + 1) }
        var returned = [UInt8](repeating: 0, count: 7)
        XCTAssertEqual(Darwin.read(pair[1], &returned, 7), 7); XCTAssertEqual(returned, incoming)
        token.cancel(); XCTAssertThrowsError(try transport.read(into: b.writeView(), deadline: uptime() + 1))
        transport.close(); XCTAssertEqual(fcntl(pair[0], F_GETFD), -1)
        transport.close() // idempotent; no stale FD reuse
    }
    func testPOSIXWriteBackpressureDeadline() throws {
        var pair: [Int32] = [-1,-1]; XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair), 0)
        let t = SerialTransport(fixtureFD: pair[0], cancellation: Cancellation())
        defer { t.close(); Darwin.close(pair[1]) }
        var size: Int32 = 1024
        XCTAssertEqual(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout.size(ofValue: size))), 0)
        let fill = [UInt8](repeating: 65, count: 1024)
        while fill.withUnsafeBytes({ Darwin.write(pair[0], $0.baseAddress!, $0.count) }) > 0 {}
        let start = uptime()
        XCTAssertThrowsError(try fill.withUnsafeBytes { try t.write($0, deadline: uptime() + 0.08) }) { XCTAssertEqual($0 as? HelperError, .timeout) }
        XCTAssertLessThan(uptime() - start, 0.5)
    }
    func testControllerPauseResumeClosesAndRenewsSessionWithoutRead() throws {
        let reader = FixtureReader(); var transports: [FakeTransport] = [], helpers: [String] = []
        let c = ConnectionController(reader: reader) { configuration, token in
            XCTAssertEqual(configuration, config)
            let t = FakeTransport()
            t.input = Array("\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> ".utf8)
            t.whenRead = { if t.input.isEmpty { Thread.sleep(forTimeInterval: 0.005) } }
            t.onWrite = { data in
                if data.count == 64 { return }
                let command = String(decoding: data, as: UTF8.self)
                if command == "status\n" { t.input += Array(status().utf8) }
                else if command.hasPrefix("clipboard ") {
                    let session = String(command.dropFirst(10).dropLast()); helpers.append(session)
                    let h = Wire.frame(0); h.put(1, at: 8); h.put(UInt64(session, radix: 16)!, at: 16)
                    t.input += Array((command.dropLast() + "\r\n").utf8) + Array(h.readView())
                }
            }
            transports.append(t); return t
        }
        for _ in 0..<2 {
            var stopped = false
            c.onState = { if $0 == .connected { c.stop { stopped = true } } }
            c.start(config)
            let deadline = uptime() + 3
            while !stopped && uptime() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
            XCTAssertTrue(stopped); XCTAssertFalse(c.running)
        }
        XCTAssertEqual(transports.count, 2); XCTAssertTrue(transports.allSatisfy { $0.closes == 1 })
        XCTAssertEqual(reader.calls, 0); XCTAssertEqual(helpers.count, 2); XCTAssertTrue(helpers[0] != helpers[1])
    }
    func testControllerBoundedReconnectAndIdentityFailureNoRetry() {
        for wrongIdentity in [false, true] {
            var calls = 0, finished = false
            let c = ConnectionController(reader: FixtureReader()) { _, _ in
                calls += 1
                if !wrongIdentity { throw HelperError.io }
                let t = FakeTransport(); t.input = Array(("\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> " + status("FFFFFFFFFFFFFFFF")).utf8)
                return t
            }
            c.onState = { if case .failed = $0 { finished = true } }
            c.start(config)
            let deadline = uptime() + 5
            while !finished && uptime() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
            XCTAssertTrue(finished); XCTAssertEqual(calls, wrongIdentity ? 1 : 4); XCTAssertFalse(c.running)
        }
    }
}
