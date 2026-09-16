/* Native wire/state tests: no Pico SDK, UART, USB, flash, or real clock. */
#include "fw_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    fw_batch_kind_t kind;
    uint8_t payload[8];
} frame_t;

static void sample_page(uint8_t *page, unsigned number) {
    for (unsigned i = 0; i < FW_BATCH_PAGE_SIZE; ++i)
        page[i] = (uint8_t)((number >> 8) ^ number ^ (i * 37u) ^ (i >> 3));
}

static void frames(uint32_t tag, uint32_t address, const uint8_t *page, frame_t out[65]) {
    fw_batch_tx_t tx = {0};
    CHECK(fw_batch_tx_begin(&tx, tag, address, page));
    for (unsigned i = 0; i < 65; ++i) {
        CHECK(fw_batch_tx_peek(&tx, &out[i].kind, out[i].payload));
        CHECK(fw_batch_tx_sent(&tx));
    }
    CHECK(!tx.active);
    CHECK(!fw_batch_tx_peek(&tx, &out[0].kind, out[0].payload));
    CHECK(!fw_batch_tx_sent(&tx));
}

static fw_batch_rx_result_t receive(fw_batch_rx_t *rx, const frame_t *frame) {
    return fw_batch_rx_receive(rx, frame->kind, frame->payload);
}

static void test_tokens_do_not_wrap_or_reuse(void) {
    fw_batch_token_t tokens;
    fw_batch_token_init(&tokens, 0);
    for (unsigned i = 1; i <= 4096; ++i) {
        uint32_t tag = 0;
        CHECK(fw_batch_token_next(&tokens, &tag));
        CHECK(tag == i * FW_BATCH_WORDS);
    }
    fw_batch_token_init(&tokens, UINT64_C(0x123456789abcdef0));
    uint32_t tag;
    CHECK(fw_batch_token_next(&tokens, &tag));
    CHECK(tag == (UINT32_C(0x88888888) & FW_BATCH_TAG_MASK));
    fw_batch_token_init(&tokens, FW_BATCH_TAG_MASK - FW_BATCH_WORDS);
    CHECK(fw_batch_token_next(&tokens, &tag));
    CHECK(tag == FW_BATCH_TAG_MASK - FW_BATCH_WORDS);
    CHECK(fw_batch_token_next(&tokens, &tag));
    CHECK(tag == FW_BATCH_TAG_MASK);
    CHECK(tokens.exhausted);
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(!fw_batch_token_next(&tokens, &tag));
        CHECK(tag == FW_BATCH_TAG_MASK);
    }
    fw_batch_token_init(&tokens, 64);
    CHECK(!fw_batch_token_next(&tokens, NULL));
    CHECK(tokens.next == 64);
    CHECK(!fw_batch_token_next(NULL, &tag));
    fw_batch_token_init(NULL, 0);
}

static void test_capability_is_correlated_and_pinned(void) {
    uint8_t payload[8];
    const uint8_t expected[8] = {0x40, 0x23, 0x01, 0x00, 0xcd, 0xab, 1, 0};
    CHECK(fw_batch_caps_encode_request(0x12340, 0xabcd, payload));
    CHECK(memcmp(payload, expected, sizeof(payload)) == 0);
    uint32_t tag = 99;
    uint16_t version = 99;
    CHECK(fw_batch_caps_decode_request(payload, &tag, &version));
    CHECK(tag == 0x12340 && version == 0xabcd);
    for (unsigned bit = 0; bit < 16; ++bit) {
        uint8_t wrong[8];
        memcpy(wrong, payload, 8);
        wrong[6 + bit / 8] ^= 1u << (bit % 8);
        CHECK(!fw_batch_caps_decode_request(wrong, &tag, &version));
    }
    CHECK(!fw_batch_caps_encode_request(0, 104, payload));
    CHECK(!fw_batch_caps_encode_request(65, 104, payload));
    CHECK(!fw_batch_caps_encode_request(64, 0, payload));
    CHECK(!fw_batch_caps_decode_request(NULL, &tag, &version));
    CHECK(!fw_batch_caps_decode_request(payload, NULL, &version));
    CHECK(!fw_batch_caps_decode_request(payload, &tag, NULL));
    CHECK(fw_batch_caps_encode_response(0x12340, 0xdeadbeef, payload));
    CHECK(fw_batch_caps_response_matches(payload, 0x12340, 0xdeadbeef));
    for (unsigned bit = 0; bit < 64; ++bit) {
        uint8_t wrong[8];
        memcpy(wrong, payload, 8);
        wrong[bit / 8] ^= 1u << (bit % 8);
        CHECK(!fw_batch_caps_response_matches(wrong, 0x12340, 0xdeadbeef));
    }
    CHECK(!fw_batch_caps_response_matches(payload, 0x12380, 0xdeadbeef));
    CHECK(!fw_batch_caps_response_matches(payload, 0x12340, 0xdeadbeee));
    CHECK(!fw_batch_caps_response_matches(payload, 0, 0));
}

static void test_page_wire_bounds_and_endianness(void) {
    uint8_t payload[8];
    uint32_t tag, address;
    for (unsigned page = 0; page < FW_BATCH_IMAGE_SIZE / FW_BATCH_PAGE_SIZE; ++page) {
        CHECK(fw_batch_request_encode(0xabcdefc0, page * 256u, payload));
        CHECK(fw_batch_request_decode(payload, &tag, &address));
        CHECK(tag == 0xabcdefc0 && address == page * 256u);
    }
    CHECK(!fw_batch_request_encode(0, 0, payload));
    CHECK(!fw_batch_request_encode(1, 0, payload));
    for (unsigned i = 1; i < 256; ++i)
        CHECK(!fw_batch_request_encode(64, i, payload));
    CHECK(!fw_batch_request_encode(64, FW_BATCH_IMAGE_SIZE, payload));
    CHECK(!fw_batch_request_encode(64, UINT32_MAX, payload));
    CHECK(!fw_batch_request_encode(64, 0, NULL));
    const uint8_t wrong_address[8] = {64, 0, 0, 0, 0, 0, 4, 0};
    CHECK(!fw_batch_request_decode(wrong_address, &tag, &address));
    const uint8_t word[4] = {0x78, 0x56, 0x34, 0x12};
    const uint8_t expected[8] = {0xff, 0xff, 0xff, 0xff, 0x78, 0x56, 0x34, 0x12};
    CHECK(fw_batch_data_encode(FW_BATCH_TAG_MASK, 63, word, payload));
    CHECK(memcmp(payload, expected, 8) == 0);
    CHECK(!fw_batch_data_encode(64, 64, word, payload));
    CHECK(!fw_batch_data_encode(64, UINT32_MAX, word, payload));
    CHECK(!fw_batch_data_encode(65, 0, word, payload));
    CHECK(!fw_batch_data_encode(64, 0, NULL, payload));
    CHECK(!fw_batch_end_encode(65, 0, payload));
}

static void test_page_crc_known_vectors(void) {
    uint8_t page[256] = {0};
    CHECK(fw_batch_page_crc(page) == UINT32_C(0x0d968558));
    for (unsigned i = 0; i < 256; ++i)
        page[i] = (uint8_t)i;
    CHECK(fw_batch_page_crc(page) == UINT32_C(0x29058c73));
}

static void test_reordering_and_duplicate_progress(void) {
    uint8_t page[256], target[256];
    sample_page(page, 15);
    frame_t packet[65];
    frames(0x600, 0x100, page, packet);
    for (unsigned early_end = 0; early_end < 2; ++early_end) {
        memset(target, 0xa5, sizeof(target));
        fw_batch_rx_t rx = {0};
        CHECK(fw_batch_rx_begin(&rx, 0x600, 0x100, target));
        /* Preparing the next collector may happen before the preceding page is
         * programmed. It must never clear caller-owned page storage. */
        for (unsigned i = 0; i < sizeof(target); ++i)
            CHECK(target[i] == 0xa5);
        if (early_end) {
            CHECK(receive(&rx, &packet[64]) == FW_BATCH_RX_PROGRESS);
            CHECK(receive(&rx, &packet[64]) == FW_BATCH_RX_IGNORED);
        }
        for (unsigned i = 0; i < 64; ++i) {
            unsigned word = (i * 37u) % 64u;
            bool complete = early_end && i == 63;
            CHECK(receive(&rx, &packet[word])
                  == (complete ? FW_BATCH_RX_COMPLETE : FW_BATCH_RX_PROGRESS));
            uint64_t received_before = rx.received;
            CHECK(receive(&rx, &packet[word]) == FW_BATCH_RX_IGNORED);
            CHECK(rx.received == received_before);
        }
        if (!early_end) {
            CHECK(rx.active);
            CHECK(receive(&rx, &packet[64]) == FW_BATCH_RX_COMPLETE);
        }
        CHECK(!rx.active && !rx.invalid);
        CHECK(rx.received == UINT64_MAX);
        CHECK(memcmp(page, target, sizeof(page)) == 0);
        CHECK(receive(&rx, &packet[64]) == FW_BATCH_RX_IGNORED);
    }
}

static void test_missing_word_end_and_wrong_tags_cannot_complete(void) {
    uint8_t page[256], target[256];
    sample_page(page, 3);
    frame_t packet[65], stale[65];
    frames(0x100, 0, page, packet);
    frames(0x140, 0, page, stale);
    for (unsigned missing = 0; missing < 65; ++missing) {
        fw_batch_rx_t rx;
        CHECK(fw_batch_rx_begin(&rx, 0x100, 0, target));
        for (unsigned i = 0; i < 65; ++i) {
            CHECK(receive(&rx, &stale[i]) == FW_BATCH_RX_IGNORED);
            if (i != missing)
                CHECK(receive(&rx, &packet[i]) == FW_BATCH_RX_PROGRESS);
        }
        CHECK(rx.active && !rx.invalid);
        CHECK(receive(&rx, &packet[missing]) == FW_BATCH_RX_COMPLETE);
    }
}

static void test_conflicting_duplicates_and_bad_crc_poison_request(void) {
    uint8_t page[256], target[256];
    sample_page(page, 10);
    frame_t packet[65];
    frames(64, 0, page, packet);
    for (unsigned conflicting = 0; conflicting < 65; ++conflicting) {
        fw_batch_rx_t rx;
        CHECK(fw_batch_rx_begin(&rx, 64, 0, target));
        CHECK(receive(&rx, &packet[conflicting]) == FW_BATCH_RX_PROGRESS);
        frame_t bad = packet[conflicting];
        bad.payload[4] ^= 1;
        CHECK(receive(&rx, &bad) == FW_BATCH_RX_INVALID);
        CHECK(rx.invalid && !rx.active);
        for (unsigned i = 0; i < 65; ++i)
            CHECK(receive(&rx, &packet[i]) == FW_BATCH_RX_IGNORED);
    }
    /* The outer UART checksum already rejects wire flips. Independently prove
     * page validation catches every possible one-bit word/END corruption even
     * if it reaches this layer as an otherwise valid frame. */
    for (unsigned damaged = 0; damaged < 65; ++damaged) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            fw_batch_rx_t rx;
            CHECK(fw_batch_rx_begin(&rx, 64, 0, target));
            for (unsigned i = 0; i < 65; ++i) {
                frame_t current = packet[i];
                if (i == damaged)
                    current.payload[4 + bit / 8] ^= 1u << (bit % 8);
                CHECK(receive(&rx, &current)
                      == (i == 64 ? FW_BATCH_RX_INVALID : FW_BATCH_RX_PROGRESS));
            }
            CHECK(rx.invalid && !rx.active);
        }
    }
}

typedef struct {
    unsigned attempts;
    unsigned admitted;
    frame_t frame[65];
} emitter_t;

static bool backpressured_emit(void *context, fw_batch_kind_t kind, const uint8_t payload[8]) {
    emitter_t *emitter = context;
    /* Independent ordinary-traffic pressure declines three of every four polls. */
    if (++emitter->attempts % 4)
        return false;
    CHECK(emitter->admitted < 65);
    emitter->frame[emitter->admitted].kind = kind;
    memcpy(emitter->frame[emitter->admitted++].payload, payload, 8);
    return true;
}

static void test_source_cursor_advances_only_after_admission(void) {
    uint8_t page[256];
    sample_page(page, 7);
    fw_batch_tx_t tx;
    CHECK(fw_batch_tx_begin(&tx, 0x100, 0x500, page));
    emitter_t emitter = {0};
    while (tx.active) {
        uint8_t before = tx.cursor;
        bool accepted = fw_batch_tx_pump(&tx, backpressured_emit, &emitter);
        CHECK(accepted == (emitter.attempts % 4 == 0));
        if (!accepted)
            CHECK(tx.cursor == before);
    }
    CHECK(emitter.admitted == 65 && emitter.attempts == 260);
    frame_t expected[65];
    frames(0x100, 0x500, page, expected);
    for (unsigned i = 0; i < 65; ++i) {
        CHECK(emitter.frame[i].kind == expected[i].kind);
        CHECK(memcmp(emitter.frame[i].payload, expected[i].payload, 8) == 0);
    }
    CHECK(!fw_batch_tx_pump(&tx, backpressured_emit, &emitter));
    CHECK(emitter.attempts == 260);
}

static void test_invalid_arguments_and_cancellation(void) {
    uint8_t page[256] = {0};
    fw_batch_rx_t rx = {0};
    fw_batch_tx_t tx = {0};
    CHECK(fw_batch_rx_begin(&rx, 64, 0, page));
    CHECK(fw_batch_tx_begin(&tx, 64, 0, page));
    CHECK(!fw_batch_rx_begin(&rx, 65, 0, page));
    CHECK(!fw_batch_tx_begin(&tx, 65, 0, page));
    CHECK(rx.active && tx.active && rx.tag == 64 && tx.tag == 64);
    CHECK(!fw_batch_rx_begin(&rx, 64, 1, page));
    CHECK(!fw_batch_tx_begin(&tx, 64, 1, page));
    CHECK(!fw_batch_rx_begin(&rx, 64, 0, NULL));
    CHECK(!fw_batch_tx_begin(&tx, 64, 0, NULL));
    uint8_t payload[8] = {64};
    CHECK(fw_batch_rx_receive(&rx, (fw_batch_kind_t)99, payload) == FW_BATCH_RX_IGNORED);
    CHECK(rx.active && rx.received == 0 && !rx.have_end);
    CHECK(fw_batch_rx_receive(&rx, FW_BATCH_DATA, NULL) == FW_BATCH_RX_IGNORED);
    CHECK(!fw_batch_tx_pump(&tx, NULL, NULL));
    CHECK(tx.cursor == 0 && tx.active);
    fw_batch_rx_cancel(&rx);
    fw_batch_tx_cancel(&tx);
    CHECK(!rx.active && !tx.active && !rx.page && !tx.page);
    CHECK(fw_batch_rx_receive(&rx, FW_BATCH_DATA, payload) == FW_BATCH_RX_IGNORED);
    CHECK(!fw_batch_tx_sent(&tx));
    fw_batch_rx_cancel(NULL);
    fw_batch_tx_cancel(NULL);
}

static void test_full_slot_with_drops_retries_and_late_replies(void) {
    static uint8_t source[FW_BATCH_IMAGE_SIZE], copied[FW_BATCH_IMAGE_SIZE];
    uint8_t page_buffer[FW_BATCH_PAGE_SIZE];
    fw_batch_token_t tokens;
    fw_batch_token_init(&tokens, UINT64_C(0x123456789abcdef0));
    frame_t previous[65] = {0};
    unsigned completed_pages = 0;
    for (unsigned address = 0; address < FW_BATCH_IMAGE_SIZE; address += FW_BATCH_PAGE_SIZE) {
        unsigned number = address / FW_BATCH_PAGE_SIZE;
        sample_page(source + address, number);
        bool drop = number % 11 == 0;
        for (unsigned attempt = 0; attempt < (drop ? 2u : 1u); ++attempt) {
            uint32_t tag;
            CHECK(fw_batch_token_next(&tokens, &tag));
            frame_t packet[65];
            frames(tag, address, source + address, packet);
            fw_batch_rx_t rx;
            CHECK(fw_batch_rx_begin(&rx, tag, address, page_buffer));
            bool complete = false;
            for (unsigned i = 0; i < 65; ++i) {
                unsigned shuffled = (i * 17u) % 65u;
                /* Late frames from a previous page or timed-out attempt never
                 * enter the new page, even when their index matches. */
                CHECK(receive(&rx, &previous[shuffled]) == FW_BATCH_RX_IGNORED);
                if (drop && attempt == 0 && shuffled == number % 64u)
                    continue;
                fw_batch_rx_result_t result = receive(&rx, &packet[shuffled]);
                CHECK(result == FW_BATCH_RX_PROGRESS || result == FW_BATCH_RX_COMPLETE);
                CHECK(receive(&rx, &packet[shuffled]) == FW_BATCH_RX_IGNORED);
                if (result == FW_BATCH_RX_COMPLETE) {
                    CHECK(!complete);
                    complete = true;
                }
            }
            memcpy(previous, packet, sizeof(previous));
            if (drop && attempt == 0) {
                CHECK(!complete && rx.active);
                fw_batch_rx_cancel(&rx);
                continue;
            }
            CHECK(complete && !rx.active && !rx.invalid);
            CHECK(memcmp(page_buffer, source + address, FW_BATCH_PAGE_SIZE) == 0);
            memcpy(copied + address, page_buffer, FW_BATCH_PAGE_SIZE);
            ++completed_pages;
        }
    }
    CHECK(completed_pages == FW_BATCH_IMAGE_SIZE / FW_BATCH_PAGE_SIZE);
    CHECK(memcmp(source, copied, sizeof(source)) == 0);
}

int main(void) {
    test_tokens_do_not_wrap_or_reuse();
    test_capability_is_correlated_and_pinned();
    test_page_wire_bounds_and_endianness();
    test_page_crc_known_vectors();
    test_reordering_and_duplicate_progress();
    test_missing_word_end_and_wrong_tags_cannot_complete();
    test_conflicting_duplicates_and_bad_crc_poison_request();
    test_source_cursor_advances_only_after_admission();
    test_invalid_arguments_and_cancellation();
    test_full_slot_with_drops_retries_and_late_replies();
    puts("firmware page batch tests passed");
    return EXIT_SUCCESS;
}
