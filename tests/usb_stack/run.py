#!/usr/bin/env python3
"""Compile real TinyUSB device/control/HID/MSC/CDC stacks against a virtual DCD."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
TINYUSB = ROOT / "pico-sdk/lib/tinyusb/src"
APP_SOURCES = [ROOT / "src/usb_descriptors.c", ROOT / "src/usb.c", ROOT / "src/console.c", ROOT / "src/history.c"]
STACK_SOURCES = [TINYUSB / source for source in ["tusb.c", "common/tusb_fifo.c", "device/usbd.c", "device/usbd_control.c", "class/hid/hid_device.c", "class/msc/msc_device.c", "class/cdc/cdc_device.c"]]
SOURCES = [ROOT / "tests/usb_stack/test_usb_stack.c", *APP_SOURCES, *STACK_SOURCES]


def build(directory, sanitize=True, coverage=False):
    binary = Path(directory) / "usb-stack-test"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections",
               "-DDH_CONSOLE=1", "-DVERSION_MAJOR=0", "-DVERSION_MINOR=98",
               "-include", str(ROOT / "tests/usb_stack/native_options.h"),
               "-g", "-O1",
               f"-I{ROOT / 'tests/usb_stack'}", f"-I{TINYUSB}", f"-I{ROOT / 'tests/storage'}", f"-I{ROOT / 'src/include'}",
               str(ROOT / "tests/usb_stack/test_usb_stack.c")]
    command += list(map(str, APP_SOURCES))
    command += list(map(str, STACK_SOURCES))
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    if coverage:
        command += ["-fprofile-instr-generate", "-fcoverage-mapping"]
    command += ["-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    return binary


def main():
    with tempfile.TemporaryDirectory(prefix="deskhop-usb-stack-") as directory:
        subprocess.run([str(build(directory))], check=True, timeout=30)


if __name__ == "__main__":
    main()
