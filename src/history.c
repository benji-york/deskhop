#include "history.h"

_Static_assert(sizeof(history_event_t) == 24, "History events must remain 24 bytes");

void history_store_init(history_store_t *store) {
    *store = (history_store_t){.next_seq = 1};
}

void history_store_record(history_store_t *store, uint64_t time_us, history_type_t type,
                          uint8_t a, uint8_t b, uint32_t value) {
    if (store->next_seq == UINT64_MAX)
        return;

    const uint64_t seq = store->next_seq++;
    store->records[(seq - 1) % HISTORY_CAPACITY] = (history_event_t){
        .seq = seq,
        .time_us = time_us,
        .value = value,
        .type = (uint8_t)type,
        .a = a,
        .b = b,
    };
    if (store->count < HISTORY_CAPACITY)
        store->count++;
    else
        store->overwritten++;
}

history_window_t history_store_window(const history_store_t *store, unsigned limit) {
    const unsigned count = limit < store->count ? limit : store->count;
    return (history_window_t){
        .first_seq = store->next_seq - count,
        .end_seq = store->next_seq,
        .oldest_seq = store->next_seq - store->count,
        .overwritten = store->overwritten,
        .count = count,
    };
}

bool history_store_read(const history_store_t *store, uint64_t seq, history_event_t *out) {
    if (seq < store->next_seq - store->count || seq >= store->next_seq)
        return false;
    *out = store->records[(seq - 1) % HISTORY_CAPACITY];
    return true;
}
