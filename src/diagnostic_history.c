/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "main.h"
#include "diagnostic_history.h"

/* Sparse events from both cores share one ring. Each lock scope touches only
 * counters and at most one 24-byte record. Formatting, USB/UART calls, and
 * other application locks stay outside. There is brief spin-lock contention;
 * no lock is held while waiting for the terminal or copying the whole ring. */
static critical_section_t history_lock;
static history_store_t history_store;
static bool initialized;

void diagnostic_history_init(void) {
    if (!initialized)
        critical_section_init(&history_lock);
    history_store_init(&history_store);
    initialized = true;
}

void diagnostic_history_record(history_type_t type, uint8_t a, uint8_t b, uint32_t value) {
    if (!initialized)
        return;
    critical_section_enter_blocking(&history_lock);
    history_store_record(&history_store, time_us_64(), type, a, b, value);
    critical_section_exit(&history_lock);
}

history_window_t diagnostic_history_window(unsigned limit) {
    if (!initialized)
        return (history_window_t){0};
    critical_section_enter_blocking(&history_lock);
    history_window_t window = history_store_window(&history_store, limit);
    critical_section_exit(&history_lock);
    return window;
}

bool diagnostic_history_read(uint64_t seq, history_event_t *event) {
    if (!initialized)
        return false;
    critical_section_enter_blocking(&history_lock);
    bool found = history_store_read(&history_store, seq, event);
    critical_section_exit(&history_lock);
    return found;
}
