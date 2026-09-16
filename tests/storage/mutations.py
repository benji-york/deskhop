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

from run import ROOT, SOURCES, build, build_saturation

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
     "if (global_state.reboot_requested || global_state.maintenance_reserved)",
     "if (global_state.maintenance_reserved)"),
    ("consume-word-on-tx-full", "utils.c",
     "if (!queue_try_add(&state->uart_tx_queue, &packet))\n        return false;",
     "if (!queue_try_add(&state->uart_tx_queue, &packet))\n        state->fw.byte_done = false;"),
    ("rewrite-page-zero-before-response", "tasks.c",
     "if (!state->fw.page_pending)\n        return;",
     "if (!state->fw.page_pending && state->fw.address != 0)\n        return;"),
    ("omit-final-page", "tasks.c",
     "if (!state->fw.page_pending)\n        return;",
     "if (!state->fw.page_pending || state->fw.address >= STAGING_IMAGE_SIZE)\n        return;"),
    ("batch-prefetch-before-commit", "tasks.c",
     "    if (state->batch.mode == FW_BATCH_PAGES)\n        commit_pending_firmware_page(state);",
     "    /* mutant: rely on the later commit, after admitting the next burst */"),
    ("bypass-batch-page-crc", "fw_batch.c",
     "if (fw_batch_page_crc(state->page) != state->checksum)", "if (false)"),
    ("accept-stale-batch-tag", "fw_batch.c",
     "    uint32_t id = read32(in);\n    if (kind == FW_BATCH_DATA)",
     "    uint32_t id = read32(in);\n"
     "    state->tag = id & FW_BATCH_TAG_MASK; /* mutant: accept unsolicited generation */\n"
     "    if (kind == FW_BATCH_DATA)"),
    ("ignore-conflicting-batch-word", "fw_batch.c",
     "if (memcmp(state->page + 4 * word, in + 4, 4) != 0)", "if (false)"),
    ("fallback-synthesizes-page-completion", "firmware_batch.c",
     "    state->batch.mode = FW_BATCH_LEGACY;",
     "    state->batch.mode = FW_BATCH_LEGACY;\n    state->fw.byte_done = true;"),
    ("accept-stale-response", "fw_update.c",
     "&& response_address == expected_address", "&& true"),
    ("config-save-during-update", "utils.c",
     "if (state->fw.upgrade_in_progress)", "if (false)"),
    ("omit-cross-core-flash-lock", "utils.c",
     "critical_section_enter_blocking(&flash_access_critical_section);",
     "/* mutant: omit flash exclusion */"),
    ("omit-peer-progress-observation", "handlers.c",
     "diagnostic_update_progress(state->fw.address);",
     "(void)state->fw.address;"),
    ("omit-usb-progress-observation", "ramdisk.c",
     "diagnostic_update_progress(global_state.uf2_blocks_received_count * FLASH_PAGE_SIZE);",
     "(void)global_state.uf2_blocks_received_count;"),
    ("omit-peer-failure-before-reset", "tasks.c",
     "diagnostic_update_phase(DIAGNOSTIC_UPDATE_FAILED);",
     "/* mutant: failure is invisible before ROM reset */"),
    ("omit-console-disabled-checkpoint", "tasks.c",
     "    diagnostic_runtime_checkpoint(0);\n#if DH_CONSOLE && CFG_TUD_CDC",
     "#if DH_CONSOLE && CFG_TUD_CDC\n    diagnostic_runtime_checkpoint(0);"),
    ("omit-verification-program-generation", "utils.c",
     "firmware_verify_invalidate_range(target_addr, FLASH_PAGE_SIZE);",
     "/* mutant: programming leaves verification generation unchanged */"),
    ("omit-verification-recovery-generation", "utils.c",
     "firmware_verify_invalidate_range((uint32_t)ADDR_FW_RUNNING - XIP_BASE, FLASH_SECTOR_SIZE);",
     "/* mutant: recovery leaves verification generation unchanged */"),
    ("exclude-metadata-from-verification-generation", "utils.c",
     "(uint64_t)slot_start + STAGING_IMAGE_SIZE",
     "(uint64_t)slot_start + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE"),
    ("verify-during-update-or-reboot", "utils.c",
     "global_state.fw.upgrade_in_progress || global_state.fw.image_dirty\n        || global_state.reboot_requested",
     "false"),
    ("omit-verification-generation-check", "utils.c",
     "(compare && generation != firmware_flash_generation)", "false"),
    ("wrap-verification-generation", "utils.c",
     "&& firmware_flash_generation != UINT64_MAX", "&& true"),
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
            if name == "wrap-verification-generation":
                binary = build_saturation(path / "saturation", source_root=path)
                command = [str(binary), args.seed, "generation-saturation"]
            else:
                binary = build(path, source_root=path)
                command = [str(binary), args.seed]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode == 0:
                raise RuntimeError(f"SURVIVED: {name}")
            details = result.stderr.splitlines()
            detail = next((line for line in details if "storage failure:" in line), details[0] if details else "runtime failure")
            print(f"KILLED {name}: {detail}")
    print(f"storage mutation check: {len(MUTATIONS)}/{len(MUTATIONS)} runtime failures detected")


if __name__ == "__main__":
    main()
