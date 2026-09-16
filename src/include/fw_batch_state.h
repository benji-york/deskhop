/* Runtime-only state; never part of persisted configuration. */
#pragma once
#include "fw_batch.h"

typedef enum {
    FW_BATCH_LEGACY = 0, FW_BATCH_PROBE, FW_BATCH_WAIT_CAPS, FW_BATCH_PAGES,
} fw_batch_mode_t;

/* Updated only under the existing firmware lock, never from history readers.
 * Totals saturate rather than wrap. Retained on the source after peer reboot.
 * Timing measures source service / inter-request gaps, not flash latency or
 * confirmed wire delivery. No per-frame events or additional locks. */
typedef struct {
    uint64_t started_us, page_started_us, page_ended_us;
    uint32_t checksum, last_page, last_word, page_requests, word_requests, retries;
    uint32_t page_service_us, page_gap_us, page_max_us;
    uint16_t version;
    uint8_t mode, quarter;
    bool active, finished, page_active, page_ended, retry_recorded;
} fw_source_profile_t;

typedef struct {
    fw_batch_token_t tokens; /* Survives firmware-pull restarts within this boot. */
    fw_batch_rx_t rx;
    fw_batch_tx_t tx;
    uint8_t source_page[FW_BATCH_PAGE_SIZE];
    fw_batch_mode_t mode;
    uint32_t caps_tag, pending_tag;
    uint32_t source_caps_tag, source_last_tag, source_checksum;
    uint16_t source_version;
    uint8_t page_attempts;
    bool initialized, source_enabled;
    fw_source_profile_t profile;
} fw_batch_state_t;
