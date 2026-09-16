/* Optional page bursts inside unchanged, checksummed UART-v1 frames. */
#include "main.h"
#include "firmware_batch.h"
#include "diagnostic_history.h"

#define FW_BATCH_MAX_ATTEMPTS 3u
_Static_assert(FW_BATCH_PAGE_SIZE == FLASH_PAGE_SIZE, "batch pages match flash pages");
_Static_assert(FW_BATCH_IMAGE_SIZE == STAGING_IMAGE_SIZE, "batch covers only firmware");

static uint32_t saturating_add(uint32_t value, uint64_t amount) {
    return amount > UINT32_MAX - value ? UINT32_MAX : value + (uint32_t)amount;
}

static void profile_begin(device_t *state, uint64_t now) {
    state->batch.profile = (fw_source_profile_t){
        .active = true, .started_us = now, .last_page = UINT32_MAX,
        .last_word = UINT32_MAX, .version = state->_running_fw.version,
        .checksum = state->_running_fw.checksum,
    };
}

static bool profile_matches(const device_t *state) {
    const fw_source_profile_t *p = &state->batch.profile;
    return p->active && p->version == state->_running_fw.version
        && p->checksum == state->_running_fw.checksum;
}

static void profile_mode(fw_source_profile_t *p, uint8_t mode, uint32_t address) {
    if (p->mode & mode)
        return;
    p->mode |= mode;
    diagnostic_history_record(HISTORY_TRANSFER_SOURCE,
        mode == TRANSFER_MODE_PAGES ? TRANSFER_BATCH_BEGIN : TRANSFER_WORDS_BEGIN,
        p->mode, address);
}

static void profile_progress(fw_source_profile_t *p, uint32_t bytes) {
    unsigned quarter = bytes / (STAGING_IMAGE_SIZE / 4);
    /* Record only exact quarter-end offsets actually observed. A jumping
     * request does not backfill earlier milestones or prove byte coverage. */
    if (quarter > p->quarter && bytes % (STAGING_IMAGE_SIZE / 4) == 0) {
        p->quarter = quarter;
        diagnostic_history_record(HISTORY_TRANSFER_SOURCE, TRANSFER_PROGRESS, p->mode, bytes);
    }
}

static void profile_finish(fw_source_profile_t *p, uint8_t phase, uint64_t now) {
    if (p->finished)
        return;
    p->finished = true;
    diagnostic_history_record(HISTORY_TRANSFER_SOURCE, phase, p->mode, STAGING_IMAGE_SIZE);
    const uint32_t times[] = {saturating_add(0, now - p->started_us),
        p->page_service_us, p->page_gap_us, p->page_max_us};
    for (unsigned i = 0; i < sizeof(times) / sizeof(times[0]); ++i)
        diagnostic_history_record(HISTORY_TRANSFER_TIMING, i + 1, p->mode, times[i]);
    const uint32_t counts[] = {p->page_requests, p->word_requests, p->retries};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
        diagnostic_history_record(HISTORY_TRANSFER_COUNT, i + 1, p->mode, counts[i]);
}

static void profile_page_stop(fw_source_profile_t *p, uint64_t now) {
    if (!p->page_active)
        return;
    uint64_t duration = now - p->page_started_us;
    p->page_service_us = saturating_add(p->page_service_us, duration);
    uint32_t bounded = saturating_add(0, duration);
    if (bounded > p->page_max_us)
        p->page_max_us = bounded;
    p->page_active = false;
}

void firmware_source_word_locked(device_t *state, uint32_t address) {
    /* Exclude the legacy sentinel and anything not a real aligned word. */
    if (address >= STAGING_IMAGE_SIZE || address % sizeof(uint32_t))
        return;
    fw_source_profile_t *p = &state->batch.profile;
    uint64_t now = time_us_64();
    if (!profile_matches(state)
        || (address == 0 && p->word_requests && p->last_word != 0))
        profile_begin(state, now);
    profile_mode(p, TRANSFER_MODE_WORDS, address);
    if (p->finished)
        return; /* The summary is a snapshot through the first end observation.
                 * A later page->words transition is still recorded above. */
    profile_page_stop(p, now);
    p->page_ended = false;
    p->word_requests = saturating_add(p->word_requests, 1);
    p->last_word = address;
    profile_progress(p, address + sizeof(uint32_t));
    if (address + sizeof(uint32_t) == STAGING_IMAGE_SIZE)
        profile_finish(p, TRANSFER_WORDS_END, now);
}

void firmware_batch_init(device_t *state, uint64_t boot_session) {
    state->batch = (fw_batch_state_t){.initialized = true};
    fw_batch_token_init(&state->batch.tokens, boot_session);
}

static void use_words(device_t *state) {
    state->batch.mode = FW_BATCH_LEGACY;
    state->batch.pending_tag = 0;
    fw_batch_rx_cancel(&state->batch.rx);
    state->fw.request_pending = false;
    /* Preserve completion ownership. In particular, fallback after a partial
     * page must not look like a completed page if the legacy queue is full. */
}

void firmware_batch_begin_locked(device_t *state) {
    fw_batch_rx_cancel(&state->batch.rx);
    fw_batch_tx_cancel(&state->batch.tx);
    state->batch.mode = state->batch.initialized ? FW_BATCH_PROBE : FW_BATCH_LEGACY;
    state->batch.caps_tag = state->batch.pending_tag = 0;
    state->batch.page_attempts = 0;
    state->batch.source_enabled = false;
    state->batch.profile = (fw_source_profile_t){0};
    /* Do not alter fw.byte_done: a restart may still need its old-reply drain. */
}

bool firmware_batch_accepts_word(const device_t *state) {
    return state->batch.mode == FW_BATCH_LEGACY;
}

bool firmware_batch_request_locked(device_t *state, uint32_t address) {
    fw_batch_state_t *batch = &state->batch;
    uint8_t payload[8];
    if (batch->mode == FW_BATCH_PROBE) {
        if (!batch->pending_tag && !fw_batch_token_next(&batch->tokens, &batch->pending_tag)) {
            use_words(state);
        } else {
            fw_batch_caps_encode_request(batch->pending_tag, state->fw.version, payload);
            if (!queue_packet_try(payload, FW_BATCH_CAPS_REQUEST_MSG, sizeof(payload)))
                return false;
            batch->caps_tag = batch->pending_tag;
            batch->pending_tag = 0;
            batch->mode = FW_BATCH_WAIT_CAPS;
            state->fw.byte_done = false;
            state->fw.request_pending = false;
            state->fw.requested_at_us = time_us_32();
            return false;
        }
    }
    if (batch->mode == FW_BATCH_WAIT_CAPS) {
        /* The existing 100ms action timer reached this branch. Old firmware
         * ignores the probe; no version-number guess enables batch reception. */
        use_words(state);
    }
    if (batch->mode == FW_BATCH_PAGES) {
        if (batch->page_attempts >= FW_BATCH_MAX_ATTEMPTS
            || (!batch->pending_tag && !fw_batch_token_next(&batch->tokens, &batch->pending_tag))) {
            /* No partial page has touched the running CRC/address/flash.
             * Legacy words now replace the entire uncommitted page. */
            use_words(state);
        } else {
            if (!fw_batch_request_encode(batch->pending_tag, address, payload)
                || !queue_packet_try(payload, FW_BATCH_PAGE_REQUEST_MSG, sizeof(payload)))
                return false;
            fw_batch_rx_begin(&batch->rx, batch->pending_tag, address, state->page_buffer);
            batch->pending_tag = 0;
            ++batch->page_attempts;
            state->fw.byte_done = false;
            state->fw.request_pending = true;
            state->fw.requested_at_us = time_us_32();
            return true;
        }
    }
    return request_byte(state, address);
}

static bool can_serve(const device_t *state) {
    return !state->reboot_requested && !state->maintenance_reserved
        && !state->fw.upgrade_in_progress && !state->fw.image_dirty;
}

static void source_activity(device_t *state) {
    state->maintenance_source_seen = true;
    state->maintenance_source_last_us = time_us_64();
}

static void receive_caps_request(const uint8_t payload[8], device_t *state) {
    uint32_t tag;
    uint16_t version;
    uint8_t reply[8];
    if (!can_serve(state) || !fw_batch_caps_decode_request(payload, &tag, &version)
        || version != state->_running_fw.version)
        return;
    fw_batch_caps_encode_response(tag, state->_running_fw.checksum, reply);
    if (!queue_packet_try(reply, FW_BATCH_CAPS_RESPONSE_MSG, sizeof(reply)))
        return;
    fw_batch_state_t *batch = &state->batch;
    bool new_profile = !profile_matches(state) || !batch->source_enabled || batch->source_caps_tag != tag;
    if (!batch->source_enabled || batch->source_caps_tag != tag) {
        fw_batch_tx_cancel(&batch->tx);
        batch->source_last_tag = tag;
    }
    if (new_profile) {
        profile_begin(state, time_us_64());
        diagnostic_history_record(HISTORY_TRANSFER_SOURCE, TRANSFER_CAPS_QUEUED,
                                  TRANSFER_MODE_NONE, tag);
    }
    batch->source_caps_tag = tag;
    batch->source_version = version;
    batch->source_checksum = state->_running_fw.checksum;
    batch->source_enabled = true;
    source_activity(state);
}

static void receive_page_request(const uint8_t payload[8], device_t *state) {
    uint32_t tag, address;
    fw_batch_state_t *batch = &state->batch;
    if (!can_serve(state) || !batch->source_enabled
        || batch->source_version != state->_running_fw.version
        || batch->source_checksum != state->_running_fw.checksum
        || !fw_batch_request_decode(payload, &tag, &address)
        || tag <= batch->source_last_tag)
        return;
    uint64_t now = time_us_64();
    /* One bounded flash copy, not 64 blocking queue additions. New retry tokens
     * replace the old page cursor; any old DMA frame remains harmlessly tagged. */
    read_flash_bytes(ADDR_FW_RUNNING + address, batch->source_page, sizeof(batch->source_page));
    fw_batch_tx_begin(&batch->tx, tag, address, batch->source_page);
    batch->source_last_tag = tag;
    fw_source_profile_t *p = &batch->profile;
    if (!profile_matches(state))
        profile_begin(state, now);
    profile_mode(p, TRANSFER_MODE_PAGES, address);
    if (p->last_page == address && !p->retry_recorded) {
        p->retry_recorded = true;
        diagnostic_history_record(HISTORY_TRANSFER_SOURCE, TRANSFER_RETRY, p->mode, address);
    }
    if (!p->finished) {
        if (p->last_page == address)
            p->retries = saturating_add(p->retries, 1);
        profile_page_stop(p, now);
        if (p->page_ended)
            p->page_gap_us = saturating_add(p->page_gap_us, now - p->page_ended_us);
        p->page_ended = false;
        p->page_started_us = now;
        p->page_active = true;
        p->last_page = address;
        p->page_requests = saturating_add(p->page_requests, 1);
    }
    source_activity(state);
}

static void receive_page_data(uart_packet_t *packet, device_t *state) {
    if (state->reboot_requested || state->maintenance_reserved
        || state->fw.source != FW_UPDATE_SOURCE_PULL || !state->fw.request_pending
        || state->batch.mode != FW_BATCH_PAGES || state->batch.rx.address != state->fw.address)
        return;
    fw_batch_rx_result_t result = fw_batch_rx_receive(&state->batch.rx,
        packet->type == FW_BATCH_PAGE_DATA_MSG ? FW_BATCH_DATA : FW_BATCH_END, packet->data);
    if (result != FW_BATCH_RX_COMPLETE)
        return; /* A bad/missing page is retried with a fresh token at the deadline. */
    uint32_t address = state->fw.address;
    if ((address & 0xfff) == 0)
        toggle_led();
    if (address < STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE)
        for (unsigned i = 0; i < FLASH_PAGE_SIZE; ++i)
            state->fw.checksum = crc32_iter(state->fw.checksum, state->page_buffer[i]);
    state->fw.address += FLASH_PAGE_SIZE;
    state->fw.progressed_at_us = time_us_32();
    state->fw.request_pending = false;
    state->fw.byte_done = true;
    state->fw.page_pending = true;
    state->batch.page_attempts = 0;
    diagnostic_update_progress(state->fw.address);
}

void firmware_batch_packet(uart_packet_t *packet, device_t *state) {
    firmware_update_lock();
    switch (packet->type) {
    case FW_BATCH_CAPS_REQUEST_MSG:
        receive_caps_request(packet->data, state);
        break;
    case FW_BATCH_CAPS_RESPONSE_MSG:
        if (!state->reboot_requested && !state->maintenance_reserved
            && state->fw.source == FW_UPDATE_SOURCE_PULL && state->fw.address == 0
            && state->batch.mode == FW_BATCH_WAIT_CAPS
            && fw_batch_caps_response_matches(packet->data, state->batch.caps_tag, state->fw.peer_checksum)) {
            state->batch.mode = FW_BATCH_PAGES;
            state->fw.byte_done = true;
        }
        break;
    case FW_BATCH_PAGE_REQUEST_MSG:
        receive_page_request(packet->data, state);
        break;
    case FW_BATCH_PAGE_DATA_MSG:
    case FW_BATCH_PAGE_END_MSG:
        receive_page_data(packet, state);
        break;
    default:
        break;
    }
    firmware_update_unlock();
}

bool firmware_batch_next_tx(device_t *state, uart_packet_t *packet) {
    if (!firmware_update_try_lock())
        return false;
    fw_batch_state_t *batch = &state->batch;
    bool ready = false;
    if (!can_serve(state) || (batch->profile.active && !profile_matches(state)))
        batch->profile = (fw_source_profile_t){0};
    if (!can_serve(state) || batch->source_version != state->_running_fw.version
        || batch->source_checksum != state->_running_fw.checksum) {
        batch->source_enabled = false;
        fw_batch_tx_cancel(&batch->tx);
    } else {
        fw_batch_kind_t kind;
        if (fw_batch_tx_peek(&batch->tx, &kind, packet->data)) {
            packet->type = kind == FW_BATCH_DATA ? FW_BATCH_PAGE_DATA_MSG : FW_BATCH_PAGE_END_MSG;
            fw_batch_tx_sent(&batch->tx);
            if (kind == FW_BATCH_END && batch->profile.active && !batch->profile.finished) {
                /* This frame has been selected for the caller's DMA admission.
                 * No receive acknowledgement or physical delivery is implied. */
                fw_source_profile_t *p = &batch->profile;
                uint64_t now = time_us_64();
                profile_page_stop(p, now);
                p->page_ended = true;
                p->page_ended_us = now;
                uint32_t end = batch->tx.address + FLASH_PAGE_SIZE;
                profile_progress(p, end);
                if (end == STAGING_IMAGE_SIZE)
                    profile_finish(p, TRANSFER_BATCH_END, now);
            }
            source_activity(state);
            ready = true;
        }
    }
    firmware_update_unlock();
    return ready;
}
