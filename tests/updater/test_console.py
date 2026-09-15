"""Offline parser and transport fault tests; never open a hardware device."""
from contextlib import ExitStack, contextmanager
from pathlib import Path
import stat
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from deskhop_update import console as c

IDS = {"A": "0123456789ABCDEF", "B": "FEDCBA9876543210"}
SESSIONS = {"A": "123456789abcdef0", "B": "fedcba9876543211"}


def status(at_us=1_000_000, build="0.104", local="A", peer="ok"):
    roles = [local, "B" if local == "A" else "A"] if peer == "ok" else [local]
    lines = ["status", "BEGIN status"]
    for index, role in enumerate(roles):
        lines += [f"board={role}", f"board_id={IDS[role]}", f"build={build}",
                  "image_crc_at_boot=deadbeef", f"boot_session={SESSIONS[role]}",
                  f"uptime_ms={at_us // 1000}", ""]
        lines += [f"board={role} core={core} checkpoints={at_us // 1000 + core} age_ms=1" for core in (0, 1)]
        lines += [f"board={role} update seen=0 source=none phase=idle received=0 total=262144 "
                  "progress_age_ms=unavailable target=unknown attempt=0"]
        if index:
            lines += [f"board={role} observation boot=first_seen progress=baseline update=not_observed"]
        lines += [""]
    lines += [f"peer={peer}", "verification=available", "END status", "deskhop> "]
    return "\r\n".join(lines).encode()


def verify(start=2_000_000, command_crc="12345678", build="0.104", local="A"):
    verdict = "PASS" if command_crc == "12345678" else "FAIL"
    reason = "match" if verdict == "PASS" else "crc_mismatch"
    end = start + 1_026_000
    lines = [f"verify {build} {command_crc}", "BEGIN verify", "scope=both", "claim=scan_snapshot",
             f"expected_build={build}", f"expected_crc32={command_crc}",
             "coverage_start=0x10000000", "coverage_bytes=262144"]
    for role in [local, "B" if local == "A" else "A"]:
        lines += [f"board={role} result={verdict} reason={reason}",
                  f"board={role} build={build} board_id={IDS[role]} boot_session={SESSIONS[role]}",
                  f"board={role} scan_start_us={start} scan_end_us={end} bytes=262144 crc32=12345678",
                  f"board={role} metadata_magic=0000f00d metadata_version={c.encoded_build(build)} metadata_reserved=0 metadata_crc32=deadbeef",
                  f"board={role} boot_crc32=deadbeef generation_start=12 generation_end=12"]
        lines += [f"board={role} core={core} start={start // 1000 + core} end={end // 1000 + core} age_ms=1 valid=1"
                  for core in (0, 1)]
    lines += [f"result={verdict} reason={'both_match' if verdict == 'PASS' else 'board_failure'}",
              "END verify", "deskhop> "]
    return "\r\n".join(lines).encode()


def history(at_us=2_000_000, build="0.104", local="A", count=16, gap=False):
    lines = [f"history {count}", "BEGIN history", "scope=both", f"requested_per_board={count}",
             "timing=approximate_snapshot_alignment", "capacity_per_board=64"]
    roles = [local, "B" if local == "A" else "A"]
    for index, role in enumerate(roles):
        if index:
            lines += [""]
        lines += [f"board={role}", f"boot_session={SESSIONS[role]}",
                  f"sampled_uptime_ms={at_us // 1000}", "returned=1", "overwritten=0"]
    lines += ["peer=ok", "peer_capture_bound_us=5000", ""]
    for role in roles:
        lines += [f"GAP board={role} seq=1" if gap else
                  f"board={role} seq=1 uptime_ms=1 age_ms={at_us // 1000 - 1} event=boot build={build} output=A"]
    lines += ["END history", "deskhop> "]
    return "\r\n".join(lines).encode()


HELP = ("help\r\nBEGIN help\r\nDeskHop console - diagnostics and disruptive maintenance.\r\n"
        "  help Show help.\r\n  status Show status.\r\n  history [count] Show events.\r\n"
        "  verify <build> <crc32> Check firmware.\r\n"
        "  bootloader A|B DISRUPTIVE: selected board enters disk-free USB ROM.\r\n"
        "Diagnostics are read-only and query both boards with bounded peer timeouts.\r\n"
        "Bootloader requires physical A or B; no default/both. Upload with picotool\r\n"
        "on the target's USB-connected computer; peer acceptance is not boot proof.\r\n"
        "GAP means unavailable capture data.\r\n"
        "Verify CRC: full 256KiB firmware including metadata; configuration excluded.\r\n"
        "PASS describes a fresh scan; rerun verify after changes.\r\n"
        "Status image CRC is boot metadata only; it is not the verify CRC.\r\n"
        "END help\r\ndeskhop> ").encode()


def bootloader(role="A"):
    return (f"bootloader {role}\r\nBEGIN bootloader\r\nboard={role} result=accepted scope=local\r\n"
            "action=enter_disk_free_usb_rom_after_reply\r\nEND bootloader\r\n").encode()


class ParserTests(unittest.TestCase):
    def setUp(self):
        self.identities = c.parse_status(status())["boards"]

    def parse_verify(self, raw, **kwargs):
        return c.parse_verify(raw, "0.104", "12345678", "deadbeef", self.identities, **kwargs)

    def test_status_roles_and_unavailable_peer(self):
        self.assertEqual(c.parse_status(status())["local"], "A")
        self.assertEqual(c.parse_status(status(local="B"))["local"], "B")
        for outcome in c.PEER_OUTCOMES[1:]:
            parsed = c.parse_status(status(local="B", peer=outcome))
            self.assertEqual(list(parsed["boards"]), ["B"])
            self.assertEqual(parsed["peer"], outcome)

    def test_status_rejects_ambiguous_or_unknown_evidence(self):
        raw = status()
        changes = [(b"board=B\r\n", b"board=A\r\n"),
                   (IDS["B"].encode(), IDS["A"].encode()),
                   (b"core=1 checkpoints=1001", b"core=0 checkpoints=1001"),
                   (b"checkpoints=1000", b"checkpoints=4294967296"),
                   (b"checkpoints=1000", b"checkpoints=unavailable"),
                   (b"age_ms=1", b"age_ms=-1"), (b"phase=idle", b"phase=surprise"),
                   (b"received=0", b"received=1"), (b"received=0", b"received=256"),
                   (b"total=262144", b"total=0"), (b"attempt=0", b"attempt=1"),
                   (b"progress=baseline", b"progress=anything"),
                   (b"peer=ok", b"peer=busy"), (b"verification=available", b"verification=passed"),
                   (b"build=0.104", b"build=0.1000"), (b"uptime_ms=1000", b"uptime_ms=-1"),
                   (b"board_id=0123456789ABCDEF", b"board_id=0123456789ABCDEG")]
        for old, new in changes:
            with self.subTest(old=old, new=new), self.assertRaises(c.ProtocolError):
                c.parse_status(raw.replace(old, new, 1))
        for malformed in (raw + b"tail", raw[:-1], raw.replace(b"BEGIN status", b"BEGIN status\r\nBEGIN status"),
                          raw.replace(b"\r\n", b"\r", 1), raw + b"\xff"):
            with self.subTest(malformed=malformed[-40:]), self.assertRaises(c.ProtocolError):
                c.parse_status(malformed)

    def test_status_active_update(self):
        raw = status().replace(b"seen=0 source=none phase=idle received=0 total=262144 "
                               b"progress_age_ms=unavailable target=unknown attempt=0",
                               b"seen=1 source=peer phase=receiving received=65536 total=262144 "
                               b"progress_age_ms=1 target=0.105 attempt=1", 1)
        self.assertEqual(c.parse_status(raw)["boards"]["A"]["update"]["received"], 65536)
        with self.assertRaises(c.ProtocolError):
            c.parse_status(raw.replace(b"target=0.105", b"target=unknown"))

    def test_help_requires_maintenance_description(self):
        c.validate_help(HELP)
        for old, new in [(b"bootloader A|B", b"bootloader"), (b"peer acceptance is not boot proof", b"boot proved"),
                         (b"GAP", b"gap"), (b"diagnostics and disruptive maintenance", b"all commands are read-only")]:
            with self.subTest(old=old), self.assertRaises(c.ProtocolError):
                c.validate_help(HELP.replace(old, new))

    def test_correct_wrong_correct_fresh_scans(self):
        first = self.parse_verify(verify())
        second = self.parse_verify(verify(4_000_000, "12345679"), command_crc="12345679", previous=first)
        third = self.parse_verify(verify(6_000_000), previous=second)
        self.assertEqual([v["result"] for v in (first, second, third)], ["PASS", "FAIL", "PASS"])
        with self.assertRaises(c.ProtocolError):
            self.parse_verify(verify(), previous=first)

    def test_no_release_or_board_hardcoding(self):
        identities = c.parse_status(status(build="1.5", local="B"))["boards"]
        parsed = c.parse_verify(verify(build="1.5", local="B"), "1.5", "12345678", "deadbeef", identities)
        self.assertEqual(list(parsed["boards"]), ["B", "A"])

    def test_verify_rejects_wrong_checksum_identity_coverage_and_progress(self):
        raw = verify()
        changes = [(b"expected_crc32=12345678", b"expected_crc32=12345679"),
                   (b"coverage_bytes=262144", b"coverage_bytes=258048"),
                   (b"coverage_start=0x10000000", b"coverage_start=0x10040000"),
                   (b"bytes=262144 crc32=12345678", b"bytes=262143 crc32=12345678"),
                   (b"bytes=262144 crc32=12345678", b"bytes=262144 crc32=87654321"),
                   (IDS["B"].encode(), IDS["A"].encode()),
                   (SESSIONS["A"].encode(), b"0000000000000000"),
                   (b"board=B build=0.104", b"board=B build=0.103"),
                   (b"metadata_version=204", b"metadata_version=203"),
                   (b"metadata_reserved=0", b"metadata_reserved=1"),
                   (b"metadata_magic=0000f00d", b"metadata_magic=00000000"),
                   (b"metadata_crc32=deadbeef", b"metadata_crc32=00000000"),
                   (b"boot_crc32=deadbeef", b"boot_crc32=00000000"),
                   (b"generation_start=12 generation_end=12", b"generation_start=12 generation_end=13"),
                   (b"start=2000 end=3026", b"start=2000 end=2000"),
                   (b"start=2000 end=3026", b"start=999 end=3026"),
                   (b"age_ms=1 valid=1", b"age_ms=101 valid=1"),
                   (b"age_ms=1 valid=1", b"age_ms=1 valid=0"),
                   (b"scan_start_us=2000000", b"scan_start_us=0"),
                   (b"result=PASS reason=both_match", b"result=FAIL reason=board_failure")]
        for old, new in changes:
            with self.subTest(old=old, new=new), self.assertRaises(c.ProtocolError):
                self.parse_verify(raw.replace(old, new, 1))

    def test_history_fresh_sessions_gaps_and_retained_content(self):
        first = c.validate_history(history(), self.identities)
        c.validate_history(history(3_000_000, count=64), self.identities, previous=first)
        c.validate_history(history(gap=True), self.identities)
        c.validate_history(history(local="B"), self.identities)
        with self.assertRaises(c.ProtocolError):
            c.validate_history(history(), self.identities, previous=first)
        with self.assertRaises(c.ProtocolError):
            c.validate_history(history(3_000_000).replace(b"output=A", b"output=B", 1), self.identities, previous=first)

    def test_history_rejects_wrong_identity_counts_age_and_events(self):
        raw = history()
        changes = [(b"returned=1", b"returned=0"), (b"overwritten=0", b"overwritten=1"),
                   (b"seq=1", b"seq=2"), (b"age_ms=1999", b"age_ms=2001"),
                   (b"sampled_uptime_ms=2000", b"sampled_uptime_ms=999"),
                   (SESSIONS["A"].encode(), b"0000000000000000"),
                   (b"event=boot build=0.104 output=A", b"event=anything"),
                   (b"event=boot build=0.104 output=A", b"event=boot build=0.104 output=A extra=1"),
                   (b"peer=ok", b"peer=busy"), (b"requested_per_board=16", b"requested_per_board=64")]
        for old, new in changes:
            with self.subTest(old=old), self.assertRaises(c.ProtocolError):
                c.validate_history(raw.replace(old, new, 1), self.identities)


class FakeSerial:
    """Controlled clock and descriptor double, including short reads/writes."""
    def __init__(self, responses=()):
        self.now = 0.0
        self.rx = bytearray(b"\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> ")
        self.responses = list(responses)
        self.sent, self.partial = bytearray(), bytearray()
        self.write_limit, self.read_limit = 4096, 4096
        self.stall_write = False
        self.closed = []
        self.ioctls = []
        self.attrs = [0, 0, 0, 0, 0, 0, [0] * 32]
        self.restored = []

    def sleep(self, seconds):
        self.now += seconds

    def select(self, readers, writers, _errors, timeout):
        self.now += min(0.01, timeout)
        return (readers if self.rx else [], [] if self.stall_write else writers, [])

    def read(self, _fd, count):
        count = min(count, self.read_limit)
        result = bytes(self.rx[:count])
        del self.rx[:count]
        return result

    def write(self, _fd, payload):
        result = payload[:self.write_limit]
        self.sent += result
        self.partial += result
        if self.partial.endswith(b"\n"):
            self.partial.clear()
            if self.responses:
                self.rx += self.responses.pop(0)
        return len(result)


@contextmanager
def serial_double(fake):
    with ExitStack() as stack:
        for target, replacement in [
            ("time.monotonic", lambda: fake.now), ("time.sleep", fake.sleep),
            ("os.open", lambda *_args: 7), ("os.fstat", lambda _fd: SimpleNamespace(st_mode=stat.S_IFCHR)),
            ("os.close", fake.closed.append), ("os.read", fake.read), ("os.write", fake.write),
            ("select.select", fake.select), ("fcntl.ioctl", lambda *args: fake.ioctls.append(args)),
            ("termios.tcgetattr", lambda _fd: fake.attrs),
            ("termios.tcsetattr", lambda _fd, _when, attrs: fake.restored.append(attrs)),
            ("termios.tcflush", lambda *_args: None), ("tty.setraw", lambda *_args: None),
        ]:
            stack.enter_context(patch("deskhop_update.console." + target, replacement))
        yield


class TransportTests(unittest.TestCase):
    def test_fragmented_reads_and_short_writes(self):
        fake = FakeSerial([status()])
        fake.write_limit, fake.read_limit = 1, 3
        with serial_double(fake), c.Console("/dev/cu.fixture") as con:
            self.assertEqual(con.command("status"), status())
        self.assertEqual(bytes(fake.sent), b"status\n")
        self.assertEqual(fake.closed, [7])
        self.assertTrue(any(args[1] == c.termios.TIOCMBIC for args in fake.ioctls))

    def test_command_allowlist_prevents_control_or_maintenance_injection(self):
        fake = FakeSerial()
        with serial_double(fake), c.Console("/dev/cu.fixture") as con:
            for command in ("bootloader A", "status\nbootloader A", "reboot", "history 65", "verify 66.1 deadbeef"):
                with self.subTest(command=command), self.assertRaises(c.ProtocolError):
                    con.command(command)
        self.assertEqual(bytes(fake.sent), b"")

    def test_stalled_reads_and_writes_are_bounded(self):
        for stall_write in (False, True):
            fake = FakeSerial()
            with serial_double(fake), c.Console("/dev/cu.fixture", timeout=0.3, total_timeout=1) as con:
                fake.stall_write = stall_write
                with self.assertRaises(TimeoutError):
                    con.command("status")
                self.assertLessEqual(fake.now, 1)
            self.assertEqual(fake.closed, [7])

    def test_total_budget_bounds_multiple_commands(self):
        fake = FakeSerial([status()] * 10)
        fake.read_limit = 100
        with serial_double(fake), c.Console("/dev/cu.fixture", timeout=1, total_timeout=0.5) as con:
            with self.assertRaises(TimeoutError):
                for _ in range(10):
                    con.command("status")
            self.assertLessEqual(fake.now, 0.51)

    def test_rx_byte_cap_includes_last_read(self):
        fake = FakeSerial([b"X" * 5000])
        with serial_double(fake), patch.object(c, "MAX_RX_BYTES", 1024), c.Console("/dev/cu.fixture") as con:
            with self.assertRaises(c.ProtocolError):
                con.command("status")
            self.assertEqual(con.rx_bytes, 1024)

    def test_response_cap_and_unsolicited_trailing_bytes(self):
        for response in (b"X" * (c.MAX_RESPONSE_BYTES + 100), status() + b"trailer"):
            fake = FakeSerial([response])
            with serial_double(fake), c.Console("/dev/cu.fixture") as con:
                with self.assertRaises(c.ProtocolError):
                    con.command("status")

    def test_partial_write_error_is_not_retried(self):
        fake = FakeSerial()
        with serial_double(fake), c.Console("/dev/cu.fixture") as con:
            def failing_write(_fd, payload):
                if fake.sent:
                    raise OSError("USB disappeared")
                fake.sent.extend(payload[:3])
                return 3
            with patch.object(c.os, "write", failing_write), self.assertRaises(c.BootloaderUnconfirmed):
                con.bootloader("A")
            with self.assertRaises(c.ProtocolError):
                con.bootloader("A")
            self.assertEqual(bytes(fake.sent), b"boo")

    def test_bootloader_returns_without_prompt_and_keeps_dtr(self):
        fake = FakeSerial([bootloader("B")])
        with serial_double(fake), c.Console("/dev/cu.fixture") as con:
            ioctls_before = len(fake.ioctls)
            self.assertEqual(con.bootloader("B"), bootloader("B"))
            self.assertEqual(fake.closed, [])
            self.assertEqual(len(fake.ioctls), ioctls_before)
            with self.assertRaises(c.ProtocolError):
                con.command("status")
        self.assertEqual(bytes(fake.sent), b"bootloader B\n")

    def test_bootloader_rejections_remote_acceptance_and_timeout_are_unconfirmed(self):
        replies = [b"", bootloader("B"), bootloader().replace(b"scope=local", b"scope=remote"),
                   b"bootloader A\r\nBEGIN bootloader\r\nboard=A result=rejected reason=busy\r\nEND bootloader\r\ndeskhop> ",
                   b"bootloader A\r\nBEGIN bootloader\r\nboard=A result=accepted reason=peer_admitted_not_boot_proof\r\nEND bootloader\r\ndeskhop> ",
                   bootloader()[:-1], bootloader() + b"junk"]
        for response in replies:
            fake = FakeSerial([response])
            with serial_double(fake), c.Console("/dev/cu.fixture", timeout=0.3) as con:
                with self.subTest(response=response), self.assertRaises(c.BootloaderUnconfirmed):
                    con.bootloader("A")
                with self.assertRaises(c.ProtocolError):
                    con.bootloader("A")
            self.assertEqual(bytes(fake.sent), b"bootloader A\n")

    def test_invalid_targets_and_port_rejected_without_access(self):
        fake = FakeSerial()
        with serial_double(fake), c.Console("/dev/cu.fixture") as con:
            for role in ("a", "both", "A\n", "", "A B"):
                with self.assertRaises(c.ProtocolError):
                    con.bootloader(role)
        with patch.object(c.os, "open", side_effect=AssertionError("must not open")):
            for port in ("/tmp/file", "relative", "/dev/../tmp/file"):
                with self.assertRaises(c.ProtocolError):
                    c.Console(port)

    def test_bad_greeting_or_setup_failure_closes_descriptor(self):
        fake = FakeSerial()
        fake.rx = bytearray(b"not a DeskHop\r\ndeskhop> ")
        with serial_double(fake), self.assertRaises(c.ProtocolError):
            c.Console("/dev/cu.fixture").open()
        self.assertEqual(fake.closed, [7])
        fake = FakeSerial()
        with serial_double(fake), patch.object(c.termios, "tcgetattr", side_effect=OSError("bad tty")), self.assertRaises(OSError):
            c.Console("/dev/cu.fixture").open()
        self.assertEqual(fake.closed, [7])

    def test_disconnect_during_cleanup_does_not_prevent_close(self):
        fake = FakeSerial()
        with serial_double(fake):
            con = c.Console("/dev/cu.fixture").open()
            with patch.object(c.fcntl, "ioctl", side_effect=OSError(6, "device gone")), \
                    patch.object(c.termios, "tcsetattr", side_effect=OSError(5, "I/O error")):
                con.close()
            self.assertEqual(fake.closed, [7])
            self.assertIsNone(con.fd)
            self.assertGreaterEqual(len(con.cleanup_errors), 2)
            con.close()
            self.assertEqual(fake.closed, [7])


if __name__ == "__main__":
    unittest.main()
