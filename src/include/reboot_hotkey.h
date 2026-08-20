/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REBOOT_HOTKEY_REQUIRED_TAPS  3
#define REBOOT_HOTKEY_TAP_TIMEOUT_US 1000000u

typedef enum {
    REBOOT_HOTKEY_PASS,
    REBOOT_HOTKEY_SWALLOW,
    REBOOT_HOTKEY_TAP,
    REBOOT_HOTKEY_TRIGGER,
    REBOOT_HOTKEY_CANCEL,
} reboot_hotkey_result_t;

typedef struct {
    uint8_t completed_taps;
    uint8_t source;
    uint64_t first_tap_us;
} reboot_hotkey_sequence_t;

typedef struct {
    bool q_down;
    bool valid;
    uint64_t pressed_at_us;
} reboot_hotkey_source_t;

reboot_hotkey_result_t reboot_hotkey_process_report(
    reboot_hotkey_sequence_t *,
    reboot_hotkey_source_t *,
    size_t,
    uint8_t,
    uint8_t,
    const uint8_t *,
    size_t,
    uint8_t,
    uint8_t,
    uint64_t);
void reboot_hotkey_reset(reboot_hotkey_sequence_t *);
void reboot_hotkey_reset_source(reboot_hotkey_source_t *);
