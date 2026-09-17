/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "clipboard_cdc.h"
#include "clipboard.h"
#include <string.h>

static struct {
    bool active, request_valid;
    uint64_t session;
    clipboard_request_t request;
    uint8_t rx[64], tx[64];
    unsigned rx_used, tx_used, tx_sent;
} channel;

static void wipe(void *memory, size_t length) {
    volatile uint8_t *p = memory;
    while (length--) *p++ = 0;
}
static uint64_t get(const uint8_t *p, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void put(uint8_t *p, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) p[i] = value >> (8 * i);
}
static bool zeros(const uint8_t *p, unsigned count) {
    for (unsigned i = 0; i < count; ++i) if (p[i]) return false;
    return true;
}
static void frame(uint8_t opcode) {
    wipe(channel.tx, sizeof(channel.tx));
    memcpy(channel.tx, "DHC2", 4);
    channel.tx[4] = opcode;
    channel.tx_used = sizeof(channel.tx);
    channel.tx_sent = 0;
}
void clipboard_cdc_close(uint64_t now) {
    clipboard_helper_close(now);
    wipe(&channel, sizeof(channel));
}
bool clipboard_cdc_open(uint64_t session, uint64_t boot, uint64_t now) {
    clipboard_cdc_close(now);
    if (!session || !clipboard_helper_open(session, now)) return false;
    channel.active = true;
    channel.session = session;
    frame(0);
    put(channel.tx + 8, boot, 8);
    put(channel.tx + 16, session, 8);
    return true;
}
bool clipboard_cdc_alive(uint64_t now) {
    if (channel.active && !clipboard_helper_active(now)) clipboard_cdc_close(now);
    return channel.active;
}
static bool dispatch(uint64_t now) {
    const uint8_t *p = channel.rx + 8;
    if (memcmp(channel.rx, "DHC2", 4) || !zeros(channel.rx + 5, 3)) return false;
    unsigned used;
    switch (channel.rx[4]) {
    case 2: {
        clipboard_request_t request = {
            .boot_source = get(p, 8), .boot_target = get(p + 8, 8),
            .helper_session = get(p + 16, 8), .nonce = get(p + 24, 8),
            .focus = get(p + 32, 8),
        };
        if (!channel.request_valid || memcmp(&request, &channel.request, sizeof(request))
            || !zeros(p + 47, 9)) return false;
        bool valid = clipboard_helper_reply_begin(&request, p[40], (uint16_t)get(p + 41, 2),
                                                   (uint32_t)get(p + 43, 4), now);
        if (valid && p[40] != CLIPBOARD_OK) channel.request_valid = false;
        return valid;
    }
    case 3:
        used = p[10];
        if (!channel.request_valid || get(p, 8) != channel.request.nonce
            || used == 0 || used > 40 || !zeros(p + 11 + used, 45 - used)) return false;
        return clipboard_helper_reply_chunk((uint16_t)get(p + 8, 2), p + 11, used, now);
    case 4:
        if (!channel.request_valid || get(p, 8) != channel.request.nonce
            || !zeros(p + 8, 48)) return false;
        channel.request_valid = false;
        return clipboard_helper_reply_commit(now);
    case 5:
        if (get(p, 8) != channel.session || !zeros(p + 8, 48)) return false;
        clipboard_helper_keepalive(now);
        return true;
    default: return false;
    }
}
void clipboard_cdc_receive(uint8_t byte, uint64_t now) {
    if (!clipboard_cdc_alive(now)) return;
    channel.rx[channel.rx_used++] = byte;
    if (channel.rx_used != sizeof(channel.rx)) return;
    bool valid = dispatch(now);
    wipe(channel.rx, sizeof(channel.rx));
    channel.rx_used = 0;
    if (!valid) clipboard_cdc_close(now);
}
const uint8_t *clipboard_cdc_output(size_t *length, uint64_t now) {
    *length = 0;
    if (!clipboard_cdc_alive(now)) return NULL;
    if (!channel.tx_used && clipboard_helper_poll(&channel.request)) {
        channel.request_valid = true;
        frame(1);
        put(channel.tx + 8, channel.request.boot_source, 8);
        put(channel.tx + 16, channel.request.boot_target, 8);
        put(channel.tx + 24, channel.request.helper_session, 8);
        put(channel.tx + 32, channel.request.nonce, 8);
        put(channel.tx + 40, channel.request.focus, 8);
    }
    *length = channel.tx_used - channel.tx_sent;
    return *length ? channel.tx + channel.tx_sent : NULL;
}
void clipboard_cdc_consumed(size_t length) {
    if (length > channel.tx_used - channel.tx_sent) return;
    wipe(channel.tx + channel.tx_sent, length);
    channel.tx_sent += length;
    if (channel.tx_sent == channel.tx_used) channel.tx_used = channel.tx_sent = 0;
}
