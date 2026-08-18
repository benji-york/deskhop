/* Native tests for the host-independent firmware pull state machine. */

#include "fw_update.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                   \
                    __FILE__, __LINE__, #condition);                            \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

static void test_sources_are_isolated(void) {
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_NONE, false, false,
                                UINT32_MAX, 0, 0, 0) == FW_UPDATE_WAIT);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_DROP, false, true,
                                UINT32_MAX, 0, 0, 0) == FW_UPDATE_WAIT);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL_PAUSED, false, true,
                                UINT32_MAX, 0, 0, 0) == FW_UPDATE_WAIT);
}

static void test_peer_protocol_is_explicit(void) {
    CHECK(fw_update_peer_compatible(FW_UPDATE_PROTOCOL_MARKER));
    CHECK(!fw_update_peer_compatible(0));
    CHECK(!fw_update_peer_compatible(1));
}

static void test_initial_request_and_timeout(void) {
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, true, false,
                                10, 0, 10, 10) == FW_UPDATE_REQUEST);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, false,
                                FW_UPDATE_RESPONSE_TIMEOUT_US - 1, 0, 0, 0)
          == FW_UPDATE_WAIT);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, false,
                                FW_UPDATE_RESPONSE_TIMEOUT_US, 0, 0, 0)
          == FW_UPDATE_REQUEST);
}

static void test_timeout_wraparound(void) {
    uint32_t requested = UINT32_MAX - 49;

    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, false,
                                requested + FW_UPDATE_RESPONSE_TIMEOUT_US - 1,
                                requested, requested, requested)
          == FW_UPDATE_WAIT);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, false,
                                requested + FW_UPDATE_RESPONSE_TIMEOUT_US,
                                requested, requested, requested)
          == FW_UPDATE_REQUEST);
}

static void test_stall_recovery_policy(void) {
    uint32_t now = FW_UPDATE_STALL_TIMEOUT_US;

    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, false,
                                now, now, 0, now) == FW_UPDATE_ABANDON);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, true,
                                now, now, 0, now) == FW_UPDATE_RESTART);
    CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, false, true,
                                now, now, 0,
                                now - FW_UPDATE_PEER_TIMEOUT_US)
          == FW_UPDATE_PAUSE);
}

static void test_only_outstanding_exact_response_is_accepted(void) {
    CHECK(fw_update_response_expected(FW_UPDATE_SOURCE_PULL, true, 100, 100));
    CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_PULL, false, 100, 100));
    CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_DROP, true, 100, 100));
    CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_PULL_PAUSED, true, 100, 100));
    CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_PULL, true, 104, 100));
    CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_PULL, true, 100, 104));
}

static void test_build_metadata_is_an_independent_authority(void) {
    const uint32_t magic = 0xf00d;
    const uint16_t version = 185;
    const uint32_t checksum = 0x12345678;

    CHECK(fw_update_metadata_matches(magic, version, checksum, checksum,
                                     version, true));
    CHECK(!fw_update_metadata_matches(0, version, checksum, checksum,
                                      version, true));
    CHECK(!fw_update_metadata_matches(magic, version, checksum + 1, checksum,
                                      version, true));
    CHECK(!fw_update_metadata_matches(magic, version - 1, checksum, checksum,
                                      version, true));
    CHECK(fw_update_metadata_matches(magic, version - 1, checksum, checksum,
                                     0, false));
}

static void test_uf2_blocks_can_arrive_out_of_order_once(void) {
    uint32_t bitmap[1024 / 32] = {0};
    uint32_t sectors_erased[64 / 32] = {0};
    uint8_t erase_count[64] = {0};
    uint16_t received_count = 0;

    /* 37 is coprime with 1024, so this visits every block in a shuffled order. */
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t block = (i * 37) % 1024;
        CHECK(fw_update_mark_block(bitmap, &received_count, block));
        uint32_t sector = block / 16;
        if (fw_update_mark_block(sectors_erased, NULL, sector))
            erase_count[sector]++;
        CHECK(!fw_update_mark_block(bitmap, &received_count, block));
        CHECK(received_count == i + 1);
    }

    CHECK(received_count == 1024);
    for (uint32_t i = 0; i < 1024 / 32; i++)
        CHECK(bitmap[i] == UINT32_MAX);
    for (uint32_t i = 0; i < 64; i++)
        CHECK(erase_count[i] == 1);
}

static void test_full_transfer_with_drops_and_duplicates(void) {
    const uint32_t word_count = 262144 / sizeof(uint32_t);
    uint32_t address = 0;
    uint32_t now = UINT32_MAX - 50000;
    uint32_t requested_at = now;
    uint32_t progressed_at = now;
    bool word_complete = true;
    bool request_pending = false;
    bool image_dirty = false;

    for (uint32_t word = 0; word < word_count; word++) {
        CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, word_complete,
                                    image_dirty, now, requested_at,
                                    progressed_at, now) == FW_UPDATE_REQUEST);

        word_complete = false;
        request_pending = true;
        requested_at = now;

        /* Deterministically lose one response, then request the same address. */
        if (word % 997 == 0) {
            now += FW_UPDATE_RESPONSE_TIMEOUT_US - 1;
            CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, word_complete,
                                        image_dirty, now, requested_at,
                                        progressed_at, now) == FW_UPDATE_WAIT);
            now++;
            CHECK(fw_update_next_action(FW_UPDATE_SOURCE_PULL, word_complete,
                                        image_dirty, now, requested_at,
                                        progressed_at, now) == FW_UPDATE_REQUEST);
            requested_at = now;
        }

        CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_PULL,
                                           request_pending, address,
                                           address + sizeof(uint32_t)));
        CHECK(fw_update_response_expected(FW_UPDATE_SOURCE_PULL,
                                          request_pending, address, address));

        request_pending = false;
        word_complete = true;
        address += sizeof(uint32_t);
        progressed_at = now;

        /* A duplicate from a retransmission cannot advance the next word. */
        CHECK(!fw_update_response_expected(FW_UPDATE_SOURCE_PULL,
                                           request_pending, address,
                                           address - sizeof(uint32_t)));

        if ((address & 0xff) == 0)
            image_dirty = true;
        now++;
    }

    CHECK(address == 262144);
}

int main(void) {
    test_sources_are_isolated();
    test_peer_protocol_is_explicit();
    test_initial_request_and_timeout();
    test_timeout_wraparound();
    test_stall_recovery_policy();
    test_only_outstanding_exact_response_is_accepted();
    test_build_metadata_is_an_independent_authority();
    test_uf2_blocks_can_arrive_out_of_order_once();
    test_full_transfer_with_drops_and_duplicates();

    puts("firmware update tests passed");
    return EXIT_SUCCESS;
}
