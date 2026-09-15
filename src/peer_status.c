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

static uint32_t snapshot_crc(const uint8_t *bytes, unsigned count) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < count; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

static uint32_t fallback_token(uint32_t token) {
    uint64_t rotated = (uint64_t)token + UINT32_C(0x80000000);
    if (rotated > UINT32_MAX)
        rotated -= UINT32_MAX;
    return (uint32_t)rotated;
}

static unsigned chunk_count(uint8_t protocol) {
    return protocol == PEER_STATUS_LEGACY_PROTOCOL ? PEER_STATUS_LEGACY_CHUNK_COUNT
                                                    : PEER_STATUS_CHUNK_COUNT;
}

static void encode_snapshot(uint8_t bytes[PEER_STATUS_SNAPSHOT_SIZE],
                             const peer_status_snapshot_t *snapshot, uint8_t protocol) {
    memset(bytes, 0, PEER_STATUS_SNAPSHOT_SIZE);
    bytes[0] = protocol;
    bytes[1] = snapshot->role;
    write_le(bytes + 2, snapshot->major, 2);
    write_le(bytes + 4, snapshot->minor, 2);
    memcpy(bytes + 6, snapshot->board_id, 8);
    write_le(bytes + 14, snapshot->boot_session, 8);
    write_le(bytes + 22, snapshot->uptime_ms, 8);
    write_le(bytes + 30, snapshot->image_crc_at_boot, 4);
    if (protocol == PEER_STATUS_LEGACY_PROTOCOL) {
        write_le(bytes + 34, snapshot_crc(bytes, 34), 4);
        return;
    }
    const diagnostic_runtime_snapshot_t *runtime = &snapshot->runtime;
    bytes[34] = runtime->core_valid | (runtime->update_seen ? 4u : 0);
    bytes[35] = runtime->phase;
    write_le(bytes + 36, runtime->core_ticks[0], 4);
    write_le(bytes + 40, runtime->core_ticks[1], 4);
    write_le(bytes + 44, runtime->core_age_ms[0], 4);
    write_le(bytes + 48, runtime->core_age_ms[1], 4);
    write_le(bytes + 52, runtime->received_bytes, 4);
    write_le(bytes + 56, runtime->total_bytes, 4);
    write_le(bytes + 60, runtime->progress_age_ms, 4);
    write_le(bytes + 64, runtime->target_version, 2);
    bytes[66] = runtime->source;
    write_le(bytes + 68, runtime->update_attempt, 4);
    write_le(bytes + 72, snapshot_crc(bytes, 72), 4);
}

static bool decode_snapshot(peer_status_snapshot_t *snapshot, const uint8_t *bytes) {
    *snapshot = (peer_status_snapshot_t){
        .protocol = bytes[0], .role = bytes[1],
        .major = (uint16_t)read_le(bytes + 2, 2),
        .minor = (uint16_t)read_le(bytes + 4, 2),
        .boot_session = read_le(bytes + 14, 8),
        .uptime_ms = read_le(bytes + 22, 8),
        .image_crc_at_boot = (uint32_t)read_le(bytes + 30, 4),
    };
    memcpy(snapshot->board_id, bytes + 6, 8);
    if (snapshot->protocol == PEER_STATUS_LEGACY_PROTOCOL)
        return bytes[38] == 0 && read_le(bytes + 34, 4) == snapshot_crc(bytes, 34);
    if (bytes[34] > 7 || bytes[35] > DIAGNOSTIC_UPDATE_ABANDONED
        || bytes[66] > DIAGNOSTIC_SOURCE_USB || bytes[67] || bytes[76] || bytes[77]
        || read_le(bytes + 72, 4) != snapshot_crc(bytes, 72))
        return false;
    diagnostic_runtime_snapshot_t *runtime = &snapshot->runtime;
    runtime->core_valid = bytes[34] & 3;
    runtime->update_seen = (bytes[34] & 4) != 0;
    runtime->phase = bytes[35];
    runtime->core_ticks[0] = (uint32_t)read_le(bytes + 36, 4);
    runtime->core_ticks[1] = (uint32_t)read_le(bytes + 40, 4);
    runtime->core_age_ms[0] = (uint32_t)read_le(bytes + 44, 4);
    runtime->core_age_ms[1] = (uint32_t)read_le(bytes + 48, 4);
    runtime->received_bytes = (uint32_t)read_le(bytes + 52, 4);
    runtime->total_bytes = (uint32_t)read_le(bytes + 56, 4);
    runtime->progress_age_ms = (uint32_t)read_le(bytes + 60, 4);
    runtime->target_version = (uint16_t)read_le(bytes + 64, 2);
    runtime->source = bytes[66];
    runtime->update_attempt = (uint32_t)read_le(bytes + 68, 4);
    if (runtime->received_bytes > runtime->total_bytes || runtime->total_bytes > UINT32_C(262144))
        return false;
    return runtime->update_seen || (runtime->phase == DIAGNOSTIC_UPDATE_IDLE
           && runtime->source == DIAGNOSTIC_SOURCE_NONE && !runtime->received_bytes
           && !runtime->target_version && !runtime->update_attempt);
}

static void finish_client(peer_status_t *state, peer_status_outcome_t outcome) {
    state->result = (peer_status_result_t){.token = state->client_token, .outcome = outcome};
    if (outcome == PEER_STATUS_OK
        && !decode_snapshot(&state->result.snapshot, state->client_bytes))
        state->result.outcome = PEER_STATUS_INVALID;
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
    state->client_token = state->client_wire_token = token;
    state->client_protocol = PEER_STATUS_PROTOCOL;
    state->client_started_us = now_us;
    state->client_received = 0;
    memset(state->client_bytes, 0, sizeof(state->client_bytes));
    return PEER_STATUS_STARTED;
}

bool peer_status_receive_request(peer_status_t *state, const uint8_t payload[8],
                                 const peer_status_snapshot_t *snapshot, uint64_t now_us) {
    expire(state, now_us);
    uint32_t token = (uint32_t)read_le(payload, 4);
    if (!token || (payload[4] != PEER_STATUS_PROTOCOL && payload[4] != PEER_STATUS_LEGACY_PROTOCOL) || payload[5] || payload[6] || payload[7]
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
    state->server_chunk_count = (uint8_t)chunk_count(payload[4]);
    encode_snapshot(state->server_bytes, snapshot, payload[4]);
    return true;
}

void peer_status_receive_response(peer_status_t *state, const uint8_t payload[8], uint64_t now_us) {
    expire(state, now_us);
    if (!state->client_active || !state->client_sent
        || read_le(payload, 4) != state->client_wire_token)
        return;
    unsigned index = payload[4];
    unsigned chunks = chunk_count(state->client_protocol);
    if (index >= chunks
        || (index == 0 && (payload[5] != state->client_protocol
                          || payload[6] != (state->local_role ^ 1u)))) {
        finish_client(state, PEER_STATUS_INVALID);
        return;
    }
    uint32_t mask = UINT32_C(1) << index;
    uint8_t *dest = state->client_bytes + index * 3;
    if (state->client_received & mask) {
        if (memcmp(dest, payload + 5, 3) != 0)
            finish_client(state, PEER_STATUS_INVALID);
        return;
    }
    memcpy(dest, payload + 5, 3);
    state->client_received |= mask;
    if (state->client_received == (UINT32_C(1) << chunks) - 1u)
        finish_client(state, PEER_STATUS_OK);
}

void peer_status_task(peer_status_t *state, uint64_t now_us, peer_status_tx_fn tx, void *context) {
    expire(state, now_us);
    if (state->client_active && state->client_sent && !state->client_received
        && state->client_protocol == PEER_STATUS_PROTOCOL
        && now_us - state->client_sent_at_us >= PEER_STATUS_FALLBACK_US) {
        state->client_protocol = PEER_STATUS_LEGACY_PROTOCOL;
        state->client_wire_token = fallback_token(state->client_token);
        state->client_sent = false;
    }
    bool request = state->client_active && !state->client_sent;
    bool response = state->server_active;
    if ((!request && !response) || !tx
        || (state->tx_attempted && now_us - state->last_tx_attempt_us < PEER_STATUS_TX_INTERVAL_US))
        return;

    peer_status_packet_t kind = response && (!request || state->prefer_response)
                                   ? PEER_STATUS_RESPONSE : PEER_STATUS_REQUEST;
    uint8_t payload[PEER_STATUS_PACKET_SIZE] = {0};
    if (kind == PEER_STATUS_REQUEST) {
        write_le(payload, state->client_wire_token, 4);
        payload[4] = state->client_protocol;
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
    if (kind == PEER_STATUS_REQUEST) {
        state->client_sent = true;
        state->client_sent_at_us = now_us;
    } else if (++state->server_next_chunk == state->server_chunk_count)
        state->server_active = false;
}

bool peer_status_take_result(peer_status_t *state, peer_status_result_t *result) {
    if (!state->result_ready)
        return false;
    *result = state->result;
    state->result_ready = false;
    return true;
}
