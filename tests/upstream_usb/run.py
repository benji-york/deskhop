#!/usr/bin/env python3
"""Execute vendored USB transfer/copy functions with deterministic bus responses."""
import argparse
import os
import platform
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PIO = ROOT / "Pico-PIO-USB/src"
RP = ROOT / "pico-sdk/lib/tinyusb/src/portable/raspberrypi/rp2040"


def extract(path, marker, suffix=""):
    """Preserve a real definition verbatim; do not duplicate its logic in the test."""
    source = path.read_text()
    if source.count(marker) != 1:
        raise ValueError(f"{path}: expected exactly one {marker!r}")
    start = source.index(marker)
    brace = source.index("{", start)
    # Ignore braces in comments/literals while retaining original byte offsets.
    clean = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                   lambda match: " " * len(match[0]), source, flags=re.S)
    depth = 1
    end = brace + 1
    while depth:
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    if suffix:
        if not source.startswith(suffix, end):
            raise ValueError(f"{path}: unexpected definition suffix")
        end += len(suffix)
    line = source.count("\n", 0, start) + 1
    return f'#line {line} "{path}"\n' + source[start:end] + "\n"


def generated_headers(directory):
    definitions = [extract(PIO / "pio_usb_host.c", "enum {\n  TRANSACTION_MAX_RETRY", ";")]
    definitions += [extract(PIO / "pio_usb_ll.h", "static inline __force_inline uint16_t\npio_usb_ll_get_transaction_len")]
    definitions += [extract(PIO / "pio_usb.c", marker) for marker in [
        "static inline __force_inline void prepare_tx_data",
        "bool __no_inline_not_in_flash_func(pio_usb_ll_transfer_start)",
        "bool __no_inline_not_in_flash_func(pio_usb_ll_transfer_continue)",
        "void __no_inline_not_in_flash_func(pio_usb_ll_transfer_complete)",
    ]]
    definitions += [extract(PIO / "pio_usb_host.c", f"static int __no_inline_not_in_flash_func({name})")
                    for name in ["usb_in_transaction", "usb_out_transaction", "usb_setup_transaction"]]
    (directory / "pio_production.h").write_text("\n".join(definitions))
    definitions = [extract(RP / "rp2040_usb.h", "typedef struct hw_endpoint", " hw_endpoint_t;")]
    definitions += [extract(RP / "rp2040_usb.c", marker) for marker in [
        "static void unaligned_memcpy",
        "static uint32_t __tusb_irq_path_func(prepare_ep_buffer)",
        "static uint16_t __tusb_irq_path_func(sync_ep_buffer)",
    ]]
    (directory / "dpram_production.h").write_text("\n".join(definitions))
    (directory / "dpram_reset_production.h").write_text(extract(
        RP / "dcd_rp2040.c", "static void __tusb_irq_path_func(reset_non_control_endpoints)"))


def check_arm_byte_access(directory):
    compiler = os.environ.get("ARM_CC") or shutil.which("arm-none-eabi-gcc")
    bundled = Path("/opt/homebrew/opt/arm-gcc-bin@14/bin/arm-none-eabi-gcc")
    if compiler is None and bundled.is_file():
        compiler = str(bundled)
    if compiler is None:
        print("DPRAM ARM byte-access check skipped: set ARM_CC to arm-none-eabi-gcc")
        return
    source = directory / "copy_probe.c"
    source.write_text("#include <stddef.h>\n#include <stdint.h>\n" +
                      extract(RP / "rp2040_usb.c", "static void unaligned_memcpy") +
                      "void usb_copy_probe(uint8_t *d, const uint8_t *s, size_t n) {\n"
                      "  unaligned_memcpy(d, s, n);\n}\n")
    assembly = subprocess.check_output([*shlex.split(compiler), "-std=c11", "-O3",
                                       "-mcpu=cortex-m0plus", "-mthumb", "-ffreestanding",
                                       "-S", str(source), "-o", "-"], text=True)
    memory_ops = re.findall(r"^\s+(ldr\w*|str\w*|ldm\w*|stm\w*)\s", assembly, re.M)
    assert "ldrb" in memory_ops and "strb" in memory_ops, memory_ops
    assert set(memory_ops) <= {"ldrb", "strb"}, memory_ops
    print("DPRAM optimized Cortex-M0+ copy uses byte loads/stores only")


def build(directory):
    binary = directory / "upstream-usb-test"
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", f"-I{PIO}", f"-I{directory}",
                    str(HERE / "test_upstream_usb.c"), "-o", str(binary)], check=True)
    return binary


def check_cdc_wipe(directory):
    tinyusb = ROOT / "pico-sdk/lib/tinyusb/src"
    binary = directory / "cdc-wipe-test"
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-ffunction-sections", "-fdata-sections",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    f"-I{ROOT / 'tests/usb_stack'}", f"-I{tinyusb}",
                    str(HERE / "test_cdc_wipe.c"), str(tinyusb / "common/tusb_fifo.c"),
                    "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections",
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def check_mutations(directory):
    # Mutate only ephemeral extracted definitions, never the checkout.
    cases = [
        ("fail first transaction", "pio_production.h", "TRANSACTION_MAX_RETRY = 3", "TRANSACTION_MAX_RETRY = 1"),
        ("stale count on new transfer", "pio_production.h", "ep->failed_count = 0;", "ep->failed_count += 0;"),
        ("missing IN success reset", "pio_production.h", "ep->failed_count = 0; // a sound", "ep->failed_count += 0; // a sound"),
        ("SETUP retry becomes OUT", "pio_production.h", "ep->data_id = USB_PID_SETUP;", "ep->data_id = 0;"),
        ("failed SETUP counts bytes", "pio_production.h", "res = -1;\n    // data_id", "res = -1;\n    ep->actual_len = 8;\n    // data_id"),
        ("wrong DPRAM buffer", "dpram_production.h", "ep->hw_data_buf + buf_id * 64, ep->user_buf", "ep->hw_data_buf, ep->user_buf"),
        ("missing DPRAM receive wipe", "dpram_production.h", "consumed[i] = 0;", "consumed[i] = consumed[i];"),
        ("missing DPRAM reset wipe", "dpram_reset_production.h", "abandoned[i] = 0;", "abandoned[i] = abandoned[i];"),
    ]
    for name, header, before, after in cases:
        generated_headers(directory)
        path = directory / header
        source = path.read_text()
        assert before in source, name
        path.write_text(source.replace(before, after, 1))
        result = subprocess.run([str(build(directory))], capture_output=True, text=True)
        assert result.returncode == -signal.SIGABRT, (name, result.returncode, result.stderr)
        assert "assert" in result.stderr.lower(), (name, result.stderr)
        print(f"Rejected mutation: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutations", action="store_true", help="also check eight real-body regression mutations")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="deskhop-upstream-usb-") as tmp:
        directory = Path(tmp)
        generated_headers(directory)
        binary = build(directory)
        subprocess.run([str(binary)], check=True)
        check_arm_byte_access(directory)
        check_cdc_wipe(directory)
        if args.mutations:
            check_mutations(directory)


if __name__ == "__main__":
    main()
