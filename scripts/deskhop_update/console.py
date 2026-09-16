"""Bounded POSIX CDC transport and strict, artifact-parameterized diagnostics.

Only ``bootloader`` is disruptive. It is sent once, never retried, and a local
acceptance reply is not proof of ROM enumeration. Keep the context open through
all PICOBOOT operations and normal application reboot, matching the successful
macOS diagnostic. This lifetime alone has not resolved the observed USB timeout.
Close before opening the new application console. Exceptions still clean up
without retry.
"""
from __future__ import annotations

import array
import copy
import fcntl
import math
import os
import re
import select
import stat
import termios
import time
import tty

PROMPT = b"deskhop> "
IMAGE_BYTES = 262144
MAX_U64 = (1 << 64) - 1
MAX_U32 = (1 << 32) - 1
MAX_RESPONSE_BYTES = 32 * 1024
MAX_RX_BYTES = 512 * 1024
PEER_OUTCOMES = ("ok", "timeout_or_unsupported", "invalid", "busy")
PHASES = ("idle", "receiving", "paused", "validating", "reboot_pending", "failed", "abandoned")
BOOTS = ("unobserved", "first_seen", "same_boot", "new_boot", "identity_changed")
PROGRESS = ("unavailable", "baseline", "advancing", "not_advancing")
EXECUTION = ("not_observed", "pending_reboot", "awaiting_progress", "confirmed", "unexpected_boot")


class ProtocolError(ValueError):
    """Malformed, ambiguous, stale, or contradictory diagnostic evidence."""


class BootloaderUnconfirmed(RuntimeError):
    """Request may have reached firmware; no automatic retry is safe."""


def require(condition, message):
    if not condition:
        raise ProtocolError(message)


def decimal(value, maximum, label, minimum=0):
    require(re.fullmatch(r"[0-9]{1,20}", value) is not None, f"invalid {label}")
    number = int(value)
    require(minimum <= number <= maximum, f"{label} out of range")
    return number


def hex_value(value, digits, label):
    require(isinstance(value, str) and re.fullmatch(rf"[0-9a-fA-F]{{{digits}}}", value) is not None,
            f"invalid {label}")
    return value.lower()


def encoded_build(value):
    require(isinstance(value, str) and re.fullmatch(r"(?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,2})", value)
            is not None, "invalid build")
    major, minor = map(int, value.split("."))
    result = major * 1000 + minor + 100
    require(result <= 65535, "build out of range")
    return result


def version(value, unknown=False):
    if not (unknown and value == "unknown"):
        encoded_build(value)
    return value


def normalized(raw):
    require(isinstance(raw, bytes) and len(raw) <= MAX_RESPONSE_BYTES, "response exceeds byte limit")
    try:
        text = raw.decode("ascii", errors="strict").replace("\r\n", "\n")
    except UnicodeError as exc:
        raise ProtocolError("non-ASCII response") from exc
    require(all(char == "\n" or 32 <= ord(char) < 127 for char in text), "unexpected response control byte")
    return text


def ordered_fields(text, names):
    parts = text.split(" ")
    require(len(parts) == len(names), "missing, duplicated, or extra fields")
    result = {}
    for part, name in zip(parts, names):
        require(part.startswith(name + "=") and part != name + "=", f"expected ordered field {name}")
        result[name] = part[len(name) + 1:]
    return result


def frame_body(raw, command, name):
    text = normalized(raw)
    begin, end = f"BEGIN {name}\n", f"END {name}\n"
    require(text.count(begin) == text.count(end) == 1, f"ambiguous {name} framing")
    prefix, rest = text.split(begin)
    body, suffix = rest.split(end)
    require(prefix == command + "\n" and suffix == "deskhop> ", f"unexpected {name} echo/trailer")
    require(body.endswith("\n"), f"incomplete {name} body")
    return body[:-1].split("\n")


class Console:
    """One explicit character device; no discovery, subprocesses, or shell."""

    def __init__(self, port: str, *, timeout=6.0, total_timeout=120.0, transcript=None):
        require(isinstance(port, str) and port.startswith("/dev/") and "\x00" not in port
                and not any(part in (".", "..", "") for part in port.split("/")[2:]),
                "provide one explicit absolute /dev/ serial port")
        require(all(isinstance(n, (int, float)) and math.isfinite(n) and n > 0
                    for n in (timeout, total_timeout)), "timeouts must be finite and positive")
        self.port, self.timeout, self.total_timeout = port, float(timeout), float(total_timeout)
        self.transcript = transcript
        self.events = []
        self.cleanup_errors = []
        self.fd = self.original_attrs = None
        self.pending = b""
        self.rx_bytes = 0
        self.started = time.monotonic()
        self.deadline = self.started + self.total_timeout
        self.bootloader_sent = False

    def _log(self, direction, data):
        event = {"elapsed": round(time.monotonic() - self.started, 6),
                 "direction": direction, "data": data.decode("ascii", "backslashreplace")
                 if isinstance(data, bytes) else str(data)}
        self.events.append(event)
        if callable(self.transcript):
            self.transcript(event)
        elif self.transcript is not None:
            self.transcript.write(f"[{event['elapsed']:.6f}] {direction}: {event['data']!r}\n")
            self.transcript.flush()

    def _remaining(self, deadline):
        remaining = min(deadline, self.deadline) - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("bounded serial operation timed out")
        return min(remaining, 0.1)

    def _dtr(self, enabled):
        fcntl.ioctl(self.fd, termios.TIOCMBIS if enabled else termios.TIOCMBIC,
                    array.array("i", [termios.TIOCM_DTR]))
        self._log("DTR", enabled)

    def open(self):
        require(self.fd is None, "console already open")
        self._remaining(self.deadline)
        try:
            self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            require(stat.S_ISCHR(os.fstat(self.fd).st_mode), "serial port is not a character device")
            # Prevent another terminal from consuming replies or issuing commands.
            if hasattr(termios, "TIOCEXCL"):
                fcntl.ioctl(self.fd, termios.TIOCEXCL)
            self.original_attrs = copy.deepcopy(termios.tcgetattr(self.fd))
            tty.setraw(self.fd, termios.TCSANOW)
            attrs = termios.tcgetattr(self.fd)
            attrs[2] = (attrs[2] | termios.CLOCAL | termios.CREAD) & ~getattr(termios, "CRTSCTS", 0)
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            self._log("OPEN", self.port)
            self._dtr(False)
            require(time.monotonic() + 0.15 < self.deadline, "insufficient serial session budget")
            time.sleep(0.15)
            termios.tcflush(self.fd, termios.TCIFLUSH)
            self._dtr(True)
            greeting = self._read_until(PROMPT, time.monotonic() + self.timeout)
            require(greeting == b"\r\nDeskHop diagnostic console. Type help.\r\ndeskhop> "
                    and not self.pending, "fresh console greeting missing or ambiguous")
            return self
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self.open()

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.fd is None:
            return
        fd = self.fd
        # Best-effort cleanup must continue after USB disappears into ROM.
        actions = [("DTR drop", lambda: self._dtr(False))]
        if self.original_attrs is not None:
            actions.append(("restore terminal", lambda: termios.tcsetattr(fd, termios.TCSANOW, self.original_attrs)))
        if hasattr(termios, "TIOCNXCL"):
            actions.append(("release exclusive", lambda: fcntl.ioctl(fd, termios.TIOCNXCL)))
        actions.append(("close descriptor", lambda: os.close(fd)))
        for label, action in actions:
            try:
                action()
            except Exception as exc:
                self.cleanup_errors.append(f"{label}: {exc}")
        self.fd = self.original_attrs = None
        self.pending = b""

    def _send(self, payload, deadline):
        require(self.fd is not None, "console is not open")
        self._log("TX", payload)
        offset = 0
        while offset < len(payload):
            _, ready, _ = select.select([], [self.fd], [], self._remaining(deadline))
            if not ready:
                continue
            try:
                count = os.write(self.fd, payload[offset:])
            except (BlockingIOError, InterruptedError):
                continue
            require(0 < count <= len(payload) - offset, "serial write made no progress")
            offset += count

    def _read_until(self, marker, deadline):
        while marker not in self.pending:
            require(len(self.pending) < MAX_RESPONSE_BYTES and self.rx_bytes < MAX_RX_BYTES,
                    "console output exceeded byte cap")
            ready, _, _ = select.select([self.fd], [], [], self._remaining(deadline))
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, min(1024, MAX_RESPONSE_BYTES - len(self.pending),
                                            MAX_RX_BYTES - self.rx_bytes))
            except (BlockingIOError, InterruptedError):
                continue
            if not chunk:
                # VMIN=0 permits an empty read. A permanently disconnected device
                # still consumes the bounded operation deadline, never busy-spins.
                time.sleep(min(0.01, self._remaining(deadline)))
                continue
            self.rx_bytes += len(chunk)
            self.pending += chunk
            self._log("RX", chunk)
        end = self.pending.index(marker) + len(marker)
        response, self.pending = self.pending[:end], self.pending[end:]
        return response

    def command(self, text: str, timeout=None) -> bytes:
        require(not self.bootloader_sent, "no further commands after bootloader request")
        require(text in ("help", "status", "history")
                or re.fullmatch(r"history (?:[1-9]|[1-5][0-9]|6[0-4])", text) is not None
                or re.fullmatch(r"verify (?:0|[1-9][0-9]{0,4})\.(?:0|[1-9][0-9]{0,2}) [0-9a-f]{8}", text) is not None,
                "unsupported diagnostic command")
        if text.startswith("verify "):
            encoded_build(text.split()[1])
        duration = self.timeout if timeout is None else timeout
        require(isinstance(duration, (int, float)) and math.isfinite(duration) and duration > 0,
                "command timeout must be positive and finite")
        require(not self.pending, "unsolicited bytes before command")
        deadline = time.monotonic() + duration
        self._send((text + "\n").encode("ascii"), deadline)
        raw = self._read_until(PROMPT, deadline)
        require(not self.pending, "unsolicited bytes after command")
        return raw

    def bootloader(self, target: str) -> bytes:
        require(target in ("A", "B"), "bootloader requires exactly physical A or B")
        require(not self.bootloader_sent and not self.pending, "bootloader request already sent or unsolicited input")
        self.bootloader_sent = True  # Set before the first byte: partial writes are not retried.
        try:
            deadline = time.monotonic() + self.timeout
            self._send(f"bootloader {target}\n".encode("ascii"), deadline)
            raw = self._read_until(b"END bootloader\r\n", deadline)
            expected = (f"bootloader {target}\r\nBEGIN bootloader\r\n"
                        f"board={target} result=accepted scope=local\r\n"
                        "action=enter_disk_free_usb_rom_after_reply\r\nEND bootloader\r\n").encode("ascii")
            require(raw == expected and not self.pending,
                    "bootloader did not return exact local acceptance; inspect transcript and USB state")
            return raw
        except Exception as exc:
            raise BootloaderUnconfirmed("Bootloader outcome unconfirmed; do not resend automatically. "
                                        "Inspect the retained transcript and USB/ROM identity.") from exc


def parse_status(raw) -> dict:
    """Parse a snapshot without assuming which physical board owns the console."""
    lines = [line for line in frame_body(raw, "status", "status") if line]
    cursor = 0

    def take():
        nonlocal cursor
        require(cursor < len(lines), "missing status line")
        value = lines[cursor]
        cursor += 1
        return value

    boards = {}
    while cursor < len(lines) and lines[cursor] in ("board=A", "board=B"):
        result = {}
        for field in ("board", "board_id", "build", "image_crc_at_boot", "boot_session", "uptime_ms"):
            row = ordered_fields(take(), (field,))
            result[field] = row[field]
        role = result["board"]
        require(role in ("A", "B") and role not in boards, "duplicate status board")
        result["board_id"] = hex_value(result["board_id"], 16, "board ID").upper()
        result["boot_session"] = hex_value(result["boot_session"], 16, "boot session")
        result["image_crc_at_boot"] = hex_value(result["image_crc_at_boot"], 8, "boot CRC")
        version(result["build"])
        result["uptime_ms"] = decimal(result["uptime_ms"], MAX_U64 // 1000, "uptime")
        cores = []
        for core in (0, 1):
            row = ordered_fields(take(), ("board", "core", "checkpoints", "age_ms"))
            require(row["board"] == role and row["core"] == str(core), "mislabeled core")
            absent = row["checkpoints"] == "unavailable"
            require(absent == (row["age_ms"] == "unavailable"), "inconsistent core availability")
            cores.append({"core": core, "checkpoints": None if absent else decimal(row["checkpoints"], MAX_U32, "checkpoints"),
                          "age_ms": None if absent else decimal(row["age_ms"], MAX_U32, "core age")})
        prefix = f"board={role} update "
        line = take()
        require(line.startswith(prefix), "missing update state")
        update = ordered_fields(line[len(prefix):], ("seen", "source", "phase", "received", "total",
                                                    "progress_age_ms", "target", "attempt"))
        update["seen"] = decimal(update["seen"], 1, "update seen")
        require(update["source"] in ("none", "peer", "usb") and update["phase"] in PHASES, "invalid update state")
        update["total"] = decimal(update["total"], IMAGE_BYTES, "image total", IMAGE_BYTES)
        update["received"] = decimal(update["received"], IMAGE_BYTES, "received bytes")
        require(update["received"] % 256 == 0, "received progress is not page-aligned")
        update["progress_age_ms"] = None if update["progress_age_ms"] == "unavailable" else decimal(update["progress_age_ms"], MAX_U32, "progress age")
        update["target"] = version(update["target"], unknown=True)
        update["attempt"] = decimal(update["attempt"], MAX_U32, "attempt")
        if not update["seen"]:
            require(update["source"] == "none" and update["phase"] == "idle" and update["received"] == 0
                    and update["progress_age_ms"] is None and update["target"] == "unknown" and update["attempt"] == 0,
                    "unseen update contains progress")
        else:
            require(update["source"] != "none" and update["phase"] != "idle"
                    and (update["source"] != "peer" or update["target"] != "unknown"), "seen update lacks source/phase/target")
        result["cores"], result["update"] = cores, update
        if boards:
            prefix = f"board={role} observation "
            line = take()
            require(line.startswith(prefix), "missing peer observation")
            observation = ordered_fields(line[len(prefix):], ("boot", "progress", "update"))
            require(observation["boot"] in BOOTS and observation["progress"] in PROGRESS
                    and observation["update"] in EXECUTION, "invalid peer observation")
            result["observation"] = observation
        boards[role] = result
    require(boards, "status has no local board")
    peer = ordered_fields(take(), ("peer",))["peer"]
    require(peer in PEER_OUTCOMES and (peer == "ok") == (len(boards) == 2), "peer outcome disagrees with status")
    require(take() == "verification=available" and cursor == len(lines), "incorrect capability or extra status rows")
    require(len({board["board_id"] for board in boards.values()}) == len(boards), "duplicate board UID")
    return {"boards": boards, "local": next(iter(boards)), "peer": peer, "verification": "available"}


def validate_help(raw, require_bootloader=True):
    lines = frame_body(raw, "help", "help")
    commands = [line.split()[0] for line in lines if line.startswith("  ")]
    expected = ["help", "status", "history", "verify"]
    require(commands == expected + ["bootloader"] if require_bootloader else commands in (expected, expected + ["bootloader"]),
            "unexpected help command list")
    phrases = ["GAP", "full 256KiB firmware including metadata; configuration excluded",
               "PASS describes a fresh scan", "Status image CRC is boot metadata only"]
    if "bootloader" in commands:
        require(not any("all commands are read-only" in line for line in lines), "misleading maintenance help")
        phrases += ["diagnostics and disruptive maintenance", "Diagnostics are read-only and query both boards",
                    "bounded peer timeouts", "bootloader A|B", "DISRUPTIVE", "disk-free USB ROM",
                    "physical A or B; no default/both", "target's USB-connected computer", "peer acceptance is not boot proof"]
    for phrase in phrases:
        require(any(phrase in line for line in lines), f"missing help explanation: {phrase}")


def parse_verify(raw, build: str, slot_crc: str, boot_crc: str, identities: dict,
                 command_crc: str | None = None, previous: dict | None = None) -> dict:
    metadata_version = encoded_build(build)
    slot_crc = hex_value(slot_crc, 8, "expected full-slot CRC")
    boot_crc = hex_value(boot_crc, 8, "expected metadata CRC")
    command_crc = slot_crc if command_crc is None else hex_value(command_crc, 8, "command CRC")
    command = f"verify {build} {command_crc}"
    lines = frame_body(raw, command, "verify")
    require(len(lines) == 21, "missing, duplicated, or extra verify evidence")
    require(tuple(lines[:6]) == ("scope=both", "claim=scan_snapshot", f"expected_build={build}",
                                 f"expected_crc32={command_crc}", "coverage_start=0x10000000", f"coverage_bytes={IMAGE_BYTES}"),
            "verify expectation/coverage header differs")
    verdict = "PASS" if command_crc == slot_crc else "FAIL"
    reason = "match" if verdict == "PASS" else "crc_mismatch"
    boards = {}
    for index in (0, 1):
        base = 6 + index * 7
        role = ordered_fields(lines[base], ("board", "result", "reason"))["board"]
        require(role in ("A", "B") and role not in boards and role in identities, "wrong/duplicate verify board")

        def row(offset, names):
            values = ordered_fields(lines[base + offset], ("board", *names))
            require(values.pop("board") == role, "wrong-role verify row")
            return values

        decision = row(0, ("result", "reason"))
        require(decision == {"result": verdict, "reason": reason}, f"{role} unexpected verify verdict")
        identity = row(1, ("build", "board_id", "boot_session"))
        identity["board_id"] = hex_value(identity["board_id"], 16, "verify UID").upper()
        identity["boot_session"] = hex_value(identity["boot_session"], 16, "verify session")
        require(identity["build"] == build and all(identity[key] == identities[role][key] for key in identity),
                f"{role} verify identity differs from preceding status")
        scan = row(2, ("scan_start_us", "scan_end_us", "bytes", "crc32"))
        for field in ("scan_start_us", "scan_end_us"):
            scan[field] = decimal(scan[field], MAX_U64, field)
        start, end = scan["scan_start_us"], scan["scan_end_us"]
        require(1024000 <= end - start < 3000000, f"{role} scan duration violates page budget/deadline")
        require(start >= identities[role]["uptime_ms"] * 1000, f"{role} scan predates status")
        scan["bytes"] = decimal(scan["bytes"], IMAGE_BYTES, "scan bytes", IMAGE_BYTES)
        scan["crc32"] = hex_value(scan["crc32"], 8, "scan CRC")
        require(scan["crc32"] == slot_crc, f"{role} fresh CRC differs from artifact")
        metadata = row(3, ("metadata_magic", "metadata_version", "metadata_reserved", "metadata_crc32"))
        require(hex_value(metadata["metadata_magic"], 8, "metadata magic") == "0000f00d"
                and metadata["metadata_version"] == str(metadata_version) and metadata["metadata_reserved"] == "0"
                and hex_value(metadata["metadata_crc32"], 8, "metadata CRC") == boot_crc,
                f"{role} metadata differs from artifact")
        generation = row(4, ("boot_crc32", "generation_start", "generation_end"))
        require(hex_value(generation["boot_crc32"], 8, "boot CRC") == boot_crc, f"{role} cached metadata differs")
        for field in ("generation_start", "generation_end"):
            generation[field] = decimal(generation[field], MAX_U64 - 1, field)
        require(generation["generation_start"] == generation["generation_end"], f"{role} firmware changed during scan")
        cores = []
        for core in (0, 1):
            values = row(5 + core, ("core", "start", "end", "age_ms", "valid"))
            require(values.pop("core") == str(core) and values["valid"] == "1", f"{role} core evidence invalid")
            for field in ("start", "end", "age_ms"):
                values[field] = decimal(values[field], MAX_U32, f"core {field}")
            require(0 < ((values["end"] - values["start"]) & MAX_U32) < 1 << 31
                    and values["age_ms"] <= 100, f"{role} core {core} did not advance freshly")
            baseline = identities[role]["cores"][core]["checkpoints"]
            require(baseline is not None and ((values["start"] - baseline) & MAX_U32) < 1 << 31,
                    f"{role} core {core} scan predates status")
            if previous:
                old = previous["boards"][role]["cores"][core]
                require(0 < ((values["start"] - old["end"]) & MAX_U32) < 1 << 31,
                        f"{role} core {core} scan checkpoint reused")
            cores.append(values)
        if previous:
            old = previous["boards"][role]
            require(identity == old["identity"] and start > old["scan"]["scan_end_us"]
                    and generation == old["generation"], f"{role} scan reused or firmware identity/generation changed")
        boards[role] = {"verdict": decision, "identity": identity, "scan": scan,
                        "metadata": metadata, "generation": generation, "cores": cores}
    require(lines[20] == f"result={verdict} reason={'both_match' if verdict == 'PASS' else 'board_failure'}",
            "overall verify verdict disagrees")
    return {"command": command, "expected_slot_crc32": slot_crc, "command_crc32": command_crc,
            "result": verdict, "boards": boards}


def _parse_transfer_event(name, rest):
    """Sparse source-side measurements, not receiver/flash acceptance evidence."""
    selector = "phase" if name == "transfer_source" else "metric"
    values = ordered_fields(rest, (selector, "mode", "value"))
    value = decimal(values["value"], MAX_U32, "transfer value")
    mode = values["mode"]
    if name == "transfer_source":
        phase = values["phase"]
        modes = {"caps_queued": ("none",), "batch_begin": ("pages", "mixed"),
                 "words_begin": ("words", "mixed"), "retry": ("pages", "mixed"),
                 "progress": ("pages", "words", "mixed"),
                 "batch_end": ("pages", "mixed"), "words_end": ("words", "mixed")}
        require(phase in modes and mode in modes[phase], "invalid transfer phase/mode")
        if phase == "caps_queued":
            require(0 < value <= 0xffffffc0 and value % 64 == 0, "invalid transfer capability tag")
        elif phase in ("batch_begin", "words_begin", "retry"):
            alignment = 4 if phase == "words_begin" else 256
            require(value < IMAGE_BYTES and value % alignment == 0, "invalid transfer offset")
        elif phase == "progress":
            require(value in (65536, 131072, 196608, IMAGE_BYTES), "invalid transfer milestone")
        else:
            require(value == IMAGE_BYTES, "invalid transfer endpoint")
    else:
        metrics = {"transfer_timing": ("elapsed_us", "page_service_us", "page_gap_us", "page_max_us"),
                   "transfer_count": ("page_requests", "word_requests", "page_retries")}
        require(mode in ("pages", "words", "mixed") and values["metric"] in metrics[name],
                "invalid transfer metric/mode")
    values["value"] = value
    return {"event": name, **values}


def _parse_event(text):
    name, _, rest = text.partition(" ")
    if name in ("transfer_source", "transfer_timing", "transfer_count"):
        return _parse_transfer_event(name, rest)
    schemas = {"boot": ("build", "output"), "output_local": ("old", "new"), "output_peer": ("old", "new"),
               "usb_mount": (), "usb_unmount": (),
               "hid_mount": ("device", "instance", "protocol", "keyboard", "mouse"),
               "hid_unmount": ("device", "instance", "protocol", "keyboard", "mouse"),
               "descriptor_rejected": ("device", "instance"), "packet_checksum_error": ("packet_type",),
               "uart_dropped": ("packet_type",), "update_begin": ("source", "target"),
               "update_progress": ("source", "percent", "received"), "update_phase": ("phase", "source", "target"),
               "peer_observed": ("peer", "boot", "build"), "peer_progress": ("peer", "progress", "build")}
    require(name in schemas, "unknown history event")
    if not schemas[name]:
        require(text == name, "unexpected fields for fieldless history event")
        return {"event": name}
    values = ordered_fields(rest, schemas[name])
    for key, value in values.items():
        if key in ("output", "old", "new", "peer"):
            require(value in ("A", "B"), "invalid event role")
        elif key == "build":
            version(value)
        elif key == "target":
            version(value, unknown=True)
            require(values["source"] != "peer" or value != "unknown", "missing peer update target")
        elif key == "source":
            require(value in ("peer", "usb"), "invalid event source")
        elif key == "phase":
            require(value in PHASES[1:], "invalid event phase")
        elif key == "boot":
            require(value in ("first_seen", "new_boot", "identity_changed"), "invalid observed boot")
        elif key == "progress":
            require(value == "advancing", "invalid observed progress")
        else:
            maximum = 1 if key in ("keyboard", "mouse") else MAX_U32 if key in ("packet_type", "received") else 255
            values[key] = decimal(value, maximum, f"event {key}")
    if name == "update_progress":
        require(values["percent"] in (25, 50, 75, 100) and 0 < values["received"] <= IMAGE_BYTES
                and values["received"] % 256 == 0
                and values["received"] // (IMAGE_BYTES // 4) * 25 == values["percent"], "invalid progress milestone")
    return {"event": name, **values}


def validate_history(raw, identities: dict, previous=None) -> dict:
    text = normalized(raw)
    command = text.split("\n", 1)[0]
    require(re.fullmatch(r"history(?: (?:[1-9]|[1-5][0-9]|6[0-4]))?", command) is not None, "invalid history echo")
    requested = int(command.split()[1]) if " " in command else 16
    lines = frame_body(raw, command, "history")
    cursor = 0

    def field(key):
        nonlocal cursor
        require(cursor < len(lines), "truncated history metadata")
        value = ordered_fields(lines[cursor], (key,))[key]
        cursor += 1
        return value

    require(field("scope") == "both" and field("requested_per_board") == str(requested)
            and field("timing") == "approximate_snapshot_alignment" and field("capacity_per_board") == "64",
            "invalid history scope/count/clock/capacity")
    boards = {}
    for index in (0, 1):
        if index:
            if cursor >= len(lines) or lines[cursor] != "":
                break
            cursor += 1
        role = field("board")
        require(role in identities and role in ("A", "B") and role not in boards, "wrong/duplicate history board")
        boot = hex_value(field("boot_session"), 16, "history session")
        require(boot == identities[role]["boot_session"], f"{role} history/status session differs")
        board = {"board": role, "boot_session": boot,
                 "sampled_uptime_ms": decimal(field("sampled_uptime_ms"), MAX_U64 // 1000, "history uptime"),
                 "returned": decimal(field("returned"), requested, "history returned"),
                 "overwritten": decimal(field("overwritten"), MAX_U64, "history overwritten")}
        require(board["sampled_uptime_ms"] >= identities[role]["uptime_ms"], "history predates status")
        boards[role] = board
    peer = field("peer")
    require(peer in PEER_OUTCOMES and (peer == "ok") == (len(boards) == 2), "history peer outcome mismatch")
    bound = decimal(field("peer_capture_bound_us"), MAX_U64, "capture bound") if peer == "ok" else None
    require(cursor < len(lines) and lines[cursor] == "", "missing history row separator")
    rows, per_board, last_time, last_age = [], {role: [] for role in boards}, {}, None
    for line in lines[cursor + 1:]:
        gap = re.fullmatch(r"GAP board=([AB]) seq=([0-9]+)", line)
        if gap:
            role, sequence = gap.groups()
            row = {"gap": True, "board": role, "seq": decimal(sequence, MAX_U64 - 1, "sequence", 1)}
        else:
            match = re.fullmatch(r"board=([AB]) seq=([0-9]+) uptime_ms=([0-9]+) age_ms=([0-9]+) event=(.+)", line)
            require(match is not None, "malformed history row")
            role, sequence, uptime, age, event = match.groups()
            require(role in boards, "history event has no metadata")
            row = {"gap": False, "board": role, "seq": decimal(sequence, MAX_U64 - 1, "sequence", 1),
                   "uptime_ms": decimal(uptime, MAX_U64 // 1000, "event uptime"),
                   "age_ms": decimal(age, MAX_U64 // 1000, "event age"), **_parse_event(event)}
            require(row.get("peer") != role, "peer event names its own board")
            if row["event"] == "boot":
                require(row["build"] == identities[role]["build"], "boot event/status build differs")
            delta = boards[role]["sampled_uptime_ms"] - row["uptime_ms"] - row["age_ms"]
            require(0 <= delta <= 1 and row["uptime_ms"] >= last_time.get(role, 0), "invalid event age/order")
            require(last_age is None or row["age_ms"] <= last_age, "merged history is not oldest first")
            last_time[role], last_age = row["uptime_ms"], row["age_ms"]
        require(role in boards, "history row has no board metadata")
        prior = per_board[role]
        require(not prior or row["seq"] == prior[-1]["seq"] + 1, "history sequences are not contiguous")
        prior.append(row)
        rows.append(row)
    for role, board in boards.items():
        retained = per_board[role]
        latest = retained[-1]["seq"] if retained else 0
        require(len(retained) == board["returned"] == min(requested, latest, 64)
                and board["overwritten"] == max(0, latest - 64), "history retention counts disagree")
        if previous and role in previous["boards"]:
            old = previous["boards"][role]
            require(board["boot_session"] == old["boot_session"]
                    and board["sampled_uptime_ms"] > old["sampled_uptime_ms"]
                    and board["overwritten"] >= old["overwritten"], "history session/uptime/overwrite regressed")
            old_rows = {r["seq"]: r for r in previous["rows"] if r["board"] == role}
            require(not old_rows or latest >= max(old_rows), "history latest sequence regressed")
            for row in retained:
                old_row = old_rows.get(row["seq"])
                if old_row and not row["gap"] and not old_row["gap"]:
                    require({k: v for k, v in row.items() if k != "age_ms"}
                            == {k: v for k, v in old_row.items() if k != "age_ms"}, "retained event changed")
    return {"command": command, "requested_per_board": requested, "peer": peer,
            "peer_capture_bound_us": bound, "boards": boards, "rows": rows}
