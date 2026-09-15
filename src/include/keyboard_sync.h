/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* Runtime-only, core-1-owned keyboard transport. No saved configuration. */
void keyboard_sync_init(uint64_t boot_session);
void keyboard_sync_publish(void);
void keyboard_sync_reset(void);
void keyboard_sync_task(device_t *state);
void keyboard_sync_receive(uart_packet_t *packet, device_t *state);
void keyboard_synthetic_receive(uart_packet_t *packet, device_t *state);
