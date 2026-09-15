#pragma once

#include "history.h"

#define PEER_HISTORY_PROTOCOL 2u
#define PEER_HISTORY_LEGACY_PROTOCOL 1u
#define PEER_HISTORY_LEGACY_EVENT_MAX 10u
#define PEER_HISTORY_MAX_COUNT 64u
#define PEER_HISTORY_HEADER_SIZE 64u
#define PEER_HISTORY_RECORD_SIZE 24u
#define PEER_HISTORY_MAX_WIRE_SIZE 1604u
#define PEER_HISTORY_MAX_CHUNKS (PEER_HISTORY_MAX_WIRE_SIZE / 2u)
#define PEER_HISTORY_REQUEST_INTERVAL_US UINT64_C(200000)
#define PEER_HISTORY_TX_INTERVAL_US UINT64_C(1000)
#define PEER_HISTORY_TIMEOUT_US UINT64_C(3000000)
#define PEER_HISTORY_FALLBACK_US UINT64_C(250000)

typedef struct {
    uint8_t role, protocol;
    uint64_t boot_session, sampled_at_us;
    history_window_t window;
    uint64_t gap_mask;
    history_event_t events[PEER_HISTORY_MAX_COUNT];
} peer_history_snapshot_t;

typedef enum {
    PEER_HISTORY_OK,
    PEER_HISTORY_TIMEOUT,
    PEER_HISTORY_INVALID,
} peer_history_outcome_t;

typedef struct {
    uint32_t token;
    peer_history_outcome_t outcome;
    uint64_t requested_at_us, first_response_us;
    /* Valid only for OK; immutable from peek_result() until release_result(). */
    peer_history_snapshot_t snapshot;
} peer_history_result_t;

typedef enum {
    PEER_HISTORY_STARTED,
    PEER_HISTORY_BUSY,
    PEER_HISTORY_BAD_ARGUMENT,
} peer_history_start_t;

typedef enum { PEER_HISTORY_REQUEST, PEER_HISTORY_RESPONSE } peer_history_packet_t;

typedef bool (*peer_history_tx_fn)(void *context, peer_history_packet_t kind,
                                   const uint8_t payload[8]);
typedef history_window_t (*peer_history_begin_fn)(void *context, unsigned limit,
                                                  uint64_t *sampled_at_us);
typedef bool (*peer_history_read_fn)(void *context, uint64_t seq, history_event_t *event);

/* Single-owner core-1 state. Callbacks must not block or reenter the module.
 * begin/read are called only by task(); read copies at most one event per tick.
 * Each direction computes at most 64 CRC bytes per tick. TX attempts, including
 * refusals, are spaced by 1 ms. A borrowed result never blocks the server.
 * All timestamps use one monotonic microsecond clock. */
typedef struct {
    uint8_t local_role;
    uint64_t boot_session;
    bool client_active, client_sent, result_ready;
    uint8_t client_phase, client_limit, client_validate_index, client_protocol;
    uint32_t client_token, client_wire_token, client_crc;
    uint64_t client_started_us, client_sent_at_us, client_first_response_us, client_previous_time_us;
    unsigned client_wire_size, client_received_count, client_crc_offset;
    uint8_t client_received[(PEER_HISTORY_MAX_CHUNKS + 7u) / 8u];
    uint8_t client_bytes[PEER_HISTORY_MAX_WIRE_SIZE];
    peer_history_result_t result;

    bool server_seen_request;
    uint8_t server_phase, server_limit, server_capture_index, server_protocol;
    uint32_t server_token, server_crc;
    uint64_t server_started_us, server_sampled_at_us, server_gap_mask;
    unsigned server_wire_size, server_next_chunk, server_crc_offset;
    history_window_t server_window;
    uint8_t server_bytes[PEER_HISTORY_MAX_WIRE_SIZE];

    bool tx_attempted, prefer_response;
    uint64_t last_tx_attempt_us;
} peer_history_t;

void peer_history_init(peer_history_t *, uint8_t role, uint64_t boot_session);
peer_history_start_t peer_history_start(peer_history_t *, uint32_t token, unsigned count,
                                        uint64_t requested_at_us);
bool peer_history_receive_request(peer_history_t *, const uint8_t payload[8], uint64_t now_us);
void peer_history_receive_response(peer_history_t *, const uint8_t payload[8], uint64_t now_us);
void peer_history_task(peer_history_t *, uint64_t now_us, peer_history_tx_fn, void *context,
                       peer_history_begin_fn, peer_history_read_fn);
const peer_history_result_t *peer_history_peek_result(const peer_history_t *);
void peer_history_release_result(peer_history_t *);
