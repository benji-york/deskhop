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
#include <limits.h>
#include "config_migration.h"
#include "structs.h"
#include "misc.h"
#include "screen.h"
#include "config_confirm.h"

#define CONFIG_V8_SIZE_BYTES     136
#define CONFIG_V8_RESERVED_OFFSET 128

/*==============================================================================
 *  Configuration Data
 *  Structures and variables related to device configuration.
 *==============================================================================*/

extern const config_t default_config;

/*==============================================================================
 *  Configuration API
 *  Functions and data structures for accessing and modifying configuration.
 *==============================================================================*/

extern const field_map_t api_field_map[];
const field_map_t* get_field_map_entry(uint32_t);
const field_map_t* get_field_map_index(uint32_t);
size_t             get_field_map_length(void);

/*==============================================================================
 *  Configuration Management and Packet Processing
 *  Functions for loading, saving, wiping, and resetting device configuration.
 *==============================================================================*/

/* Protect brief RAM configuration mutations/snapshots, never flash or queues. */
void config_lock(void);
void config_unlock(void);
/* Persisted layout is unchanged. Both direct and proxied SETs carry six value
 * bytes; byte seven in a direct UINT64 request must be zero. Existing full-width
 * persisted durations remain valid and are never narrowed during load/save. */
#define CONFIG_TIMEOUT_MAX_US UINT64_C(281474976710655)
#define CONFIG_SPEED_MAX 128
#define CONFIG_SCREEN_COUNT_MAX INT32_MAX
void config_snapshot(const device_t *, config_t *);
bool config_validate(const config_t *);
bool config_repair(config_t *);
bool config_set_value(device_t *, uint8_t, const uint8_t value[7]);
bool config_set_value64(device_t *, uint8_t, uint64_t);
uint32_t config_digest(const device_t *);
config_confirm_status_t config_check_value64(const device_t *, uint8_t, uint64_t, uint32_t *);
config_confirm_status_t config_save_confirmed(device_t *, uint32_t, uint32_t *);
bool config_set_border(device_t *, uint8_t, const border_size_t *);
bool config_set_screensaver_mode(device_t *, uint8_t, uint8_t);
void config_set_screen_index(device_t *, uint8_t, uint32_t);
void load_config(device_t *);
void queue_cfg_packet(uart_packet_t *, device_t *);
bool queue_cfg_packet_try(uart_packet_t *, device_t *);
void reset_config_timer(device_t *);
void save_config(device_t *);
bool validate_packet(uart_packet_t *);
void wipe_config(void);
