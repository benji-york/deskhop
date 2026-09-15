/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "peer_verify.h"

#include <string.h>

enum { SERVER_IDLE, SERVER_ACQUIRE, SERVER_POLL, SERVER_SEND };

static uint64_t read_le(const uint8_t *bytes, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void write_le(uint8_t *bytes, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t wire_crc(const uint8_t *bytes) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 128; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

static void encode_snapshot(uint8_t *bytes, const verify_snapshot_t *snapshot) {
    memset(bytes, 0, PEER_VERIFY_SNAPSHOT_SIZE);
    bytes[0] = PEER_VERIFY_PROTOCOL;
    bytes[1] = snapshot->role;
    bytes[2] = (uint8_t)snapshot->outcome;
    bytes[3] = snapshot->start.core_valid | (snapshot->end.core_valid << 2);
    write_le(bytes + 4, snapshot->major, 2);
    write_le(bytes + 6, snapshot->minor, 2);
    memcpy(bytes + 8, snapshot->board_id, 8);
    write_le(bytes + 16, snapshot->boot_session, 8);
    write_le(bytes + 24, snapshot->started_us, 8);
    write_le(bytes + 32, snapshot->completed_us, 8);
    write_le(bytes + 40, snapshot->bytes_read, 4);
    write_le(bytes + 44, snapshot->slot_crc32, 4);
    write_le(bytes + 48, snapshot->metadata_magic, 4);
    write_le(bytes + 52, snapshot->metadata_version, 2);
    write_le(bytes + 54, snapshot->metadata_reserved, 2);
    write_le(bytes + 56, snapshot->metadata_crc32, 4);
    write_le(bytes + 60, snapshot->generation_start, 8);
    write_le(bytes + 68, snapshot->generation_end, 8);
    for (unsigned i = 0; i < 2; ++i) {
        write_le(bytes + 76 + 4 * i, snapshot->start.core_ticks[i], 4);
        write_le(bytes + 84 + 4 * i, snapshot->end.core_ticks[i], 4);
        write_le(bytes + 92 + 4 * i, snapshot->start.core_age_ms[i], 4);
        write_le(bytes + 100 + 4 * i, snapshot->end.core_age_ms[i], 4);
    }
    bytes[108] = snapshot->start.phase;
    bytes[109] = snapshot->end.phase;
    bytes[110] = snapshot->start.source;
    bytes[111] = snapshot->end.source;
    write_le(bytes + 112, snapshot->start.update_attempt, 4);
    write_le(bytes + 116, snapshot->end.update_attempt, 4);
    write_le(bytes + 120, snapshot->image_crc_at_boot, 4);
    write_le(bytes + 128, wire_crc(bytes), 4);
}

static bool decode_snapshot(verify_snapshot_t *snapshot, const uint8_t *bytes,
                             uint8_t expected_role) {
    if (bytes[0] != PEER_VERIFY_PROTOCOL || bytes[1] != expected_role
        || bytes[2] > VERIFY_SCAN_TIMEOUT || bytes[3] > 15
        || bytes[124] || bytes[125] || bytes[126] || bytes[127]
        || bytes[108] > DIAGNOSTIC_UPDATE_ABANDONED || bytes[109] > DIAGNOSTIC_UPDATE_ABANDONED
        || bytes[110] > DIAGNOSTIC_SOURCE_USB || bytes[111] > DIAGNOSTIC_SOURCE_USB
        || read_le(bytes + 128, 4) != wire_crc(bytes))
        return false;
    *snapshot = (verify_snapshot_t){
        .role = bytes[1], .outcome = (verify_scan_outcome_t)bytes[2],
        .major = (uint16_t)read_le(bytes + 4, 2), .minor = (uint16_t)read_le(bytes + 6, 2),
        .boot_session = read_le(bytes + 16, 8),
        .started_us = read_le(bytes + 24, 8), .completed_us = read_le(bytes + 32, 8),
        .bytes_read = (uint32_t)read_le(bytes + 40, 4), .slot_crc32 = (uint32_t)read_le(bytes + 44, 4),
        .metadata_magic = (uint32_t)read_le(bytes + 48, 4),
        .metadata_version = (uint16_t)read_le(bytes + 52, 2),
        .metadata_reserved = (uint16_t)read_le(bytes + 54, 2),
        .metadata_crc32 = (uint32_t)read_le(bytes + 56, 4),
        .generation_start = read_le(bytes + 60, 8), .generation_end = read_le(bytes + 68, 8),
        .image_crc_at_boot = (uint32_t)read_le(bytes + 120, 4),
        .start = {.core_valid = bytes[3] & 3, .phase = bytes[108], .source = bytes[110],
                  .update_attempt = (uint32_t)read_le(bytes + 112, 4)},
        .end = {.core_valid = (bytes[3] >> 2) & 3, .phase = bytes[109], .source = bytes[111],
                .update_attempt = (uint32_t)read_le(bytes + 116, 4)},
    };
    memcpy(snapshot->board_id, bytes + 8, 8);
    for (unsigned i = 0; i < 2; ++i) {
        snapshot->start.core_ticks[i] = (uint32_t)read_le(bytes + 76 + 4 * i, 4);
        snapshot->end.core_ticks[i] = (uint32_t)read_le(bytes + 84 + 4 * i, 4);
        snapshot->start.core_age_ms[i] = (uint32_t)read_le(bytes + 92 + 4 * i, 4);
        snapshot->end.core_age_ms[i] = (uint32_t)read_le(bytes + 100 + 4 * i, 4);
    }
    return snapshot->bytes_read <= VERIFY_IMAGE_BYTES
           && (snapshot->outcome != VERIFY_SCAN_COMPLETE
               || (snapshot->bytes_read == VERIFY_IMAGE_BYTES
                   && snapshot->generation_start == snapshot->generation_end
                   && snapshot->started_us <= snapshot->completed_us));
}

static void finish_client(peer_verify_t *state, verify_transport_t transport) {
    state->result = (verify_result_t){.token = state->client_token, .remote = true,
                                    .transport = transport};
    if (transport == VERIFY_TRANSPORT_OK
        && !decode_snapshot(&state->result.snapshot, state->client_bytes, state->local_role ^ 1u))
        state->result.transport = VERIFY_TRANSPORT_INVALID;
    state->client_active = false;
    state->result_ready = true;
}

static void expire_client(peer_verify_t *state, uint64_t now_us) {
    if (state->client_active && now_us - state->client_started_us >= PEER_VERIFY_TIMEOUT_US)
        finish_client(state, VERIFY_TRANSPORT_TIMEOUT);
}

void peer_verify_init(peer_verify_t *state, uint8_t role) {
    *state = (peer_verify_t){.local_role = role};
}

peer_verify_start_t peer_verify_start(peer_verify_t *state, uint32_t token, uint64_t requested_us) {
    if (state->client_active || state->result_ready)
        return PEER_VERIFY_BUSY;
    if (!token || state->local_role > 1)
        return PEER_VERIFY_BAD_ARGUMENT;
    state->client_active = true;
    state->client_sent = false;
    state->client_token = token;
    state->client_started_us = requested_us;
    state->client_received = 0;
    memset(state->client_bytes, 0, sizeof(state->client_bytes));
    return PEER_VERIFY_STARTED;
}

bool peer_verify_receive_request(peer_verify_t *state, const uint8_t payload[8], uint64_t now_us) {
    uint32_t token = (uint32_t)read_le(payload, 4);
    if (!token || payload[4] != PEER_VERIFY_PROTOCOL || payload[5] || payload[6] || payload[7]
        || state->local_role > 1 || state->server_phase != SERVER_IDLE)
        return false;
    if (state->server_seen_request && (token == state->server_token
        || now_us - state->server_started_us < PEER_VERIFY_QUERY_INTERVAL_US))
        return false;
    state->server_seen_request = true;
    state->server_token = token;
    state->server_started_us = now_us;
    state->server_phase = SERVER_ACQUIRE;
    state->server_next_chunk = 0;
    return true;
}

void peer_verify_receive_response(peer_verify_t *state, const uint8_t payload[8], uint64_t now_us) {
    expire_client(state, now_us);
    if (!state->client_active || !state->client_sent || read_le(payload, 4) != state->client_token)
        return;
    unsigned index = payload[4];
    if (index >= PEER_VERIFY_CHUNK_COUNT
        || (!index && (payload[5] != PEER_VERIFY_PROTOCOL || payload[6] != (state->local_role ^ 1u)))) {
        finish_client(state, VERIFY_TRANSPORT_INVALID);
        return;
    }
    uint64_t mask = UINT64_C(1) << index;
    uint8_t *dest = state->client_bytes + index * 3;
    if (state->client_received & mask) {
        if (memcmp(dest, payload + 5, 3) != 0)
            finish_client(state, VERIFY_TRANSPORT_INVALID);
        return;
    }
    memcpy(dest, payload + 5, 3);
    state->client_received |= mask;
    if (state->client_received == (UINT64_C(1) << PEER_VERIFY_CHUNK_COUNT) - 1)
        finish_client(state, VERIFY_TRANSPORT_OK);
}

static void server_task(peer_verify_t *state, uint64_t now_us, void *context,
                         peer_verify_acquire_fn acquire, peer_verify_poll_fn poll,
                         peer_verify_cancel_fn cancel) {
    if (state->server_phase == SERVER_IDLE)
        return;
    if (now_us - state->server_started_us >= PEER_VERIFY_TIMEOUT_US) {
        if (state->server_phase == SERVER_POLL)
            cancel(context, state->server_token);
        state->server_phase = SERVER_IDLE;
        return;
    }
    if (state->server_phase == SERVER_ACQUIRE) {
        /* Never acquire ownership without the callbacks needed to release it. */
        if (acquire && poll && cancel && acquire(context, state->server_token, state->server_started_us))
            state->server_phase = SERVER_POLL;
    } else if (state->server_phase == SERVER_POLL) {
        verify_snapshot_t snapshot;
        if (poll(context, state->server_token, &snapshot)) {
            encode_snapshot(state->server_bytes, &snapshot);
            state->server_phase = SERVER_SEND;
        }
    }
}

void peer_verify_task(peer_verify_t *state, uint64_t now_us, peer_verify_tx_fn tx, void *context,
                      peer_verify_acquire_fn acquire, peer_verify_poll_fn poll,
                      peer_verify_cancel_fn cancel) {
    expire_client(state, now_us);
    server_task(state, now_us, context, acquire, poll, cancel);
    bool request = state->client_active && !state->client_sent;
    bool response = state->server_phase == SERVER_SEND;
    if ((!request && !response) || !tx
        || (state->tx_attempted && now_us - state->last_tx_attempt_us < PEER_VERIFY_TX_INTERVAL_US))
        return;
    peer_verify_packet_t kind = response && (!request || state->prefer_response)
                                   ? PEER_VERIFY_RESPONSE : PEER_VERIFY_REQUEST;
    uint8_t payload[8] = {0};
    if (kind == PEER_VERIFY_REQUEST) {
        write_le(payload, state->client_token, 4);
        payload[4] = PEER_VERIFY_PROTOCOL;
    } else {
        write_le(payload, state->server_token, 4);
        payload[4] = state->server_next_chunk;
        memcpy(payload + 5, state->server_bytes + state->server_next_chunk * 3, 3);
    }
    state->tx_attempted = true;
    state->last_tx_attempt_us = now_us;
    if (!tx(context, kind, payload))
        return;
    /* A refused external grant must not consume this direction's turn. */
    state->prefer_response = kind == PEER_VERIFY_REQUEST;
    if (kind == PEER_VERIFY_REQUEST)
        state->client_sent = true;
    else if (++state->server_next_chunk == PEER_VERIFY_CHUNK_COUNT)
        state->server_phase = SERVER_IDLE;
}

bool peer_verify_take_result(peer_verify_t *state, verify_result_t *result) {
    if (!state->result_ready)
        return false;
    *result = state->result;
    state->result_ready = false;
    return true;
}
