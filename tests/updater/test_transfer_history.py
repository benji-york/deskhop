"""Upgrade acceptance with sparse transfer telemetry; all devices are doubles."""
import hashlib
from pathlib import Path
import tempfile
import unittest
import zlib

from test_console import IDS, TRANSFER_EVENTS, history
from test_workflow import FakeBackend, image
from deskhop_update.console import ProtocolError
from deskhop_update.workflow import Updater


class TransferHistoryWorkflowTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="deskhop-transfer-history-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        data, boot_crc = image(205)
        bin_path, uf2_path = self.directory / "candidate.bin", self.directory / "candidate.uf2"
        bin_path.write_bytes(data)
        uf2_path.write_bytes(b"offline UF2 fixture")
        self.candidate = {"build": "0.105", "encoded_version": 205, "boot_crc": boot_crc,
                          "slot_crc": f"{zlib.crc32(data):08x}", "image": data,
                          "bin_path": bin_path, "uf2_path": uf2_path,
                          "bin_sha256": hashlib.sha256(data).hexdigest(),
                          "uf2_sha256": hashlib.sha256(uf2_path.read_bytes()).hexdigest()}

    def updater(self, local, mode, events=TRANSFER_EVENTS):
        self.backend = backend = FakeBackend(self.candidate, local=local)

        def source_history(raw):
            count = int(raw.split(b"\r\n", 1)[0].split()[1])
            return history(int(backend.clock * 1_000_000), build=self.candidate["build"],
                           local=local, count=count, events=events)

        backend.response_mutators["history"] = source_history
        profile = {"target": local, "port": "/dev/cu.fixture", "uids": dict(IDS)}
        return Updater(self.candidate, profile, backend, self.directory,
                       verification_mode=mode, progress=lambda _value: None)

    def assert_profile_retained(self, result, mode):
        self.assertEqual(result["stage"], "complete")
        self.assertTrue(result["firmware_verified"])
        self.assertEqual(len(result["verifications"]), 1 if mode == "normal" else 3)
        self.assertEqual([entry["requested_per_board"] for entry in result["histories"]], [16, 64])
        for entry in result["histories"]:
            rows = [row for row in entry["rows"] if row.get("event", "").startswith("transfer_")]
            self.assertEqual(len(rows), len(TRANSFER_EVENTS))
            self.assertEqual(rows[-1]["metric"], "page_retries")

    def test_upgrade_profiles_do_not_break_either_verification_mode_or_board(self):
        for mode in ("normal", "thorough"):
            for local in ("A", "B"):
                with self.subTest(mode=mode, local=local):
                    result = self.updater(local, mode).run()
                    self.assert_profile_retained(result, mode)
                    self.assertTrue(result["picotool_verified"])
                    self.assertTrue(result["settings_unchanged"])
                    self.assertEqual(result["independent_readback"], mode == "thorough")

    def test_read_only_profiles_do_not_gain_write_or_acceptance_authority(self):
        for mode in ("normal", "thorough"):
            with self.subTest(mode=mode):
                updater = self.updater("A", mode)
                self.backend.running_candidate = True
                result = updater.verify_only()
                self.assert_profile_retained(result, mode)
                self.assertFalse(result["write_started"])
                self.assertFalse(result["reboot_requested"])
                self.assertEqual(result["input_acceptance"], "pending")
                self.assertFalse(any(call[0] in ("load", "save", "reboot", "bootloader")
                                     for call in self.backend.calls))

    def test_malformed_source_history_still_fails_closed_without_retry(self):
        for mode in ("normal", "thorough"):
            with self.subTest(mode=mode):
                updater = self.updater("A", mode,
                                       events=("transfer_source phase=batch_end mode=pages value=262140",))
                with self.assertRaises(ProtocolError):
                    updater.run()
                self.assertFalse(updater.record["firmware_verified"])
                self.assertEqual(updater.record["stage"], "failed")
                self.assertEqual(updater.record["failed_stage"], "verifying_both")
                self.assertEqual(sum(call[0] == "load" for call in self.backend.calls), 1)
                self.assertEqual(sum(call[0] == "reboot" for call in self.backend.calls), 1)


if __name__ == "__main__":
    unittest.main()
