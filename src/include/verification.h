/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "diagnostic_runtime.h"

#define VERIFY_IMAGE_BYTES UINT32_C(262144)
#define VERIFY_CHUNK_BYTES 256u
#define VERIFY_TIMEOUT_US UINT64_C(3000000)
#define VERIFY_CORE_MAX_AGE_MS 100u

typedef enum {
    VERIFY_SCAN_COMPLETE = 0,
    VERIFY_SCAN_UPDATE_ACTIVE,
    VERIFY_SCAN_CHANGED,
    VERIFY_SCAN_BUSY,
    VERIFY_SCAN_TIMEOUT,
} verify_scan_outcome_t;

/* Non-COMPLETE snapshots can contain partial evidence; generation_end is
 * meaningful only for a successfully completed stable scan. */
typedef struct {
    uint8_t role;
    uint16_t major, minor;
    uint8_t board_id[8];
    uint64_t boot_session;
    uint32_t image_crc_at_boot;
    uint64_t started_us, completed_us;
    uint64_t generation_start, generation_end;
    uint32_t bytes_read, slot_crc32;
    uint32_t metadata_magic;
    uint16_t metadata_version, metadata_reserved;
    uint32_t metadata_crc32;
    diagnostic_runtime_snapshot_t start, end;
    verify_scan_outcome_t outcome;
} verify_snapshot_t;

typedef enum { VERIFY_TRANSPORT_OK, VERIFY_TRANSPORT_TIMEOUT,
               VERIFY_TRANSPORT_INVALID, VERIFY_TRANSPORT_BUSY } verify_transport_t;
typedef struct {
    uint32_t token;
    bool remote;
    verify_transport_t transport;
    verify_snapshot_t snapshot;
} verify_result_t;

typedef enum { VERIFY_PASS, VERIFY_FAIL, VERIFY_UNVERIFIED } verify_verdict_t;
typedef enum {
    VERIFY_REASON_MATCH, VERIFY_REASON_BUILD, VERIFY_REASON_CRC, VERIFY_REASON_METADATA,
    VERIFY_REASON_UPDATE_ACTIVE, VERIFY_REASON_CHANGED, VERIFY_REASON_BUSY,
    VERIFY_REASON_TIMEOUT, VERIFY_REASON_INVALID, VERIFY_REASON_CORE_UNAVAILABLE,
    VERIFY_REASON_CORE_PROGRESS, VERIFY_REASON_EXPIRED,
} verify_reason_t;
typedef struct { verify_verdict_t verdict; verify_reason_t reason; } verify_assessment_t;

/* Full-slot CRC differs from the payload-only checksum stored in boot metadata.
 * Results describe the scan interval, not a persistent verification lease. */
verify_assessment_t verification_assess(const verify_result_t *, uint16_t expected_version,
                                         uint32_t expected_slot_crc32);
