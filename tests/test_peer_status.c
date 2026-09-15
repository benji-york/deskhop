#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "peer_status.h"

typedef struct {
    unsigned attempts;
    unsigned count;
    unsigned refuse;
    bool refuse_all;
    bool alternating_grant;
    peer_status_packet_t kind[64];
    uint8_t payload[64][8];
} transport_t;

static bool transmit(void *context, peer_status_packet_t kind, const uint8_t payload[8]) {
    transport_t *transport = context;
    transport->attempts++;
    if (transport->refuse_all || transport->refuse
        || (transport->alternating_grant && transport->attempts % 2)) {
        if (transport->refuse)
            transport->refuse--;
        return false;
    }
    assert(transport->count < 64);
    transport->kind[transport->count] = kind;
    memcpy(transport->payload[transport->count++], payload, 8);
    return true;
}

static peer_status_snapshot_t example(uint8_t role) {
    return (peer_status_snapshot_t){
        .role = role, .protocol = PEER_STATUS_PROTOCOL,
        .runtime = {
            .core_valid = 3, .update_seen = true, .phase = DIAGNOSTIC_UPDATE_RECEIVING,
            .core_ticks = {UINT32_C(0x11223344), UINT32_C(0xfffffffe)},
            .core_age_ms = {23, 456}, .received_bytes = 123456, .total_bytes = 262144,
            .progress_age_ms = 789, .target_version = 200, .source = DIAGNOSTIC_SOURCE_PEER,
            .update_attempt = UINT32_C(0x89abcdef),
        },
        .major = 0x1234,
        .minor = 0x5678,
        .board_id = {0, 1, 2, 3, 4, 5, 6, 7},
        .boot_session = UINT64_C(0x8877665544332211),
        .uptime_ms = UINT64_C(0x1020304050607080),
        .image_crc_at_boot = UINT32_C(0xa1b2c3d4),
    };
}

static void assert_snapshot(const peer_status_snapshot_t *actual,
                            const peer_status_snapshot_t *expected) {
    assert(actual->role == expected->role && actual->protocol == expected->protocol);
    assert(actual->runtime.core_valid == expected->runtime.core_valid);
    for (unsigned i = 0; i < 2; ++i) {
        assert(actual->runtime.core_ticks[i] == expected->runtime.core_ticks[i]);
        assert(actual->runtime.core_age_ms[i] == expected->runtime.core_age_ms[i]);
    }
    assert(actual->runtime.update_seen == expected->runtime.update_seen);
    assert(actual->runtime.phase == expected->runtime.phase);
    assert(actual->runtime.source == expected->runtime.source);
    assert(actual->runtime.received_bytes == expected->runtime.received_bytes);
    assert(actual->runtime.total_bytes == expected->runtime.total_bytes);
    assert(actual->runtime.progress_age_ms == expected->runtime.progress_age_ms);
    assert(actual->runtime.target_version == expected->runtime.target_version);
    assert(actual->runtime.update_attempt == expected->runtime.update_attempt);
    assert(actual->major == expected->major);
    assert(actual->minor == expected->minor);
    assert(memcmp(actual->board_id, expected->board_id, 8) == 0);
    assert(actual->boot_session == expected->boot_session);
    assert(actual->uptime_ms == expected->uptime_ms);
    assert(actual->image_crc_at_boot == expected->image_crc_at_boot);
}

static void request(uint8_t payload[8], uint32_t token) {
    for (unsigned i = 0; i < 4; i++)
        payload[i] = (uint8_t)(token >> (8 * i));
    payload[4] = PEER_STATUS_PROTOCOL;
    payload[5] = payload[6] = payload[7] = 0;
}

static transport_t replies_version(uint32_t token, uint8_t role, uint8_t version) {
    peer_status_t server;
    peer_status_init(&server, role);
    peer_status_snapshot_t snapshot = example(role);
    uint8_t payload[8];
    request(payload, token);
    payload[4] = version;
    assert(peer_status_receive_request(&server, payload, &snapshot, 0));
    /* An accepted transfer must own its bytes, not retain the source pointer. */
    memset(&snapshot, 0xa5, sizeof(snapshot));
    transport_t transport = {0};
    unsigned chunks = version == 1 ? 13 : 26;
    for (unsigned i = 0; i < chunks; i++)
        peer_status_task(&server, i * 1000, transmit, &transport);
    assert(transport.count == chunks);
    assert(!server.server_active);
    return transport;
}

static transport_t replies(uint32_t token, uint8_t role) {
    return replies_version(token, role, PEER_STATUS_PROTOCOL);
}

static void start_client(peer_status_t *client, uint8_t role, uint32_t token, uint64_t now_us) {
    peer_status_init(client, role);
    assert(peer_status_start(client, token, now_us) == PEER_STATUS_STARTED);
    transport_t transport = {0};
    peer_status_task(client, now_us, transmit, &transport);
    assert(transport.count == 1);
    assert(transport.kind[0] == PEER_STATUS_REQUEST);
    uint8_t expected[8];
    request(expected, token);
    assert(memcmp(transport.payload[0], expected, 8) == 0);
}

static peer_status_result_t take(peer_status_t *state, uint32_t token,
                                  peer_status_outcome_t outcome) {
    peer_status_result_t result;
    assert(peer_status_take_result(state, &result));
    assert(result.token == token);
    assert(result.outcome == outcome);
    assert(!peer_status_take_result(state, &result));
    return result;
}

static void test_wire_and_reordering(void) {
    /* This vector was specified with Python struct.pack + zlib.crc32, without
     * using the production encoder or CRC implementation. */
    const uint8_t legacy_expected[39] = {
        0x01, 0x01, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x77, 0x88, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10,
        0xd4, 0xc3, 0xb2, 0xa1, 0x12, 0x5c, 0xf2, 0xd0, 0x00,
    };
    transport_t packets = replies_version(UINT32_C(0x12345678), 1, 1);
    for (unsigned i = 0; i < 13; i++)
        assert(memcmp(packets.payload[i] + 5, legacy_expected + i * 3, 3) == 0);
    const uint8_t expected[78] = {
        0x02, 0x01, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x80, 0x70,
        0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0xd4, 0xc3, 0xb2, 0xa1, 0x07, 0x01,
        0x44, 0x33, 0x22, 0x11, 0xfe, 0xff, 0xff, 0xff, 0x17, 0x00, 0x00, 0x00,
        0xc8, 0x01, 0x00, 0x00, 0x40, 0xe2, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x15, 0x03, 0x00, 0x00, 0xc8, 0x00, 0x01, 0x00, 0xef, 0xcd, 0xab, 0x89,
        0x5d, 0x79, 0x43, 0x45, 0x00, 0x00,
    };
    packets = replies(UINT32_C(0x12345678), 1);
    for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT; i++) {
        const uint8_t *p = packets.payload[i];
        assert(packets.kind[i] == PEER_STATUS_RESPONSE);
        assert(p[0] == 0x78 && p[1] == 0x56 && p[2] == 0x34 && p[3] == 0x12);
        assert(p[4] == i);
        assert(memcmp(p + 5, expected + i * 3, 3) == 0);
    }
    for (uint8_t local_role = 0; local_role < 2; local_role++) {
        packets = replies(7, local_role ^ 1u);
        peer_status_snapshot_t expected_snapshot = example(local_role ^ 1u);
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            for (unsigned rotation = 0; rotation < PEER_STATUS_CHUNK_COUNT; rotation++) {
                peer_status_t client;
                start_client(&client, local_role, 7, 0);
                for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT; i++) {
                    unsigned index = ((reverse ? PEER_STATUS_CHUNK_COUNT - 1 - i : i) + rotation) % PEER_STATUS_CHUNK_COUNT;
                    peer_status_receive_response(&client, packets.payload[index], 1000 + i);
                    peer_status_receive_response(&client, packets.payload[index], 1000 + i);
                }
                peer_status_result_t result = take(&client, 7, PEER_STATUS_OK);
                assert_snapshot(&result.snapshot, &expected_snapshot);
            }
        }
    }
}

static void test_corruption_and_stale_packets(void) {
    transport_t packets = replies(7, 1);
    /* Every bit of every snapshot byte is protected, including the zero pad
     * outside the CRC and the CRC itself. No corrupted snapshot is accepted. */
    for (unsigned offset = 0; offset < PEER_STATUS_SNAPSHOT_SIZE; offset++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            peer_status_t client;
            start_client(&client, 0, 7, 0);
            for (unsigned index = 0; index < PEER_STATUS_CHUNK_COUNT; index++) {
                uint8_t payload[8];
                memcpy(payload, packets.payload[index], 8);
                if (index == offset / 3)
                    payload[5 + offset % 3] ^= 1u << bit;
                peer_status_receive_response(&client, payload, 1000 + index);
            }
            take(&client, 7, PEER_STATUS_INVALID);
        }
    }
    for (unsigned index = PEER_STATUS_CHUNK_COUNT; index < 256; index++) {
        peer_status_t client;
        start_client(&client, 0, 7, 0);
        uint8_t payload[8];
        memcpy(payload, packets.payload[0], 8);
        payload[4] = index;
        peer_status_receive_response(&client, payload, 1000);
        take(&client, 7, PEER_STATUS_INVALID);
    }
    peer_status_t client;
    start_client(&client, 0, 7, 0);
    for (unsigned index = 0; index < PEER_STATUS_CHUNK_COUNT; index++) {
        uint8_t payload[8];
        memcpy(payload, packets.payload[index], 8);
        payload[0] = 6;  /* A previous query. */
        peer_status_receive_response(&client, payload, 1000);
        payload[4] = 255;  /* Stale malformed packets are still irrelevant. */
        peer_status_receive_response(&client, payload, 1000);
        payload[0] = 0;
        peer_status_receive_response(&client, payload, 1000);
    }
    assert(client.client_received == 0 && client.client_active);
    for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT; i++)
        peer_status_receive_response(&client, packets.payload[i], 2000);
    take(&client, 7, PEER_STATUS_OK);

    start_client(&client, 0, 7, 0);
    peer_status_receive_response(&client, packets.payload[2], 1000);
    uint8_t conflict[8];
    memcpy(conflict, packets.payload[2], 8);
    conflict[6] ^= 0x80;
    peer_status_receive_response(&client, conflict, 1001);
    take(&client, 7, PEER_STATUS_INVALID);

    /* Valid CRC cannot authorize a response from the local board role. */
    packets = replies(7, 0);
    start_client(&client, 0, 7, 0);
    for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT; i++)
        peer_status_receive_response(&client, packets.payload[i], 1000);
    take(&client, 7, PEER_STATUS_INVALID);
}

static void test_client_lifetime_and_refusal(void) {
    peer_status_t client;
    peer_status_init(&client, 0);
    assert(peer_status_start(&client, 0, 0) == PEER_STATUS_BAD_ARGUMENT);
    assert(peer_status_start(&client, 7, 0) == PEER_STATUS_STARTED);
    assert(peer_status_start(&client, 8, 0) == PEER_STATUS_BUSY);
    transport_t transport = {.refuse = 1};
    peer_status_task(&client, 0, transmit, &transport);
    assert(transport.attempts == 1 && !client.client_sent);
    peer_status_task(&client, 0, transmit, &transport);
    peer_status_task(&client, 999, transmit, &transport);
    assert(transport.attempts == 1);
    /* Responses preceding successful enqueue cannot satisfy freshness. */
    transport_t packets = replies(7, 1);
    for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT; i++)
        peer_status_receive_response(&client, packets.payload[i], 999);
    assert(client.client_received == 0 && client.client_active);
    peer_status_task(&client, 1000, transmit, &transport);
    assert(transport.attempts == 2 && client.client_sent && transport.count == 1);
    for (unsigned i = 2000; i < 500000; i += 1000)
        peer_status_task(&client, i, transmit, &transport);
    assert(transport.attempts == 3 && client.client_active);
    /* An old peer silently ignoring the request gives an explicit timeout. */
    peer_status_task(&client, 500000, transmit, &transport);
    assert(peer_status_start(&client, 8, 500000) == PEER_STATUS_BUSY);
    take(&client, 7, PEER_STATUS_TIMEOUT);
    assert(peer_status_start(&client, 8, 500000) == PEER_STATUS_STARTED);
    peer_status_task(&client, 1000000, transmit, &transport);
    assert(transport.attempts == 3);  /* Stale dequeued requests never send. */
    take(&client, 8, PEER_STATUS_TIMEOUT);

    peer_status_init(&client, 0);
    assert(peer_status_start(&client, 9, 0) == PEER_STATUS_STARTED);
    transport = (transport_t){.refuse_all = true};
    peer_status_task(&client, 0, transmit, &transport);
    peer_status_task(&client, 499000, transmit, &transport);
    peer_status_task(&client, 500000, transmit, &transport);
    assert(transport.attempts == 2 && transport.count == 0);
    take(&client, 9, PEER_STATUS_TIMEOUT);

    /* A final chunk at the deadline is too late, even without an intervening
     * task tick. A complete response just before the deadline is accepted. */
    for (unsigned late = 0; late < 2; late++) {
        start_client(&client, 0, 7, 0);
        for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT - 1; i++)
            peer_status_receive_response(&client, packets.payload[i], 499999);
        peer_status_receive_response(&client, packets.payload[PEER_STATUS_CHUNK_COUNT - 1], 499999 + late);
        take(&client, 7, late ? PEER_STATUS_TIMEOUT : PEER_STATUS_OK);
    }
    /* Losing any one chunk leaves an incomplete response, never a partially
     * populated success; late arrivals cannot change the completed outcome. */
    for (unsigned missing = 0; missing < PEER_STATUS_CHUNK_COUNT; missing++) {
        start_client(&client, 0, 7, 0);
        for (unsigned i = 0; i < PEER_STATUS_CHUNK_COUNT; i++)
            if (i != missing)
                peer_status_receive_response(&client, packets.payload[i], 1000);
        assert(client.client_active);
        peer_status_task(&client, 500000, NULL, NULL);
        peer_status_receive_response(&client, packets.payload[missing], 500001);
        take(&client, 7, PEER_STATUS_TIMEOUT);
        peer_status_receive_response(&client, packets.payload[missing], 500002);
        assert(!client.result_ready);
    }
    /* Unsigned elapsed-time arithmetic remains valid across counter wrap. */
    uint64_t start = UINT64_MAX - 100;
    start_client(&client, 0, 7, start);
    peer_status_task(&client, start + 499999, NULL, NULL);
    assert(client.client_active);
    peer_status_task(&client, start + 500000, NULL, NULL);
    take(&client, 7, PEER_STATUS_TIMEOUT);
    peer_status_init(&client, 2);
    assert(peer_status_start(&client, 7, 0) == PEER_STATUS_BAD_ARGUMENT);
}

static void test_server_validation_and_rate_limits(void) {
    peer_status_t server;
    peer_status_init(&server, 1);
    peer_status_snapshot_t snapshot = example(1);
    uint8_t payload[8];
    request(payload, 0);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 0));
    for (unsigned index = 4; index < 8; index++) {
        request(payload, 7);
        payload[index] = index == 4 ? 3 : 1;
        assert(!peer_status_receive_request(&server, payload, &snapshot, 0));
    }
    request(payload, 7);
    snapshot.role = 0;
    assert(!peer_status_receive_request(&server, payload, &snapshot, 0));
    snapshot.role = 1;
    assert(peer_status_receive_request(&server, payload, &snapshot, 0));
    transport_t transport = {.refuse = 1};
    peer_status_task(&server, 0, transmit, &transport);
    peer_status_task(&server, 999, transmit, &transport);
    assert(transport.attempts == 1 && server.server_next_chunk == 0);
    peer_status_task(&server, 1000, transmit, &transport);
    assert(transport.attempts == 2 && server.server_next_chunk == 1);
    assert(transport.payload[0][4] == 0);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 1000));
    request(payload, 8);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 1000));
    for (unsigned i = 2; i <= PEER_STATUS_CHUNK_COUNT; i++)
        peer_status_task(&server, i * 1000, transmit, &transport);
    assert(transport.count == PEER_STATUS_CHUNK_COUNT && !server.server_active);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 199999));
    request(payload, 7);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 200000));
    request(payload, 8);
    assert(peer_status_receive_request(&server, payload, &snapshot, 200000));

    /* New requests cannot replace a partially queued snapshot, even after the
     * rate-limit interval. After expiry a new token can make progress. */
    transport.refuse_all = true;
    peer_status_task(&server, 200000, transmit, &transport);
    request(payload, 9);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 400000));
    assert(!peer_status_receive_request(&server, payload, &snapshot, 699999));
    assert(peer_status_receive_request(&server, payload, &snapshot, 700000));
    peer_status_task(&server, 1200000, transmit, &transport);
    assert(!server.server_active);
    assert(!peer_status_receive_request(&server, payload, &snapshot, 1200000));
    request(payload, 10);
    assert(peer_status_receive_request(&server, payload, &snapshot, 1200000));
}

static void test_bidirectional_queries(void) {
    peer_status_t boards[2];
    peer_status_snapshot_t snapshots[2] = {example(0), example(1)};
    transport_t wire[2] = {{0}, {0}};
    unsigned delivered[2] = {0, 0};
    for (unsigned i = 0; i < 2; i++) {
        peer_status_init(&boards[i], i);
        assert(peer_status_start(&boards[i], 10 + i, 0) == PEER_STATUS_STARTED);
    }
    for (unsigned tick = 0; tick < 30; tick++) {
        uint64_t now_us = tick * 1000;
        for (unsigned i = 0; i < 2; i++) {
            unsigned attempts = wire[i].attempts;
            peer_status_task(&boards[i], now_us, transmit, &wire[i]);
            assert(wire[i].attempts - attempts <= 1);
            attempts = wire[i].attempts;
            peer_status_task(&boards[i], now_us, transmit, &wire[i]);
            assert(wire[i].attempts == attempts);
        }
        for (unsigned i = 0; i < 2; i++) {
            while (delivered[i] < wire[i].count) {
                unsigned index = delivered[i]++;
                if (wire[i].kind[index] == PEER_STATUS_REQUEST)
                    assert(peer_status_receive_request(&boards[i ^ 1], wire[i].payload[index],
                                                       &snapshots[i ^ 1], now_us));
                else
                    peer_status_receive_response(&boards[i ^ 1], wire[i].payload[index], now_us);
            }
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        assert(wire[i].count == PEER_STATUS_CHUNK_COUNT + 1);
        peer_status_result_t result = take(&boards[i], 10 + i, PEER_STATUS_OK);
        assert_snapshot(&result.snapshot, &snapshots[i ^ 1]);
    }

    /* After a refused slot, the retained request and then server reply both
     * progress as soon as the shared queue grants space again. */
    peer_status_t state;
    peer_status_init(&state, 0);
    assert(peer_status_start(&state, 20, 0) == PEER_STATUS_STARTED);
    uint8_t payload[8];
    request(payload, 21);
    assert(peer_status_receive_request(&state, payload, &snapshots[0], 0));
    transport_t transport = {.refuse_all = true};
    peer_status_task(&state, 0, transmit, &transport);
    transport.refuse_all = false;
    peer_status_task(&state, 1000, transmit, &transport);
    assert(transport.kind[0] == PEER_STATUS_REQUEST);
    peer_status_task(&state, 2000, transmit, &transport);
    assert(transport.kind[1] == PEER_STATUS_RESPONSE);
    peer_status_task(&state, 3000, transmit, &transport);
    assert(transport.kind[2] == PEER_STATUS_RESPONSE);
    assert(transport.payload[2][4] == 1);
}

static void test_shared_pacer_fairness(void) {
    peer_status_t state;
    peer_status_init(&state, 0);
    assert(peer_status_start(&state, 31, 0) == PEER_STATUS_STARTED);
    peer_status_snapshot_t snapshot = example(0);
    uint8_t payload[8];
    request(payload, 32);
    assert(peer_status_receive_request(&state, payload, &snapshot, 0));
    /* Another diagnostic protocol takes every other UART opportunity. Both
     * directions must progress within two grants, without waiting for all
     * 13 response chunks. This catches preference changes on denied slots. */
    transport_t transport = {.alternating_grant = true};
    for (unsigned tick = 0; tick < 4; ++tick)
        peer_status_task(&state, tick * 1000, transmit, &transport);
    assert(transport.count == 2);
    assert(state.client_sent && state.server_next_chunk == 1);
    assert(transport.kind[0] == PEER_STATUS_REQUEST);
    assert(transport.kind[1] == PEER_STATUS_RESPONSE);
}

static void test_legacy_fallback(void) {
    const uint32_t tokens[] = {1, UINT32_C(0x7fffffff), UINT32_C(0x80000000), UINT32_MAX};
    const uint32_t fallback[] = {UINT32_C(0x80000001), UINT32_MAX, 1, UINT32_C(0x80000000)};
    for (unsigned i = 0; i < 4; ++i) {
        peer_status_t client;
        peer_status_init(&client, 0);
        assert(peer_status_start(&client, tokens[i], 0) == PEER_STATUS_STARTED);
        transport_t sent = {0};
        /* A queued request starts its probe clock only when enqueued; the
         * overall deadline still includes these first 200 ms. */
        peer_status_task(&client, 200000, transmit, &sent);
        assert(sent.count == 1 && sent.payload[0][4] == 2);
        peer_status_task(&client, 349999, transmit, &sent);
        assert(sent.count == 1);
        peer_status_task(&client, 350000, transmit, &sent);
        assert(sent.count == 2 && sent.payload[1][4] == 1);
        uint8_t expected[8];
        request(expected, fallback[i]); expected[4] = 1;
        assert(memcmp(sent.payload[1], expected, 8) == 0);

        /* A delayed or malformed v2 reply cannot contaminate the fallback. */
        uint8_t stale[8];
        request(stale, tokens[i]); stale[4] = 255;
        peer_status_receive_response(&client, stale, 351000);
        assert(client.client_active && client.client_received == 0);
        transport_t legacy = replies_version(fallback[i], 1, 1);
        for (unsigned chunk = 0; chunk < 13; ++chunk)
            peer_status_receive_response(&client, legacy.payload[chunk], 352000 + chunk);
        peer_status_result_t result = take(&client, tokens[i], PEER_STATUS_OK);
        peer_status_snapshot_t original = example(1);
        original.protocol = 1;
        original.runtime = (diagnostic_runtime_snapshot_t){0};
        assert_snapshot(&result.snapshot, &original);
        assert(result.observation.boot == DIAGNOSTIC_PEER_UNOBSERVED);
        assert(result.observation.progress == DIAGNOSTIC_PROGRESS_UNAVAILABLE);
        assert(result.observation.update == DIAGNOSTIC_EXECUTION_NOT_OBSERVED);
    }
    /* Receiving even one matching v2 chunk establishes support; an incomplete
     * v2 snapshot remains a timeout, rather than silently downgrading. */
    peer_status_t client;
    start_client(&client, 0, 7, 0);
    transport_t packets = replies(7, 1), sent = {0};
    peer_status_receive_response(&client, packets.payload[10], 1000);
    peer_status_task(&client, 150000, transmit, &sent);
    assert(sent.count == 0 && client.client_protocol == 2);
    peer_status_task(&client, 500000, transmit, &sent);
    take(&client, 7, PEER_STATUS_TIMEOUT);
}

static uint32_t reverse_bits(uint32_t value, unsigned count) {
    uint32_t reversed = 0;
    for (unsigned bit = 0; bit < count; ++bit) {
        reversed = reversed * 2 + (value & 1);
        value /= 2;
    }
    return reversed;
}

/* Independent MSB-first CRC oracle for repaired-checksum structural cases. */
static uint32_t oracle_crc(const uint8_t *bytes, unsigned count) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < count; ++i) {
        crc ^= reverse_bits(bytes[i], 8) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & UINT32_C(0x80000000)) ? UINT32_C(0x04c11db7) : 0);
    }
    return ~reverse_bits(crc, 32);
}

static void test_runtime_validation(void) {
    transport_t packets = replies(7, 1);
    uint8_t good[78], wire[78];
    for (unsigned i = 0; i < 26; ++i)
        memcpy(good + i * 3, packets.payload[i] + 5, 3);
    assert(oracle_crc((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    const struct { unsigned offset, size; uint32_t value; } invalid[] = {
        {34, 1, 8}, {35, 1, 7}, {66, 1, 3}, {67, 1, 1}, {76, 1, 1}, {77, 1, 1},
        {52, 4, 262145}, {56, 4, 262145}, {56, 4, 123455}, {34, 1, 3},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(wire, good, sizeof(wire));
        for (unsigned byte = 0; byte < invalid[i].size; ++byte)
            wire[invalid[i].offset + byte] = (uint8_t)(invalid[i].value >> (byte * 8));
        uint32_t crc = oracle_crc(wire, 72);
        for (unsigned byte = 0; byte < 4; ++byte)
            wire[72 + byte] = (uint8_t)(crc >> (byte * 8));
        peer_status_t client;
        start_client(&client, 0, 7, 0);
        for (unsigned chunk = 0; chunk < 26; ++chunk) {
            uint8_t payload[8];
            memcpy(payload, packets.payload[chunk], 8);
            memcpy(payload + 5, wire + chunk * 3, 3);
            peer_status_receive_response(&client, payload, 1000 + chunk);
        }
        take(&client, 7, PEER_STATUS_INVALID);
    }
    /* Unpublished checkpoints and no update are represented by validity bits,
     * with saturated ages permitted instead of invented zero measurements. */
    peer_status_snapshot_t idle = example(1);
    idle.runtime = (diagnostic_runtime_snapshot_t){
        .total_bytes = 262144, .core_age_ms = {UINT32_MAX, UINT32_MAX},
        .progress_age_ms = UINT32_MAX,
    };
    peer_status_t server, client;
    peer_status_init(&server, 1);
    uint8_t payload[8]; request(payload, 7);
    assert(peer_status_receive_request(&server, payload, &idle, 0));
    transport_t transport = {0};
    start_client(&client, 0, 7, 0);
    for (unsigned i = 0; i < 26; ++i) {
        peer_status_task(&server, i * 1000, transmit, &transport);
        peer_status_receive_response(&client, transport.payload[i], i * 1000);
    }
    peer_status_result_t result = take(&client, 7, PEER_STATUS_OK);
    assert_snapshot(&result.snapshot, &idle);
}

int main(void) {
    test_wire_and_reordering();
    test_corruption_and_stale_packets();
    test_client_lifetime_and_refusal();
    test_server_validation_and_rate_limits();
    test_bidirectional_queries();
    test_shared_pacer_fairness();
    test_legacy_fallback();
    test_runtime_validation();
    puts("peer status: v1/v2 wire vectors, 104 reorderings, 624 corruptions, timeout, backpressure and bidirectional contracts passed");
    return 0;
}
