"""Upgrade state-machine acceptance tests with no hardware or subprocesses.

Console responses use the real parser fixtures. Device effects are recorded by
the backend double, so tests assert *which operations never happen* on failure.
"""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from deskhop_update.console import BootloaderUnconfirmed
from deskhop_update.workflow import Updater, backup_version, check_status, progressing
from deskhop_update.platform import DeploymentError
from test_console import IDS, SESSIONS, HELP, status, verify, history


def image(version):
    data = bytearray(b"\xff" * 262144)
    data[:4] = b"DHOP"
    crc = zlib.crc32(data[:258048])
    struct.pack_into("<IHHI", data, 258048, 0xf00d, version, 0, crc)
    return bytes(data), f"{crc:08x}"


class FakeConsole:
    def __init__(self, backend):
        self.backend = backend
        self.cleanup_errors = []
        backend.consoles.append(self)

    def __enter__(self):
        assert not self.backend.console_open
        assert not self.backend.rom, "Do not open a CDC console while the target is in ROM"
        self.backend.console_open = True
        self.backend.calls.append(("console_open",))
        return self

    def __exit__(self, *_args):
        self.backend.console_open = False
        self.backend.calls.append(("console_close",))
        if self is self.backend.consoles[0]:
            self.cleanup_errors.extend(self.backend.cleanup_errors)

    def command(self, command):
        b = self.backend
        assert b.console_open
        assert not b.rom, "Do not send diagnostic commands through the retained old console"
        b.calls.append(("command", command))
        b.clock += 0.05
        build = b.candidate["build"] if b.running_candidate else "0.104"
        if command == "help":
            return HELP
        if command == "status":
            b.status_count += 1
            at = int(b.clock * 1_000_000)
            if b.freeze_from is not None and b.status_count >= b.freeze_from:
                at = b.last_status_us
            b.last_status_us = at
            raw = status(at, build=build, local=b.local)
            crc = b.candidate["boot_crc"] if b.running_candidate else b.old_crc
            raw = raw.replace(b"deadbeef", crc.encode())
            if not b.running_candidate:
                for role in ("A", "B"):
                    raw = raw.replace(SESSIONS[role].encode(), ("a" if role == "A" else "b").encode() * 16)
            if b.rollout_stuck and b.running_candidate:
                peer = "B" if b.local == "A" else "A"
                raw = raw.replace(f"board={peer}\r\nboard_id={IDS[peer]}\r\nbuild={build}".encode(),
                                  f"board={peer}\r\nboard_id={IDS[peer]}\r\nbuild=0.104".encode())
            if b.wrong_uid:
                raw = raw.replace(IDS["B"].encode(), b"0000000000000000")
            if b.update_active:
                raw = raw.replace(b"seen=0 source=none phase=idle received=0 total=262144 "
                                  b"progress_age_ms=unavailable target=unknown attempt=0",
                                  b"seen=1 source=peer phase=receiving received=65536 total=262144 "
                                  b"progress_age_ms=1 target=0.105 attempt=1", 1)
            return raw
        if command.startswith("verify "):
            supplied = command.split()[2]
            actual = b.candidate["slot_crc"]
            template_crc = "12345678" if supplied == actual else "12345679"
            raw = verify(int(b.clock * 1_000_000), template_crc, build=build, local=b.local)
            raw = re.sub(rb"\b12345679\b", f"{int(actual, 16) ^ 1:08x}".encode(), raw)
            raw = re.sub(rb"\b12345678\b", actual.encode(), raw).replace(b"deadbeef", b.candidate["boot_crc"].encode())
            b.clock += 1.026
            return raw
        if command.startswith("history "):
            return history(int(b.clock * 1_000_000), build=build, local=b.local, count=int(command.split()[1]))
        raise AssertionError(f"Unexpected command: {command}")

    def bootloader(self, target):
        b = self.backend
        assert b.console_open
        b.calls.append(("bootloader", target))
        b.serial_bootloader_requested = True
        if b.boot_error:
            raise b.boot_error
        b.rom = True


class FakeBackend:
    def __init__(self, candidate, local="A"):
        self.candidate, self.local = candidate, local
        self.clock = 10.0
        self.calls = []
        self.consoles = []
        self.cleanup_errors = []
        self.operation_errors = {}
        self.serial_bootloader_requested = False
        self.console_open = self.rom = self.running_candidate = False
        self.old, self.old_crc = image(204)
        self.settings = b"settings" + b"\xff" * (4096 - 8)
        self.boot_error = self.enumeration_error = None
        self.bad_readback = self.bad_settings = self.rollout_stuck = False
        self.wrong_uid = self.update_active = False
        self.freeze_from = None
        self.status_count = self.last_status_us = 0
        self.mutate_before_load = None

    def now(self):
        return self.clock

    def pause(self, seconds):
        self.clock += seconds

    def console(self, **_kwargs):
        return FakeConsole(self)

    def health(self, boot=False, baseline=None):
        self.calls.append(("health", boot))
        assert boot == self.rom
        assert baseline in (None, ["media-id"])
        if boot and self.mutate_before_load:
            self.mutate_before_load()
            self.mutate_before_load = None
        return ["media-id"]

    def wait_bootloader(self, uid, baseline):
        self.calls.append(("wait_bootloader", uid))
        assert self.console_open, "DTR dropped before ROM enumeration"
        assert self.rom and baseline == ["media-id"]
        if self.enumeration_error:
            raise self.enumeration_error
        self.identity(uid)

    def identity(self, uid):
        self.calls.append(("identity", uid))
        assert uid == IDS[self.local] and self.rom

    def save(self, label, start, end, uid):
        self.calls.append(("save", label, start, end, uid))
        assert self.rom
        assert self.console_open == self.serial_bootloader_requested, "Old CDC context must span ROM operations"
        assert uid == IDS[self.local]
        if label in self.operation_errors:
            raise self.operation_errors[label]
        if label == "firmware-before":
            return self.old
        if label == "settings-before":
            return self.settings
        if label == "firmware-after":
            return b"x" + self.candidate["image"][1:] if self.bad_readback else self.candidate["image"]
        if label == "settings-after":
            return b"x" + self.settings[1:] if self.bad_settings else self.settings
        raise AssertionError(f"Unexpected save: {label}")

    def load(self, path, uid):
        self.calls.append(("load", path, uid))
        assert self.rom and uid == IDS[self.local]
        assert self.console_open == self.serial_bootloader_requested, "Old CDC context must span the load"
        if "load" in self.operation_errors:
            raise self.operation_errors["load"]

    def reboot(self, uid):
        self.calls.append(("reboot", uid))
        assert self.rom and uid == IDS[self.local]
        assert self.console_open == self.serial_bootloader_requested, "Old CDC context must span normal reboot"
        if "reboot" in self.operation_errors:
            raise self.operation_errors["reboot"]
        self.rom, self.running_candidate = False, True
        self.clock += 0.5

    def wait_port(self):
        self.calls.append(("wait_port",))
        assert not self.console_open, "Close the old CDC context before waiting for a fresh application port"


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.directory = Path(self.stack.enter_context(tempfile.TemporaryDirectory(prefix="deskhop-updater-test-")))
        data, boot_crc = image(205)
        bin_path, uf2_path = self.directory / "candidate.bin", self.directory / "candidate.uf2"
        bin_path.write_bytes(data)
        uf2_path.write_bytes(b"offline UF2 fixture; transport never reads this")
        self.candidate = {"build": "0.105", "encoded_version": 205, "boot_crc": boot_crc,
                          "slot_crc": f"{zlib.crc32(data):08x}", "image": data,
                          "bin_path": bin_path, "uf2_path": uf2_path,
                          "bin_sha256": hashlib.sha256(data).hexdigest(),
                          "uf2_sha256": hashlib.sha256(uf2_path.read_bytes()).hexdigest()}
        self.profile = {"target": "A", "port": "/dev/cu.fixture", "uids": dict(IDS)}
        self.backend = FakeBackend(self.candidate)

    def updater(self, **kwargs):
        return Updater(self.candidate, self.profile, self.backend, self.directory,
                       progress=lambda _value: None, **kwargs)

    def calls(self, name):
        return [call for call in self.backend.calls if call[0] == name]

    def assert_no_write(self):
        self.assertEqual(self.calls("load"), [])
        self.assertEqual(self.calls("reboot"), [])

    def assert_serial_flash_lifecycle(self):
        calls = self.backend.calls
        opens = [index for index, call in enumerate(calls) if call[0] == "console_open"]
        closes = [index for index, call in enumerate(calls) if call[0] == "console_close"]
        self.assertEqual(len(opens), 2)
        self.assertEqual(len(closes), 2)
        enumeration = calls.index(("wait_bootloader", IDS[self.backend.local]))
        wait_port = calls.index(("wait_port",))
        self.assertLess(opens[0], enumeration)
        rom_operations = [index for index, call in enumerate(calls) if call[0] in ("save", "load", "reboot")]
        self.assertEqual(len(rom_operations), 6)
        for index in rom_operations:
            self.assertLess(enumeration, index)
            self.assertLess(index, closes[0])
        self.assertLess(closes[0], wait_port)
        self.assertLess(wait_port, opens[1])
        self.assertLess(opens[1], closes[1])
        self.assertFalse(any(call[0] == "command" for call in calls[enumeration:opens[1]]))
        for index, call in enumerate(calls):
            if call[0] == "command" and call[1].startswith(("verify ", "history ")):
                self.assertLess(opens[1], index)
                self.assertLess(index, closes[1])
        self.assertFalse(self.backend.console_open)

    def test_success_local_a_and_full_diagnostics(self):
        result = self.updater().run()
        self.assertEqual(result["stage"], "complete")
        self.assertTrue(result["firmware_verified"])
        self.assertTrue(result["independent_readback"])
        self.assertTrue(result["settings_unchanged"])
        self.assertEqual(result["input_acceptance"], "pending")
        self.assertEqual(self.calls("bootloader"), [("bootloader", "A")])
        self.assertEqual(len(self.calls("load")), 1)
        self.assertEqual(self.calls("reboot"), [("reboot", IDS["A"])])
        self.assertEqual([v["result"] for v in result["verifications"]], ["PASS", "FAIL", "PASS"])
        self.assertEqual(len(result["histories"]), 2)
        commands = [call[1] for call in self.calls("command")]
        self.assertEqual(len([command for command in commands if command.startswith("verify ")]), 3)
        self.assert_serial_flash_lifecycle()
        journal = json.loads((self.directory / "result.json").read_text())
        self.assertTrue(journal["firmware_verified"])

    def test_success_local_b(self):
        self.profile["target"] = self.backend.local = "B"
        result = self.updater().run()
        self.assertEqual(result["target"], "B")
        self.assertEqual(self.calls("bootloader"), [("bootloader", "B")])
        self.assertEqual(self.calls("reboot"), [("reboot", IDS["B"])])
        self.assert_serial_flash_lifecycle()

    def test_already_bootloader_opens_only_fresh_application_console(self):
        for role in ("A", "B"):
            with self.subTest(role=role):
                self.profile["target"] = role
                self.backend = FakeBackend(self.candidate, local=role)
                self.backend.rom = True
                result = self.updater(already_bootloader=True).run()
                self.assertTrue(result["firmware_verified"])
                self.assertTrue(result["independent_readback"])
                self.assertTrue(result["settings_unchanged"])
                self.assertEqual(self.calls("bootloader"), [])
                self.assertEqual(self.calls("wait_bootloader"), [])
                self.assertEqual(len(self.calls("console_open")), 1)
                self.assertEqual(len(self.calls("console_close")), 1)
                self.assertEqual(len(self.calls("load")), 1)
                self.assertEqual(self.calls("reboot"), [("reboot", IDS[role])])
                calls = self.backend.calls
                self.assertLess(calls.index(("reboot", IDS[role])), calls.index(("wait_port",)))
                self.assertLess(calls.index(("wait_port",)), calls.index(("console_open",)))
                self.assertFalse(self.backend.console_open)

    def test_rom_transport_failures_close_once_and_never_continue_or_retry(self):
        operations = ("enumeration", "firmware-before", "settings-before", "load",
                      "firmware-after", "settings-after", "reboot")
        expected_stages = ("bootloader_requested", "backing_up", "backing_up", "flashing",
                           "readback", "readback", "reboot_requested")
        for role in ("A", "B"):
            for failed_index, (operation, stage) in enumerate(zip(operations, expected_stages)):
                with self.subTest(role=role, operation=operation):
                    self.profile["target"] = role
                    self.backend = FakeBackend(self.candidate, local=role)
                    error = DeploymentError(f"injected {operation} failure")
                    if operation == "enumeration":
                        self.backend.enumeration_error = error
                    else:
                        self.backend.operation_errors[operation] = error
                    with self.assertRaises(DeploymentError) as caught:
                        self.updater().run()
                    self.assertIs(caught.exception, error)
                    self.assertEqual(self.calls("bootloader"), [("bootloader", role)])
                    self.assertEqual(len(self.calls("console_open")), 1)
                    self.assertEqual(len(self.calls("console_close")), 1)
                    self.assertFalse(self.backend.console_open)
                    self.assertTrue(self.backend.rom)
                    attempted = []
                    for call in self.backend.calls:
                        if call[0] == "wait_bootloader":
                            attempted.append("enumeration")
                        elif call[0] == "save":
                            attempted.append(call[1])
                        elif call[0] in ("load", "reboot"):
                            attempted.append(call[0])
                    self.assertEqual(attempted, list(operations[:failed_index + 1]))
                    self.assertEqual(self.backend.calls[-1], ("console_close",))
                    self.assertEqual(self.calls("wait_port"), [])
                    journal = json.loads((self.directory / "result.json").read_text())
                    self.assertEqual(journal["failed_stage"], stage)
                    self.assertEqual(journal["write_started"], failed_index >= operations.index("load"))
                    self.assertEqual(journal["reboot_requested"], operation == "reboot")

    def test_old_console_cleanup_errors_are_recorded_after_success(self):
        self.backend.cleanup_errors = ["DTR: [Errno 6] Device not configured", "termios: stale device"]
        result = self.updater().run()
        self.assertTrue(result["firmware_verified"])
        self.assertEqual(result["bootloader_console_cleanup_errors"], self.backend.cleanup_errors)
        self.assert_serial_flash_lifecycle()
        journal = json.loads((self.directory / "result.json").read_text())
        self.assertEqual(journal["bootloader_console_cleanup_errors"], self.backend.cleanup_errors)

    def test_old_console_cleanup_errors_do_not_mask_original_failure(self):
        self.backend.cleanup_errors = ["close: stale device"]
        error = DeploymentError("original backup failure")
        self.backend.operation_errors["firmware-before"] = error
        with self.assertRaises(DeploymentError) as caught:
            self.updater().run()
        self.assertIs(caught.exception, error)
        self.assert_no_write()
        self.assertFalse(self.backend.console_open)
        journal = json.loads((self.directory / "result.json").read_text())
        self.assertEqual(journal["error"], "DeploymentError: original backup failure")
        self.assertEqual(journal["bootloader_console_cleanup_errors"], self.backend.cleanup_errors)

    def test_unconfirmed_bootloader_never_flashes_or_retries(self):
        self.backend.boot_error = BootloaderUnconfirmed("reply truncated")
        with self.assertRaises(BootloaderUnconfirmed):
            self.updater().run()
        self.assertEqual(len(self.calls("bootloader")), 1)
        self.assertEqual(self.calls("wait_bootloader"), [])
        self.assert_no_write()
        self.assertFalse(self.backend.console_open)

    def test_rom_identity_or_enumeration_failure_never_flashes(self):
        self.backend.enumeration_error = DeploymentError("wrong ROM identity")
        with self.assertRaises(DeploymentError):
            self.updater().run()
        self.assert_no_write()
        self.assertEqual(len(self.calls("bootloader")), 1)
        self.assertFalse(self.backend.console_open)

    def test_bad_backup_leaves_target_in_rom(self):
        self.backend.old = b"\x00" + self.backend.old[1:]
        with self.assertRaisesRegex(DeploymentError, "Existing image is invalid"):
            self.updater().run()
        self.assert_no_write()
        self.assertTrue(self.backend.rom)

    def test_already_bootloader_invalid_or_newer_backup_refused(self):
        for old_version in (201, 205, 206):
            with self.subTest(version=old_version):
                self.backend = FakeBackend(self.candidate)
                self.backend.rom = True
                self.backend.old, _ = image(old_version)
                with self.assertRaises(DeploymentError):
                    self.updater(already_bootloader=True).run()
                self.assert_no_write()
                self.assertEqual(self.calls("bootloader"), [])
                self.assertEqual(self.calls("console_open"), [])

    def test_short_backup_refused(self):
        self.backend.old = self.backend.old[:-1]
        with self.assertRaisesRegex(DeploymentError, "Short firmware backup"):
            self.updater().run()
        self.assert_no_write()

    def test_readback_mismatch_never_reboots_or_retries(self):
        self.backend.bad_readback = True
        with self.assertRaisesRegex(DeploymentError, "readback differs"):
            self.updater().run()
        self.assertEqual(len(self.calls("load")), 1)
        self.assertEqual(self.calls("reboot"), [])
        self.assertTrue(self.backend.rom)
        journal = json.loads((self.directory / "result.json").read_text())
        self.assertTrue(journal["write_started"])
        self.assertFalse(journal["reboot_requested"])
        self.assertEqual(journal["failed_stage"], "readback")

    def test_changed_settings_never_reboots(self):
        self.backend.bad_settings = True
        with self.assertRaisesRegex(DeploymentError, "settings changed"):
            self.updater().run()
        self.assertEqual(len(self.calls("load")), 1)
        self.assertEqual(self.calls("reboot"), [])
        self.assertTrue(self.backend.rom)

    def test_propagation_timeout_never_repeats_flash_or_reboot(self):
        self.backend.rollout_stuck = True
        with self.assertRaisesRegex(DeploymentError, "rollout did not settle"):
            self.updater(rollout_timeout=1).run()
        self.assertEqual(len(self.calls("bootloader")), 1)
        self.assertEqual(len(self.calls("load")), 1)
        self.assertEqual(len(self.calls("reboot")), 1)
        self.assertLess(self.backend.clock, 15)

    def test_already_current_verifies_without_device_writes(self):
        self.backend.running_candidate = True
        result = self.updater().run()
        self.assertTrue(result["already_current"])
        self.assertTrue(result["firmware_verified"])
        self.assertFalse(result["write_started"])
        self.assertEqual(self.calls("bootloader"), [])
        self.assertEqual(self.calls("save"), [])
        self.assertEqual(self.calls("wait_bootloader"), [])
        self.assertEqual(self.calls("wait_port"), [])
        self.assertEqual(len(self.calls("console_open")), 1)
        self.assertEqual(len(self.calls("console_close")), 1)
        self.assertFalse(self.backend.console_open)
        self.assert_no_write()

    def test_uid_mismatch_refused_before_bootloader(self):
        self.backend.wrong_uid = True
        with self.assertRaisesRegex(DeploymentError, "unexpected physical identity"):
            self.updater().run()
        self.assertEqual(self.calls("bootloader"), [])
        self.assert_no_write()

    def test_wrong_local_target_refused(self):
        self.backend.local = "B"
        with self.assertRaisesRegex(DeploymentError, "USB-connected to this Mac"):
            self.updater().run()
        self.assertEqual(self.calls("bootloader"), [])
        self.assert_no_write()

    def test_active_update_refused_before_bootloader(self):
        self.backend.update_active = True
        with self.assertRaisesRegex(DeploymentError, "already active"):
            self.updater().run()
        self.assertEqual(self.calls("bootloader"), [])
        self.assert_no_write()

    def test_stalled_core_progress_fails_diagnostics(self):
        self.backend.running_candidate = True
        self.backend.freeze_from = 3
        with self.assertRaisesRegex(DeploymentError, "Both cores on both boards must advance"):
            self.updater().run()
        self.assert_no_write()
        self.assertEqual(self.calls("bootloader"), [])

    def test_candidate_mutation_before_preflight_refused(self):
        self.candidate["uf2_path"].write_bytes(b"changed after manifest validation")
        with self.assertRaisesRegex(DeploymentError, "[Cc]andidate|artifact|UF2|checksum|changed"):
            self.updater().run()
        self.assert_no_write()
        self.assertEqual(self.calls("bootloader"), [])

    def test_candidate_mutation_while_entering_rom_refused_before_write(self):
        self.backend.mutate_before_load = lambda: self.candidate["bin_path"].write_bytes(b"changed during enumeration")
        with self.assertRaisesRegex(DeploymentError, "[Cc]andidate|artifact|BIN|checksum|changed"):
            self.updater().run()
        self.assert_no_write()


if __name__ == "__main__":
    unittest.main()
