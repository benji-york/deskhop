/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "diagnostic_peer.h"

typedef struct { uint32_t token; uint64_t requested_at_us; } request_t;
static queue_t requests, results;
static bool initialized;
/* Only core 1 accesses the following state after startup. */
static peer_status_t protocol;
static peer_status_snapshot_t identity;
static peer_status_result_t pending_result;
static bool result_pending;
static bool query_completed;
static uint64_t last_query_completed_us;

void diagnostic_peer_shutdown(void) {
    if (initialized) {
        queue_free(&requests);
        queue_free(&results);
        initialized = false;
    }
}

void diagnostic_peer_init(const peer_status_snapshot_t *snapshot) {
    diagnostic_peer_shutdown();
    identity = *snapshot;
    peer_status_init(&protocol, identity.role);
    result_pending = false;
    query_completed = false;
    queue_init(&requests, sizeof(request_t), 1);
    queue_init(&results, sizeof(peer_status_result_t), 1);
    initialized = true;
}

bool diagnostic_peer_request(uint32_t token, uint64_t requested_at_us) {
    if (!initialized || !token)
        return false;
    /* Only core 0 consumes results. Discard at most one old terminal session. */
    peer_status_result_t stale;
    queue_try_remove(&results, &stale);
    request_t request = {.token = token, .requested_at_us = requested_at_us};
    return queue_try_add(&requests, &request);
}

bool diagnostic_peer_poll(peer_status_result_t *result) {
    return initialized && queue_try_remove(&results, result);
}

static bool transmit(void *unused, peer_status_packet_t kind, const uint8_t payload[8]) {
    (void)unused;
    return queue_packet_try(payload, kind == PEER_STATUS_REQUEST
                                      ? DIAGNOSTIC_STATUS_REQUEST_MSG
                                      : DIAGNOSTIC_STATUS_RESPONSE_MSG, 8);
}

void diagnostic_peer_task(uint64_t now_us) {
    if (!initialized)
        return;
    if (result_pending && queue_try_add(&results, &pending_result))
        result_pending = false;
    /* Peek rather than drop a request while the previous query completes.
       Its original core-0 timestamp still bounds its lifetime. */
    request_t request;
    /* The server limits requests to one per 200 ms. Pacing from completion
       prevents a promptly repeated status command from being silently ignored;
       this also survives closing/reopening the terminal. */
    bool cooldown = query_completed
                    && now_us - last_query_completed_us < PEER_STATUS_QUERY_INTERVAL_US;
    if (!result_pending && !cooldown && queue_try_peek(&requests, &request)) {
        peer_status_start_t started = peer_status_start(&protocol, request.token,
                                                        request.requested_at_us);
        if (started != PEER_STATUS_BUSY)
            queue_try_remove(&requests, &request);
    }
    peer_status_task(&protocol, now_us, transmit, NULL);
    if (!result_pending) {
        result_pending = peer_status_take_result(&protocol, &pending_result);
        if (result_pending) {
            query_completed = true;
            last_query_completed_us = now_us;
        }
    }
}

void diagnostic_peer_receive(bool response, const uint8_t data[8], uint64_t now_us) {
    if (!initialized)
        return;
    if (response) {
        peer_status_receive_response(&protocol, data, now_us);
    } else {
        peer_status_snapshot_t snapshot = identity;
        snapshot.uptime_ms = now_us / 1000;
        peer_status_receive_request(&protocol, data, &snapshot, now_us);
    }
}
