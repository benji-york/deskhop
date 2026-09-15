#include "peer_status.h"

#include <string.h>

static uint64_t read_le(const uint8_t *bytes, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static void write_le(uint8_t *bytes, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t snapshot_crc(const uint8_t *bytes) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 34; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

static void encode_snapshot(uint8_t bytes[PEER_STATUS_SNAPSHOT_SIZE],
                            const peer_status_snapshot_t *snapshot) {
    bytes[0] = PEER_STATUS_PROTOCOL;
    bytes[1] = snapshot->role;
    write_le(bytes + 2, snapshot->major, 2);
    write_le(bytes + 4, snapshot->minor, 2);
    memcpy(bytes + 6, snapshot->board_id, 8);
    write_le(bytes + 14, snapshot->boot_session, 8);
    write_le(bytes + 22, snapshot->uptime_ms, 8);
    write_le(bytes + 30, snapshot->image_crc_at_boot, 4);
    write_le(bytes + 34, snapshot_crc(bytes), 4);
    bytes[38] = 0;
}

static void decode_snapshot(peer_status_snapshot_t *snapshot, const uint8_t *bytes) {
    *snapshot = (peer_status_snapshot_t){
        .role = bytes[1],
        .major = (uint16_t)read_le(bytes + 2, 2),
        .minor = (uint16_t)read_le(bytes + 4, 2),
        .boot_session = read_le(bytes + 14, 8),
        .uptime_ms = read_le(bytes + 22, 8),
        .image_crc_at_boot = (uint32_t)read_le(bytes + 30, 4),
    };
    memcpy(snapshot->board_id, bytes + 6, 8);
}

static void finish_client(peer_status_t *state, peer_status_outcome_t outcome) {
    state->result = (peer_status_result_t){.token = state->client_token, .outcome = outcome};
    if (outcome == PEER_STATUS_OK)
        decode_snapshot(&state->result.snapshot, state->client_bytes);
    state->client_active = false;
    state->result_ready = true;
}

static void expire(peer_status_t *state, uint64_t now_us) {
    if (state->client_active && now_us - state->client_started_us >= PEER_STATUS_TIMEOUT_US)
        finish_client(state, PEER_STATUS_TIMEOUT);
    if (state->server_active && now_us - state->server_started_us >= PEER_STATUS_TIMEOUT_US)
        state->server_active = false;
}

void peer_status_init(peer_status_t *state, uint8_t local_role) {
    *state = (peer_status_t){.local_role = local_role};
}

peer_status_start_t peer_status_start(peer_status_t *state, uint32_t token, uint64_t now_us) {
    if (state->client_active || state->result_ready)
        return PEER_STATUS_BUSY;
    if (!token || state->local_role > 1)
        return PEER_STATUS_BAD_ARGUMENT;
    state->client_active = true;
    state->client_sent = false;
    state->client_token = token;
    state->client_started_us = now_us;
    state->client_received = 0;
    memset(state->client_bytes, 0, sizeof(state->client_bytes));
    return PEER_STATUS_STARTED;
}

bool peer_status_receive_request(peer_status_t *state, const uint8_t payload[8],
                                 const peer_status_snapshot_t *snapshot, uint64_t now_us) {
    expire(state, now_us);
    uint32_t token = (uint32_t)read_le(payload, 4);
    if (!token || payload[4] != PEER_STATUS_PROTOCOL || payload[5] || payload[6] || payload[7]
        || state->local_role > 1 || snapshot->role != state->local_role || state->server_active)
        return false;
    if (state->server_seen_request
        && (token == state->server_token
            || now_us - state->server_started_us < PEER_STATUS_QUERY_INTERVAL_US))
        return false;
    state->server_seen_request = true;
    state->server_active = true;
    state->server_token = token;
    state->server_started_us = now_us;
    state->server_next_chunk = 0;
    encode_snapshot(state->server_bytes, snapshot);
    return true;
}

void peer_status_receive_response(peer_status_t *state, const uint8_t payload[8], uint64_t now_us) {
    expire(state, now_us);
    if (!state->client_active || !state->client_sent
        || read_le(payload, 4) != state->client_token)
        return;
    unsigned index = payload[4];
    if (index >= PEER_STATUS_CHUNK_COUNT
        || (index == 0 && (payload[5] != PEER_STATUS_PROTOCOL
                          || payload[6] != (state->local_role ^ 1u)))
        || (index == PEER_STATUS_CHUNK_COUNT - 1 && payload[7] != 0)) {
        finish_client(state, PEER_STATUS_INVALID);
        return;
    }
    uint16_t mask = (uint16_t)(1u << index);
    uint8_t *dest = state->client_bytes + index * 3;
    if (state->client_received & mask) {
        if (memcmp(dest, payload + 5, 3) != 0)
            finish_client(state, PEER_STATUS_INVALID);
        return;
    }
    memcpy(dest, payload + 5, 3);
    state->client_received |= mask;
    if (state->client_received == (1u << PEER_STATUS_CHUNK_COUNT) - 1u) {
        bool valid = read_le(state->client_bytes + 34, 4) == snapshot_crc(state->client_bytes);
        finish_client(state, valid ? PEER_STATUS_OK : PEER_STATUS_INVALID);
    }
}

void peer_status_task(peer_status_t *state, uint64_t now_us, peer_status_tx_fn tx, void *context) {
    expire(state, now_us);
    bool request = state->client_active && !state->client_sent;
    bool response = state->server_active;
    if ((!request && !response) || !tx
        || (state->tx_attempted && now_us - state->last_tx_attempt_us < PEER_STATUS_TX_INTERVAL_US))
        return;

    peer_status_packet_t kind = response && (!request || state->prefer_response)
                                   ? PEER_STATUS_RESPONSE : PEER_STATUS_REQUEST;
    uint8_t payload[PEER_STATUS_PACKET_SIZE] = {0};
    if (kind == PEER_STATUS_REQUEST) {
        write_le(payload, state->client_token, 4);
        payload[4] = PEER_STATUS_PROTOCOL;
    } else {
        write_le(payload, state->server_token, 4);
        payload[4] = state->server_next_chunk;
        memcpy(payload + 5, state->server_bytes + state->server_next_chunk * 3, 3);
    }
    state->tx_attempted = true;
    state->last_tx_attempt_us = now_us;
    if (!tx(context, kind, payload))
        return;
    /* A denied shared UART slot must not consume this direction's turn. */
    state->prefer_response = kind == PEER_STATUS_REQUEST;
    if (kind == PEER_STATUS_REQUEST)
        state->client_sent = true;
    else if (++state->server_next_chunk == PEER_STATUS_CHUNK_COUNT)
        state->server_active = false;
}

bool peer_status_take_result(peer_status_t *state, peer_status_result_t *result) {
    if (!state->result_ready)
        return false;
    *result = state->result;
    state->result_ready = false;
    return true;
}
