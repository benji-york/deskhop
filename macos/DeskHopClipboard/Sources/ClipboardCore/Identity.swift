import Foundation

public enum Identity {
    // Accept only the diagnostic grammar we understand, including complete
    // ordered rows. Never search for a UID substring in an arbitrary response.
    public static func localBoot(_ data: Bytes, configuration: Configuration) throws -> UInt64 {
        try configuration.validate()
        try require(data.count <= 32768 && data.readView().allSatisfy { $0 == 10 || $0 == 13 || (32...126).contains($0) }, .identity)
        let text = String(decoding: data.readView(), as: UTF8.self).replacingOccurrences(of: "\r\n", with: "\n")
        try require(!text.contains("\r") && text.hasPrefix("status\nBEGIN status\n") && text.hasSuffix("END status\ndeskhop> "), .identity)
        var lines = text.components(separatedBy: "\n").filter { !$0.isEmpty }
        try require(lines.count >= 15, .identity)
        try require(lines.removeFirst() == "status" && lines.removeFirst() == "BEGIN status", .identity)
        var cursor = 0, ids = Set<String>(), boot: UInt64 = 0, roles = [String]()
        func take() throws -> String { try require(cursor < lines.count, .identity); defer { cursor += 1 }; return lines[cursor] }
        func field(_ key: String) throws -> String {
            let line = try take(); try require(line.hasPrefix(key + "="), .identity)
            let value = String(line.dropFirst(key.count + 1)); try require(!value.isEmpty && !value.contains(" "), .identity); return value
        }
        func number(_ value: String, max: UInt64 = .max) throws {
            try require(matches(value, "[0-9]{1,20}"), .identity)
            try require(UInt64(value).map { $0 <= max } ?? false, .identity)
        }
        func hex(_ value: String, width: Int) throws {
            try require(matches(value, "[a-fA-F0-9]{\(width)}"), .identity)
        }
        func fields(_ line: String, keys: [String]) throws -> [String] {
            let parts = line.components(separatedBy: " "); try require(parts.count == keys.count, .identity)
            return try zip(parts, keys).map { part, key in
                try require(part.hasPrefix(key + "=") && part.count > key.count + 1, .identity)
                return String(part.dropFirst(key.count + 1))
            }
        }
        while cursor < lines.count && (lines[cursor] == "board=A" || lines[cursor] == "board=B") {
            let role = try field("board")
            try require(!roles.contains(role) && roles.count < 2, .identity)
            let id = try field("board_id"); try hex(id, width: 16)
            try require(ids.insert(id.uppercased()).inserted, .identity)
            try require(try field("build") == configuration.build, .identity)
            try hex(try field("image_crc_at_boot"), width: 8)
            let session = try field("boot_session"); try hex(session, width: 16)
            try require(UInt64(session, radix: 16)! != 0, .identity)
            if roles.isEmpty { try require(id.uppercased() == configuration.boardID, .identity); boot = UInt64(session, radix: 16)! }
            try number(try field("uptime_ms"), max: UInt64.max / 1000)
            for core in 0...1 {
                let v = try fields(take(), keys: ["board", "core", "checkpoints", "age_ms"])
                try require(v[0] == role && v[1] == String(core), .identity)
                if v[2] == "unavailable" { try require(v[3] == "unavailable", .identity) }
                else { try number(v[2], max: UInt64(UInt32.max)); try number(v[3], max: UInt64(UInt32.max)) }
            }
            let line = try take(), prefix = "board=\(role) update "
            try require(line.hasPrefix(prefix), .identity)
            let v = try fields(String(line.dropFirst(prefix.count)), keys: ["seen", "source", "phase", "received", "total", "progress_age_ms", "target", "attempt"])
            try require(v == ["0", "none", "idle", "0", "262144", "unavailable", "unknown", "0"], .identity)
            if !roles.isEmpty {
                let line = try take(), prefix = "board=\(role) observation "
                try require(line.hasPrefix(prefix), .identity)
                let v = try fields(String(line.dropFirst(prefix.count)), keys: ["boot", "progress", "update"])
                try require(["unobserved", "first_seen", "same_boot", "new_boot", "identity_changed"].contains(v[0]) && ["unavailable", "baseline", "advancing", "not_advancing"].contains(v[1]) && ["not_observed", "pending_reboot", "awaiting_progress", "confirmed", "unexpected_boot"].contains(v[2]), .identity)
            }
            roles.append(role)
        }
        let peer = try field("peer")
        try require(["ok", "timeout_or_unsupported", "invalid", "busy"].contains(peer) && ((peer == "ok") == (roles.count == 2)), .identity)
        try require(try take() == "verification=available", .identity)
        try require(try take() == "END status", .identity); try require(try take() == "deskhop> ", .identity)
        try require(cursor == lines.count && boot != 0, .identity)
        return boot
    }
}
