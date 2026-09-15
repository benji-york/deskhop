#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "peer_verify.h"

/* Independently generated with Python struct.pack_into and zlib.crc32. The
 * nonzero metadata reserved word must survive transport for policy assessment. */
static const uint8_t vector[132] = {
    0x01, 0x01, 0x00, 0x0f, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x40, 0xe2, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x24, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x21, 0x43, 0x65, 0x87,
    0x0d, 0xf0, 0x00, 0x00, 0xc9, 0x00, 0x57, 0x13, 0xd4, 0xc3, 0xb2, 0xa1,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xfa, 0xff, 0xff, 0xff, 0x44, 0x33, 0x22, 0x11,
    0x04, 0x00, 0x00, 0x00, 0x56, 0x34, 0x22, 0x11, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x7e, 0x3a, 0x78,
};

typedef struct {
    uint64_t now_us, busy_until_us, ready_us, requested_us, last_tx_us;
    uint32_t acquired_token;
    bool acquired, never_ready, refuse_all, attempted;
    unsigned acquire_count, poll_count, cancel_count, attempts, count, delivered, refuse, grant_every;
    peer_verify_packet_t kind[128];
    uint8_t payload[128][8];
    verify_snapshot_t snapshot;
} context_t;

static verify_snapshot_t example(uint8_t role) {
    return (verify_snapshot_t){
        .role = role, .major = 0x1234, .minor = 0x5678,
        .board_id = {0, 1, 2, 3, 4, 5, 6, 7}, .boot_session = UINT64_C(0x8877665544332211),
        .started_us = 123456, .completed_us = 1123456,
        .bytes_read = VERIFY_IMAGE_BYTES, .slot_crc32 = UINT32_C(0x87654321),
        .metadata_magic = 0xf00d, .metadata_version = 201, .metadata_reserved = 0x1357,
        .metadata_crc32 = UINT32_C(0xa1b2c3d4), .image_crc_at_boot = UINT32_C(0x12345678),
        .generation_start = UINT64_C(0x100000002), .generation_end = UINT64_C(0x100000002),
        .start = {.core_valid = 3, .core_ticks = {UINT32_C(0xfffffffa), UINT32_C(0x11223344)},
                  .core_age_ms = {1, 2}, .update_attempt = 3},
        .end = {.core_valid = 3, .core_ticks = {4, UINT32_C(0x11223456)},
                .core_age_ms = {3, 4}, .update_attempt = 3},
        .outcome = VERIFY_SCAN_COMPLETE,
    };
}

static bool transmit(void *opaque, peer_verify_packet_t kind, const uint8_t payload[8]) {
    context_t *c = opaque;
    assert(!c->attempted || c->now_us - c->last_tx_us >= 1000);
    c->attempted = true;
    c->last_tx_us = c->now_us;
    c->attempts++;
    if (c->refuse_all || c->refuse || (c->grant_every && c->attempts % c->grant_every)) {
        if (c->refuse)
            c->refuse--;
        return false;
    }
    assert(c->count < 128);
    c->kind[c->count] = kind;
    memcpy(c->payload[c->count++], payload, 8);
    return true;
}

static bool acquire(void *opaque, uint32_t token, uint64_t requested_us) {
    context_t *c = opaque;
    assert(!c->acquired);
    c->acquire_count++;
    if (c->now_us < c->busy_until_us)
        return false;
    c->acquired = true;
    c->acquired_token = token;
    c->requested_us = requested_us;
    return true;
}

static bool poll_scan(void *opaque, uint32_t token, verify_snapshot_t *snapshot) {
    context_t *c = opaque;
    assert(c->acquired && c->acquired_token == token);
    c->poll_count++;
    if (c->never_ready || c->now_us < c->ready_us)
        return false;
    *snapshot = c->snapshot;
    c->acquired = false;
    return true;
}

static void cancel(void *opaque, uint32_t token) {
    context_t *c = opaque;
    assert(c->acquired && c->acquired_token == token);
    c->acquired = false;
    c->cancel_count++;
}

static void task(peer_verify_t *state, context_t *c, uint64_t now_us) {
    c->now_us = now_us;
    unsigned before = c->acquire_count + c->poll_count;
    peer_verify_task(state, now_us, transmit, c, acquire, poll_scan, cancel);
    assert(c->acquire_count + c->poll_count - before <= 1);
}

static void put(uint8_t *bytes, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        bytes[i] = (uint8_t)(value % 256);
        value /= 256;
    }
}

/* MSB-first polynomial with reflected input/output is independent of the
 * production reflected, LSB-first implementation. */
static uint32_t reverse(uint32_t value, unsigned bits) {
    uint32_t result = 0;
    for (unsigned i = 0; i < bits; ++i) {
        result = result * 2 + value % 2;
        value /= 2;
    }
    return result;
}

static void fix_crc(uint8_t *bytes) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 128; ++i) {
        crc ^= reverse(bytes[i], 8) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & UINT32_C(0x80000000)) ? UINT32_C(0x04c11db7) : 0);
    }
    put(bytes + 128, ~reverse(crc, 32), 4);
}

static void request(uint8_t payload[8], uint32_t token) {
    memset(payload, 0, 8);
    put(payload, token, 4);
    payload[4] = 1;
}

static void response(uint8_t payload[8], uint32_t token, unsigned index, const uint8_t *bytes) {
    put(payload, token, 4);
    payload[4] = (uint8_t)index;
    memcpy(payload + 5, bytes + index * 3, 3);
}

static void start_client(peer_verify_t *state, uint8_t role, uint32_t token, uint64_t now_us) {
    peer_verify_init(state, role);
    assert(peer_verify_start(state, token, now_us) == PEER_VERIFY_STARTED);
    context_t c = {0};
    task(state, &c, now_us);
    assert(c.count == 1 && c.kind[0] == PEER_VERIFY_REQUEST);
    uint8_t expected[8];
    request(expected, token);
    assert(memcmp(expected, c.payload[0], 8) == 0);
}

static verify_result_t take(peer_verify_t *state, uint32_t token, verify_transport_t transport) {
    verify_result_t result;
    assert(peer_verify_take_result(state, &result));
    assert(result.token == token && result.remote && result.transport == transport);
    assert(!peer_verify_take_result(state, &result));
    return result;
}

static void assert_runtime(const diagnostic_runtime_snapshot_t *a,
                            const diagnostic_runtime_snapshot_t *b) {
    assert(a->core_valid == b->core_valid && a->phase == b->phase && a->source == b->source);
    assert(a->update_attempt == b->update_attempt);
    for (unsigned i = 0; i < 2; ++i) {
        assert(a->core_ticks[i] == b->core_ticks[i]);
        assert(a->core_age_ms[i] == b->core_age_ms[i]);
    }
    assert(!a->update_seen && !a->received_bytes && !a->total_bytes);
    assert(!a->progress_age_ms && !a->target_version);
}

static void assert_snapshot(const verify_snapshot_t *a, const verify_snapshot_t *b) {
    assert(a->role == b->role && a->major == b->major && a->minor == b->minor);
    assert(memcmp(a->board_id, b->board_id, 8) == 0);
    assert(a->boot_session == b->boot_session && a->image_crc_at_boot == b->image_crc_at_boot);
    assert(a->started_us == b->started_us && a->completed_us == b->completed_us);
    assert(a->generation_start == b->generation_start && a->generation_end == b->generation_end);
    assert(a->bytes_read == b->bytes_read && a->slot_crc32 == b->slot_crc32);
    assert(a->metadata_magic == b->metadata_magic && a->metadata_version == b->metadata_version);
    assert(a->metadata_reserved == b->metadata_reserved && a->metadata_crc32 == b->metadata_crc32);
    assert(a->outcome == b->outcome);
    assert_runtime(&a->start, &b->start);
    assert_runtime(&a->end, &b->end);
}

static void feed(peer_verify_t *state, uint32_t token, const uint8_t *bytes, unsigned rotation,
                   bool backwards, int missing) {
    for (unsigned j = 0; j < 44; ++j) {
        unsigned i = ((backwards ? 43 - j : j) + rotation) % 44;
        if ((int)i == missing)
            continue;
        uint8_t payload[8];
        response(payload, token, i, bytes);
        peer_verify_receive_response(state, payload, 2000 + j * 1000);
        peer_verify_receive_response(state, payload, 2000 + j * 1000);
    }
}

static void test_wire_and_orders(void) {
    uint8_t bytes[132];
    memcpy(bytes, vector, sizeof(bytes));
    fix_crc(bytes);
    assert(memcmp(bytes, vector, sizeof(bytes)) == 0);
    for (uint8_t role = 0; role < 2; ++role) {
        peer_verify_t server;
        peer_verify_init(&server, role);
        context_t c = {.snapshot = example(role)};
        uint8_t payload[8];
        request(payload, UINT32_C(0x89abcdef));
        assert(peer_verify_receive_request(&server, payload, 0));
        assert(!c.acquire_count && !c.poll_count && !c.count);
        task(&server, &c, 0);
        assert(c.acquire_count == 1 && !c.poll_count && !c.count);
        task(&server, &c, 1000);
        assert(c.poll_count == 1 && c.count == 1 && !c.acquired);
        /* No provider memory is retained after successful poll. */
        memset(&c.snapshot, 0xa5, sizeof(c.snapshot));
        for (unsigned i = 2; i <= 44; ++i)
            task(&server, &c, i * 1000);
        assert(c.count == 44 && !c.cancel_count && c.poll_count == 1);
        bytes[1] = role;
        fix_crc(bytes);
        for (unsigned i = 0; i < 44; ++i) {
            response(payload, UINT32_C(0x89abcdef), i, bytes);
            assert(c.kind[i] == PEER_VERIFY_RESPONSE);
            assert(memcmp(payload, c.payload[i], 8) == 0);
        }
        for (unsigned rotation = 0; rotation < 44; ++rotation) {
            for (unsigned backwards = 0; backwards < 2; ++backwards) {
                peer_verify_t client;
                start_client(&client, role ^ 1u, 7, 0);
                feed(&client, 7, bytes, rotation, backwards != 0, -1);
                assert(peer_verify_start(&client, 8, 60000) == PEER_VERIFY_BUSY);
                verify_result_t result = take(&client, 7, VERIFY_TRANSPORT_OK);
                verify_snapshot_t expected = example(role);
                assert_snapshot(&result.snapshot, &expected);
                assert(peer_verify_start(&client, 8, 60000) == PEER_VERIFY_STARTED);
                assert_snapshot(&result.snapshot, &expected);
            }
        }
    }
}

static void test_corrupt_and_missing(void) {
    for (unsigned byte = 0; byte < 132; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint8_t bytes[132];
            memcpy(bytes, vector, sizeof(bytes));
            bytes[byte] ^= (uint8_t)(1u << bit);
            peer_verify_t client;
            start_client(&client, 0, 7, 0);
            feed(&client, 7, bytes, 0, false, -1);
            take(&client, 7, VERIFY_TRANSPORT_INVALID);
        }
    }
    for (int missing = 0; missing < 44; ++missing) {
        peer_verify_t client;
        start_client(&client, 0, 7, 0);
        feed(&client, 7, vector, 0, false, missing);
        verify_result_t result;
        assert(!peer_verify_take_result(&client, &result));
        context_t c = {0};
        task(&client, &c, PEER_VERIFY_TIMEOUT_US - 1);
        assert(!peer_verify_take_result(&client, &result));
        task(&client, &c, PEER_VERIFY_TIMEOUT_US);
        take(&client, 7, VERIFY_TRANSPORT_TIMEOUT);
    }
    for (unsigned index = 44; index < 256; ++index) {
        peer_verify_t client;
        start_client(&client, 0, 7, 0);
        uint8_t payload[8] = {7, 0, 0, 0, (uint8_t)index, 0, 0, 0};
        peer_verify_receive_response(&client, payload, 1000);
        take(&client, 7, VERIFY_TRANSPORT_INVALID);
    }
    for (unsigned index = 0; index < 44; ++index) {
        peer_verify_t client;
        start_client(&client, 0, 7, 0);
        uint8_t payload[8];
        response(payload, 7, index, vector);
        peer_verify_receive_response(&client, payload, 1000);
        payload[7] ^= 0x80;
        peer_verify_receive_response(&client, payload, 1001);
        take(&client, 7, VERIFY_TRANSPORT_INVALID);
    }
}

static void check_frame(uint8_t bytes[132], verify_transport_t expected) {
    fix_crc(bytes);
    peer_verify_t client;
    start_client(&client, 0, 7, 0);
    feed(&client, 7, bytes, 0, false, -1);
    take(&client, 7, expected);
}

static void test_semantic_validation(void) {
    static const struct { unsigned offset; uint8_t value; } bad[] = {
        {0, 2}, {1, 0}, {1, 2}, {2, 5}, {3, 16},
        {108, 7}, {109, 7}, {110, 3}, {111, 3},
        {124, 1}, {125, 1}, {126, 1}, {127, 1},
    };
    uint8_t bytes[132];
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memcpy(bytes, vector, sizeof(bytes));
        bytes[bad[i].offset] = bad[i].value;
        check_frame(bytes, VERIFY_TRANSPORT_INVALID);
    }
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(bytes, vector, sizeof(bytes));
        if (i == 0) put(bytes + 40, VERIFY_IMAGE_BYTES + 1, 4);
        if (i == 1) put(bytes + 40, VERIFY_IMAGE_BYTES - 1, 4);
        if (i == 2) bytes[68]++;
        if (i == 3) put(bytes + 32, 123455, 8);
        check_frame(bytes, VERIFY_TRANSPORT_INVALID);
    }
    /* Complete with equal endpoint times is a structurally valid observation;
     * policy separately decides whether sufficient core progress was observed. */
    memcpy(bytes, vector, sizeof(bytes));
    put(bytes + 32, 123456, 8);
    check_frame(bytes, VERIFY_TRANSPORT_OK);
    for (unsigned outcome = VERIFY_SCAN_UPDATE_ACTIVE; outcome <= VERIFY_SCAN_TIMEOUT; ++outcome) {
        memcpy(bytes, vector, sizeof(bytes));
        bytes[2] = (uint8_t)outcome;
        bytes[68]++;
        bytes[108] = DIAGNOSTIC_UPDATE_RECEIVING;
        bytes[109] = DIAGNOSTIC_UPDATE_ABANDONED;
        bytes[110] = DIAGNOSTIC_SOURCE_PEER;
        bytes[111] = DIAGNOSTIC_SOURCE_USB;
        put(bytes + 40, 256, 4);
        check_frame(bytes, VERIFY_TRANSPORT_OK);
        put(bytes + 40, VERIFY_IMAGE_BYTES + 1, 4);
        check_frame(bytes, VERIFY_TRANSPORT_INVALID);
    }
}

static void test_stale_and_deadlines(void) {
    peer_verify_t client;
    peer_verify_init(&client, 0);
    assert(peer_verify_start(&client, 0, 0) == PEER_VERIFY_BAD_ARGUMENT);
    assert(peer_verify_start(&client, UINT32_MAX, 1000) == PEER_VERIFY_STARTED);
    context_t c = {.refuse_all = true};
    task(&client, &c, 1000);
    feed(&client, UINT32_MAX, vector, 0, false, -1);
    assert(client.client_active && !client.client_received);
    c.refuse_all = false;
    task(&client, &c, 2000);
    uint8_t stale[8] = {5, 0, 0, 0, 255, 0xff, 0xff, 0xff};
    peer_verify_receive_response(&client, stale, 2001);
    assert(client.client_active);
    feed(&client, UINT32_MAX, vector, 0, false, -1);
    take(&client, UINT32_MAX, VERIFY_TRANSPORT_OK);
    assert(peer_verify_start(&client, 2, 100000) == PEER_VERIFY_STARTED);
    task(&client, &c, 3099999);
    response(stale, 2, 0, vector);
    peer_verify_receive_response(&client, stale, 3100000);
    take(&client, 2, VERIFY_TRANSPORT_TIMEOUT);
    assert(peer_verify_start(&client, 3, 3200000) == PEER_VERIFY_STARTED);
    c.refuse_all = true;
    task(&client, &c, 3200000);
    task(&client, &c, 6200000);
    take(&client, 3, VERIFY_TRANSPORT_TIMEOUT);
    assert(peer_verify_start(&client, 4, 0) == PEER_VERIFY_STARTED);
    unsigned attempts = c.attempts;
    task(&client, &c, 7000000);
    assert(c.attempts == attempts);
    take(&client, 4, VERIFY_TRANSPORT_TIMEOUT);
    peer_verify_init(&client, 2);
    assert(peer_verify_start(&client, 1, 0) == PEER_VERIFY_BAD_ARGUMENT);
}

static void test_provider_lifetime_and_rate(void) {
    peer_verify_t server;
    peer_verify_init(&server, 1);
    uint8_t payload[8];
    for (unsigned i = 0; i < 5; ++i) {
        request(payload, 7);
        if (i == 0) payload[0] = 0;
        else payload[i + 3] = 2;
        assert(!peer_verify_receive_request(&server, payload, 0));
    }
    request(payload, 7);
    context_t c = {.snapshot = example(1), .busy_until_us = 5000, .ready_us = 10000};
    assert(peer_verify_receive_request(&server, payload, 0));
    for (uint64_t now = 0; now <= 9000; now += 1000)
        task(&server, &c, now);
    assert(c.acquire_count == 6 && c.poll_count == 4 && !c.count && c.acquired);
    assert(c.requested_us == 0 && c.acquired_token == 7);
    assert(!peer_verify_receive_request(&server, payload, 9000));
    for (uint64_t now = 10000; now <= 53000; now += 1000)
        task(&server, &c, now);
    assert(c.count == 44 && !c.acquired && !c.cancel_count);
    assert(!peer_verify_receive_request(&server, payload, 300000));
    request(payload, 8);
    assert(!peer_verify_receive_request(&server, payload, 199999));
    assert(peer_verify_receive_request(&server, payload, 200000));
    c.never_ready = true;
    task(&server, &c, 200000);
    assert(c.acquired);
    task(&server, &c, 3199999);
    assert(c.acquired);
    unsigned polls = c.poll_count;
    request(payload, 9);
    /* Receive cannot cancel an expired provider or replace its ownership. */
    assert(!peer_verify_receive_request(&server, payload, 3200000));
    assert(!c.cancel_count);
    task(&server, &c, 3200000);
    assert(c.cancel_count == 1 && !c.acquired && c.poll_count == polls);
    task(&server, &c, 3201000);
    assert(c.cancel_count == 1);
    assert(peer_verify_receive_request(&server, payload, 3201000));
    c.busy_until_us = UINT64_MAX;
    task(&server, &c, 3201000);
    unsigned acquisitions = c.acquire_count;
    task(&server, &c, 6201000);
    assert(c.acquire_count == acquisitions && c.cancel_count == 1);
    request(payload, 10);
    assert(peer_verify_receive_request(&server, payload, 6401000));
    c.busy_until_us = 0;
    c.never_ready = false;
    c.refuse_all = true;
    task(&server, &c, 6401000);
    task(&server, &c, 6402000);
    assert(!c.acquired);
    task(&server, &c, 9401000);
    assert(c.cancel_count == 1); /* Completed provider was already released. */
    peer_verify_init(&server, 2);
    assert(!peer_verify_receive_request(&server, payload, 9500000));
}

static void test_pacing_fairness_and_null_callbacks(void) {
    peer_verify_t state;
    peer_verify_init(&state, 0);
    context_t c = {.snapshot = example(0), .refuse_all = true};
    uint8_t payload[8];
    request(payload, 9);
    assert(peer_verify_receive_request(&state, payload, 0));
    c.now_us = 0;
    peer_verify_task(&state, 0, NULL, &c, acquire, poll_scan, NULL);
    assert(!c.acquire_count);
    task(&state, &c, 0);
    task(&state, &c, 1);
    assert(c.attempts == 1); /* First response refused at t=1. */
    task(&state, &c, 1000);
    assert(c.attempts == 1);
    task(&state, &c, 1001);
    assert(c.attempts == 2);
    assert(peer_verify_start(&state, 7, 2000) == PEER_VERIFY_STARTED);
    c.refuse_all = false;
    c.grant_every = 3;
    for (uint64_t now = 2001; now < 150000; now += 1000)
        task(&state, &c, now);
    assert(c.count == 45 && c.kind[0] == PEER_VERIFY_REQUEST);
    for (unsigned i = 1; i < 45; ++i)
        assert(c.kind[i] == PEER_VERIFY_RESPONSE);
    assert(c.attempts == 135);

    /* Even-numbered external grants reproduce the phase-lock regression: if a
     * denied request consumes its turn, only responses win grants until the
     * entire stream drains. A refused request must remain first in line. */
    peer_verify_init(&state, 0);
    c = (context_t){.snapshot = example(0), .refuse_all = true};
    assert(peer_verify_receive_request(&state, payload, 0));
    task(&state, &c, 0);
    task(&state, &c, 1000);
    task(&state, &c, 2000);
    assert(c.attempts == 2);
    assert(peer_verify_start(&state, 7, 3000) == PEER_VERIFY_STARTED);
    c.refuse_all = false;
    c.grant_every = 2;
    task(&state, &c, 3000);
    assert(!c.count);
    task(&state, &c, 4000);
    assert(c.count == 1 && c.kind[0] == PEER_VERIFY_REQUEST);
}

static void deliver(context_t *from, peer_verify_t *to, uint64_t now_us) {
    while (from->delivered < from->count) {
        unsigned i = from->delivered++;
        if (from->kind[i] == PEER_VERIFY_REQUEST)
            assert(peer_verify_receive_request(to, from->payload[i], now_us));
        else
            peer_verify_receive_response(to, from->payload[i], now_us);
    }
}

static void test_bidirectional_and_result_copy(void) {
    peer_verify_t a, b;
    peer_verify_init(&a, 0);
    peer_verify_init(&b, 1);
    context_t ca = {.snapshot = example(0), .busy_until_us = 8000, .ready_us = 110000,
                    .grant_every = 3};
    context_t cb = {.snapshot = example(1), .busy_until_us = 18000, .ready_us = 120000,
                    .grant_every = 3};
    ca.snapshot.outcome = VERIFY_SCAN_CHANGED;
    ca.snapshot.generation_end++;
    ca.snapshot.bytes_read = 123456;
    ca.snapshot.start.phase = DIAGNOSTIC_UPDATE_RECEIVING;
    ca.snapshot.end.phase = DIAGNOSTIC_UPDATE_PAUSED;
    ca.snapshot.start.source = DIAGNOSTIC_SOURCE_PEER;
    ca.snapshot.end.source = DIAGNOSTIC_SOURCE_USB;
    ca.snapshot.start.update_seen = true; /* Members absent from wire are zero on decode. */
    ca.snapshot.start.total_bytes = VERIFY_IMAGE_BYTES;
    assert(peer_verify_start(&a, 0xabcdef01, 0) == PEER_VERIFY_STARTED);
    assert(peer_verify_start(&b, 0xabcdef02, 0) == PEER_VERIFY_STARTED);
    for (uint64_t now = 0; now < 300000; now += 1000) {
        task(&a, &ca, now);
        task(&b, &cb, now);
        deliver(&ca, &b, now);
        deliver(&cb, &a, now);
    }
    assert(ca.count == 45 && cb.count == 45);
    verify_result_t ra = take(&a, 0xabcdef01, VERIFY_TRANSPORT_OK);
    assert_snapshot(&ra.snapshot, &cb.snapshot);
    /* A retained client result does not block independent server service. */
    uint8_t payload[8];
    request(payload, 42);
    assert(peer_verify_receive_request(&b, payload, 300000));
    unsigned count = cb.count;
    for (uint64_t now = 300000; now < 450000; now += 1000)
        task(&b, &cb, now);
    assert(cb.count == count + 44);
    verify_result_t rb = take(&b, 0xabcdef02, VERIFY_TRANSPORT_OK);
    assert_snapshot(&rb.snapshot, &ca.snapshot);
    assert_snapshot(&ra.snapshot, &cb.snapshot);
}

int main(void) {
    test_wire_and_orders();
    test_corrupt_and_missing();
    test_semantic_validation();
    test_stale_and_deadlines();
    test_provider_lifetime_and_rate();
    test_pacing_fairness_and_null_callbacks();
    test_bidirectional_and_result_copy();
    puts("peer verify: wire vector, 176 orders, 1056 corruptions, loss, providers, deadlines and fair pacing passed");
    return 0;
}
