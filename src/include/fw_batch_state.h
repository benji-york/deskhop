/* Runtime-only state; never part of persisted configuration. */
#pragma once
#include "fw_batch.h"

typedef enum {
    FW_BATCH_LEGACY = 0, FW_BATCH_PROBE, FW_BATCH_WAIT_CAPS, FW_BATCH_PAGES,
} fw_batch_mode_t;

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
} fw_batch_state_t;
