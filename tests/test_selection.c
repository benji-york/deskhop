#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "selection.h"

static void assert_token(selection_state_t state, uint32_t counter, uint8_t origin, uint8_t output) {
    assert(state.counter == counter);
    assert(state.origin == origin);
    assert(state.output == output);
}

int main(void) {
    unsigned cases = 0;
    /* Independently specified total order for a finite interval without wrap:
     * all delivery orders must preserve the greatest (counter, origin) token.
     * A single token cannot legitimately name two different values. */
    for (uint32_t ac = 0; ac < 8; ac++)
        for (uint32_t bc = 0; bc < 8; bc++)
            for (uint8_t ao = 0; ao < 2; ao++)
                for (uint8_t bo = 0; bo < 2; bo++)
                    for (uint8_t av = 0; av < 2; av++)
                        for (uint8_t bv = 0; bv < 2; bv++) {
                            if (ac == bc && ao == bo && av != bv)
                                continue;
                            selection_state_t a = {.counter=ac, .origin=ao, .output=av, .joined=true};
                            selection_state_t b = {.counter=bc, .origin=bo, .output=bv, .joined=true};
                            selection_state_t left = a, right = b;
                            selection_merge(&left, &b, 0);
                            selection_merge(&right, &a, 1);
                            selection_state_t expected = ac > bc || (ac == bc && ao > bo) ? a : b;
                            assert_token(left, expected.counter, expected.origin, expected.output);
                            assert_token(right, expected.counter, expected.origin, expected.output);
                            assert(!selection_merge(&left, &left, 0));
                            cases++;
                        }

    selection_state_t passive = {0};
    selection_state_t live = {.counter=100, .origin=1, .output=1, .joined=true};
    assert(selection_merge(&passive, &live, 0));
    assert_token(passive, 100, 1, 1);
    selection_state_t early = {0};
    selection_request(&early, 0, 0);
    assert(early.pending_local);
    assert(selection_merge(&early, &live, 0));
    assert_token(early, 101, 0, 0);
    assert(early.joined && !early.pending_local);
    assert(!selection_merge(&early, &live, 0));
    assert_token(early, 101, 0, 0);

    /* First replies during a two-fresh-peer handshake cannot impersonate a
     * surviving joined peer and rebase the losing concurrent intent. */
    for (unsigned first = 0; first < 2; first++) {
        selection_state_t peers[2] = {{0}, {0}};
        selection_request(&peers[0], 1, 0);
        selection_request(&peers[1], 0, 1);
        selection_state_t first_message = peers[first];
        unsigned receiver = 1 - first;
        assert(selection_merge(&peers[receiver], &first_message, receiver));
        assert(peers[receiver].joining && !peers[receiver].joined);
        selection_state_t reply = peers[receiver];
        assert(selection_merge(&peers[first], &reply, first));
        assert(peers[first].joined);
        reply = peers[first];
        assert(selection_merge(&peers[receiver], &reply, receiver));
        for (unsigned i = 0; i < 2; i++) {
            assert(peers[i].joined && !peers[i].joining && !peers[i].pending_local);
            assert_token(peers[i], 1, 1, 0);
        }
    }

    live.counter = UINT32_MAX;
    selection_state_t wrapping = {0};
    assert(selection_merge(&wrapping, &live, 0));
    selection_request(&wrapping, 0, 0);
    assert_token(wrapping, 0, 0, 0);
    assert(!selection_merge(&wrapping, &live, 0));
    live.counter = UINT32_C(0x80000000);
    assert(!selection_merge(&wrapping, &live, 0));

    /* Exact eight-byte payload and flags are independent of C layout/endianness. */
    const uint8_t expected[8] = {1, 0x81, 0x53, 0x31, 0x78, 0x56, 0x34, 0x12};
    live = (selection_state_t){.counter=UINT32_C(0x12345678), .origin=1, .output=1, .joined=true};
    uint8_t payload[8];
    selection_encode(&live, payload);
    assert(memcmp(payload, expected, sizeof(payload)) == 0);
    selection_state_t decoded;
    assert(selection_decode(expected, &decoded));
    assert_token(decoded, UINT32_C(0x12345678), 1, 1);
    assert(decoded.joined && !decoded.joining && !decoded.pending_local);
    payload[1] = 0xc1;
    assert(!selection_decode(payload, &decoded));
    memset(payload, 0, sizeof(payload));
    payload[0] = 1;
    assert(selection_is_legacy(payload));
    assert(!selection_decode(payload, &decoded));
    payload[7] = 1;
    assert(!selection_is_legacy(payload));

    printf("selection: %u delivery-order pairs, startup intent, wrap and wire contracts passed\n", cases);
    return 0;
}
