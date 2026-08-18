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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A firmware word normally makes a UART round trip in well under a millisecond.
   Leave ample headroom for HID traffic and flash erases, but never wait forever
   for a request or response that was dropped from a full queue. */
#define FW_UPDATE_RESPONSE_TIMEOUT_US 100000u

/* A retry is not progress. If no valid word arrives for this long, restart a
   pull from a live peer or enter ROM recovery if the partially-written image no
   longer has a source from which it can be repaired. */
#define FW_UPDATE_STALL_TIMEOUT_US 30000000u
#define FW_UPDATE_PEER_TIMEOUT_US   3500000u

/* Bytes 2-3 of a heartbeat identify peers that advertise the build checksum in
   bytes 4-7. Older firmware left this field zero and must not be treated as a
   checksummed source merely because its numeric version is higher. */
#define FW_UPDATE_PROTOCOL_MARKER 0xd485u

typedef enum {
    FW_UPDATE_SOURCE_NONE = 0,
    FW_UPDATE_SOURCE_PULL,
    FW_UPDATE_SOURCE_PULL_PAUSED,
    FW_UPDATE_SOURCE_DROP,
} fw_update_source_t;

typedef enum {
    FW_UPDATE_WAIT = 0,
    FW_UPDATE_REQUEST,
    FW_UPDATE_ABANDON,
    FW_UPDATE_RESTART,
    FW_UPDATE_PAUSE,
} fw_update_action_t;

bool fw_update_peer_compatible(uint16_t protocol_marker);

fw_update_action_t fw_update_next_action(fw_update_source_t source,
                                         bool word_complete,
                                         bool image_dirty,
                                         uint32_t now_us,
                                         uint32_t requested_at_us,
                                         uint32_t progressed_at_us,
                                         uint32_t peer_seen_at_us);

bool fw_update_response_expected(fw_update_source_t source,
                                 bool request_pending,
                                 uint32_t expected_address,
                                 uint32_t response_address);

bool fw_update_metadata_matches(uint32_t magic,
                                uint16_t version,
                                uint32_t embedded_checksum,
                                uint32_t calculated_checksum,
                                uint16_t expected_version,
                                bool check_version);

bool fw_update_mark_block(uint32_t *bitmap,
                          uint16_t *received_count,
                          uint32_t block_number);
