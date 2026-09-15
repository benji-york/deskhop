#include "peer_history.h"

#include <string.h>

enum { CLIENT_RECEIVE, CLIENT_CRC, CLIENT_HEADER, CLIENT_RECORDS };
enum { SERVER_IDLE, SERVER_BEGIN, SERVER_RECORDS, SERVER_CRC, SERVER_SEND };

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

static unsigned wire_size(unsigned limit) {
    return PEER_HISTORY_HEADER_SIZE + limit * PEER_HISTORY_RECORD_SIZE + 4;
}

static uint32_t crc_step(uint32_t crc, const uint8_t *bytes, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return crc;
}

static bool valid_window(const history_window_t *window, unsigned limit) {
    if (!window->first_seq || !window->oldest_seq || window->end_seq < window->first_seq
        || window->oldest_seq > window->first_seq || window->count > limit
        || window->end_seq - window->first_seq != window->count
        || window->end_seq - window->oldest_seq > HISTORY_CAPACITY
        || window->overwritten == UINT64_MAX
        || window->oldest_seq != window->overwritten + 1)
        return false;
    uint64_t retained = window->end_seq - window->oldest_seq;
    unsigned selected = retained < limit ? (unsigned)retained : limit;
    return window->count == selected
           && (!window->overwritten || retained == HISTORY_CAPACITY)
           && (window->count || window->end_seq == 1);
}

static void finish_client(peer_history_t *state, peer_history_outcome_t outcome) {
    state->client_active = false;
    state->result.token = state->client_token;
    state->result.outcome = outcome;
    state->result.requested_at_us = state->client_started_us;
    state->result.first_response_us = state->client_first_response_us;
    state->result_ready = true;
}

static void expire(peer_history_t *state, uint64_t now_us) {
    if (state->client_active && now_us - state->client_started_us >= PEER_HISTORY_TIMEOUT_US)
        finish_client(state, PEER_HISTORY_TIMEOUT);
    if (state->server_phase != SERVER_IDLE
        && now_us - state->server_started_us >= PEER_HISTORY_TIMEOUT_US)
        state->server_phase = SERVER_IDLE;
}

void peer_history_init(peer_history_t *state, uint8_t role, uint64_t boot_session) {
    *state = (peer_history_t){.local_role = role, .boot_session = boot_session};
}

peer_history_start_t peer_history_start(peer_history_t *state, uint32_t token, unsigned count,
                                        uint64_t requested_at_us) {
    if (state->client_active || state->result_ready)
        return PEER_HISTORY_BUSY;
    if (!token || count == 0 || count > PEER_HISTORY_MAX_COUNT || state->local_role > 1)
        return PEER_HISTORY_BAD_ARGUMENT;
    state->client_active = true;
    state->client_sent = false;
    state->client_phase = CLIENT_RECEIVE;
    state->client_limit = (uint8_t)count;
    state->client_token = token;
    state->client_started_us = requested_at_us;
    state->client_first_response_us = 0;
    state->client_received_count = 0;
    state->client_wire_size = wire_size(count);
    state->client_crc = UINT32_MAX;
    state->client_crc_offset = 0;
    state->client_validate_index = 0;
    state->client_previous_time_us = 0;
    memset(state->client_received, 0, sizeof(state->client_received));
    return PEER_HISTORY_STARTED;
}

bool peer_history_receive_request(peer_history_t *state, const uint8_t payload[8], uint64_t now_us) {
    expire(state, now_us);
    uint32_t token = (uint32_t)read_le(payload, 4);
    unsigned count = payload[5];
    if (!token || payload[4] != PEER_HISTORY_PROTOCOL || !count
        || count > PEER_HISTORY_MAX_COUNT || payload[6] || payload[7]
        || state->local_role > 1 || state->server_phase != SERVER_IDLE)
        return false;
    if (state->server_seen_request
        && (token == state->server_token
            || now_us - state->server_started_us < PEER_HISTORY_REQUEST_INTERVAL_US))
        return false;
    state->server_seen_request = true;
    state->server_token = token;
    state->server_limit = (uint8_t)count;
    state->server_started_us = now_us;
    state->server_phase = SERVER_BEGIN;
    state->server_capture_index = 0;
    state->server_next_chunk = 0;
    state->server_wire_size = wire_size(count);
    state->server_gap_mask = 0;
    state->server_crc = UINT32_MAX;
    state->server_crc_offset = 0;
    return true;
}

void peer_history_receive_response(peer_history_t *state, const uint8_t payload[8], uint64_t now_us) {
    expire(state, now_us);
    if (!state->client_active || !state->client_sent
        || read_le(payload, 4) != state->client_token)
        return;
    unsigned index = (unsigned)read_le(payload + 4, 2);
    if (index >= state->client_wire_size / 2) {
        finish_client(state, PEER_HISTORY_INVALID);
        return;
    }
    uint8_t mask = (uint8_t)(1u << (index % 8));
    uint8_t *dest = state->client_bytes + index * 2;
    if (state->client_received[index / 8] & mask) {
        if (memcmp(dest, payload + 6, 2) != 0)
            finish_client(state, PEER_HISTORY_INVALID);
        return;
    }
    if (!state->client_received_count)
        state->client_first_response_us = now_us;
    memcpy(dest, payload + 6, 2);
    state->client_received[index / 8] |= mask;
    if (++state->client_received_count == state->client_wire_size / 2)
        state->client_phase = CLIENT_CRC;
}

static void encode_header(peer_history_t *state) {
    uint8_t *bytes = state->server_bytes;
    memset(bytes, 0, PEER_HISTORY_HEADER_SIZE);
    bytes[0] = PEER_HISTORY_PROTOCOL;
    bytes[1] = state->local_role;
    bytes[2] = (uint8_t)state->server_window.count;
    bytes[3] = PEER_HISTORY_RECORD_SIZE;
    write_le(bytes + 8, state->boot_session, 8);
    write_le(bytes + 16, state->server_sampled_at_us, 8);
    write_le(bytes + 24, state->server_window.first_seq, 8);
    write_le(bytes + 32, state->server_window.end_seq, 8);
    write_le(bytes + 40, state->server_window.oldest_seq, 8);
    write_le(bytes + 48, state->server_window.overwritten, 8);
}

static void encode_record(uint8_t *bytes, const history_event_t *event) {
    write_le(bytes, event->seq, 8);
    write_le(bytes + 8, event->time_us, 8);
    write_le(bytes + 16, event->value, 4);
    bytes[20] = event->type;
    bytes[21] = event->a;
    bytes[22] = event->b;
    bytes[23] = event->reserved;
}

static void server_work(peer_history_t *state, peer_history_begin_fn begin,
                         peer_history_read_fn read, void *context) {
    if (state->server_phase == SERVER_BEGIN) {
        if (!begin || !read)
            return;
        state->server_window = begin(context, state->server_limit, &state->server_sampled_at_us);
        if (!valid_window(&state->server_window, state->server_limit)) {
            state->server_phase = SERVER_IDLE;
            return;
        }
        encode_header(state);
        state->server_phase = SERVER_RECORDS;
    } else if (state->server_phase == SERVER_RECORDS) {
        if (!read)
            return;
        unsigned index = state->server_capture_index++;
        uint8_t *bytes = state->server_bytes + PEER_HISTORY_HEADER_SIZE
                         + index * PEER_HISTORY_RECORD_SIZE;
        history_event_t event;
        if (index >= state->server_window.count) {
            memset(bytes, 0, PEER_HISTORY_RECORD_SIZE);
        } else if (!read(context, state->server_window.first_seq + index, &event)) {
            state->server_gap_mask |= UINT64_C(1) << index;
            memset(bytes, 0, PEER_HISTORY_RECORD_SIZE);
        } else {
            encode_record(bytes, &event);
        }
        if (state->server_capture_index == state->server_limit) {
            write_le(state->server_bytes + 56, state->server_gap_mask, 8);
            state->server_phase = SERVER_CRC;
        }
    } else if (state->server_phase == SERVER_CRC) {
        unsigned remaining = state->server_wire_size - 4 - state->server_crc_offset;
        unsigned count = remaining < 64 ? remaining : 64;
        state->server_crc = crc_step(state->server_crc,
                                     state->server_bytes + state->server_crc_offset, count);
        state->server_crc_offset += count;
        if (state->server_crc_offset == state->server_wire_size - 4) {
            write_le(state->server_bytes + state->server_crc_offset, ~state->server_crc, 4);
            state->server_phase = SERVER_SEND;
        }
    }
}

static bool decode_header(peer_history_t *state) {
    const uint8_t *bytes = state->client_bytes;
    peer_history_snapshot_t *snapshot = &state->result.snapshot;
    if (bytes[0] != PEER_HISTORY_PROTOCOL || bytes[1] != (state->local_role ^ 1u)
        || bytes[3] != PEER_HISTORY_RECORD_SIZE || read_le(bytes + 4, 4))
        return false;
    snapshot->role = bytes[1];
    snapshot->boot_session = read_le(bytes + 8, 8);
    snapshot->sampled_at_us = read_le(bytes + 16, 8);
    snapshot->window = (history_window_t){
        .first_seq = read_le(bytes + 24, 8),
        .end_seq = read_le(bytes + 32, 8),
        .oldest_seq = read_le(bytes + 40, 8),
        .overwritten = read_le(bytes + 48, 8),
        .count = bytes[2],
    };
    if (!valid_window(&snapshot->window, state->client_limit))
        return false;
    snapshot->gap_mask = read_le(bytes + 56, 8);
    uint64_t used = snapshot->window.count == 64 ? UINT64_MAX
                    : (UINT64_C(1) << snapshot->window.count) - 1;
    return (snapshot->gap_mask & ~used) == 0;
}

static bool decode_record(peer_history_t *state, unsigned index) {
    peer_history_snapshot_t *snapshot = &state->result.snapshot;
    history_event_t *event = &snapshot->events[index];
    if (index >= state->client_limit) {
        *event = (history_event_t){0};
        return true;
    }
    const uint8_t *bytes = state->client_bytes + PEER_HISTORY_HEADER_SIZE
                           + index * PEER_HISTORY_RECORD_SIZE;
    if (index >= snapshot->window.count || (snapshot->gap_mask & (UINT64_C(1) << index))) {
        for (unsigned i = 0; i < PEER_HISTORY_RECORD_SIZE; ++i)
            if (bytes[i])
                return false;
        *event = (history_event_t){0};
        return true;
    }
    *event = (history_event_t){
        .seq = read_le(bytes, 8),
        .time_us = read_le(bytes + 8, 8),
        .value = (uint32_t)read_le(bytes + 16, 4),
        .type = bytes[20], .a = bytes[21], .b = bytes[22], .reserved = bytes[23],
    };
    if (event->seq != snapshot->window.first_seq + index
        || event->type < HISTORY_BOOT || event->type > HISTORY_UART_DROPPED
        || event->reserved || event->time_us > snapshot->sampled_at_us
        || event->time_us < state->client_previous_time_us)
        return false;
    state->client_previous_time_us = event->time_us;
    return true;
}

static void client_work(peer_history_t *state) {
    if (!state->client_active)
        return;
    if (state->client_phase == CLIENT_CRC) {
        unsigned remaining = state->client_wire_size - 4 - state->client_crc_offset;
        unsigned count = remaining < 64 ? remaining : 64;
        state->client_crc = crc_step(state->client_crc,
                                     state->client_bytes + state->client_crc_offset, count);
        state->client_crc_offset += count;
        if (state->client_crc_offset == state->client_wire_size - 4) {
            if (read_le(state->client_bytes + state->client_crc_offset, 4) != ~state->client_crc)
                finish_client(state, PEER_HISTORY_INVALID);
            else
                state->client_phase = CLIENT_HEADER;
        }
    } else if (state->client_phase == CLIENT_HEADER) {
        if (!decode_header(state))
            finish_client(state, PEER_HISTORY_INVALID);
        else
            state->client_phase = CLIENT_RECORDS;
    } else if (state->client_phase == CLIENT_RECORDS) {
        if (!decode_record(state, state->client_validate_index++))
            finish_client(state, PEER_HISTORY_INVALID);
        else if (state->client_validate_index == PEER_HISTORY_MAX_COUNT)
            finish_client(state, PEER_HISTORY_OK);
    }
}

void peer_history_task(peer_history_t *state, uint64_t now_us, peer_history_tx_fn tx, void *context,
                       peer_history_begin_fn begin, peer_history_read_fn read) {
    expire(state, now_us);
    server_work(state, begin, read, context);
    client_work(state);
    bool request = state->client_active && !state->client_sent;
    bool response = state->server_phase == SERVER_SEND;
    if ((!request && !response) || !tx
        || (state->tx_attempted && now_us - state->last_tx_attempt_us < PEER_HISTORY_TX_INTERVAL_US))
        return;
    peer_history_packet_t kind = response && (!request || state->prefer_response)
                                  ? PEER_HISTORY_RESPONSE : PEER_HISTORY_REQUEST;
    uint8_t payload[8] = {0};
    if (kind == PEER_HISTORY_REQUEST) {
        write_le(payload, state->client_token, 4);
        payload[4] = PEER_HISTORY_PROTOCOL;
        payload[5] = state->client_limit;
    } else {
        write_le(payload, state->server_token, 4);
        write_le(payload + 4, state->server_next_chunk, 2);
        memcpy(payload + 6, state->server_bytes + state->server_next_chunk * 2, 2);
    }
    state->tx_attempted = true;
    state->last_tx_attempt_us = now_us;
    if (!tx(context, kind, payload))
        return;
    /* Keep a refused candidate preferred. Otherwise an alternating external
     * arbiter can deny every request and grant only response candidates. */
    state->prefer_response = kind == PEER_HISTORY_REQUEST;
    if (kind == PEER_HISTORY_REQUEST)
        state->client_sent = true;
    else if (++state->server_next_chunk == state->server_wire_size / 2)
        state->server_phase = SERVER_IDLE;
}

const peer_history_result_t *peer_history_peek_result(const peer_history_t *state) {
    return state->result_ready ? &state->result : NULL;
}

void peer_history_release_result(peer_history_t *state) {
    state->result_ready = false;
}
