// Deliberately framework-free: execute real Swift core with only fixture data.
import Foundation
import Darwin
func failure(_ file: StaticString, _ line: UInt) -> Never {
    fputs("Swift clipboard fixture assertion failed at \(file):\(line)\n", stderr); exit(1)
}
func XCTAssertEqual<T: Equatable>(_ a: @autoclosure () throws -> T, _ b: @autoclosure () throws -> T, file: StaticString = #filePath, line: UInt = #line) {
    do { if try a() != b() { failure(file, line) } } catch { failure(file, line) }
}
func XCTAssertTrue(_ a: @autoclosure () throws -> Bool, file: StaticString = #filePath, line: UInt = #line) {
    do { if try !a() { failure(file, line) } } catch { failure(file, line) }
}
func XCTAssertFalse(_ a: @autoclosure () throws -> Bool, file: StaticString = #filePath, line: UInt = #line) { XCTAssertTrue(try !a(), file: file, line: line) }
func XCTAssertLessThan<T: Comparable>(_ a: T, _ b: T, file: StaticString = #filePath, line: UInt = #line) { XCTAssertTrue(a < b, file: file, line: line) }
func XCTAssertGreaterThan<T: Comparable>(_ a: T, _ b: T, file: StaticString = #filePath, line: UInt = #line) { XCTAssertTrue(a > b, file: file, line: line) }
func XCTAssertThrowsError<T>(_ expression: @autoclosure () throws -> T, file: StaticString = #filePath, line: UInt = #line, _ handler: (Error) -> Void = { _ in }) {
    do { _ = try expression(); failure(file, line) } catch { handler(error) }
}
let tests = CoreTests()
let cases: [(String, () throws -> Void)] = [
    ("POSIX socket transport cancellation and close", tests.testPOSIXTransportWithOnlySocketFixtures),
    ("POSIX backpressure deadline", tests.testPOSIXWriteBackpressureDeadline),
    ("controller pause/resume fresh sessions", tests.testControllerPauseResumeClosesAndRenewsSessionWithoutRead),
    ("bounded reconnect and identity stop", tests.testControllerBoundedReconnectAndIdentityFailureNoRetry),
    ("all bytes and bounds", tests.testAllBytesAndExactBounds),
    ("owned buffer erasure", tests.testOwnedBuffersClearAllCapacity),
    ("canonical framing", tests.testCanonicalFramesAndBinding),
    ("golden CRC and full response", tests.testResponseFramesGoldenCRCAndWholeText),
    ("reject without partial response and wipe on failure", tests.testFailureAndUnsupportedNeverPartiallyRespond),
    ("current reads and replay/session rejection", tests.testSessionFreshCurrentReadsReplayAndAllIdentityFields),
    ("timeout cancellation and write failure", tests.testTimeoutCancellationAndWriteFailureWipeNoEnd),
    ("partial expiry and no idle read", tests.testPartialFrameExpiryAndIdleNoClipboardRead),
    ("strict identity and settings", tests.testIdentityStrictGrammarAndConfiguration),
    ("fragmented diagnostic handshake", tests.testHandshakeFragmentedExactAndNoReadOrDisruptiveCommand),
    ("explicit login changes only", tests.testLoginIsOnlyChangedByExplicitPolicyAction),
    ("native reader process bounds", tests.testNativeReaderBoundedPositiveMalformedOversizeTimeoutAndCancellation)
]
for (name, test) in cases {
    do { try test(); print("PASS \(name)") }
    catch { fputs("FAIL \(name)\n", stderr); exit(1) }
}
print("\(cases.count) Swift clipboard fixture groups passed")
