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

#include "main.h"

#define MACOS_ZOOM_MODIFIERS (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI)

static void send_zoom_assist_state(device_t *state, uint8_t output) {
    if (output >= NUM_SCREENS)
        return;

    zoom_tracker_t *tracker = &state->zoom_assist[output];
    zoom_assist_sync_t sync = {
        .output = output,
        .zoom_in_direction = tracker->zoom_in_direction,
        .active = tracker->active,
        .exit_pending = tracker->exit_pending,
        .debt = tracker->debt,
        .overscroll = tracker->overscroll,
    };

    queue_packet((uint8_t *)&sync, ZOOM_ASSIST_MSG, sizeof(sync));
}

bool zoom_assist_is_active(const device_t *state) {
    if (state->active_output >= NUM_SCREENS)
        return false;

    return state->config.output[state->active_output].os == MACOS
           && (state->zoom_assist[state->active_output].active
               || state->zoom_activation_pending[state->active_output]);
}

bool mouse_uses_relative_mode(const device_t *state) {
    return state->relative_mouse || state->gaming_mode || zoom_assist_is_active(state);
}

bool is_macos_zoom_scroll(const device_t *state, int32_t wheel) {
    if (wheel == 0 || state->active_output >= NUM_SCREENS)
        return false;

    const output_t *output = &state->config.output[state->active_output];
    uint64_t now = time_us_64();
    uint8_t modifiers = state->local_modifiers;

    if (zoom_modifier_state_is_fresh(state->peer_modifiers,
                                     state->peer_modifiers_last_seen, now))
        modifiers |= state->peer_modifiers;

    return output->os == MACOS && (modifiers & MACOS_ZOOM_MODIFIERS) != 0;
}

void clear_zoom_assist(device_t *state, uint8_t output, bool forget_direction) {
    if (output >= NUM_SCREENS)
        return;

    zoom_tracker_clear(&state->zoom_assist[output], forget_direction);
    send_zoom_assist_state(state, output);
}

void observe_zoom_scroll(device_t *state, int32_t wheel) {
    /* The Pico connected to this output owns its inference state. A pointing
       device attached to the other Pico forwards the wheel report here first;
       only the owner mutates state and then mirrors the result back. */
    if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT || !is_macos_zoom_scroll(state, wheel))
        return;

    zoom_tracker_t *tracker = &state->zoom_assist[state->active_output];
    zoom_tracker_observe(tracker, wheel, time_us_64());

    /* Always reply to a qualifying forwarded gesture. Even an already-at-1x
       zoom-out must clear the source Pico's provisional activation latch. */
    send_zoom_assist_state(state, state->active_output);
}

void publish_local_modifiers(device_t *state) {
    uint8_t modifiers = 0;

    for (uint8_t i = 0; i < MAX_DEVICES; i++)
        modifiers |= state->local_kbd_states[i].modifier;

    if (modifiers == state->local_modifiers)
        return;

    state->local_modifiers = modifiers;
    send_value(modifiers, MODIFIER_STATE_MSG);
}

void sync_owned_zoom_assist(device_t *state) {
    send_value(state->local_modifiers, MODIFIER_STATE_MSG);

    if (BOARD_ROLE < NUM_SCREENS)
        send_zoom_assist_state(state, BOARD_ROLE);
}

void zoom_assist_task(device_t *state) {
    /* Each Pico owns the state of the Mac connected to its device port. Only
       that owner completes a pending timeout, preventing crossed timer packets
       from racing when both boards hold synchronized copies. */
    uint64_t now = time_us_64();
    if (!zoom_modifier_state_is_fresh(state->peer_modifiers,
                                      state->peer_modifiers_last_seen, now))
        state->peer_modifiers = 0;

    if (BOARD_ROLE >= NUM_SCREENS)
        return;

    zoom_tracker_t *tracker = &state->zoom_assist[BOARD_ROLE];
    if (zoom_tracker_task(tracker, now))
        send_zoom_assist_state(state, BOARD_ROLE);
}

void handle_modifier_state_msg(uart_packet_t *packet, device_t *state) {
    state->peer_modifiers = packet->data[0];
    state->peer_modifiers_last_seen = time_us_64();
}

void handle_zoom_assist_msg(uart_packet_t *packet, device_t *state) {
    zoom_assist_sync_t *sync = (zoom_assist_sync_t *)packet->data;

    if (sync->output >= NUM_SCREENS || sync->active > 1 || sync->exit_pending > 1)
        return;

    /* Each Pico is authoritative for the output physically connected to it.
       Never let a crossed or stale peer packet overwrite our owned state. */
    if (sync->output == BOARD_ROLE)
        return;

    if (sync->zoom_in_direction < -1 || sync->zoom_in_direction > 1)
        return;

    if (sync->debt > ZOOM_ASSIST_DEBT_CAP)
        return;

    zoom_tracker_t *tracker = &state->zoom_assist[sync->output];
    state->zoom_activation_pending[sync->output] = false;
    tracker->zoom_in_direction = sync->zoom_in_direction;
    tracker->active = sync->active;
    tracker->exit_pending = sync->exit_pending;
    tracker->debt = sync->debt;
    tracker->overscroll = sync->overscroll;
    tracker->exit_deadline = sync->exit_pending
                                 ? time_us_64() + ZOOM_ASSIST_QUIET_TIME_US
                                 : 0;
}

void handle_zoom_assist_clear_msg(uart_packet_t *packet, device_t *state) {
    uint8_t output = packet->data[0];

    if (output != BOARD_ROLE || output >= NUM_SCREENS)
        return;

    clear_zoom_assist(state, output, true);
}
