#!/usr/bin/env python3
"""Build actual firmware storage units; run deterministic NOR/queue scenarios."""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ["utils.c", "ramdisk.c", "tasks.c", "handlers.c", "fw_update.c",
           "config_migration.c", "constants.c", "defaults.c", "protocol.c", "selection.c",
           "diagnostic_runtime.c", "diagnostic_history.c", "history.c"]


def build(directory, source_root=ROOT / "src", sanitize=True, coverage=False):
    binary = Path(directory) / "storage-test"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-pointer-to-int-cast",
               "-ffunction-sections", "-fdata-sections", "-g", "-O1"]
    if coverage:
        command += ["-fprofile-instr-generate", "-fcoverage-mapping"]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    command += [f"-I{ROOT / 'tests/storage'}", f"-I{ROOT / 'tests/hid_stubs'}",
                f"-I{ROOT / 'src/include'}", f"-I{ROOT / 'pico-sdk/lib/tinyusb/src'}",
                str(ROOT / "tests/storage/test_storage.c")]
    command += [str(source_root / file) for file in SOURCES]
    command += ["-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections",
                "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=lambda x: int(x, 0), default=1)
    parser.add_argument("--seeds", type=int, default=1)
    parser.add_argument("--no-sanitize", action="store_true")
    parser.add_argument("--trace", type=Path, help="JSONL event output (single seed only)")
    args = parser.parse_args()
    if args.trace and args.seeds != 1:
        parser.error("--trace requires --seeds 1")
    environment = dict(os.environ)
    if args.trace:
        environment["DESKHOP_STORAGE_TRACE"] = str(args.trace.resolve())
    with tempfile.TemporaryDirectory(prefix="deskhop-storage-") as directory:
        binary = build(directory, sanitize=not args.no_sanitize)
        for seed in range(args.seed, args.seed + args.seeds):
            subprocess.run([str(binary), str(seed)], check=True, env=environment)


if __name__ == "__main__":
    main()
