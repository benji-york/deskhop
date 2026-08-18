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

#include "zoom_tracker.h"
#include <limits.h>

static uint16_t saturating_add(uint16_t value, uint32_t amount, uint16_t maximum) {
    if (amount >= maximum || value >= maximum - amount)
        return maximum;

    return value + amount;
}

int8_t zoom_canonicalize_wheel(int32_t wheel) {
    if (wheel > INT8_MAX)
        return INT8_MAX;

    if (wheel < INT8_MIN)
        return INT8_MIN;

    return (int8_t)wheel;
}

bool zoom_modifier_state_is_fresh(uint8_t modifiers, uint64_t last_seen, uint64_t now) {
    return modifiers != 0 && now - last_seen <= ZOOM_ASSIST_MODIFIER_TIMEOUT_US;
}

bool zoom_tracker_observe(zoom_tracker_t *tracker, int32_t wheel, uint64_t now) {
    if (wheel == 0)
        return false;

    int8_t direction = wheel < 0 ? -1 : 1;
    uint32_t amount = wheel < 0 ? (uint32_t)(-(int64_t)wheel) : (uint32_t)wheel;

    /* A new wheel event extends/cancels the quiet period. */
    tracker->exit_pending = false;
    tracker->exit_deadline = 0;

    /* The user's first Command-scroll after boot is expected to be zoom-in
       from 1x. Keeping this runtime-only avoids persistent configuration and
       naturally follows the host's current Natural Scrolling setting. */
    if (tracker->zoom_in_direction == 0)
        tracker->zoom_in_direction = direction;

    if (direction == tracker->zoom_in_direction) {
        tracker->debt = saturating_add(tracker->debt, amount, ZOOM_ASSIST_DEBT_CAP);
        tracker->overscroll = 0;
        tracker->active = true;
        return true;
    }

    /* A zoom-out gesture while already believed to be at 1x is harmless and
       must not activate Zoom Assist. */
    if (!tracker->active)
        return false;

    if (amount < tracker->debt) {
        tracker->debt -= amount;
        tracker->overscroll = 0;
        return true;
    }

    amount -= tracker->debt;
    tracker->debt = 0;
    tracker->overscroll = saturating_add(tracker->overscroll, amount, UINT16_MAX);

    if (tracker->overscroll >= ZOOM_ASSIST_EXIT_OVERSCROLL) {
        tracker->exit_pending = true;
        tracker->exit_deadline = now + ZOOM_ASSIST_QUIET_TIME_US;
    }

    return true;
}

bool zoom_tracker_task(zoom_tracker_t *tracker, uint64_t now) {
    if (!tracker->active || !tracker->exit_pending || now < tracker->exit_deadline)
        return false;

    tracker->active = false;
    tracker->exit_pending = false;
    tracker->debt = 0;
    tracker->overscroll = 0;
    tracker->exit_deadline = 0;
    return true;
}

bool zoom_tracker_clear(zoom_tracker_t *tracker, bool forget_direction) {
    bool changed = tracker->active || tracker->exit_pending || tracker->debt != 0
                   || tracker->overscroll != 0
                   || (forget_direction && tracker->zoom_in_direction != 0);
    int8_t direction = forget_direction ? 0 : tracker->zoom_in_direction;

    *tracker = (zoom_tracker_t){.zoom_in_direction = direction};
    return changed;
}
