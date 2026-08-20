/* Native tests for the guarded three-tap DeskHop reboot shortcut. */

#include "reboot_hotkey.h"

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

#define SOURCE_A 0
#define SOURCE_B 1
#define MODIFIERS 0x21
#define KEY_Q 0x14
#define KEY_A 0x04
#define KEY_COUNT 6

static reboot_hotkey_result_t report(reboot_hotkey_sequence_t *sequence,
                                     reboot_hotkey_source_t *sources,
                                     uint8_t source,
                                     uint8_t modifiers,
                                     uint8_t key,
                                     uint64_t now) {
    uint8_t keys[KEY_COUNT] = {key};
    return reboot_hotkey_process_report(sequence, sources, 2, source,
                                        modifiers, keys, KEY_COUNT,
                                        MODIFIERS, KEY_Q, now);
}

static void complete_tap(reboot_hotkey_sequence_t *sequence,
                         reboot_hotkey_source_t *sources,
                         uint8_t source,
                         uint64_t now,
                         reboot_hotkey_result_t expected_release) {
    CHECK(report(sequence, sources, source, MODIFIERS, KEY_Q, now)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(sequence, sources, source, 0, 0, now + 20)
          == expected_release);
}

static void test_three_completed_taps_trigger_once(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    complete_tap(&sequence, sources, SOURCE_A, 0, REBOOT_HOTKEY_TAP);
    complete_tap(&sequence, sources, SOURCE_A, 100, REBOOT_HOTKEY_TAP);
    complete_tap(&sequence, sources, SOURCE_A, 200, REBOOT_HOTKEY_TRIGGER);
    complete_tap(&sequence, sources, SOURCE_A, 300, REBOOT_HOTKEY_TAP);
}

static void test_held_and_malformed_chords_do_not_repeat_or_leak(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, KEY_Q, 0)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, KEY_Q, 10)
          == REBOOT_HOTKEY_SWALLOW);

    uint8_t keys[KEY_COUNT] = {KEY_Q, KEY_A};
    CHECK(reboot_hotkey_process_report(&sequence, sources, 2, SOURCE_A,
                                       MODIFIERS, keys, KEY_COUNT,
                                       MODIFIERS, KEY_Q, 20)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(&sequence, sources, SOURCE_A, 0, 0, 30)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(sequence.completed_taps == 0);
}

static void test_timeout_and_unrelated_key_restart_sequence(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    complete_tap(&sequence, sources, SOURCE_A, 0, REBOOT_HOTKEY_TAP);
    complete_tap(&sequence, sources, SOURCE_A,
                 REBOOT_HOTKEY_TAP_TIMEOUT_US + 100,
                 REBOOT_HOTKEY_TAP);
    CHECK(sequence.completed_taps == 1);

    CHECK(report(&sequence, sources, SOURCE_A, 0, KEY_A,
                 REBOOT_HOTKEY_TAP_TIMEOUT_US + 200)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(sequence.completed_taps == 0);
}

static void test_wrong_chord_and_source_change_are_safe(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    CHECK(report(&sequence, sources, SOURCE_A, 0, KEY_Q, 0)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(sequence.completed_taps == 0);
    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS | 0x02, KEY_Q, 10)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(sequence.completed_taps == 0);

    complete_tap(&sequence, sources, SOURCE_A, 100, REBOOT_HOTKEY_TAP);
    complete_tap(&sequence, sources, SOURCE_B, 200, REBOOT_HOTKEY_TAP);
    CHECK(sequence.completed_taps == 1);
    CHECK(sequence.source == SOURCE_B);
}

static void test_overlong_press_is_not_a_tap(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, KEY_Q, 0)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, KEY_Q,
                 REBOOT_HOTKEY_TAP_TIMEOUT_US + 1)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(&sequence, sources, SOURCE_A, 0, 0,
                 REBOOT_HOTKEY_TAP_TIMEOUT_US + 20)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(sequence.completed_taps == 0);
}

static void test_modifiers_may_remain_held_between_taps(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, 0, 0)
          == REBOOT_HOTKEY_PASS);
    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, KEY_Q, 10)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, 0, 30)
          == REBOOT_HOTKEY_TAP);
}

static void test_entire_sequence_must_fit_in_timeout(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    complete_tap(&sequence, sources, SOURCE_A, 0, REBOOT_HOTKEY_TAP);
    complete_tap(&sequence, sources, SOURCE_A, 500000, REBOOT_HOTKEY_TAP);
    complete_tap(&sequence, sources, SOURCE_A,
                 REBOOT_HOTKEY_TAP_TIMEOUT_US + 1,
                 REBOOT_HOTKEY_TAP);
    CHECK(sequence.completed_taps == 1);
}

static void test_other_source_cancels_armed_press(void) {
    reboot_hotkey_sequence_t sequence = {0};
    reboot_hotkey_source_t sources[2] = {0};

    CHECK(report(&sequence, sources, SOURCE_A, MODIFIERS, KEY_Q, 0)
          == REBOOT_HOTKEY_SWALLOW);
    CHECK(report(&sequence, sources, SOURCE_B, 0, KEY_A, 10)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(report(&sequence, sources, SOURCE_A, 0, 0, 20)
          == REBOOT_HOTKEY_CANCEL);
    CHECK(sequence.completed_taps == 0);
}

int main(void) {
    test_three_completed_taps_trigger_once();
    test_held_and_malformed_chords_do_not_repeat_or_leak();
    test_timeout_and_unrelated_key_restart_sequence();
    test_wrong_chord_and_source_change_are_safe();
    test_overlong_press_is_not_a_tap();
    test_modifiers_may_remain_held_between_taps();
    test_entire_sequence_must_fit_in_timeout();
    test_other_source_cancels_armed_press();

    puts("reboot hotkey tests passed");
    return EXIT_SUCCESS;
}
