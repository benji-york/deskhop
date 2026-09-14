#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "history.h"

_Static_assert(sizeof(history_event_t) == 24, "Event storage budget changed");
_Static_assert(offsetof(history_event_t, time_us) == 8, "Unexpected event layout");
_Static_assert(offsetof(history_event_t, value) == 16, "Unexpected event layout");
_Static_assert(offsetof(history_event_t, type) == 20, "Unexpected event layout");
_Static_assert(offsetof(history_event_t, reserved) == 23, "Unexpected event layout");

static history_event_t example(uint64_t seq) {
    return (history_event_t){
        .seq = seq,
        /* Include repeated, decreasing, and large timestamps: the store must
         * preserve samples exactly, without reordering or truncation. */
        .time_us = UINT64_MAX - (seq / 3) * UINT64_C(65537),
        .value = (uint32_t)(seq * UINT64_C(0xdeadbeef)),
        .type = (uint8_t)(1 + seq % 10),
        .a = (uint8_t)(seq * 7),
        .b = (uint8_t)(255 - seq * 11),
    };
}

static void record_example(history_store_t *store, uint64_t seq) {
    history_event_t event = example(seq);
    history_store_record(store, event.time_us, (history_type_t)event.type,
                          event.a, event.b, event.value);
}

static void assert_event(const history_event_t *actual, const history_event_t *expected) {
    assert(actual->seq == expected->seq);
    assert(actual->time_us == expected->time_us);
    assert(actual->value == expected->value);
    assert(actual->type == expected->type);
    assert(actual->a == expected->a);
    assert(actual->b == expected->b);
    assert(actual->reserved == 0);
}

static void assert_absent(const history_store_t *store, uint64_t seq) {
    history_event_t untouched;
    memset(&untouched, 0xa5, sizeof(untouched));
    const history_event_t before = untouched;
    assert(!history_store_read(store, seq, &untouched));
    assert(memcmp(&untouched, &before, sizeof(untouched)) == 0);
}

static void test_empty_and_reset(void) {
    history_store_t store;
    memset(&store, 0xa5, sizeof(store));
    history_store_init(&store);
    for (unsigned limit = 0; limit <= HISTORY_CAPACITY + 2; limit++) {
        history_window_t window = history_store_window(&store, limit);
        assert(window.first_seq == 1);
        assert(window.end_seq == 1);
        assert(window.oldest_seq == 1);
        assert(window.overwritten == 0);
        assert(window.count == 0);
    }
    assert_absent(&store, 0);
    assert_absent(&store, 1);
    assert_absent(&store, UINT64_MAX);

    for (unsigned i = 1; i <= HISTORY_CAPACITY * 3; i++)
        record_example(&store, i);
    history_store_init(&store);
    history_window_t window = history_store_window(&store, UINT_MAX);
    assert(window.count == 0 && window.overwritten == 0);
    assert(window.first_seq == 1 && window.end_seq == 1 && window.oldest_seq == 1);
    assert_absent(&store, HISTORY_CAPACITY * 3);
    record_example(&store, 1);
    history_event_t read;
    history_event_t expected = example(1);
    assert(history_store_read(&store, 1, &read));
    assert_event(&read, &expected);
}

/* Reference model is a shifting chronological list, independent of the
 * production ring's slot arithmetic. Compare every supported limit while
 * crossing many wraps, including loss of exactly the oldest event. */
static void test_model_and_all_limits(void) {
    history_store_t store;
    history_store_init(&store);
    history_event_t model[HISTORY_CAPACITY];
    unsigned retained = 0;
    uint64_t removed = 0;

    for (uint64_t written = 1; written <= HISTORY_CAPACITY * 40 + 7; written++) {
        if (retained == HISTORY_CAPACITY) {
            memmove(model, model + 1, sizeof(model) - sizeof(model[0]));
            retained--;
            removed++;
        }
        model[retained++] = example(written);
        record_example(&store, written);

        for (unsigned test = 0; test <= HISTORY_CAPACITY + 3; test++) {
            unsigned limit = test == HISTORY_CAPACITY + 3 ? UINT_MAX : test;
            unsigned selected = limit < retained ? limit : retained;
            history_window_t window = history_store_window(&store, limit);
            assert(window.count == selected);
            assert(window.end_seq == written + 1);
            assert(window.oldest_seq == model[0].seq);
            assert(window.overwritten == removed);
            assert(window.first_seq == (selected ? model[retained - selected].seq
                                                 : written + 1));

            for (unsigned i = 0; i < selected; i++) {
                history_event_t read;
                assert(history_store_read(&store, window.first_seq + i, &read));
                assert_event(&read, &model[retained - selected + i]);
            }
        }
        assert_absent(&store, 0);
        assert_absent(&store, model[0].seq - 1);
        assert_absent(&store, written + 1);
        assert_absent(&store, UINT64_MAX);
    }
}

static void test_fixed_window_and_slow_reader(void) {
    history_store_t store;
    history_store_init(&store);
    for (uint64_t seq = 1; seq <= 64; seq++)
        record_example(&store, seq);
    const history_window_t window = history_store_window(&store, 10);
    assert(window.first_seq == 55 && window.end_seq == 65);

    history_event_t copy;
    assert(history_store_read(&store, 55, &copy));
    for (uint64_t seq = 65; seq <= 120; seq++)
        record_example(&store, seq);
    history_event_t expected = example(55);
    assert_event(&copy, &expected);

    unsigned missing = 0;
    unsigned found = 0;
    for (uint64_t seq = window.first_seq; seq < window.end_seq; seq++) {
        history_event_t read;
        if (!history_store_read(&store, seq, &read)) {
            assert(seq == 55 || seq == 56);
            missing++;
        } else {
            expected = example(seq);
            assert_event(&read, &expected);
            found++;
        }
    }
    assert(missing == 2 && found == 8);
    /* The original command's metadata and end do not acquire new events. */
    assert(window.first_seq == 55 && window.end_seq == 65);
    assert(window.oldest_seq == 1 && window.overwritten == 0 && window.count == 10);

    for (uint64_t seq = 121; seq <= 184; seq++)
        record_example(&store, seq);
    for (uint64_t seq = window.first_seq; seq < window.end_seq; seq++)
        assert_absent(&store, seq);
    expected = example(55);
    assert_event(&copy, &expected);

    /* Altering a copy never mutates the stored event. */
    assert(history_store_read(&store, 184, &copy));
    memset(&copy, 0, sizeof(copy));
    assert(history_store_read(&store, 184, &copy));
    expected = example(184);
    assert_event(&copy, &expected);
}

static void test_sequence_exhaustion(void) {
    history_store_t store;
    history_store_init(&store);
    /* Construct the valid state after UINT64_MAX-6 accepted records. This
     * reaches the boundary without requiring centuries of test writes. */
    store.next_seq = UINT64_MAX - 5;
    store.count = HISTORY_CAPACITY;
    store.overwritten = store.next_seq - 1 - HISTORY_CAPACITY;
    for (uint64_t seq = store.next_seq - HISTORY_CAPACITY; seq < store.next_seq; seq++)
        store.records[(seq - 1) % HISTORY_CAPACITY] = example(seq);
    for (uint64_t seq = UINT64_MAX - 5; seq < UINT64_MAX; seq++)
        record_example(&store, seq);

    history_window_t window = history_store_window(&store, UINT_MAX);
    assert(window.first_seq == UINT64_MAX - HISTORY_CAPACITY);
    assert(window.end_seq == UINT64_MAX);
    assert(window.oldest_seq == window.first_seq);
    assert(window.count == HISTORY_CAPACITY);
    assert(window.overwritten == UINT64_MAX - 1 - HISTORY_CAPACITY);
    for (uint64_t seq = window.first_seq; seq < window.end_seq; seq++) {
        history_event_t read;
        history_event_t expected = example(seq);
        assert(history_store_read(&store, seq, &read));
        assert_event(&read, &expected);
    }
    const history_store_t before = store;
    for (unsigned attempt = 0; attempt < 100; attempt++)
        history_store_record(&store, 0, HISTORY_BOOT, 0, 0, 0);
    assert(memcmp(&store, &before, sizeof(store)) == 0);
    assert_absent(&store, 0);
    assert_absent(&store, UINT64_MAX);
}

int main(void) {
    test_empty_and_reset();
    test_model_and_all_limits();
    test_fixed_window_and_slow_reader();
    test_sequence_exhaustion();
    puts("history: all regression tests passed");
    return 0;
}
