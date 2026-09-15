/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "verification.h"

#define PEER_VERIFY_PROTOCOL 1u
#define PEER_VERIFY_PACKET_SIZE 8u
#define PEER_VERIFY_SNAPSHOT_SIZE 132u
#define PEER_VERIFY_CHUNK_COUNT 44u
#define PEER_VERIFY_TIMEOUT_US VERIFY_TIMEOUT_US
#define PEER_VERIFY_QUERY_INTERVAL_US UINT64_C(200000)
#define PEER_VERIFY_TX_INTERVAL_US UINT64_C(1000)

typedef enum { PEER_VERIFY_STARTED, PEER_VERIFY_BUSY, PEER_VERIFY_BAD_ARGUMENT } peer_verify_start_t;
typedef enum { PEER_VERIFY_REQUEST, PEER_VERIFY_RESPONSE } peer_verify_packet_t;

/* TX returns true only after copying the complete packet into its queue. */
typedef bool (*peer_verify_tx_fn)(void *, peer_verify_packet_t, const uint8_t payload[8]);
/* Acquire may return false while the shared scanner is busy. Poll returns true
 * only after copying and releasing the completed scanner result. Cancel releases
 * an acquired scan that did not complete before the server deadline. */
typedef bool (*peer_verify_acquire_fn)(void *, uint32_t token, uint64_t requested_us);
typedef bool (*peer_verify_poll_fn)(void *, uint32_t token, verify_snapshot_t *);
typedef void (*peer_verify_cancel_fn)(void *, uint32_t token);

/* All state and entry points belong to one thread (core 1). Callbacks must not
 * block or reenter. Provider callbacks run only in task(): at most one acquire
 * or poll per call. The provider advances the scanner separately. Each direction
 * has a fixed three-second lifetime; the client retains its original caller
 * timestamp, including time waiting for UART service. Only task transmits, at
 * most one attempt per millisecond including refusals. Results own their bytes. */
typedef struct {
    uint8_t local_role;
    bool client_active, client_sent, result_ready;
    uint32_t client_token;
    uint64_t client_started_us, client_received;
    uint8_t client_bytes[PEER_VERIFY_SNAPSHOT_SIZE];
    verify_result_t result;

    uint8_t server_phase, server_next_chunk;
    bool server_seen_request;
    uint32_t server_token;
    uint64_t server_started_us;
    uint8_t server_bytes[PEER_VERIFY_SNAPSHOT_SIZE];

    bool tx_attempted, prefer_response;
    uint64_t last_tx_attempt_us;
} peer_verify_t;

void peer_verify_init(peer_verify_t *, uint8_t role);
/* Use a fresh nonzero token for every query in a boot session. BUSY includes an
 * unconsumed result. No backwards-compatible fallback exists for verification. */
peer_verify_start_t peer_verify_start(peer_verify_t *, uint32_t token, uint64_t requested_us);
/* Malformed requests, active service, repeated last accepted token, and requests
 * within 200 ms of the last acceptance are ignored. No provider work occurs. */
bool peer_verify_receive_request(peer_verify_t *, const uint8_t payload[8], uint64_t now_us);
/* Reordering and identical duplicates are accepted. Matching malformed replies
 * finish INVALID; stale/unsolicited replies are ignored. */
void peer_verify_receive_response(peer_verify_t *, const uint8_t payload[8], uint64_t now_us);
void peer_verify_task(peer_verify_t *, uint64_t now_us, peer_verify_tx_fn, void *context,
                      peer_verify_acquire_fn, peer_verify_poll_fn, peer_verify_cancel_fn);
bool peer_verify_take_result(peer_verify_t *, verify_result_t *);
