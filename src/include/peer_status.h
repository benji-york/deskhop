#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PEER_STATUS_PROTOCOL 1u
#define PEER_STATUS_PACKET_SIZE 8u
#define PEER_STATUS_SNAPSHOT_SIZE 39u
#define PEER_STATUS_CHUNK_COUNT 13u
#define PEER_STATUS_TIMEOUT_US UINT64_C(500000)
#define PEER_STATUS_QUERY_INTERVAL_US UINT64_C(200000)
#define PEER_STATUS_TX_INTERVAL_US UINT64_C(1000)

/* Identity is captured at boot; uptime is sampled for each accepted request.
 * image_crc_at_boot is metadata, not an independently verified flash digest. */
typedef struct {
    uint8_t role;
    uint16_t major;
    uint16_t minor;
    uint8_t board_id[8];
    uint64_t boot_session;
    uint64_t uptime_ms;
    uint32_t image_crc_at_boot;
} peer_status_snapshot_t;

typedef enum {
    PEER_STATUS_OK,
    PEER_STATUS_TIMEOUT,
    PEER_STATUS_INVALID,
} peer_status_outcome_t;

typedef struct {
    uint32_t token;
    peer_status_outcome_t outcome;
    /* Populated only for PEER_STATUS_OK. */
    peer_status_snapshot_t snapshot;
} peer_status_result_t;

typedef enum {
    PEER_STATUS_STARTED,
    PEER_STATUS_BUSY,
    PEER_STATUS_BAD_ARGUMENT,
} peer_status_start_t;

typedef enum {
    PEER_STATUS_REQUEST,
    PEER_STATUS_RESPONSE,
} peer_status_packet_t;

/* Return true only when the complete packet was copied into the transport
 * queue. The callback must neither block nor reenter this module. */
typedef bool (*peer_status_tx_fn)(void *context, peer_status_packet_t kind,
                                 const uint8_t payload[PEER_STATUS_PACKET_SIZE]);

/* All entry points and this caller-owned state belong to one thread (core 1).
 * No entry point sends except task(), which attempts at most one packet and
 * spaces all attempts, including refusals, by at least 1 ms. Both directions
 * have independent 500 ms lifetimes, measured from start/request acceptance.
 * Times must come from the same monotonic microsecond clock. */
typedef struct {
    uint8_t local_role;
    bool client_active;
    bool client_sent;
    bool result_ready;
    uint32_t client_token;
    uint64_t client_started_us;
    uint16_t client_received;
    uint8_t client_bytes[PEER_STATUS_SNAPSHOT_SIZE];
    peer_status_result_t result;

    bool server_active;
    bool server_seen_request;
    uint32_t server_token;
    uint64_t server_started_us;
    uint8_t server_next_chunk;
    uint8_t server_bytes[PEER_STATUS_SNAPSHOT_SIZE];

    bool tx_attempted;
    bool prefer_response;
    uint64_t last_tx_attempt_us;
} peer_status_t;

void peer_status_init(peer_status_t *, uint8_t local_role);
/* Supply a fresh nonzero token for each query; do not reuse tokens in one
 * boot session. BUSY includes an unconsumed result. */
peer_status_start_t peer_status_start(peer_status_t *, uint32_t token, uint64_t now_us);
/* Malformed requests, an active server, a repeated last accepted token, and
 * requests within 200 ms of the last acceptance are ignored. Snapshot bytes
 * are copied during acceptance and cannot change while the reply is queued. */
bool peer_status_receive_request(peer_status_t *, const uint8_t payload[8],
                                 const peer_status_snapshot_t *, uint64_t now_us);
/* Unsolicited or stale-token responses are ignored. Matching malformed replies
 * end the query as INVALID. Reordering and identical duplicates are allowed. */
void peer_status_receive_response(peer_status_t *, const uint8_t payload[8], uint64_t now_us);
void peer_status_task(peer_status_t *, uint64_t now_us, peer_status_tx_fn, void *context);
bool peer_status_take_result(peer_status_t *, peer_status_result_t *);
