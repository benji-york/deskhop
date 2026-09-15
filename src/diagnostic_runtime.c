/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "diagnostic_history.h"

/* No flash, USB, UART, formatting, or history operation under this lock.
 * Readers never acquire the firmware lock. Updater callers may hold it. */
static critical_section_t runtime_lock;
static bool initialized;
static diagnostic_runtime_snapshot_t state;
static uint64_t checkpoint_us[2], progress_us;
static unsigned reported_quarter;

void diagnostic_runtime_init(void) {
    if (!initialized)
        critical_section_init(&runtime_lock);
    state = (diagnostic_runtime_snapshot_t){.total_bytes = STAGING_IMAGE_SIZE};
    checkpoint_us[0] = checkpoint_us[1] = progress_us = 0;
    reported_quarter = 0;
    initialized = true;
}

void diagnostic_runtime_checkpoint(unsigned core) {
    if (!initialized || core > 1)
        return;
    critical_section_enter_blocking(&runtime_lock);
    ++state.core_ticks[core];
    checkpoint_us[core] = time_us_64();
    state.core_valid |= (uint8_t)(1u << core);
    critical_section_exit(&runtime_lock);
}

static uint32_t age_ms(uint64_t now, uint64_t then) {
    uint64_t age = (now - then) / 1000;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

diagnostic_runtime_snapshot_t diagnostic_runtime_snapshot(void) {
    if (!initialized)
        return (diagnostic_runtime_snapshot_t){0};
    critical_section_enter_blocking(&runtime_lock);
    diagnostic_runtime_snapshot_t result = state;
    uint64_t now = time_us_64();
    for (unsigned i = 0; i < 2; ++i)
        result.core_age_ms[i] = state.core_valid & (1u << i)
                                   ? age_ms(now, checkpoint_us[i]) : UINT32_MAX;
    result.progress_age_ms = state.update_seen ? age_ms(now, progress_us) : UINT32_MAX;
    critical_section_exit(&runtime_lock);
    return result;
}

void diagnostic_update_begin(diagnostic_update_source_t source, uint16_t target_version) {
    if (!initialized)
        return;
    critical_section_enter_blocking(&runtime_lock);
    ++state.update_attempt;
    state.update_seen = true;
    state.source = source;
    state.target_version = target_version;
    state.phase = DIAGNOSTIC_UPDATE_RECEIVING;
    state.received_bytes = 0;
    progress_us = time_us_64();
    reported_quarter = 0;
    critical_section_exit(&runtime_lock);
    diagnostic_history_record(HISTORY_UPDATE_BEGIN, source, 0, target_version);
}

void diagnostic_update_progress(uint32_t received_bytes) {
    if (!initialized)
        return;
    unsigned quarter = 0;
    uint8_t source = 0;
    critical_section_enter_blocking(&runtime_lock);
    if (state.update_seen && state.phase == DIAGNOSTIC_UPDATE_RECEIVING
        && received_bytes > state.received_bytes && received_bytes <= state.total_bytes) {
        state.received_bytes = received_bytes;
        progress_us = time_us_64();
        unsigned current = received_bytes / (STAGING_IMAGE_SIZE / 4);
        if (current > reported_quarter) {
            reported_quarter = quarter = current;
            source = state.source;
        }
    }
    critical_section_exit(&runtime_lock);
    if (quarter)
        diagnostic_history_record(HISTORY_UPDATE_PROGRESS, source, quarter * 25, received_bytes);
}

void diagnostic_update_phase(diagnostic_update_phase_t phase) {
    if (!initialized)
        return;
    bool changed;
    uint8_t source;
    uint16_t target;
    critical_section_enter_blocking(&runtime_lock);
    changed = state.update_seen && state.phase != phase;
    if (changed)
        state.phase = phase;
    source = state.source;
    target = state.target_version;
    critical_section_exit(&runtime_lock);
    if (changed)
        diagnostic_history_record(HISTORY_UPDATE_PHASE, phase, source, target);
}
