/* Host-independent recognizer for the guarded reboot keyboard shortcut. */

#include "reboot_hotkey.h"

static void reset_progress(reboot_hotkey_sequence_t *sequence) {
    sequence->completed_taps = 0;
    sequence->source = UINT8_MAX;
    sequence->first_tap_us = 0;
}

void reboot_hotkey_reset(reboot_hotkey_sequence_t *sequence) {
    reset_progress(sequence);
}

void reboot_hotkey_reset_source(reboot_hotkey_source_t *source) {
    *source = (reboot_hotkey_source_t){0};
}

static void cancel_sequence(reboot_hotkey_sequence_t *sequence,
                            reboot_hotkey_source_t *sources,
                            size_t source_count) {
    reset_progress(sequence);

    /* Preserve q_down so every already-consumed Q stays suppressed until its
       physical release, but make that release ineligible to count as a tap. */
    for (size_t i = 0; i < source_count; i++) {
        if (sources[i].q_down)
            sources[i].valid = false;
    }
}

static size_t pressed_key_count(const uint8_t *keycodes,
                                size_t key_count,
                                uint8_t trigger_key,
                                bool *trigger_is_down) {
    size_t pressed = 0;
    *trigger_is_down = false;

    for (size_t i = 0; i < key_count; i++) {
        if (keycodes[i] == 0)
            continue;

        pressed++;
        if (keycodes[i] == trigger_key)
            *trigger_is_down = true;
    }

    return pressed;
}

reboot_hotkey_result_t reboot_hotkey_process_report(
    reboot_hotkey_sequence_t *sequence,
    reboot_hotkey_source_t *sources,
    size_t source_count,
    uint8_t source,
    uint8_t modifiers,
    const uint8_t *keycodes,
    size_t key_count,
    uint8_t required_modifiers,
    uint8_t trigger_key,
    uint64_t now_us) {
    if (source >= source_count)
        return REBOOT_HOTKEY_PASS;

    reboot_hotkey_source_t *source_state = &sources[source];
    bool trigger_is_down;
    size_t pressed = pressed_key_count(keycodes, key_count, trigger_key,
                                       &trigger_is_down);
    bool exact_chord = modifiers == required_modifiers
                       && trigger_is_down
                       && pressed == 1;

    if (sequence->completed_taps != 0
        && now_us - sequence->first_tap_us > REBOOT_HOTKEY_TAP_TIMEOUT_US) {
        reset_progress(sequence);
    }

    if (source_state->q_down) {
        if (now_us - source_state->pressed_at_us > REBOOT_HOTKEY_TAP_TIMEOUT_US)
            cancel_sequence(sequence, sources, source_count);

        /* Once an exact reboot chord has been swallowed, suppress every report
           containing that Q until it is released. This prevents a malformed
           follow-up report from leaking the consumed key through recombination. */
        if (trigger_is_down) {
            if (!exact_chord)
                cancel_sequence(sequence, sources, source_count);
            return REBOOT_HOTKEY_SWALLOW;
        }

        bool completed = source_state->valid && pressed == 0;
        reboot_hotkey_reset_source(source_state);

        if (!completed) {
            cancel_sequence(sequence, sources, source_count);
            return REBOOT_HOTKEY_CANCEL;
        }

        if (sequence->completed_taps != 0 && sequence->source != source)
            cancel_sequence(sequence, sources, source_count);

        sequence->source = source;
        if (sequence->completed_taps == 0)
            sequence->first_tap_us = now_us;
        sequence->completed_taps++;

        if (sequence->completed_taps < REBOOT_HOTKEY_REQUIRED_TAPS)
            return REBOOT_HOTKEY_TAP;

        reset_progress(sequence);
        return REBOOT_HOTKEY_TRIGGER;
    }

    if (exact_chord) {
        bool another_source_is_down = false;
        for (size_t i = 0; i < source_count; i++) {
            if (i != source && sources[i].q_down) {
                another_source_is_down = true;
                break;
            }
        }

        if ((sequence->completed_taps != 0 && sequence->source != source)
            || another_source_is_down) {
            cancel_sequence(sequence, sources, source_count);
        }

        source_state->q_down = true;
        source_state->valid = true;
        source_state->pressed_at_us = now_us;
        return REBOOT_HOTKEY_SWALLOW;
    }

    /* Modifiers may remain held between manual Q taps. Any non-modifier key
       outside the exact chord cancels the destructive sequence. */
    if (pressed != 0) {
        cancel_sequence(sequence, sources, source_count);
        return REBOOT_HOTKEY_CANCEL;
    }

    return REBOOT_HOTKEY_PASS;
}
