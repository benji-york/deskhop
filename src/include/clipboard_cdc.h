/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLIPBOARD_CDC_FRAME_SIZE 64u
/* Core 0 only. Binary mode owns the CDC connection until DTR drops. */
bool clipboard_cdc_open(uint64_t session, uint64_t boot, uint64_t now);
void clipboard_cdc_close(uint64_t now);
bool clipboard_cdc_alive(uint64_t now);
void clipboard_cdc_receive(uint8_t byte, uint64_t now);
/* Peek/consume preserves partial USB writes without replaying a frame. */
const uint8_t *clipboard_cdc_output(size_t *length, uint64_t now);
void clipboard_cdc_consumed(size_t length);
