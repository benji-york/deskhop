#!/usr/bin/env python3
"""Require representative broken production variants to fail the storage suite.

Mutants live only in a temporary source directory. A build error never counts
as a killed mutant: each variant must compile, then fail a runtime oracle.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

from run import ROOT, SOURCES, build

# Each change is intentionally small and directly tied to a production invariant.
MUTATIONS = [
    ("config-set-without-snapshot-lock", "handlers.c",
     "        config_lock();\n        memcpy(ptr, &packet->data[1], map->len);\n        config_unlock();",
     "        memcpy(ptr, &packet->data[1], map->len);"),
    ("duplicate-uf2-program", "ramdisk.c",
     "if (fw_update_mark_block(global_state.uf2_blocks_received,\n"
     "                             &global_state.uf2_blocks_received_count,\n"
     "                             uf2->blockNo)) {",
     "if (true) {\n        global_state.uf2_blocks_received_count++;"),
    ("early-uf2-completion", "ramdisk.c",
     "global_state.uf2_blocks_received_count == EXPECTED_BLOCK_COUNT",
     "global_state.uf2_blocks_received_count == EXPECTED_BLOCK_COUNT - 1"),
    ("skip-independent-uf2-checksum", "ramdisk.c",
     "if (!firmware_image_is_valid(0, 0, false))", "if (false)"),
    ("allow-uf2-after-reboot-reservation", "ramdisk.c",
     "if (global_state.reboot_requested)", "if (false)"),
    ("consume-word-on-tx-full", "utils.c",
     "if (!queue_try_add(&state->uart_tx_queue, &packet))\n        return false;",
     "if (!queue_try_add(&state->uart_tx_queue, &packet))\n        state->fw.byte_done = false;"),
    ("rewrite-page-zero-before-response", "tasks.c",
     "state->fw.address != 0 && TU_U32_BYTE0(state->fw.address) == 0x00",
     "TU_U32_BYTE0(state->fw.address) == 0x00"),
    ("omit-final-page", "tasks.c",
     "state->fw.address != 0 && TU_U32_BYTE0(state->fw.address) == 0x00",
     "state->fw.address != 0 && state->fw.address < STAGING_IMAGE_SIZE && TU_U32_BYTE0(state->fw.address) == 0x00"),
    ("accept-stale-response", "fw_update.c",
     "&& response_address == expected_address", "&& true"),
    ("config-save-during-update", "utils.c",
     "if (state->fw.upgrade_in_progress)", "if (false)"),
    ("omit-cross-core-flash-lock", "utils.c",
     "critical_section_enter_blocking(&flash_access_critical_section);",
     "/* mutant: omit flash exclusion */"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", default="1")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="deskhop-storage-mutations-") as directory:
        base = Path(directory)
        baseline = base / "baseline"
        baseline.mkdir()
        binary = build(baseline)
        subprocess.run([str(binary), args.seed], check=True, capture_output=True)
        for name, filename, old, new in MUTATIONS:
            path = base / name
            path.mkdir()
            for source in SOURCES:
                shutil.copy2(ROOT / "src" / source, path / source)
            target = path / filename
            text = target.read_text()
            if old not in text:
                raise RuntimeError(f"Mutation anchor drifted: {name}")
            # Replacing every flash-lock acquisition also catches readers via
            # balanced-release checks; all other anchors are unique.
            target.write_text(text.replace(old, new))
            binary = build(path, source_root=path)
            result = subprocess.run([str(binary), args.seed], capture_output=True, text=True)
            if result.returncode == 0:
                raise RuntimeError(f"SURVIVED: {name}")
            details = result.stderr.splitlines()
            detail = next((line for line in details if "storage failure:" in line), details[0] if details else "runtime failure")
            print(f"KILLED {name}: {detail}")
    print(f"storage mutation check: {len(MUTATIONS)}/{len(MUTATIONS)} runtime failures detected")


if __name__ == "__main__":
    main()
