/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "diagnostic_peer.h"
#include "diagnostic_peer_history.h"
#include "diagnostic_history.h"

typedef struct { uint32_t token; unsigned count; uint64_t requested_at_us; } request_t;
typedef struct { uint32_t token; const peer_history_result_t *result; } notice_t;
static queue_t requests, notices, releases;
static bool initialized;
/* Core 1 owns protocol and publication state. The result is immutable between
 * publishing its descriptor and receiving its release token. SDK queue locks
 * provide the memory barriers for ownership transfer in both directions. */
static peer_history_t protocol;
static bool notified, query_completed;
static uint64_t last_query_completed_us;
/* Core 0 owns this borrowed pointer and the release retry state. */
static const peer_history_result_t *borrowed;
static bool release_pending;

void diagnostic_peer_history_shutdown(void) {
    if (initialized) {
        queue_free(&requests);
        queue_free(&notices);
        queue_free(&releases);
        initialized = false;
    }
    borrowed = NULL;
    release_pending = false;
}

void diagnostic_peer_history_init(const peer_status_snapshot_t *identity) {
    diagnostic_peer_history_shutdown();
    peer_history_init(&protocol, identity->role, identity->boot_session);
    notified = query_completed = false;
    queue_init(&requests, sizeof(request_t), 1);
    queue_init(&notices, sizeof(notice_t), 1);
    queue_init(&releases, sizeof(uint32_t), 1);
    initialized = true;
}

static void flush_release(void) {
    if (release_pending && borrowed) {
        uint32_t token = borrowed->token;
        if (queue_try_add(&releases, &token)) {
            borrowed = NULL;
            release_pending = false;
        }
    }
}

void diagnostic_peer_history_release(void) {
    if (!initialized || !borrowed)
        return;
    release_pending = true;
    flush_release();
}

const peer_history_result_t *diagnostic_peer_history_poll(void) {
    if (!initialized)
        return NULL;
    flush_release();
    if (borrowed)
        return NULL;
    notice_t notice;
    if (!queue_try_remove(&notices, &notice))
        return NULL;
    borrowed = notice.result;
    return borrowed;
}

bool diagnostic_peer_history_request(uint32_t token, unsigned count, uint64_t requested_at_us) {
    if (!initialized || !token || count < 1 || count > PEER_HISTORY_MAX_COUNT)
        return false;
    flush_release();
    if (borrowed)
        return false;
    /* Discard at most one completed query from an abandoned terminal session.
     * An in-flight old query finishes under its original deadline. */
    if (diagnostic_peer_history_poll())
        diagnostic_peer_history_release();
    if (borrowed)
        return false;
    request_t request = {.token = token, .count = count, .requested_at_us = requested_at_us};
    return queue_try_add(&requests, &request);
}

static bool transmit(void *context, peer_history_packet_t kind, const uint8_t payload[8]) {
    uint64_t now_us = *(const uint64_t *)context;
    return diagnostic_peer_tx_try(kind == PEER_HISTORY_REQUEST
                                     ? DIAGNOSTIC_HISTORY_REQUEST_MSG
                                     : DIAGNOSTIC_HISTORY_RESPONSE_MSG, payload, now_us);
}

static history_window_t begin(void *context, unsigned count, uint64_t *sampled_at_us) {
    (void)context;
    return diagnostic_history_window_at(count, sampled_at_us);
}

static bool read_event(void *context, uint64_t seq, history_event_t *event) {
    (void)context;
    return diagnostic_history_read(seq, event);
}

void diagnostic_peer_history_task(uint64_t now_us) {
    if (!initialized)
        return;
    uint32_t released;
    if (queue_try_remove(&releases, &released)) {
        const peer_history_result_t *result = peer_history_peek_result(&protocol);
        if (notified && result && result->token == released) {
            peer_history_release_result(&protocol);
            notified = false;
        }
    }
    bool cooldown = query_completed
                    && now_us - last_query_completed_us < PEER_HISTORY_REQUEST_INTERVAL_US;
    request_t request;
    if (!cooldown && queue_try_peek(&requests, &request)) {
        peer_history_start_t started = peer_history_start(&protocol, request.token,
                                                         request.count, request.requested_at_us);
        if (started != PEER_HISTORY_BUSY)
            queue_try_remove(&requests, &request);
    }
    peer_history_task(&protocol, now_us, transmit, &now_us, begin, read_event);
    const peer_history_result_t *result = peer_history_peek_result(&protocol);
    if (result && !notified) {
        notice_t notice = {.token = result->token, .result = result};
        if (queue_try_add(&notices, &notice)) {
            notified = query_completed = true;
            last_query_completed_us = now_us;
        }
    }
}

void diagnostic_peer_history_receive(bool response, const uint8_t data[8], uint64_t now_us) {
    if (!initialized)
        return;
    if (response)
        peer_history_receive_response(&protocol, data, now_us);
    else
        peer_history_receive_request(&protocol, data, now_us);
}
