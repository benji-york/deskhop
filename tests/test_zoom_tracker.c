/* Native tests for the host-independent Zoom Assist inference state machine. */
#include "zoom_tracker.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                    __FILE__, __LINE__, #condition);                            \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

static void test_activation_and_partial_zoom_out(void) {
    zoom_tracker_t tracker = {0};

    CHECK(!zoom_tracker_observe(&tracker, 0, 10));
    CHECK(zoom_tracker_observe(&tracker, 5, 20));
    CHECK(tracker.zoom_in_direction == 1);
    CHECK(tracker.active);
    CHECK(tracker.debt == 5);

    CHECK(zoom_tracker_observe(&tracker, -3, 30));
    CHECK(tracker.active);
    CHECK(tracker.debt == 2);
    CHECK(tracker.overscroll == 0);
    CHECK(!tracker.exit_pending);
}

static void test_deliberate_overscroll_and_quiet_period(void) {
    zoom_tracker_t tracker = {0};
    const uint64_t now = 1000;

    zoom_tracker_observe(&tracker, 4, now);
    zoom_tracker_observe(&tracker, -9, now + 10);
    CHECK(tracker.active);
    CHECK(tracker.debt == 0);
    CHECK(tracker.overscroll == 5);
    CHECK(!tracker.exit_pending);

    zoom_tracker_observe(&tracker, -1, now + 20);
    CHECK(tracker.exit_pending);
    CHECK(!zoom_tracker_task(&tracker,
                             now + 20 + ZOOM_ASSIST_QUIET_TIME_US - 1));
    CHECK(zoom_tracker_task(&tracker,
                            now + 20 + ZOOM_ASSIST_QUIET_TIME_US));
    CHECK(!tracker.active);
    CHECK(!tracker.exit_pending);
    CHECK(tracker.debt == 0);
    CHECK(tracker.overscroll == 0);
}

static void test_new_zoom_in_cancels_pending_exit(void) {
    zoom_tracker_t tracker = {0};

    zoom_tracker_observe(&tracker, 1, 0);
    zoom_tracker_observe(&tracker, -7, 10);
    CHECK(tracker.exit_pending);

    zoom_tracker_observe(&tracker, 2, 20);
    CHECK(tracker.active);
    CHECK(!tracker.exit_pending);
    CHECK(tracker.exit_deadline == 0);
    CHECK(tracker.debt == 2);
    CHECK(tracker.overscroll == 0);
}

static void test_debt_saturates(void) {
    zoom_tracker_t tracker = {0};

    zoom_tracker_observe(&tracker, INT32_MAX, 0);
    CHECK(tracker.debt == ZOOM_ASSIST_DEBT_CAP);
    zoom_tracker_observe(&tracker, INT32_MAX, 1);
    CHECK(tracker.debt == ZOOM_ASSIST_DEBT_CAP);

    tracker = (zoom_tracker_t){0};
    zoom_tracker_observe(&tracker, INT32_MIN, 2);
    CHECK(tracker.zoom_in_direction == -1);
    CHECK(tracker.debt == ZOOM_ASSIST_DEBT_CAP);
}

static void test_large_report_cannot_exit_before_raw_debt_is_paid(void) {
    zoom_tracker_t tracker = {0};

    zoom_tracker_observe(&tracker, 127, 0);
    zoom_tracker_observe(&tracker, -70, 1);
    CHECK(tracker.active);
    CHECK(tracker.debt == 57);
    CHECK(tracker.overscroll == 0);
    CHECK(!tracker.exit_pending);
}

static void test_opposite_direction_does_not_reactivate_at_one_x(void) {
    zoom_tracker_t tracker = {.zoom_in_direction = 1};

    CHECK(!zoom_tracker_observe(&tracker, -10, 0));
    CHECK(!tracker.active);
    CHECK(tracker.debt == 0);
    CHECK(tracker.overscroll == 0);
}

static void test_outputs_are_independent(void) {
    zoom_tracker_t trackers[2] = {0};

    zoom_tracker_observe(&trackers[0], 3, 0);
    CHECK(trackers[0].active);
    CHECK(!trackers[1].active);
    CHECK(trackers[1].zoom_in_direction == 0);

    zoom_tracker_observe(&trackers[1], -2, 1);
    CHECK(trackers[0].zoom_in_direction == 1);
    CHECK(trackers[0].debt == 3);
    CHECK(trackers[1].active);
    CHECK(trackers[1].zoom_in_direction == -1);
    CHECK(trackers[1].debt == 2);
}

static void test_manual_clear(void) {
    zoom_tracker_t tracker = {0};

    zoom_tracker_observe(&tracker, -3, 0);
    CHECK(zoom_tracker_clear(&tracker, false));
    CHECK(!tracker.active);
    CHECK(tracker.zoom_in_direction == -1);
    CHECK(!zoom_tracker_clear(&tracker, false));
    CHECK(zoom_tracker_clear(&tracker, true));
    CHECK(tracker.zoom_in_direction == 0);
}

static void test_wheel_canonicalization(void) {
    CHECK(zoom_canonicalize_wheel(0) == 0);
    CHECK(zoom_canonicalize_wheel(127) == 127);
    CHECK(zoom_canonicalize_wheel(128) == INT8_MAX);
    CHECK(zoom_canonicalize_wheel(INT32_MAX) == INT8_MAX);
    CHECK(zoom_canonicalize_wheel(-128) == -128);
    CHECK(zoom_canonicalize_wheel(-129) == INT8_MIN);
    CHECK(zoom_canonicalize_wheel(INT32_MIN) == INT8_MIN);
}

static void test_modifier_freshness(void) {
    const uint64_t seen = 1000;

    CHECK(!zoom_modifier_state_is_fresh(0, seen, seen));
    CHECK(zoom_modifier_state_is_fresh(1, seen,
                                       seen + ZOOM_ASSIST_MODIFIER_TIMEOUT_US));
    CHECK(!zoom_modifier_state_is_fresh(1, seen,
                                        seen + ZOOM_ASSIST_MODIFIER_TIMEOUT_US + 1));
}

int main(void) {
    test_activation_and_partial_zoom_out();
    test_deliberate_overscroll_and_quiet_period();
    test_new_zoom_in_cancels_pending_exit();
    test_debt_saturates();
    test_large_report_cannot_exit_before_raw_debt_is_paid();
    test_opposite_direction_does_not_reactivate_at_one_x();
    test_outputs_are_independent();
    test_manual_clear();
    test_wheel_canonicalization();
    test_modifier_freshness();

    puts("zoom_tracker tests passed");
    return EXIT_SUCCESS;
}
