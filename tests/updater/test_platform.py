"""Mocked macOS/PICOBOOT boundary checks; never contacts live hardware."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from deskhop_update import platform


UID = "1122334455667788"
PORT = "/dev/cu.usbmodem12345"
PICOTOOL = "/mock/bin/picotool"

# Structure copied from retained, successful v0.104 ioreg evidence; IDs are
# synthetic so this test is portable and contains no particular machine identity.
MEDIA = """+-o IOMediaBSDClient  <class IOMediaBSDClient, id 0x100000001, registered, matched, active, busy 0 (0 ms), retain 6>
    { "IOClass" = "IOMediaBSDClient" }
+-o AppleAPFSMediaBSDClient  <class AppleAPFSMediaBSDClient, id 0x100000002, registered, matched, active, busy 0 (0 ms), retain 7>
    { "IOClass" = "AppleAPFSMediaBSDClient" }
+-o AppleAPFSVolumeBSDClient  <class AppleAPFSVolumeBSDClient, id 0x100000003, registered, matched, active, busy 0 (0 ms), retain 7>
    { "IOClass" = "AppleAPFSVolumeBSDClient" }
"""
BOOT = """+-o RP2 Boot@02120000  <class IOUSBHostDevice, id 0x100000010, registered, matched, active, busy 0 (13 ms), retain 34>
  | {
  |   "USB Product Name" = "RP2 Boot"
  |   "sessionID" = 123456
  |   "locationID" = 34734080
  |   "USB Address" = 5
  |   "idVendor" = 11914
  |   "idProduct" = 3
  | }
  +-o AppleUSBHostCompositeDevice  <class AppleUSBHostCompositeDevice, id 0x100000011, !registered, !matched, active, busy 0, retain 4>
  |   { "bDeviceClass" = 0 }
  +-o IOUSBHostInterface@0  <class IOUSBHostInterface, id 0x100000012, registered, matched, active, busy 0 (4 ms), retain 7>
  |   {
  |     "bInterfaceClass" = 255
  |     "bInterfaceNumber" = 0
  |     "bNumEndpoints" = 2
  |   }
  +-o ExampleApp  <class AppleUSBHostDeviceUserClient, id 0x100000013, !registered, !matched, active, busy 0, retain 7>
      { "IOUserClientDefaultLocking" = Yes }
"""
IDENTITY = ("libusb: debug [libusb_open] open 2.5\n"
            f"Program Information\n name: deskhop\nDevice Information\n type: RP2040\n flash id: 0x{UID}\n")
PIN = {"registry_id": "0x100000010", "sessionID": 123456, "locationID": 34734080,
       "USB Address": 5, "idVendor": 11914, "idProduct": 3}
SELECTOR = ["--bus", "2", "--address", "5"]


class PlatformParserTests(unittest.TestCase):
    def test_media_accepts_retained_ioreg_shape_and_sorts_ids(self):
        self.assertEqual(platform.media_state(MEDIA), ["0x100000001", "0x100000002", "0x100000003"])
        self.assertEqual(platform.media_state("\n".join(reversed(MEDIA.splitlines()))),
                         platform.media_state(MEDIA))

    def test_media_empty_inactive_and_busy_are_rejected(self):
        for data in ("", "unrecognized format", MEDIA.replace("active,", "inactive,", 1),
                     MEDIA.replace("busy 0 ", "busy 1 ", 1)):
            with self.subTest(data=data[:50]), self.assertRaises(platform.DeploymentError):
                platform.media_state(data)

    def test_media_missing_or_duplicate_ids_fail_closed(self):
        for data in (MEDIA.replace("id 0x100000001", "missing identity"),
                     MEDIA.replace("0x100000002", "0x100000001")):
            with self.subTest(data=data[:100]), self.assertRaises(platform.DeploymentError):
                platform.media_state(data)

    def test_normal_mode_rejects_any_boot_device(self):
        platform.boot_state("", False)
        with self.assertRaises(platform.DeploymentError):
            platform.boot_state(BOOT, False)

    def test_boot_accepts_only_one_active_disk_free_device(self):
        platform.boot_state(BOOT, True)
        for data in ("", BOOT + BOOT, BOOT.replace("active,", "inactive,", 1),
                     BOOT.replace("busy 0 ", "busy 1 ", 1),
                     BOOT.replace('"bInterfaceClass" = 255', '"bInterfaceClass" = 8'),
                     BOOT + '"bInterfaceClass" = 8\n',
                     BOOT + '"bInterfaceClass" = 255\n',
                     BOOT.replace('"bInterfaceClass" = 255', "")):
            with self.subTest(data=data[:50]), self.assertRaises(platform.DeploymentError):
                platform.boot_state(data, True)

    def test_boot_busy_or_inactive_picoboot_interface_fails_closed(self):
        for state in ("inactive, busy 0", "active, busy 1"):
            data = BOOT.replace("matched, active, busy 0 (4 ms)", f"matched, {state} (4 ms)")
            with self.subTest(state=state), self.assertRaises(platform.DeploymentError):
                platform.boot_state(data, True)

    def test_usb_pin_reads_only_root_device_properties(self):
        self.assertEqual(platform.usb_pin(BOOT), PIN)
        child_properties = BOOT.replace('"bDeviceClass" = 0',
                                        '"bDeviceClass" = 0, "sessionID" = 999, "USB Address" = 99')
        self.assertEqual(platform.usb_pin(child_properties), PIN)

    def test_usb_pin_requires_single_well_formed_root_properties(self):
        for key, value in PIN.items():
            if key == "registry_id":
                continue
            field = f'"{key}" = {value}'
            for replacement in ("", field + "\n  | " + field,
                                f'"{key}" = garbage', f'"{key}" = -1',
                                f'"{key}" = {value}oops', f'"{key}" = {value}.5'):
                with self.subTest(key=key, replacement=replacement), \
                        self.assertRaises(platform.DeploymentError):
                    platform.usb_pin(BOOT.replace(field, replacement))

    def test_usb_pin_rejects_invalid_connection_identity_and_rom_state(self):
        invalid = [BOOT.replace("id 0x100000010", "no registry id"), BOOT + BOOT,
                   BOOT.replace('"bInterfaceClass" = 255', '"bInterfaceClass" = 8'),
                   BOOT + '"bInterfaceClass" = 255\n',
                   BOOT.replace('"idVendor" = 11914', '"idVendor" = 11915'),
                   BOOT.replace('"idProduct" = 3', '"idProduct" = 4')]
        for key in ("sessionID", "locationID", "USB Address"):
            invalid.append(BOOT.replace(f'"{key}" = {PIN[key]}', f'"{key}" = 0'))
        invalid.append(BOOT.replace('"USB Address" = 5', '"USB Address" = 128'))
        for data in invalid:
            with self.subTest(data=data[:160]), self.assertRaises(platform.DeploymentError):
                platform.usb_pin(data)

    def test_usb_pin_cannot_fill_missing_root_property_from_child(self):
        data = BOOT.replace('"sessionID" = 123456', "")
        data = data.replace('"bDeviceClass" = 0', '"bDeviceClass" = 0, "sessionID" = 123456')
        with self.assertRaises(platform.DeploymentError):
            platform.usb_pin(data)

    def test_usb_selector_requires_unique_matching_open_address(self):
        self.assertEqual(platform.usb_selector(IDENTITY, PIN), SELECTOR)
        self.assertEqual(platform.usb_selector(IDENTITY + IDENTITY, PIN), SELECTOR)
        for output in ("", IDENTITY.replace("open 2.5", "open 2.6"),
                       IDENTITY + "[libusb_open] open 3.5\n",
                       IDENTITY.replace("open 2.5", "open 0.5"),
                       IDENTITY.replace("open 2.5", "open 256.5"),
                       IDENTITY.replace("open 2.5", "open 2.5oops"),
                       IDENTITY.replace("open 2.5", "open 2.5.6")):
            with self.subTest(output=output[:70]), self.assertRaises(platform.DeploymentError):
                platform.usb_selector(output, PIN)


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="deskhop-platform-test-")
        self.addCleanup(self.temp.cleanup)
        self.evidence = Path(self.temp.name)
        self.platform_patch = patch.object(platform.sys, "platform", "darwin")
        self.which_patch = patch.object(platform.shutil, "which", return_value=PICOTOOL)
        self.platform_patch.start()
        self.which_patch.start()
        self.addCleanup(self.platform_patch.stop)
        self.addCleanup(self.which_patch.stop)
        self.backend = platform.MacBackend(self.evidence, PORT)

    def fake_subprocess(self, output=b"ok\n", returncode=0):
        def run(argv, *, stdout, stderr, timeout, env):
            self.assertIs(stderr, subprocess.STDOUT)
            self.assertGreater(timeout, 0)
            if argv[0] == PICOTOOL:
                self.assertEqual(env["LIBUSB_DEBUG"], "4")
            else:
                self.assertIsNone(env)
                stdout.write(BOOT.encode() if argv[0] == "ioreg" else output)
                return subprocess.CompletedProcess(argv, 0)
            stdout.write(output)
            return subprocess.CompletedProcess(argv, returncode)
        return run

    def bind(self):
        with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(IDENTITY.encode())):
            self.backend.identity(UID)

    @staticmethod
    def picotool_commands(run):
        return [call.args[0] for call in run.call_args_list if call.args[0][0] == PICOTOOL]

    def last_record(self, name):
        return [record for record in json.loads((self.evidence / "commands.json").read_text())
                if record["name"] == name][-1]

    def new_backend(self, name):
        evidence = self.evidence / name
        evidence.mkdir()
        return platform.MacBackend(evidence, PORT)

    def test_only_mac_explicit_callout_port_and_installed_picotool(self):
        for port in ("", "auto", "/dev/tty.usbmodem12345", "/dev/cu.a/b", "cu.usbmodem12345"):
            with self.subTest(port=port), self.assertRaises(platform.DeploymentError):
                platform.MacBackend(self.evidence, port)
        with patch.object(platform.sys, "platform", "linux"), self.assertRaises(platform.DeploymentError):
            platform.MacBackend(self.evidence, PORT)
        with patch.object(platform.shutil, "which", return_value=None), self.assertRaises(platform.DeploymentError):
            platform.MacBackend(self.evidence, PORT)

    def test_identity_uses_uid_filter_and_verifies_flash_identity(self):
        with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(IDENTITY.encode())) as run:
            self.backend.identity(UID)
        self.assertEqual(self.picotool_commands(run), [[PICOTOOL, "info", "-a", "--ser", UID]])
        self.assertEqual(self.backend.rom_session, {"uid": UID, "pin": PIN, "selector": SELECTOR})
        self.assertEqual(json.loads((self.evidence / "rom-session.json").read_text()), self.backend.rom_session)
        for index, output in enumerate((IDENTITY.replace(UID, "8877665544332211"),
                                        IDENTITY.replace("deskhop", "other"),
                                        IDENTITY + "[libusb_open] open 3.5\n")):
            backend = self.new_backend(f"bad-identity-{index}")
            with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(output.encode())):
                with self.assertRaises(platform.DeploymentError):
                    backend.identity(UID)
            self.assertIsNone(backend.rom_session)
            with patch.object(platform.subprocess, "run") as retry:
                with self.assertRaises(platform.DeploymentError):
                    backend.identity(UID)
            retry.assert_not_called()

    def test_load_and_reboot_use_bound_usb_selector_not_repeated_uid_lookup(self):
        self.bind()
        path = self.evidence / "candidate.uf2"
        with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess()) as run:
            self.backend.load(path, UID)
            self.backend.reboot(UID)
        commands = self.picotool_commands(run)
        self.assertEqual(commands, [[PICOTOOL, "load", "-v", str(path), *SELECTOR],
                                    [PICOTOOL, "reboot", "-a", *SELECTOR]])
        self.assertFalse(any("-u" in command for command in commands))
        self.assertIsNone(self.backend.rom_session)

    def test_save_uses_exact_range_and_usb_selector_and_will_not_overwrite(self):
        self.bind()
        def save(argv, *, stdout, stderr, timeout, env):
            if argv[0] != PICOTOOL:
                return self.fake_subprocess()(argv, stdout=stdout, stderr=stderr, timeout=timeout, env=env)
            self.assertEqual(argv[:5], [PICOTOOL, "save", "-r", "0x101ff000", "0x10200000"])
            self.assertEqual(argv[-4:], SELECTOR)
            self.assertEqual(env["LIBUSB_DEBUG"], "4")
            Path(argv[5]).write_bytes(b"\xFF" * 4096)
            return subprocess.CompletedProcess(argv, 0)
        with patch.object(platform.subprocess, "run", side_effect=save) as run:
            result = self.backend.save("settings-before", 0x101FF000, 0x10200000, UID)
            with self.assertRaisesRegex(platform.DeploymentError, "overwrite"):
                self.backend.save("settings-before", 0x101FF000, 0x10200000, UID)
        self.assertEqual(result, b"\xFF" * 4096)
        self.assertEqual(len(self.picotool_commands(run)), 1)
        self.assertIsNone(self.backend.rom_session)

    def test_short_read_aborts(self):
        self.bind()
        def short(argv, *, stdout, stderr, timeout, env):
            if argv[0] != PICOTOOL:
                return self.fake_subprocess()(argv, stdout=stdout, stderr=stderr, timeout=timeout, env=env)
            self.assertEqual(env["LIBUSB_DEBUG"], "4")
            Path(argv[5]).write_bytes(b"\xFF")
            return subprocess.CompletedProcess(argv, 0)
        with patch.object(platform.subprocess, "run", side_effect=short):
            with self.assertRaisesRegex(platform.DeploymentError, "short"):
                self.backend.save("settings-before", 0x101FF000, 0x10200000, UID)
        self.assertIsNone(self.backend.rom_session)

    def test_failure_is_journaled_without_retry(self):
        self.bind()
        with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(b"failed\n", 7)) as run:
            with self.assertRaisesRegex(platform.DeploymentError, "No automatic retry"):
                self.backend.reboot(UID)
        self.assertEqual(len(self.picotool_commands(run)), 1)
        record = self.last_record("reboot-application")
        self.assertEqual(record["returncode"], 7)
        self.assertIn("DeploymentError", record["error"])
        self.assertIn("seconds", record)
        self.assertEqual(record["environment_overrides"], {"LIBUSB_DEBUG": "4"})
        logs = list(self.evidence.glob("*-reboot-application.log"))
        self.assertEqual(len(logs), 1)
        self.assertEqual(logs[0].read_bytes(), b"failed\n")
        self.assertIsNone(self.backend.rom_session)

    def test_debug_environment_is_private_to_picotool_and_debug_output_is_retained(self):
        debug = "libusb: debug [libusb_get_device_list] enumerate USB devices\n"
        output = (debug + IDENTITY + "libusb: debug [libusb_exit] complete\n").encode()
        for inherited_debug in (None, "1"):
            with self.subTest(inherited_debug=inherited_debug), patch.dict(platform.os.environ):
                backend = self.new_backend(f"env-{inherited_debug}")
                platform.os.environ["DESKHOP_TEST_SECRET"] = "synthetic-secret-not-for-journaling"
                if inherited_debug is None:
                    platform.os.environ.pop("LIBUSB_DEBUG", None)
                else:
                    platform.os.environ["LIBUSB_DEBUG"] = inherited_debug
                parent_environment = dict(platform.os.environ)
                with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(output)) as run:
                    backend.identity(UID)
                identity_call = [call for call in run.call_args_list if call.args[0][0] == PICOTOOL][0]
                environment = identity_call.kwargs["env"]
                self.assertIsNot(environment, platform.os.environ)
                self.assertEqual(environment, dict(parent_environment, LIBUSB_DEBUG="4"))
                self.assertEqual(dict(platform.os.environ), parent_environment)
                journal_text = (backend.evidence / "commands.json").read_text()
                record = [entry for entry in json.loads(journal_text) if entry["name"] == "identity"][0]
                self.assertEqual(record["environment_overrides"], {"LIBUSB_DEBUG": "4"})
                self.assertNotIn("DESKHOP_TEST_SECRET", journal_text)
                self.assertNotIn("synthetic-secret-not-for-journaling", journal_text)
                logs = list(backend.evidence.glob("*-identity.log"))
                self.assertEqual(len(logs), 1)
                self.assertEqual(logs[0].read_bytes(), output)

    def test_non_picotool_commands_do_not_override_or_journal_environment(self):
        with patch.dict(platform.os.environ, {"LIBUSB_DEBUG": "2"}):
            for executable in ("ioreg", "/different/path/picotool"):
                with self.subTest(executable=executable), \
                        patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess()) as run:
                    self.backend.run("other", [executable, "--fixture"])
                self.assertIsNone(run.call_args.kwargs["env"])
                self.assertEqual(platform.os.environ["LIBUSB_DEBUG"], "2")
                record = json.loads((self.evidence / "commands.json").read_text())[-1]
                self.assertNotIn("environment_overrides", record)

    def test_timeout_is_journaled_without_retry(self):
        self.bind()
        def timeout(argv, **kwargs):
            if argv[0] == PICOTOOL:
                raise subprocess.TimeoutExpired("picotool", 30)
            return self.fake_subprocess()(argv, **kwargs)
        with patch.object(platform.subprocess, "run", side_effect=timeout) as run:
            with self.assertRaises(subprocess.TimeoutExpired):
                self.backend.load(self.evidence / "candidate.uf2", UID)
        self.assertEqual(len(self.picotool_commands(run)), 1)
        record = self.last_record("load-verify")
        self.assertIn("TimeoutExpired", record["error"])
        self.assertNotIn("returncode", record)
        self.assertEqual(record["environment_overrides"], {"LIBUSB_DEBUG": "4"})
        self.assertIsNone(self.backend.rom_session)

    def test_command_log_is_not_overwritten(self):
        (self.evidence / "001-identity.log").write_text("prior evidence")
        with patch.object(platform.subprocess, "run") as run:
            with self.assertRaises(FileExistsError):
                self.backend.run("identity", [PICOTOOL, "info", "-a", "--ser", UID])
        run.assert_not_called()
        self.assertEqual((self.evidence / "001-identity.log").read_text(), "prior evidence")

    def test_excessive_output_is_not_read_into_memory(self):
        with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(b"x" * (4 * 1024 * 1024 + 1))):
            with self.assertRaisesRegex(platform.DeploymentError, "Excessive output"):
                self.backend.identity(UID)
        self.assertIsNone(self.backend.rom_session)

    def test_unbound_operations_never_open_usb(self):
        with patch.object(platform.subprocess, "run") as run:
            for operation in (lambda: self.backend.save("unbound", 0, 4, UID),
                              lambda: self.backend.load(self.evidence / "candidate.uf2", UID),
                              lambda: self.backend.reboot(UID)):
                with self.assertRaises(platform.DeploymentError):
                    operation()
        run.assert_not_called()

    def test_bound_uid_mismatch_rejects_operation_and_consumes_session(self):
        for name in ("save", "load", "reboot"):
            with self.subTest(operation=name):
                backend = self.new_backend(f"wrong-uid-{name}")
                with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(IDENTITY.encode())):
                    backend.identity(UID)
                with patch.object(platform.subprocess, "run") as run:
                    wrong = "8877665544332211"
                    with self.assertRaises(platform.DeploymentError):
                        if name == "save":
                            backend.save("wrong-uid", 0, 4, wrong)
                        elif name == "load":
                            backend.load(self.evidence / "candidate.uf2", wrong)
                        else:
                            backend.reboot(wrong)
                run.assert_not_called()
                self.assertIsNone(backend.rom_session)

    def test_changed_pin_during_identity_cannot_bind_or_retry(self):
        for field, value in PIN.items():
            changed = (BOOT.replace(value, "0x100000099") if field == "registry_id" else
                       BOOT.replace(f'"{field}" = {value}', f'"{field}" = {value + 1}'))
            backend = self.new_backend(f"identity-change-{field.replace(' ', '-')}")
            with self.subTest(field=field), patch.object(backend, "run", side_effect=[BOOT, IDENTITY, changed]):
                with self.assertRaises(platform.DeploymentError):
                    backend.identity(UID)
            self.assertIsNone(backend.rom_session)
            with patch.object(backend, "run") as retry:
                with self.assertRaises(platform.DeploymentError):
                    backend.identity(UID)
            retry.assert_not_called()

    def test_repeated_identity_is_refused_without_second_picotool_call(self):
        self.bind()
        with patch.object(platform.subprocess, "run") as run:
            with self.assertRaises(platform.DeploymentError):
                self.backend.identity(UID)
        run.assert_not_called()
        self.assertIsNone(self.backend.rom_session)

    def test_changed_or_missing_session_blocks_each_operation_before_usb(self):
        changes = ["", BOOT + BOOT,
                   BOOT.replace("0x100000010", "0x100000099"),
                   BOOT.replace('"sessionID" = 123456', '"sessionID" = 123457'),
                   BOOT.replace('"locationID" = 34734080', '"locationID" = 34734081'),
                   BOOT.replace('"USB Address" = 5', '"USB Address" = 6')]
        for operation in ("save", "load", "reboot"):
            for index, changed in enumerate(changes):
                with self.subTest(operation=operation, change=index):
                    backend = self.new_backend(f"guard-{operation}-{index}")
                    with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(IDENTITY.encode())):
                        backend.identity(UID)
                    with patch.object(backend, "run", return_value=changed) as run:
                        with self.assertRaises(platform.DeploymentError):
                            if operation == "save":
                                backend.save("read", 0, 4, UID)
                            elif operation == "load":
                                backend.load(self.evidence / "candidate.uf2", UID)
                            else:
                                backend.reboot(UID)
                    self.assertTrue(run.called)
                    self.assertFalse(any(call.args[1][0] == PICOTOOL for call in run.call_args_list))
                    self.assertIsNone(backend.rom_session)
                    with patch.object(backend, "run") as retry:
                        with self.assertRaises(platform.DeploymentError):
                            backend.reboot(UID)
                        with self.assertRaises(platform.DeploymentError):
                            backend.identity(UID)
                    retry.assert_not_called()

    def test_post_operation_pin_change_invalidates_read_and_write(self):
        for operation in ("save", "load"):
            with self.subTest(operation=operation):
                backend = self.new_backend(f"post-{operation}")
                with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(IDENTITY.encode())):
                    backend.identity(UID)
                observations = iter((BOOT, BOOT.replace('"sessionID" = 123456', '"sessionID" = 123457')))
                def run(name, argv, timeout=20):
                    if argv[0] == "ioreg":
                        return next(observations)
                    if operation == "save":
                        Path(argv[5]).write_bytes(b"read")
                    return "ok"
                with patch.object(backend, "run", side_effect=run) as calls:
                    with self.assertRaises(platform.DeploymentError):
                        if operation == "save":
                            backend.save("read", 0, 4, UID)
                        else:
                            backend.load(self.evidence / "candidate.uf2", UID)
                self.assertEqual(sum(call.args[1][0] == PICOTOOL for call in calls.call_args_list), 1)
                self.assertIsNone(backend.rom_session)

    def test_reboot_consumes_session_even_if_command_fails(self):
        for status in (0, 7):
            with self.subTest(status=status):
                backend = self.new_backend(f"reboot-{status}")
                with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(IDENTITY.encode())):
                    backend.identity(UID)
                with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(returncode=status)) as run:
                    if status:
                        with self.assertRaises(platform.DeploymentError):
                            backend.reboot(UID)
                    else:
                        backend.reboot(UID)
                self.assertEqual(len(self.picotool_commands(run)), 1)
                self.assertIsNone(backend.rom_session)
                with patch.object(platform.subprocess, "run") as retry:
                    with self.assertRaises(platform.DeploymentError):
                        backend.reboot(UID)
                    with self.assertRaises(platform.DeploymentError):
                        backend.identity(UID)
                retry.assert_not_called()

    def test_identity_command_failure_cannot_be_retried(self):
        with patch.object(platform.subprocess, "run", side_effect=self.fake_subprocess(returncode=7)) as run:
            with self.assertRaises(platform.DeploymentError):
                self.backend.identity(UID)
        self.assertEqual(len(self.picotool_commands(run)), 1)
        self.assertIsNone(self.backend.rom_session)
        with patch.object(platform.subprocess, "run") as retry:
            with self.assertRaises(platform.DeploymentError):
                self.backend.identity(UID)
            with self.assertRaises(platform.DeploymentError):
                self.backend.load(self.evidence / "candidate.uf2", UID)
        retry.assert_not_called()

    def test_health_requires_same_media_identities(self):
        with patch.object(self.backend, "run", side_effect=[MEDIA, BOOT]):
            self.assertEqual(self.backend.health(boot=True), platform.media_state(MEDIA))
        with patch.object(self.backend, "run", side_effect=[MEDIA, ""]):
            with self.assertRaisesRegex(platform.DeploymentError, "changed"):
                self.backend.health(baseline=["0xdeadbeef"])

    def test_wait_bootloader_checks_health_then_uid(self):
        order = []
        with patch.object(self.backend, "run", side_effect=["", BOOT]), \
                patch.object(self.backend, "health", side_effect=lambda **kw: order.append(("health", kw))), \
                patch.object(self.backend, "identity", side_effect=lambda uid: order.append(("identity", uid))), \
                patch.object(platform.time, "sleep"):
            self.backend.wait_bootloader(UID, ["0x100000001"])
        self.assertEqual(order, [("health", {"boot": True, "baseline": ["0x100000001"]}), ("identity", UID)])

    def test_bootloader_timeout_is_bounded_and_does_not_select_another_device(self):
        with patch.object(self.backend, "run", return_value=""), \
                patch.object(self.backend, "identity") as identity, \
                patch.object(platform.time, "monotonic", side_effect=[0, 9]), \
                patch.object(platform.time, "sleep") as sleep:
            with self.assertRaisesRegex(platform.DeploymentError, "enumeration unconfirmed"):
                self.backend.wait_bootloader(UID, [], timeout=8)
        identity.assert_not_called()
        sleep.assert_not_called()

    def test_wait_port_only_checks_explicit_path(self):
        with patch.object(platform.Path, "exists", autospec=True, return_value=False) as exists, \
                patch.object(platform.time, "monotonic", side_effect=[0, 9]), \
                patch.object(platform.time, "sleep"):
            with self.assertRaisesRegex(platform.DeploymentError, "no other port"):
                self.backend.wait_port(timeout=8)
        self.assertEqual(exists.call_args.args[0], Path(PORT))

    def test_console_receives_only_explicit_port_and_run_scoped_transcript(self):
        with patch.object(platform, "Console") as console:
            self.backend.console(total_timeout=42)
        self.assertEqual(console.call_args.args, (PORT,))
        self.assertEqual(console.call_args.kwargs["total_timeout"], 42)
        console.call_args.kwargs["transcript"]({"command": "status"})
        self.assertEqual(json.loads((self.evidence / "serial-1.jsonl").read_text()), {"command": "status"})

    def test_deployment_lock_excludes_a_second_writer_and_releases_on_error(self):
        with platform.deployment_lock(self.evidence):
            with self.assertRaisesRegex(platform.DeploymentError, "Another updater"):
                with platform.deployment_lock(self.evidence):
                    self.fail("second writer acquired deployment lock")
        with self.assertRaisesRegex(RuntimeError, "test"):
            with platform.deployment_lock(self.evidence):
                raise RuntimeError("test")
        with platform.deployment_lock(self.evidence):
            pass


if __name__ == "__main__":
    unittest.main()
