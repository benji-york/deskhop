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

#include <stdint.h>
#include "structs.h"

/*==============================================================================
 *  Checksum Functions
 *==============================================================================*/

uint32_t calc_packet_checksum(const uart_packet_t *);
uint32_t calc_crc32(const uint8_t *, size_t);
uint32_t crc32_iter(uint32_t, const uint8_t);
bool     verify_checksum(const uart_packet_t *);

/*==============================================================================
 *  Global State
 *==============================================================================*/

extern device_t global_state;

/*==============================================================================
 *  LED Control
 *==============================================================================*/

void    blink_led(device_t *);
void    restore_leds(device_t *);
uint8_t toggle_led(void);
