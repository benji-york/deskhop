/* Native tests for system-wide keep-awake activity synchronization. */

#include "screensaver_policy.h"

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

#define US_PER_SECOND 1000000ULL

static void test_activity_age_encoding(void) {
    CHECK(activity_age_seconds(10 * US_PER_SECOND, 0, false)
          == ACTIVITY_AGE_UNKNOWN_SEC);
    CHECK(activity_age_seconds(10 * US_PER_SECOND, 10 * US_PER_SECOND, true) == 0);
    CHECK(activity_age_seconds(10 * US_PER_SECOND, 11 * US_PER_SECOND, true) == 0);
    CHECK(activity_age_seconds(10 * US_PER_SECOND, 7500000, true) == 2);

    uint64_t beyond_wire_range = ((uint64_t)ACTIVITY_AGE_MAX_SEC + 100) * US_PER_SECOND;
    CHECK(activity_age_seconds(beyond_wire_range, 0, true) == ACTIVITY_AGE_MAX_SEC);
}

static void test_activity_age_merge(void) {
    uint64_t known = 2 * US_PER_SECOND;

    CHECK(!activity_merge_age_seconds(10 * US_PER_SECOND,
                                      ACTIVITY_AGE_UNKNOWN_SEC, &known));
    CHECK(known == 2 * US_PER_SECOND);

    /* A peer event older than this Pico's boot cannot be represented and must
       not erase or refresh the history we already know. */
    CHECK(!activity_merge_age_seconds(10 * US_PER_SECOND, 11, &known));
    CHECK(known == 2 * US_PER_SECOND);

    CHECK(activity_merge_age_seconds(10 * US_PER_SECOND, 3, &known));
    CHECK(known == 7 * US_PER_SECOND);

    /* Later delivery of an older source timestamp never regresses the value. */
    CHECK(activity_merge_age_seconds(12 * US_PER_SECOND, 7, &known));
    CHECK(known == 7 * US_PER_SECOND);
}

static void test_repeated_source_packets_do_not_creep(void) {
    const uint64_t source_event = 1250000;
    uint64_t reconstructed = 0;

    uint32_t first_age = activity_age_seconds(5900000, source_event, true);
    CHECK(first_age == 4);
    CHECK(activity_merge_age_seconds(6100000, first_age, &reconstructed));
    uint64_t first_reconstruction = reconstructed;

    /* A later packet recovers after any dropped packets, but it is still based
       on the original source timestamp rather than the receiver's copy. */
    uint32_t later_age = activity_age_seconds(8900000, source_event, true);
    CHECK(later_age == 7);
    CHECK(activity_merge_age_seconds(9100000, later_age, &reconstructed));
    CHECK(reconstructed == first_reconstruction);
    CHECK(reconstructed >= source_event);
    CHECK(reconstructed - source_event < US_PER_SECOND);
}

static void test_latest_activity_uses_either_source_and_output(void) {
    uint64_t direct[ACTIVITY_OUTPUT_COUNT] = {10, 20};
    uint64_t peer[ACTIVITY_OUTPUT_COUNT] = {30, 40};

    CHECK(activity_latest_timestamp(direct, 0, peer, 0) == 0);
    CHECK(activity_latest_timestamp(direct, 1u << 0, peer, 0) == 10);
    CHECK(activity_latest_timestamp(direct, 1u << 1, peer, 0) == 20);
    CHECK(activity_latest_timestamp(direct, 0, peer, 1u << 0) == 30);
    CHECK(activity_latest_timestamp(direct, 0, peer, 1u << 1) == 40);
    CHECK(activity_latest_timestamp(direct, 0xff, peer, 0xff) == 40);

    /* Invalid entries are ignored even if their stale bytes look newer. */
    direct[0] = UINT64_MAX;
    peer[1] = UINT64_MAX;
    CHECK(activity_latest_timestamp(direct, 1u << 1, peer, 1u << 0) == 30);
}

static void test_system_timeout_policy(void) {
    CHECK(!screensaver_system_idle_timed_out(1000 * US_PER_SECOND, 0, 0));
    CHECK(!screensaver_system_idle_timed_out(299 * US_PER_SECOND, 0, 300));
    CHECK(!screensaver_system_idle_timed_out(300 * US_PER_SECOND - 1, 0, 300));
    CHECK(screensaver_system_idle_timed_out(300 * US_PER_SECOND, 0, 300));

    CHECK(!screensaver_system_idle_timed_out(500 * US_PER_SECOND,
                                             250 * US_PER_SECOND, 300));
    CHECK(screensaver_system_idle_timed_out(550 * US_PER_SECOND,
                                            250 * US_PER_SECOND, 300));
    CHECK(!screensaver_system_idle_timed_out(10 * US_PER_SECOND,
                                             11 * US_PER_SECOND, 1));
}

int main(void) {
    test_activity_age_encoding();
    test_activity_age_merge();
    test_repeated_source_packets_do_not_creep();
    test_latest_activity_uses_either_source_and_output();
    test_system_timeout_policy();

    puts("screensaver policy tests passed");
    return EXIT_SUCCESS;
}
