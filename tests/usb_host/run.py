#!/usr/bin/env python3
"""Drive the real TinyUSB host stack with deterministic virtual USB peripherals."""
import importlib.util
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TINYUSB = ROOT / "pico-sdk/lib/tinyusb/src"
SOURCES = [ROOT / "tests/usb_host/test_usb_host.c"]
SOURCES += [ROOT / "src" / source for source in [
    "usb.c", "hid_parser.c", "hid_report.c", "keyboard.c", "reboot_hotkey.c"]]
SOURCES += [TINYUSB / source for source in [
    "tusb.c", "common/tusb_fifo.c", "host/usbh.c", "host/hub.c", "class/hid/hid_host.c"]]


def build(directory, sanitize=True, coverage=False):
    directory = Path(directory)
    spec = importlib.util.spec_from_file_location("fixtures", ROOT / "tests/sim/fixtures.py")
    fixtures = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixtures)
    (directory / "fixtures.h").write_text("\n".join(
        f"static const uint8_t fixture_{name}[] = {{" + ",".join(str(x) for x in value) + "};"
        for name, value in [("keyboard", fixtures.KEYBOARD), ("mouse", fixtures.MOUSE)]))
    binary = directory / "usb-host-test"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-deprecated-non-prototype", "-Wno-sign-compare",
               "-ffunction-sections", "-fdata-sections", "-g", "-O1",
               "-include", str(ROOT / "tests/usb_stack/native_options.h"),
               f"-I{ROOT / 'tests/usb_host'}", f"-I{TINYUSB}", f"-I{ROOT / 'tests/storage'}",
               f"-I{ROOT / 'src/include'}", f"-I{directory}"]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    if coverage:
        command += ["-fprofile-instr-generate", "-fcoverage-mapping"]
    command += [str(source) for source in SOURCES]
    command += ["-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    return binary


def main():
    with tempfile.TemporaryDirectory(prefix="deskhop-usb-host-") as directory:
        subprocess.run([str(build(directory))], check=True)


if __name__ == "__main__":
    main()
