/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "diagnostic_peer.h"
#include "diagnostic_peer_history.h"
#include "diagnostic_history.h"
#include "peer_observation.h"
#include "diagnostic_verify.h"

typedef struct { uint32_t token; uint64_t requested_at_us; } request_t;
static queue_t requests, results;
static bool initialized;
/* Only core 1 accesses the following state after startup. */
static peer_status_t protocol;
static peer_status_snapshot_t identity;
static peer_observation_t observations;
static bool progress_recorded;
static peer_status_result_t pending_result;
static bool result_pending;
static bool query_completed;
static uint64_t last_query_completed_us;
static bool tx_attempted;
static unsigned first_service;
static uint64_t last_tx_attempt_us;

void diagnostic_peer_shutdown(void) {
    diagnostic_verify_shutdown();
    diagnostic_peer_history_shutdown();
    if (initialized) {
        queue_free(&requests);
        queue_free(&results);
        initialized = false;
    }
}

void diagnostic_peer_init(const peer_status_snapshot_t *snapshot) {
    diagnostic_peer_shutdown();
    identity = *snapshot;
    peer_observation_init(&observations);
    progress_recorded = false;
    peer_status_init(&protocol, identity.role);
    result_pending = false;
    query_completed = false;
    tx_attempted = false;
    first_service = 0;
    queue_init(&requests, sizeof(request_t), 1);
    queue_init(&results, sizeof(peer_status_result_t), 1);
    initialized = true;
    diagnostic_peer_history_init(snapshot);
    diagnostic_verify_init(snapshot);
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

bool diagnostic_peer_tx_try(uint8_t type, const uint8_t payload[8], uint64_t now_us) {
    if (!initialized || (tx_attempted && now_us - last_tx_attempt_us < 1000))
        return false;
    tx_attempted = true;
    last_tx_attempt_us = now_us;
    return queue_packet_try(payload, type, 8);
}

static bool transmit(void *context, peer_status_packet_t kind, const uint8_t payload[8]) {
    return diagnostic_peer_tx_try(kind == PEER_STATUS_REQUEST
                                     ? DIAGNOSTIC_STATUS_REQUEST_MSG
                                     : DIAGNOSTIC_STATUS_RESPONSE_MSG,
                                  payload, *(const uint64_t *)context);
}

static void status_task(uint64_t now_us) {
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
    peer_status_task(&protocol, now_us, transmit, &now_us);
    if (!result_pending) {
        result_pending = peer_status_take_result(&protocol, &pending_result);
        if (result_pending) {
            if (pending_result.outcome == PEER_STATUS_OK) {
                pending_result.observation = peer_observation_accept(&observations,
                                                                     &pending_result.snapshot);
                diagnostic_peer_observation_t observation = pending_result.observation;
                uint8_t role = pending_result.snapshot.role;
                uint32_t version = (uint32_t)pending_result.snapshot.major * 1000
                                   + pending_result.snapshot.minor + 100;
                if (observation.boot != DIAGNOSTIC_PEER_SAME_BOOT) {
                    progress_recorded = false;
                    diagnostic_history_record(HISTORY_PEER_OBSERVED, role, observation.boot, version);
                }
                if (!progress_recorded && observation.progress == DIAGNOSTIC_PROGRESS_ADVANCING) {
                    diagnostic_history_record(HISTORY_PEER_PROGRESS, role, observation.progress, version);
                    progress_recorded = true;
                }
            }
            query_completed = true;
            last_query_completed_us = now_us;
        }
    }
}

void diagnostic_peer_task(uint64_t now_us) {
    if (!initialized)
        return;
    /* All services get CPU work every tick; rotate UART priority across all
     * three while preserving one shared attempt per millisecond. */
    for (unsigned n = 0; n < 3; ++n) {
        switch ((first_service + n) % 3) {
        case 0: status_task(now_us); break;
        case 1: diagnostic_peer_history_task(now_us); break;
        case 2: diagnostic_verify_task(now_us); break;
        }
    }
    first_service = (first_service + 1) % 3;
}

void diagnostic_peer_receive(bool response, const uint8_t data[8], uint64_t now_us) {
    if (!initialized)
        return;
    if (response) {
        peer_status_receive_response(&protocol, data, now_us);
    } else {
        peer_status_snapshot_t snapshot = identity;
        snapshot.uptime_ms = now_us / 1000;
        snapshot.protocol = PEER_STATUS_PROTOCOL;
        snapshot.runtime = diagnostic_runtime_snapshot();
        peer_status_receive_request(&protocol, data, &snapshot, now_us);
    }
}
