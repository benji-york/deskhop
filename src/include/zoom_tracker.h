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
#include <stdint.h>

/* Keep all representable unmatched zoom input. A false "still zoomed" result
   is safe because the explicit output-switch and reset hotkeys remain usable;
   a false 1x result could unexpectedly switch computers at the screen edge. */
#define ZOOM_ASSIST_DEBT_CAP        UINT16_MAX
#define ZOOM_ASSIST_EXIT_OVERSCROLL 6
#define ZOOM_ASSIST_QUIET_TIME_US   250000
#define ZOOM_ASSIST_MODIFIER_TIMEOUT_US 2500000

typedef struct {
    int8_t zoom_in_direction;
    bool active;
    bool exit_pending;
    uint16_t debt;
    uint16_t overscroll;
    uint64_t exit_deadline;
} zoom_tracker_t;

/* Observe a Command-scroll wheel value. The first qualifying gesture after
   boot teaches the tracker which raw HID direction means zoom-in. */
bool zoom_tracker_observe(zoom_tracker_t *tracker, int32_t wheel, uint64_t now);

/* Complete a pending exit after the user's deliberate zoom-out overscroll has
   stopped. Returns true only when active state changes. */
bool zoom_tracker_task(zoom_tracker_t *tracker, uint64_t now);

/* Clear inferred state. Optionally forget the learned wheel direction too. */
bool zoom_tracker_clear(zoom_tracker_t *tracker, bool forget_direction);

/* Canonicalize wider HID fields to the signed byte DeskHop emits, and bound
   how long a mirrored modifier remains trustworthy without peer heartbeats. */
int8_t zoom_canonicalize_wheel(int32_t wheel);
bool zoom_modifier_state_is_fresh(uint8_t modifiers, uint64_t last_seen, uint64_t now);
