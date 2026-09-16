/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "diagnostic_verify.h"
#include "diagnostic_peer.h"
#include "peer_verify.h"

_Static_assert(VERIFY_IMAGE_BYTES == STAGING_IMAGE_SIZE, "verification covers the complete slot");
_Static_assert(VERIFY_CHUNK_BYTES == FLASH_PAGE_SIZE, "one flash page per diagnostic tick");
typedef struct { uint32_t token; uint64_t requested_us; } request_t;
static queue_t requests, results;
static bool initialized;
/* All remaining state belongs exclusively to core1. Result queues copy values;
 * no result storage is borrowed across cores and no PASS is cached. */
static peer_verify_t peer;
static peer_status_snapshot_t identity;
static request_t local_request;
static bool local_pending, local_result_pending, peer_result_pending;
static verify_result_t local_result, peer_result;
static uint64_t local_result_requested_us;
static uint64_t last_scan_tick_us;
static bool scan_tick_seen;
static struct {
    enum { SCAN_FREE, SCAN_LOCAL, SCAN_REMOTE } owner;
    bool begun, ready;
    uint32_t token, crc;
    uint64_t requested_us;
    firmware_metadata_t initial_metadata;
    verify_snapshot_t snapshot;
} scanner;

static verify_scan_outcome_t io_outcome(firmware_verify_io_t status) {
    switch (status) {
    case FIRMWARE_VERIFY_UPDATE_ACTIVE: return VERIFY_SCAN_UPDATE_ACTIVE;
    case FIRMWARE_VERIFY_CHANGED: return VERIFY_SCAN_CHANGED;
    case FIRMWARE_VERIFY_BUSY: return VERIFY_SCAN_BUSY;
    default: return VERIFY_SCAN_CHANGED;
    }
}

static void metadata_copy(verify_snapshot_t *snapshot, const firmware_metadata_t *metadata) {
    snapshot->metadata_magic = metadata->magic;
    snapshot->metadata_version = metadata->version;
    snapshot->metadata_reserved = metadata->_reserved;
    snapshot->metadata_crc32 = metadata->checksum;
}

static void scan_start(bool remote, uint32_t token, uint64_t requested_us) {
    memset(&scanner, 0, sizeof(scanner));
    scanner.owner = remote ? SCAN_REMOTE : SCAN_LOCAL;
    scanner.token = token;
    scanner.requested_us = requested_us;
    scanner.crc = UINT32_MAX;
    verify_snapshot_t *s = &scanner.snapshot;
    s->role = identity.role;
    s->major = identity.major;
    s->minor = identity.minor;
    memcpy(s->board_id, identity.board_id, sizeof(s->board_id));
    s->boot_session = identity.boot_session;
    s->image_crc_at_boot = identity.image_crc_at_boot;
}

static void scan_finish(verify_scan_outcome_t outcome, uint64_t now_us) {
    scanner.snapshot.outcome = outcome;
    scanner.snapshot.completed_us = now_us;
    scanner.snapshot.end = diagnostic_runtime_snapshot();
    scanner.ready = true;
}

static void scan_task(uint64_t now_us) {
    if (scanner.owner == SCAN_FREE || scanner.ready)
        return;
    if (now_us - scanner.requested_us >= VERIFY_TIMEOUT_US) {
        scan_finish(VERIFY_SCAN_TIMEOUT, now_us);
        return;
    }
    /* This guard makes the copy/CRC budget hold even if another caller invokes
     * the service more often than its normal 1kHz task table entry. */
    if (scan_tick_seen && now_us - last_scan_tick_us < 1000)
        return;
    scan_tick_seen = true;
    last_scan_tick_us = now_us;
    verify_snapshot_t *s = &scanner.snapshot;
    firmware_verify_io_t status;
    if (!scanner.begun) {
        status = firmware_verify_try_start(&s->generation_start, &scanner.initial_metadata);
        if (status == FIRMWARE_VERIFY_BUSY)
            return;
        s->started_us = now_us;
        s->start = diagnostic_runtime_snapshot();
        if (status != FIRMWARE_VERIFY_OK) {
            scan_finish(io_outcome(status), now_us);
            return;
        }
        metadata_copy(s, &scanner.initial_metadata);
        scanner.begun = true;
    } else if (s->bytes_read < VERIFY_IMAGE_BYTES) {
        uint8_t page[VERIFY_CHUNK_BYTES];
        status = firmware_verify_try_read(s->generation_start, s->bytes_read, page, sizeof(page));
        if (status == FIRMWARE_VERIFY_BUSY)
            return;
        if (status != FIRMWARE_VERIFY_OK) {
            scan_finish(io_outcome(status), now_us);
            return;
        }
        /* Flash and firmware locks are released before any CRC work. */
        for (unsigned i = 0; i < sizeof(page); ++i)
            scanner.crc = crc32_iter(scanner.crc, page[i]);
        s->bytes_read += sizeof(page);
    } else {
        firmware_metadata_t metadata;
        status = firmware_verify_try_finish(s->generation_start, &metadata);
        if (status == FIRMWARE_VERIFY_BUSY)
            return;
        if (status != FIRMWARE_VERIFY_OK) {
            scan_finish(io_outcome(status), now_us);
            return;
        }
        if (memcmp(&scanner.initial_metadata, &metadata, sizeof(metadata))) {
            scan_finish(VERIFY_SCAN_CHANGED, now_us);
            return;
        }
        metadata_copy(s, &metadata);
        s->generation_end = s->generation_start;
        s->slot_crc32 = ~scanner.crc;
        scan_finish(VERIFY_SCAN_COMPLETE, now_us);
    }
}

/* One nonblocking attempt. BUSY is a pending freshness check, not a completed
 * result: leave the scan evidence unchanged and let the caller yield. */
static bool recheck(verify_snapshot_t *snapshot) {
    if (snapshot->outcome != VERIFY_SCAN_COMPLETE)
        return true;
    firmware_metadata_t metadata;
    firmware_verify_io_t status = firmware_verify_try_finish(snapshot->generation_start, &metadata);
    if (status == FIRMWARE_VERIFY_BUSY)
        return false;
    if (status != FIRMWARE_VERIFY_OK) {
        snapshot->outcome = io_outcome(status);
    } else if (metadata.magic != snapshot->metadata_magic
               || metadata.version != snapshot->metadata_version
               || metadata._reserved != snapshot->metadata_reserved
               || metadata.checksum != snapshot->metadata_crc32) {
        snapshot->outcome = VERIFY_SCAN_CHANGED;
    }
    return true;
}

static bool recheck_before_deadline(verify_snapshot_t *snapshot, uint64_t now_us,
                                    uint64_t requested_us) {
    if (snapshot->outcome == VERIFY_SCAN_COMPLETE
        && now_us - requested_us >= VERIFY_TIMEOUT_US) {
        snapshot->outcome = VERIFY_SCAN_TIMEOUT;
        return true;
    }
    return recheck(snapshot);
}

bool diagnostic_verify_recheck_local(verify_result_t *result) {
    if (!result->remote && result->transport == VERIFY_TRANSPORT_OK)
        return recheck(&result->snapshot);
    return true;
}

void diagnostic_verify_shutdown(void) {
    if (initialized) {
        queue_free(&requests);
        queue_free(&results);
        initialized = false;
    }
}

void diagnostic_verify_init(const peer_status_snapshot_t *snapshot) {
    diagnostic_verify_shutdown();
    identity = *snapshot;
    scanner.owner = SCAN_FREE;
    local_pending = local_result_pending = peer_result_pending = false;
    scan_tick_seen = false;
    peer_verify_init(&peer, identity.role);
    queue_init(&requests, sizeof(request_t), 1);
    queue_init(&results, sizeof(verify_result_t), 2);
    initialized = true;
}

bool diagnostic_verify_request(uint32_t token, uint64_t requested_at_us) {
    if (!initialized || !token)
        return false;
    /* Discard at most two completed results from an abandoned terminal. */
    verify_result_t stale;
    for (unsigned i = 0; i < 2; ++i)
        if (!queue_try_remove(&results, &stale))
            break;
    request_t request = {token, requested_at_us};
    return queue_try_add(&requests, &request);
}

bool diagnostic_verify_poll(verify_result_t *result) {
    return initialized && queue_try_remove(&results, result);
}

static bool transmit(void *context, peer_verify_packet_t kind, const uint8_t payload[8]) {
    return diagnostic_peer_tx_try(kind == PEER_VERIFY_REQUEST ? DIAGNOSTIC_VERIFY_REQUEST_MSG
                                                             : DIAGNOSTIC_VERIFY_RESPONSE_MSG,
                                   payload, *(const uint64_t *)context);
}

static bool acquire(void *context, uint32_t token, uint64_t requested_us) {
    (void)context;
    if (scanner.owner != SCAN_FREE)
        return false;
    scan_start(true, token, requested_us);
    return true;
}

static bool poll(void *context, uint32_t token, verify_snapshot_t *snapshot) {
    if (scanner.owner != SCAN_REMOTE || scanner.token != token || !scanner.ready)
        return false;
    if (!recheck_before_deadline(&scanner.snapshot, *(const uint64_t *)context,
                                scanner.requested_us))
        return false;
    /* Peer transport freezes this scan snapshot here. It is not a remote
     * freshness lease while its protected response chunks subsequently drain. */
    *snapshot = scanner.snapshot;
    scanner.owner = SCAN_FREE;
    return true;
}

static void cancel(void *context, uint32_t token) {
    (void)context;
    if (scanner.owner == SCAN_REMOTE && scanner.token == token)
        scanner.owner = SCAN_FREE;
}

void diagnostic_verify_task(uint64_t now_us) {
    if (!initialized)
        return;
    /* Backpressure may retain a result after scanning. Recheck at the actual
     * publication attempt, not only when the scanner releases ownership. */
    if (local_result_pending
        && (local_result.transport != VERIFY_TRANSPORT_OK
            || recheck_before_deadline(&local_result.snapshot, now_us, local_result_requested_us))
        && queue_try_add(&results, &local_result))
        local_result_pending = false;
    if (peer_result_pending && queue_try_add(&results, &peer_result))
        peer_result_pending = false;
    request_t request;
    if (!local_pending && scanner.owner != SCAN_LOCAL && !local_result_pending
        && !peer_result_pending && queue_try_peek(&requests, &request)) {
        peer_verify_start_t started = peer_verify_start(&peer, request.token, request.requested_us);
        if (started != PEER_VERIFY_BUSY) {
            queue_try_remove(&requests, &request);
            local_request = request;
            local_pending = true;
        }
    }
    scan_task(now_us);
    if (scanner.owner == SCAN_LOCAL && scanner.ready && !local_result_pending
        && recheck_before_deadline(&scanner.snapshot, now_us, scanner.requested_us)) {
        local_result = (verify_result_t){.token = scanner.token, .transport = VERIFY_TRANSPORT_OK,
                                          .snapshot = scanner.snapshot};
        local_result_pending = true;
        local_result_requested_us = scanner.requested_us;
        scanner.owner = SCAN_FREE;
    }
    /* Give a waiting peer first opportunity after a local scan releases the
     * single scanner. Both jobs normally complete in ~2.1s even when both
     * terminals request both-board verification simultaneously. */
    peer_verify_task(&peer, now_us, transmit, &now_us, acquire, poll, cancel);
    if (!peer_result_pending)
        peer_result_pending = peer_verify_take_result(&peer, &peer_result);
    if (local_pending && scanner.owner == SCAN_FREE) {
        scan_start(false, local_request.token, local_request.requested_us);
        local_pending = false;
    }
    if (local_pending && now_us - local_request.requested_us >= VERIFY_TIMEOUT_US
        && !local_result_pending) {
        local_result = (verify_result_t){.token = local_request.token,
                                         .transport = VERIFY_TRANSPORT_TIMEOUT};
        local_result_pending = true;
        local_result_requested_us = local_request.requested_us;
        local_pending = false;
    }
}

void diagnostic_verify_receive(bool response, const uint8_t data[8], uint64_t now_us) {
    if (!initialized)
        return;
    if (response)
        peer_verify_receive_response(&peer, data, now_us);
    else
        peer_verify_receive_request(&peer, data, now_us);
}
