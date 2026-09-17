/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLIPBOARD_MAX_TEXT 1024u
#define CLIPBOARD_REPORT_INTERVAL_US 5000ull
#define CLIPBOARD_RESPONSE_TIMEOUT_US 3000000ull
#define CLIPBOARD_TYPING_TIMEOUT_US 15000000ull

typedef enum {
    CLIPBOARD_OK, CLIPBOARD_EMPTY, CLIPBOARD_NON_TEXT, CLIPBOARD_OVERSIZE,
    CLIPBOARD_UNSUPPORTED, CLIPBOARD_UNAVAILABLE
} clipboard_status_t;
typedef enum {
    CLIPBOARD_IDLE, CLIPBOARD_RECEIVING, CLIPBOARD_READY,
    CLIPBOARD_TYPING, CLIPBOARD_RELEASING
} clipboard_phase_t;

/* One text allocation, never a C string. No allocation, logging, or persistence. */
typedef struct {
    uint8_t text[CLIPBOARD_MAX_TEXT];
    uint64_t deadline, next_report;
    uint32_t expected_crc;
    uint16_t length, received, index;
    clipboard_phase_t phase;
    bool released, down;
} clipboard_text_t;

uint32_t clipboard_crc32(const uint8_t *, size_t);
bool clipboard_ascii_key(uint8_t byte, uint8_t *modifier, uint8_t *key);
void clipboard_text_cancel(clipboard_text_t *);
bool clipboard_text_begin(clipboard_text_t *, uint8_t status, uint16_t length,
                          uint32_t crc, uint64_t now);
bool clipboard_text_append(clipboard_text_t *, uint16_t offset,
                           const uint8_t *, size_t length, uint64_t now);
bool clipboard_text_commit(clipboard_text_t *, uint64_t now);
void clipboard_text_release(clipboard_text_t *);
void clipboard_text_tick(clipboard_text_t *, uint64_t now);
/* A preview never advances state: call accepted only after USB accepts it. */
bool clipboard_text_report(clipboard_text_t *, uint64_t now, uint8_t report[8]);
void clipboard_text_accepted(clipboard_text_t *, uint64_t now);
