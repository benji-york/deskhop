/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clipboard_state.h"
#include <string.h>

static void erase(void *memory, size_t length) {
    volatile uint8_t *p = memory;
    while (length--) *p++ = 0;
}

uint32_t clipboard_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Explicit US ANSI positions; never infer Unicode, locale, or dead-key state. */
bool clipboard_ascii_key(uint8_t byte, uint8_t *modifier, uint8_t *key) {
    *modifier = 0;
    *key = 0;
    if (byte >= 'a' && byte <= 'z') *key = byte - 'a' + 4;
    else if (byte >= 'A' && byte <= 'Z') { *key = byte - 'A' + 4; *modifier = 2; }
    else if (byte >= '1' && byte <= '9') *key = byte - '1' + 30;
    else if (byte == '0') *key = 39;
    else {
        static const char plain[] = "\n\t -=[]\\;'`,./";
        static const uint8_t keys[] = {40,43,44,45,46,47,48,49,51,52,53,54,55,56};
        static const char shifted[] = "!@#$%^&*()_+{}|:\"~<>?";
        static const uint8_t shifted_keys[] = {
            30,31,32,33,34,35,36,37,38,39,45,46,47,48,49,51,52,53,54,55,56
        };
        for (size_t i = 0; i < sizeof(plain) - 1; ++i)
            if (byte == (uint8_t)plain[i]) { *key = keys[i]; return true; }
        for (size_t i = 0; i < sizeof(shifted) - 1; ++i)
            if (byte == (uint8_t)shifted[i]) {
                *key = shifted_keys[i]; *modifier = 2; return true;
            }
        return false;
    }
    return true;
}

void clipboard_text_cancel(clipboard_text_t *state) {
    bool down = state->down;
    erase(state, sizeof(*state));
    state->down = down;
    state->phase = down ? CLIPBOARD_RELEASING : CLIPBOARD_IDLE;
}

bool clipboard_text_begin(clipboard_text_t *state, uint8_t status, uint16_t length,
                          uint32_t crc, uint64_t now) {
    if (state->phase != CLIPBOARD_IDLE || status != CLIPBOARD_OK
        || length == 0 || length > CLIPBOARD_MAX_TEXT) {
        clipboard_text_cancel(state);
        return false;
    }
    state->length = length;
    state->expected_crc = crc;
    state->deadline = now + CLIPBOARD_RESPONSE_TIMEOUT_US;
    state->phase = CLIPBOARD_RECEIVING;
    return true;
}

bool clipboard_text_append(clipboard_text_t *state, uint16_t offset,
                           const uint8_t *data, size_t length, uint64_t now) {
    if (state->phase != CLIPBOARD_RECEIVING || now >= state->deadline
        || !length || !data || offset != state->received
        || length > (size_t)(state->length - state->received)) {
        clipboard_text_cancel(state);
        return false;
    }
    memcpy(state->text + offset, data, length);
    state->received += length;
    return true;
}

bool clipboard_text_commit(clipboard_text_t *state, uint64_t now) {
    bool valid = state->phase == CLIPBOARD_RECEIVING && now < state->deadline
        && state->received == state->length
        && clipboard_crc32(state->text, state->length) == state->expected_crc;
    for (unsigned i = 0; valid && i < state->length; ++i) {
        uint8_t modifier, key;
        valid = clipboard_ascii_key(state->text[i], &modifier, &key);
    }
    if (!valid) { clipboard_text_cancel(state); return false; }
    state->phase = CLIPBOARD_READY;
    return true;
}

void clipboard_text_release(clipboard_text_t *state) { state->released = true; }

void clipboard_text_tick(clipboard_text_t *state, uint64_t now) {
    if (state->phase != CLIPBOARD_IDLE && state->phase != CLIPBOARD_RELEASING
        && now >= state->deadline)
        clipboard_text_cancel(state);
}

bool clipboard_text_report(clipboard_text_t *state, uint64_t now, uint8_t report[8]) {
    clipboard_text_tick(state, now);
    memset(report, 0, 8);
    if (state->phase == CLIPBOARD_RELEASING) return true;
    if (state->phase == CLIPBOARD_READY && state->released) {
        state->phase = CLIPBOARD_TYPING;
        state->deadline = now + CLIPBOARD_TYPING_TIMEOUT_US;
    }
    if (state->phase != CLIPBOARD_TYPING || now < state->next_report) return false;
    if (!state->down)
        clipboard_ascii_key(state->text[state->index], &report[0], &report[2]);
    return true;
}

void clipboard_text_accepted(clipboard_text_t *state, uint64_t now) {
    if (state->phase == CLIPBOARD_RELEASING) {
        state->down = false;
        clipboard_text_cancel(state);
    } else if (state->phase == CLIPBOARD_TYPING) {
        state->down = !state->down;
        if (!state->down && ++state->index == state->length) {
            clipboard_text_cancel(state);
            return;
        }
        state->next_report = now + CLIPBOARD_REPORT_INTERVAL_US;
    }
}
