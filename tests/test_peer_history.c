#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "peer_history.h"

/* An independent MSB-first CRC oracle: reflect each input byte and the final
 * result, rather than using the production reflected update loop. */
static uint32_t reverse_bits(uint32_t value, unsigned count) {
    uint32_t reversed = 0;
    for (unsigned i = 0; i < count; ++i) {
        reversed = reversed * 2 + (value & 1);
        value /= 2;
    }
    return reversed;
}

static uint32_t oracle_crc(const uint8_t *bytes, unsigned count) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < count; ++i) {
        crc ^= reverse_bits(bytes[i], 8) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & UINT32_C(0x80000000)) ? UINT32_C(0x04c11db7) : 0);
    }
    return ~reverse_bits(crc, 32);
}

static void put(uint8_t *bytes, unsigned size, uint64_t value) {
    for (unsigned i = 0; i < size; ++i) {
        bytes[i] = (uint8_t)(value % 256);
        value /= 256;
    }
}

static unsigned length(unsigned requested) { return 68 + 24 * requested; }

static void sign_wire(uint8_t *wire, unsigned requested) {
    put(wire + length(requested) - 4, 4, oracle_crc(wire, length(requested) - 4));
}

static history_event_t example(uint64_t seq) {
    return (history_event_t){
        .seq = seq, .time_us = seq * 1000, .value = (uint32_t)(seq * UINT32_C(0xdeadbeef)),
        .type = (uint8_t)(1 + seq % 10), .a = (uint8_t)(seq * 3), .b = (uint8_t)(seq * 7),
    };
}

static void put_event(uint8_t *wire, const history_event_t *event) {
    put(wire, 8, event->seq);
    put(wire + 8, 8, event->time_us);
    put(wire + 16, 4, event->value);
    wire[20] = event->type;
    wire[21] = event->a;
    wire[22] = event->b;
    wire[23] = event->reserved;
}

static void fixture(uint8_t wire[PEER_HISTORY_MAX_WIRE_SIZE], uint8_t role,
                    unsigned requested, unsigned count, uint64_t first, uint64_t oldest) {
    memset(wire, 0, PEER_HISTORY_MAX_WIRE_SIZE);
    wire[0] = PEER_HISTORY_PROTOCOL; wire[1] = role; wire[2] = (uint8_t)count; wire[3] = 24;
    put(wire + 8, 8, UINT64_C(0x8877665544332211));
    put(wire + 16, 8, 1000000);
    put(wire + 24, 8, first);
    put(wire + 32, 8, first + count);
    put(wire + 40, 8, oldest);
    put(wire + 48, 8, oldest - 1);
    for (unsigned i = 0; i < count; ++i) {
        history_event_t event = example(first + i);
        put_event(wire + 64 + i * 24, &event);
    }
    sign_wire(wire, requested);
}

static void request(uint8_t payload[8], uint32_t token, unsigned count) {
    memset(payload, 0, 8);
    put(payload, 4, token);
    payload[4] = PEER_HISTORY_PROTOCOL;
    payload[5] = (uint8_t)count;
}

typedef struct endpoint endpoint_t;
struct endpoint {
    peer_history_t protocol;
    history_store_t store;
    endpoint_t *remote;
    uint64_t now, refuse_until, missing_seq;
    unsigned refuse_every, attempts, transmitted, requests, responses, begins, reads, offered;
    uint64_t last_attempt;
    uint8_t last_request[8];
    uint8_t wire[PEER_HISTORY_MAX_WIRE_SIZE];
};

static history_window_t begin(void *context, unsigned limit, uint64_t *sampled_at_us) {
    endpoint_t *ep = context;
    ep->begins++;
    *sampled_at_us = ep->now;
    return history_store_window(&ep->store, limit);
}

static bool read_event(void *context, uint64_t seq, history_event_t *event) {
    endpoint_t *ep = context;
    ep->reads++;
    return seq != ep->missing_seq && history_store_read(&ep->store, seq, event);
}

static bool transmit(void *context, peer_history_packet_t kind, const uint8_t payload[8]) {
    endpoint_t *ep = context;
    if (ep->attempts)
        assert(ep->now - ep->last_attempt >= 1000);
    ep->last_attempt = ep->now;
    ep->attempts++;
    if (ep->now < ep->refuse_until || (ep->refuse_every && ep->attempts % ep->refuse_every == 0))
        return false;
    ep->transmitted++;
    if (kind == PEER_HISTORY_REQUEST) {
        ep->requests++;
        memcpy(ep->last_request, payload, 8);
        if (ep->remote)
            peer_history_receive_request(&ep->remote->protocol, payload, ep->now);
    } else {
        ep->responses++;
        unsigned index = payload[4] + 256u * payload[5];
        assert(index < PEER_HISTORY_MAX_CHUNKS);
        ep->wire[index * 2] = payload[6];
        ep->wire[index * 2 + 1] = payload[7];
        if (ep->remote)
            peer_history_receive_response(&ep->remote->protocol, payload, ep->now);
    }
    return true;
}

static void init(endpoint_t *ep, uint8_t role, unsigned count) {
    memset(ep, 0, sizeof(*ep));
    ep->now = 1000000;
    peer_history_init(&ep->protocol, role, UINT64_C(0x8877665544332211));
    history_store_init(&ep->store);
    for (unsigned i = 1; i <= count; ++i) {
        history_event_t event = example(i);
        history_store_record(&ep->store, event.time_us, (history_type_t)event.type,
                              event.a, event.b, event.value);
    }
}

static void tick(endpoint_t *ep, uint64_t now) {
    ep->now = now;
    unsigned reads = ep->reads, begins = ep->begins, attempts = ep->attempts;
    unsigned client_crc = ep->protocol.client_crc_offset;
    unsigned server_crc = ep->protocol.server_crc_offset;
    unsigned validated = ep->protocol.client_validate_index;
    peer_history_task(&ep->protocol, now, transmit, ep, begin, read_event);
    assert(ep->reads - reads <= 1 && ep->begins - begins <= 1 && ep->attempts - attempts <= 1);
    assert(ep->protocol.client_crc_offset - client_crc <= 64);
    assert(ep->protocol.server_crc_offset - server_crc <= 64);
    assert(ep->protocol.client_validate_index - validated <= 1);
}

static const peer_history_result_t *finish(endpoint_t *ep, uint64_t first_tick) {
    for (unsigned i = 0; i < 200; ++i) {
        const peer_history_result_t *result = peer_history_peek_result(&ep->protocol);
        if (result)
            return result;
        tick(ep, first_tick + i * 1000);
    }
    assert(!"bounded result validation did not finish");
    return NULL;
}

static void start(endpoint_t *ep, uint32_t token, unsigned count) {
    assert(peer_history_start(&ep->protocol, token, count, ep->now) == PEER_HISTORY_STARTED);
    tick(ep, ep->now);
    assert(ep->requests == 1);
    uint8_t expected[8];
    request(expected, token, count);
    assert(memcmp(ep->last_request, expected, 8) == 0);
}

static void chunk(endpoint_t *ep, const uint8_t *wire, uint32_t token, unsigned index, uint64_t now) {
    uint8_t payload[8];
    put(payload, 4, token);
    put(payload + 4, 2, index);
    payload[6] = wire[index * 2];
    payload[7] = wire[index * 2 + 1];
    peer_history_receive_response(&ep->protocol, payload, now);
}

static void deliver(endpoint_t *ep, const uint8_t *wire, uint32_t token, unsigned count,
                     unsigned rotation, bool reverse, bool duplicate) {
    unsigned chunks = length(count) / 2;
    for (unsigned i = 0; i < chunks; ++i) {
        unsigned index = ((reverse ? chunks - 1 - i : i) + rotation) % chunks;
        chunk(ep, wire, token, index, ep->now + 1000 + i);
        if (duplicate)
            chunk(ep, wire, token, index, ep->now + 1000 + i);
    }
}

static void assert_event(const history_event_t *actual, const history_event_t *expected) {
    assert(actual->seq == expected->seq && actual->time_us == expected->time_us);
    assert(actual->value == expected->value && actual->type == expected->type);
    assert(actual->a == expected->a && actual->b == expected->b && actual->reserved == 0);
}

static void test_wire_and_success(void) {
    assert(oracle_crc((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    const unsigned limits[] = {1, 16, 64};
    for (unsigned role = 0; role < 2; ++role) {
        for (unsigned n = 0; n < 3; ++n) {
            unsigned limit = limits[n];
            endpoint_t server;
            init(&server, (uint8_t)role, 80);
            uint8_t payload[8];
            request(payload, UINT32_C(0xa1b2c3d4), limit);
            assert(peer_history_receive_request(&server.protocol, payload, server.now));
            assert(server.begins == 0 && server.reads == 0);
            for (unsigned i = 0; i < 1200 && server.responses < length(limit) / 2; ++i)
                tick(&server, 1000000 + i * 1000);
            assert(server.responses == length(limit) / 2 && server.begins == 1);
            assert(server.reads == limit);
            uint8_t expected[PEER_HISTORY_MAX_WIRE_SIZE];
            fixture(expected, (uint8_t)role, limit, limit, 81 - limit, 17);
            assert(memcmp(server.wire, expected, length(limit)) == 0);

            /* Reverse and rotated wire delivery preserves source order; every
             * repeated chunk must be idempotent. */
            for (unsigned order = 0; order < 4; ++order) {
                endpoint_t client;
                init(&client, (uint8_t)(role ^ 1), 0);
                start(&client, UINT32_C(0xa1b2c3d4), limit);
                deliver(&client, expected, UINT32_C(0xa1b2c3d4), limit, order * 11,
                         order & 1, true);
                assert(!peer_history_peek_result(&client.protocol));
                const peer_history_result_t *result = finish(&client, 1010000);
                assert(result->outcome == PEER_HISTORY_OK);
                assert(result->token == UINT32_C(0xa1b2c3d4));
                assert(result->requested_at_us == 1000000 && result->first_response_us == 1001000);
                assert(result->snapshot.role == role && result->snapshot.protocol == 2);
                assert(result->snapshot.boot_session == UINT64_C(0x8877665544332211));
                assert(result->snapshot.sampled_at_us == 1000000);
                assert(result->snapshot.window.first_seq == 81 - limit);
                assert(result->snapshot.window.end_seq == 81);
                assert(result->snapshot.window.oldest_seq == 17);
                assert(result->snapshot.window.overwritten == 16);
                assert(result->snapshot.window.count == limit && result->snapshot.gap_mask == 0);
                for (unsigned i = 0; i < limit; ++i) {
                    history_event_t event = example(81 - limit + i);
                    assert_event(&result->snapshot.events[i], &event);
                }
                assert(peer_history_start(&client.protocol, 7, 1, client.now) == PEER_HISTORY_BUSY);
                peer_history_release_result(&client.protocol);
                assert(!peer_history_peek_result(&client.protocol));
                assert(peer_history_start(&client.protocol, 7, 1, client.now) == PEER_HISTORY_STARTED);
            }
        }
    }
}

static void expect_invalid(const uint8_t *wire, unsigned limit) {
    endpoint_t client;
    init(&client, 0, 0);
    start(&client, 7, limit);
    deliver(&client, wire, 7, limit, 17, true, false);
    assert(finish(&client, 1010000)->outcome == PEER_HISTORY_INVALID);
}

static void test_corruption_and_structure(void) {
    uint8_t good[PEER_HISTORY_MAX_WIRE_SIZE], wire[PEER_HISTORY_MAX_WIRE_SIZE];
    fixture(good, 1, 16, 2, 1, 1);
    /* Every transmitted byte, including checksum and zero padding, is covered
     * by CRC. Structural cases below repair CRC to test independent checks. */
    for (unsigned byte = 0; byte < length(16); ++byte) {
        memcpy(wire, good, sizeof(wire));
        wire[byte] ^= 1;
        expect_invalid(wire, 16);
    }
    const struct { unsigned offset, width; uint64_t value; } invalid[] = {
        {0,1,3}, {1,1,0}, {2,1,17}, {2,1,1}, {3,1,23}, {4,4,1},
        {24,8,0}, {24,8,UINT64_MAX}, {32,8,2}, {32,8,UINT64_MAX},
        {40,8,0}, {40,8,2}, {48,8,1}, {48,8,UINT64_MAX}, {56,8,4},
        {64,8,2}, {64+8,8,1000001}, {64+20,1,0},
        {64+23,1,1}, {88+8,8,999}, {112,1,1}, {56,8,1},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(wire, good, sizeof(wire));
        put(wire + invalid[i].offset, invalid[i].width, invalid[i].value);
        sign_wire(wire, 16);
        expect_invalid(wire, 16);
    }
    /* All unused bytes, not just the beginning of a slot, must be zero. */
    for (unsigned byte = 112; byte < length(16) - 4; ++byte) {
        memcpy(wire, good, sizeof(wire));
        wire[byte] = 1;
        sign_wire(wire, 16);
        expect_invalid(wire, 16);
    }
}

static void test_missing_stale_and_deadlines(void) {
    uint8_t wire[PEER_HISTORY_MAX_WIRE_SIZE];
    fixture(wire, 1, 1, 1, 1, 1);
    for (unsigned missing = 0; missing < length(1) / 2; ++missing) {
        endpoint_t ep;
        init(&ep, 0, 0);
        start(&ep, 7, 1);
        for (unsigned i = 0; i < length(1) / 2; ++i)
            if (i != missing)
                chunk(&ep, wire, 7, i, 1001000);
        tick(&ep, 3999999);
        assert(!peer_history_peek_result(&ep.protocol));
        tick(&ep, 4000000);
        assert(peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_TIMEOUT);
    }
    endpoint_t ep;
    init(&ep, 0, 0);
    assert(peer_history_start(&ep.protocol, 7, 1, ep.now) == PEER_HISTORY_STARTED);
    deliver(&ep, wire, 7, 1, 0, false, false); /* Request not yet enqueued. */
    assert(ep.protocol.client_received_count == 0);
    tick(&ep, ep.now);
    deliver(&ep, wire, 8, 1, 0, false, false); /* Another request's token. */
    assert(ep.protocol.client_received_count == 0);
    deliver(&ep, wire, 7, 1, 0, false, false);
    tick(&ep, 4000000); /* Receiving all bytes does not waive the deadline. */
    assert(peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_TIMEOUT);

    for (unsigned index = length(1) / 2; index < PEER_HISTORY_MAX_CHUNKS + 3; ++index) {
        init(&ep, 0, 0);
        start(&ep, 7, 1);
        uint8_t payload[8] = {7};
        put(payload + 4, 2, index);
        peer_history_receive_response(&ep.protocol, payload, 1001000);
        assert(peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_INVALID);
    }
    init(&ep, 0, 0);
    start(&ep, 7, 1);
    chunk(&ep, wire, 7, 0, 1001000);
    wire[0] ^= 1;
    chunk(&ep, wire, 7, 0, 1002000);
    assert(peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_INVALID);
}

static void test_gaps_and_empty(void) {
    const unsigned counts[] = {0, 2, 64};
    for (unsigned n = 0; n < 3; ++n) {
        endpoint_t a, b;
        init(&a, 0, 0);
        init(&b, 1, counts[n]);
        a.remote = &b; b.remote = &a;
        b.missing_seq = counts[n];
        assert(peer_history_start(&a.protocol, 11, 64, a.now) == PEER_HISTORY_STARTED);
        for (uint64_t now = 1000000; now < 3000000 && !peer_history_peek_result(&a.protocol); now += 1000) {
            tick(&a, now); tick(&b, now);
        }
        const peer_history_result_t *result = peer_history_peek_result(&a.protocol);
        assert(result && result->outcome == PEER_HISTORY_OK);
        assert(result->snapshot.window.count == counts[n]);
        assert(result->snapshot.window.first_seq == 1);
        assert(result->snapshot.window.end_seq == counts[n] + 1);
        assert(result->snapshot.gap_mask == (counts[n] ? UINT64_C(1) << (counts[n] - 1) : 0));
        for (unsigned i = 0; i < 64; ++i) {
            history_event_t event = {0};
            if (i + 1 < counts[n])
                event = example(i + 1);
            assert_event(&result->snapshot.events[i], &event);
        }
    }
}

static void test_bidirectional_and_borrow(void) {
    endpoint_t a, b;
    init(&a, 0, 80); init(&b, 1, 80);
    a.remote = &b; b.remote = &a;
    a.refuse_every = 5; b.refuse_every = 7;
    a.refuse_until = 1050000; b.refuse_until = 1070000;
    assert(peer_history_start(&a.protocol, 101, 64, a.now) == PEER_HISTORY_STARTED);
    assert(peer_history_start(&b.protocol, 202, 64, b.now) == PEER_HISTORY_STARTED);
    uint64_t now;
    for (now = 1000000; now < 3500000; now += 1000) {
        tick(&a, now); tick(&b, now);
        if (peer_history_peek_result(&a.protocol) && peer_history_peek_result(&b.protocol))
            break;
    }
    assert(now < 3500000);
    const peer_history_result_t *borrowed = peer_history_peek_result(&a.protocol);
    assert(borrowed && borrowed->outcome == PEER_HISTORY_OK);
    assert(peer_history_peek_result(&b.protocol)->outcome == PEER_HISTORY_OK);
    assert(a.responses == 802 && b.responses == 802 && a.requests == 1 && b.requests == 1);
    uint8_t saved[sizeof(*borrowed)];
    memcpy(saved, borrowed, sizeof(saved));
    peer_history_release_result(&b.protocol);
    assert(peer_history_start(&b.protocol, 203, 1, now) == PEER_HISTORY_STARTED);
    for (now += 1000; now < 5000000 && !peer_history_peek_result(&b.protocol); now += 1000) {
        tick(&a, now); tick(&b, now);
        assert(peer_history_peek_result(&a.protocol) == borrowed);
        assert(memcmp(saved, borrowed, sizeof(saved)) == 0);
    }
    assert(peer_history_peek_result(&b.protocol));
    assert(peer_history_peek_result(&b.protocol)->outcome == PEER_HISTORY_OK);
    assert(a.responses == 848); /* Served another peer while its own result stayed borrowed. */
}

static void test_capture_overwritten_by_producer(void) {
    endpoint_t a, b;
    init(&a, 0, 0); init(&b, 1, 64);
    a.remote = &b; b.remote = &a;
    assert(peer_history_start(&a.protocol, 73, 64, a.now) == PEER_HISTORY_STARTED);
    for (uint64_t now = 1000000; now < 3000000; now += 1000) {
        tick(&a, now); tick(&b, now);
        if (now == 1008000) {
            assert(b.reads == 8);
            /* The remaining old slots disappear before the incremental
             * reader reaches them; already copied records stay intact. */
            for (unsigned seq = 65; seq <= 128; ++seq) {
                history_event_t event = example(seq);
                history_store_record(&b.store, event.time_us, (history_type_t)event.type,
                                      event.a, event.b, event.value);
            }
        }
        if (peer_history_peek_result(&a.protocol))
            break;
    }
    const peer_history_result_t *result = peer_history_peek_result(&a.protocol);
    assert(result && result->outcome == PEER_HISTORY_OK);
    assert(b.begins == 1 && b.reads == 64);
    assert(result->snapshot.window.first_seq == 1 && result->snapshot.window.end_seq == 65);
    assert(result->snapshot.window.overwritten == 0);
    assert(result->snapshot.gap_mask == (UINT64_MAX << 8));
    for (unsigned i = 0; i < 64; ++i) {
        history_event_t expected = i < 8 ? example(i + 1) : (history_event_t){0};
        assert_event(&result->snapshot.events[i], &expected);
    }
}

static bool alternate_grant(void *context, peer_history_packet_t kind, const uint8_t payload[8]) {
    endpoint_t *ep = context;
    if (++ep->offered & 1u)
        return false;
    return transmit(ep, kind, payload);
}

static void test_external_pacer_does_not_starve_request(void) {
    endpoint_t ep;
    init(&ep, 0, 64);
    uint8_t payload[8];
    request(payload, 33, 64);
    assert(peer_history_receive_request(&ep.protocol, payload, ep.now));
    /* Prepare a maximum-sized server response while TX is unavailable. */
    for (unsigned i = 0; i < 100; ++i) {
        ep.now = 1000000 + i * 1000;
        peer_history_task(&ep.protocol, ep.now, NULL, &ep, begin, read_event);
    }
    assert(ep.reads == 64 && ep.responses == 0);
    assert(peer_history_start(&ep.protocol, 44, 1, ep.now) == PEER_HISTORY_STARTED);
    /* The global diagnostic arbiter grants this protocol every second tick.
     * Its new request must get that grant before the 802-response stream. */
    for (unsigned i = 0; i < 6; ++i) {
        ep.now = 1100000 + i * 1000;
        peer_history_task(&ep.protocol, ep.now, alternate_grant, &ep, begin, read_event);
        if (i == 1)
            assert(ep.requests == 1 && ep.responses == 0);
    }
    assert(ep.offered == 6 && ep.requests == 1 && ep.responses == 2);
}

static void test_legacy_wire_and_filter(void) {
    endpoint_t server;
    init(&server, 1, 0);
    const uint8_t types[] = {1, 11, 15, 255, 10};
    for (unsigned i = 0; i < sizeof(types); ++i) {
        history_event_t event = example(i + 1);
        history_store_record(&server.store, event.time_us, (history_type_t)types[i],
                              event.a, event.b, event.value);
    }
    uint8_t payload[8];
    request(payload, 11, 16); payload[4] = 1;
    assert(peer_history_receive_request(&server.protocol, payload, server.now));
    for (unsigned tick_count = 0; tick_count < 400 && server.responses != 226; ++tick_count)
        tick(&server, 1000000 + tick_count * 1000);
    assert(server.responses == 226 && server.reads == 5);

    /* The v1 byte layout and CRC remain unchanged. Future event contents must
     * be absent, with their original sequence positions represented as gaps. */
    uint8_t expected[PEER_HISTORY_MAX_WIRE_SIZE];
    fixture(expected, 1, 16, 5, 1, 1);
    expected[0] = 1;
    put(expected + 56, 8, 14);
    for (unsigned i = 0; i < sizeof(types); ++i) {
        if (types[i] > 10)
            memset(expected + 64 + i * 24, 0, 24);
        else
            expected[64 + i * 24 + 20] = types[i];
    }
    sign_wire(expected, 16);
    assert(memcmp(server.wire, expected, length(16)) == 0);

    /* A current reader still receives every new or unknown nonzero event
     * type, including its numeric arguments, without a protocol bump. */
    request(payload, 12, 16);
    assert(peer_history_receive_request(&server.protocol, payload, 1500000));
    unsigned previous_responses = server.responses;
    for (unsigned i = 0; i < 400 && server.responses != previous_responses + 226; ++i)
        tick(&server, 1500000 + i * 1000);
    endpoint_t client;
    init(&client, 0, 0);
    start(&client, 12, 16);
    deliver(&client, server.wire, 12, 16, 3, true, true);
    const peer_history_result_t *result = finish(&client, 1010000);
    assert(result->outcome == PEER_HISTORY_OK && result->snapshot.protocol == 2);
    assert(result->snapshot.gap_mask == 0 && result->snapshot.window.count == 5);
    for (unsigned i = 0; i < sizeof(types); ++i) {
        history_event_t event = example(i + 1);
        event.type = types[i];
        assert_event(&result->snapshot.events[i], &event);
    }
}

static void test_legacy_fallback_and_stale_chunks(void) {
    const uint32_t tokens[] = {1, UINT32_C(0x7fffffff), UINT32_C(0x80000000), UINT32_MAX};
    const uint32_t fallback[] = {UINT32_C(0x80000001), UINT32_MAX, 1, UINT32_C(0x80000000)};
    const unsigned limits[] = {1, 16, 64};
    for (unsigned token_index = 0; token_index < 4; ++token_index) {
        for (unsigned n = 0; n < 3; ++n) {
            unsigned limit = limits[n];
            endpoint_t ep;
            init(&ep, 0, 0);
            start(&ep, tokens[token_index], limit);
            assert(ep.last_request[4] == 2);
            tick(&ep, 1249999);
            assert(ep.requests == 1);
            tick(&ep, 1250000);
            assert(ep.requests == 2 && ep.last_request[4] == 1);
            uint8_t expected_request[8];
            request(expected_request, fallback[token_index], limit); expected_request[4] = 1;
            assert(memcmp(ep.last_request, expected_request, 8) == 0);
            uint8_t stale[8];
            request(stale, tokens[token_index], limit);
            stale[4] = stale[5] = 255;
            peer_history_receive_response(&ep.protocol, stale, 1251000);
            assert(ep.protocol.client_active && ep.protocol.client_received_count == 0);

            uint8_t wire[PEER_HISTORY_MAX_WIRE_SIZE];
            fixture(wire, 1, limit, limit, 1, 1);
            wire[0] = 1; sign_wire(wire, limit);
            deliver(&ep, wire, fallback[token_index], limit, 7, true, true);
            const peer_history_result_t *result = finish(&ep, 1260000);
            assert(result->outcome == PEER_HISTORY_OK && result->token == tokens[token_index]);
            assert(result->requested_at_us == 1000000 && result->first_response_us == 1251000);
            assert(result->snapshot.protocol == 1 && result->snapshot.window.count == limit);
            for (unsigned i = 0; i < limit; ++i) {
                history_event_t expected = example(i + 1);
                assert_event(&result->snapshot.events[i], &expected);
            }
        }
    }
    endpoint_t ep;
    init(&ep, 0, 0);
    uint8_t wire[PEER_HISTORY_MAX_WIRE_SIZE];
    fixture(wire, 1, 1, 1, 1, 1);
    start(&ep, 7, 1);
    chunk(&ep, wire, 7, 5, 1001000);
    tick(&ep, 1250000);
    assert(ep.requests == 1 && ep.protocol.client_protocol == 2);
    tick(&ep, 4000000);
    assert(peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_TIMEOUT);

    /* Fallback does not refresh the original request's three-second deadline. */
    init(&ep, 0, 0);
    assert(peer_history_start(&ep.protocol, 8, 1, 1000000) == PEER_HISTORY_STARTED);
    tick(&ep, 3900000);
    assert(ep.requests == 1);
    tick(&ep, 4000000);
    assert(ep.requests == 1 && peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_TIMEOUT);
}

static void test_rejection_pacing_and_expiry(void) {
    endpoint_t ep;
    init(&ep, 0, 1);
    assert(peer_history_start(&ep.protocol, 0, 1, ep.now) == PEER_HISTORY_BAD_ARGUMENT);
    assert(peer_history_start(&ep.protocol, 1, 0, ep.now) == PEER_HISTORY_BAD_ARGUMENT);
    assert(peer_history_start(&ep.protocol, 1, 65, ep.now) == PEER_HISTORY_BAD_ARGUMENT);
    assert(peer_history_start(&ep.protocol, 1, UINT_MAX, ep.now) == PEER_HISTORY_BAD_ARGUMENT);
    uint8_t payload[8], malformed[8];
    request(payload, 1, 1);
    const unsigned offsets[] = {0, 4, 5, 6, 7};
    for (unsigned i = 0; i < 5; ++i) {
        memcpy(malformed, payload, 8);
        malformed[offsets[i]] = offsets[i] < 6 ? 0 : 1;
        assert(!peer_history_receive_request(&ep.protocol, malformed, ep.now));
    }
    assert(peer_history_receive_request(&ep.protocol, payload, ep.now));
    assert(!peer_history_receive_request(&ep.protocol, payload, ep.now));
    for (unsigned i = 0; i < 100; ++i)
        tick(&ep, 1000000 + i * 1000);
    assert(ep.responses == 46);
    request(payload, 2, 1);
    assert(!peer_history_receive_request(&ep.protocol, payload, 1199999));
    assert(peer_history_receive_request(&ep.protocol, payload, 1200000));
    /* No provider means no capture or response; the accepted request expires. */
    peer_history_task(&ep.protocol, 4200000, NULL, NULL, NULL, NULL);
    assert(!peer_history_receive_request(&ep.protocol, payload, 4200000));
    request(payload, 3, 1);
    assert(peer_history_receive_request(&ep.protocol, payload, 4200000));

    init(&ep, 0, 64);
    request(payload, 9, 64);
    assert(peer_history_receive_request(&ep.protocol, payload, 1000000));
    tick(&ep, 1000000);
    tick(&ep, 3999999); /* A slow task may capture one event, never catch up in a loop. */
    assert(ep.reads == 1 && ep.responses == 0);
    tick(&ep, 4000000);
    assert(ep.reads == 1 && ep.responses == 0);

    init(&ep, 0, 0);
    ep.refuse_until = UINT64_MAX;
    assert(peer_history_start(&ep.protocol, 1, 1, 1000000) == PEER_HISTORY_STARTED);
    for (uint64_t now = 1000000; now < 1010000; now += 100)
        tick(&ep, now);
    assert(ep.attempts == 10 && ep.requests == 0);
    tick(&ep, 4000000);
    assert(peer_history_peek_result(&ep.protocol)->outcome == PEER_HISTORY_TIMEOUT);

    init(&ep, 0, 1);
    ep.refuse_until = UINT64_MAX;
    request(payload, 8, 1);
    assert(peer_history_receive_request(&ep.protocol, payload, 1000000));
    for (uint64_t now = 1000000; now <= 4000000; now += 1000)
        tick(&ep, now);
    assert(ep.attempts > 0 && ep.responses == 0);
    unsigned attempts = ep.attempts;
    tick(&ep, 4001000);
    assert(ep.attempts == attempts); /* No endless background retransmission. */

    init(&ep, 2, 0);
    assert(peer_history_start(&ep.protocol, 1, 1, 1000000) == PEER_HISTORY_BAD_ARGUMENT);
    assert(!peer_history_receive_request(&ep.protocol, payload, 1000000));
}

int main(void) {
    test_wire_and_success();
    test_corruption_and_structure();
    test_missing_stale_and_deadlines();
    test_gaps_and_empty();
    test_bidirectional_and_borrow();
    test_capture_overwritten_by_producer();
    test_external_pacer_does_not_starve_request();
    test_legacy_wire_and_filter();
    test_legacy_fallback_and_stale_chunks();
    test_rejection_pacing_and_expiry();
    puts("peer history: independent wire/CRC, roles/counts/orders, corruption, gaps, deadlines, pacing and borrowed-result contracts passed");
    return 0;
}
