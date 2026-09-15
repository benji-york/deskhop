/* WebHID configuration reports deliberately have their own fixed wire format.
 * UART framing must never change the descriptor's 12-byte report contract. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "packet.h"

/* CRC-8/ATM: polynomial 0x07, init 0, no reflection, xorout 0. The
 * preamble, command type and all eight payload bytes are covered. USB itself
 * additionally protects transfer framing; this is error detection, not auth. */
static inline uint8_t config_packet_crc8(const uint8_t *data, size_t length) {
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
    }
    return crc;
}

static inline void write_config_packet(uint8_t *raw, const uart_packet_t *packet) {
    raw[0] = START1;
    raw[1] = START2;
    raw[2] = packet->type;
    memcpy(raw + 3, packet->data, PACKET_DATA_LENGTH);
    raw[CONFIG_PACKET_LENGTH - 1] = config_packet_crc8(raw, CONFIG_PACKET_LENGTH - 1);
}

static inline bool read_config_packet(const uint8_t *raw, size_t length,
                                      uart_packet_t *packet) {
    if (length != CONFIG_PACKET_LENGTH || raw[0] != START1 || raw[1] != START2
        || raw[CONFIG_PACKET_LENGTH - 1] != config_packet_crc8(raw, CONFIG_PACKET_LENGTH - 1))
        return false;
    memset(packet, 0, sizeof(*packet));
    packet->type = raw[2];
    memcpy(packet->data, raw + 3, PACKET_DATA_LENGTH);
    return true;
}
