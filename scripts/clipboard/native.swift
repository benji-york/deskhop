// One on-demand plain-text read; never logs payload or writes files.
// Build: xcrun swiftc -O scripts/clipboard/native.swift -o build/clipboard-reader
import AppKit
import Darwin

let maxText = 1024
let ok: UInt8 = 0, empty: UInt8 = 1, nonText: UInt8 = 2
let oversize: UInt8 = 3, unsupported: UInt8 = 4, unavailable: UInt8 = 5

func classify(_ data: Data?) -> UInt8 {
    guard let data = data else { return nonText }
    if data.count > maxText { return oversize }
    if data.isEmpty { return empty }
    return data.allSatisfy { $0 == 9 || $0 == 10 || (32...126).contains($0) } ? ok : unsupported
}

func emit(_ status: UInt8, _ input: Data? = nil) {
    var output = Data([status])
    let count = status == ok ? (input?.count ?? 0) : 0
    output.append(UInt8(count & 255))
    output.append(UInt8(count >> 8))
    if status == ok, let input = input { output.append(input) }
    output.withUnsafeBytes { raw in
        var offset = 0
        while offset < raw.count {
            let written = Darwin.write(STDOUT_FILENO, raw.baseAddress!.advanced(by: offset), raw.count - offset)
            if written <= 0 { break }
            offset += written
        }
    }
    output.resetBytes(in: 0..<output.count)
}

// Limits are established before any pasteboard API. Core dumps are disabled;
// AppKit/OS-owned immutable storage and swap cannot be securely erased here.
var core = rlimit(rlim_cur: 0, rlim_max: 0)
if setrlimit(RLIMIT_CORE, &core) != 0 { exit(2) }

let args = CommandLine.arguments
if args.count == 2 && args[1] == "--fixture" {
    // Hardware-free path: a bounded byte fixture on stdin; no AppKit
    // pasteboard calls. 1025 bytes is enough to exercise oversize rejection.
    var input = FileHandle.standardInput.readData(ofLength: maxText + 1)
    let status = classify(input)
    emit(status, status == ok ? input : nil)
    input.resetBytes(in: 0..<input.count)
} else if args.count == 2 && args[1] == "--read-current" {
    autoreleasepool {
        let pasteboard = NSPasteboard.general
        let generation = pasteboard.changeCount
        guard let types = pasteboard.types, !types.isEmpty else { emit(empty); return }
        guard types.contains(.string) else { emit(nonText); return }
        // Public AppKit has no size-limited clipboard retrieval API. It may
        // materialize a larger string; never encode/forward more than 1024 bytes.
        guard let text = pasteboard.string(forType: .string) else { emit(unavailable); return }
        guard pasteboard.changeCount == generation else { emit(unavailable); return }
        // UTF-16 length>1024 proves UTF-8 cannot fit; avoid an oversized UTF-8 copy.
        guard (text as NSString).length <= maxText else { emit(oversize); return }
        guard text.utf8.count <= maxText else { emit(oversize); return }
        var data = Data(text.utf8)
        let status = classify(data)
        emit(status, status == ok ? data : nil)
        data.resetBytes(in: 0..<data.count)
    }
} else {
    exit(2)
}
