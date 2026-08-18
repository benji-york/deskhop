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

#include "structs.h"

typedef struct TU_ATTR_PACKED {
    uint8_t output;
    int8_t zoom_in_direction;
    uint8_t active;
    uint8_t exit_pending;
    uint16_t debt;
    uint16_t overscroll;
} zoom_assist_sync_t;

_Static_assert(sizeof(zoom_assist_sync_t) == PACKET_DATA_LENGTH,
               "Zoom Assist sync packet must fill one UART payload");

bool zoom_assist_is_active(const device_t *state);
bool mouse_uses_relative_mode(const device_t *state);
bool is_macos_zoom_scroll(const device_t *state, int32_t wheel);
void clear_zoom_assist(device_t *state, uint8_t output, bool forget_direction);
void observe_zoom_scroll(device_t *state, int32_t wheel);
void publish_local_modifiers(device_t *state);
void sync_owned_zoom_assist(device_t *state);
void zoom_assist_task(device_t *state);

void handle_modifier_state_msg(uart_packet_t *packet, device_t *state);
void handle_zoom_assist_msg(uart_packet_t *packet, device_t *state);
void handle_zoom_assist_clear_msg(uart_packet_t *packet, device_t *state);
