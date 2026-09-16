/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

#include "main.h"

/* ==================================================== *
 * Hotkeys to trigger actions via the keyboard.
 * ==================================================== */

hotkey_combo_t hotkeys[] = {
    /* Main keyboard switching hotkey. F24 is intentionally harmless if a
       malformed report ever escapes to the host. */
    {.modifier       = HOTKEY_MODIFIER,
     .keys           = {HOTKEY_TOGGLE},
     .key_count      = 1,
     .pass_to_os     = false,
     .action_handler = &output_toggle_hotkey_handler},

    /* Accept the former Ctrl+Caps Lock command while keyboards transition to
       F24. It is swallowed exactly like the primary command. */
    {.modifier       = LEGACY_HOTKEY_MODIFIER,
     .keys           = {LEGACY_HOTKEY_TOGGLE},
     .key_count      = 1,
     .pass_to_os     = false,
     .action_handler = &output_toggle_hotkey_handler},

    /* Pressing right ALT + right CTRL toggles the slow mouse mode */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_RIGHTCTRL,
     .keys           = {},
     .key_count      = 0,
     .pass_to_os     = true,
     .acknowledge    = true,
     .action_handler = &mouse_zoom_hotkey_handler},

    /* Switch lock */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTCTRL,
     .keys           = {HID_KEY_K},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &switchlock_hotkey_handler},

    /* Screen lock */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTCTRL,
     .keys           = {HID_KEY_L},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &screenlock_hotkey_handler},

    /* Toggle gaming mode */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_G},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &toggle_gaming_mode_handler},

    /* Clear inferred macOS zoom assist state and relearn wheel direction */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_Z},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &clear_zoom_assist_hotkey_handler},

    /* Enable screensaver pong for active output */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_S},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &enable_screensaver_pong_hotkey_handler},

    /* Enable screensaver jitter for active output */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_J},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &enable_screensaver_jitter_hotkey_handler},

    /* Disable screensaver for active output */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_X},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &disable_screensaver_hotkey_handler},

    /* Erase stored config */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_F12, HID_KEY_D},
     .key_count      = 2,
     .acknowledge    = true,
     .action_handler = &wipe_config_hotkey_handler},

    /* Record switch y coordinate  */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_F12, HID_KEY_Y},
     .key_count      = 2,
     .acknowledge    = true,
     .action_handler = &screen_border_hotkey_handler},

    /* Switch to configuration mode  */
    {.modifier       = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
     .keys           = {HID_KEY_C, HID_KEY_O},
     .key_count      = 2,
     .acknowledge    = true,
     .action_handler = &config_enable_hotkey_handler},

    /* Hold down left shift + right shift + F12 + A ==> firmware upgrade mode for board A (kbd) */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT | KEYBOARD_MODIFIER_LEFTSHIFT,
     .keys           = {HID_KEY_A},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &fw_upgrade_hotkey_handler_A},

    /* Hold down left shift + right shift + F12 + B ==> firmware upgrade mode for board B (mouse) */
    {.modifier       = KEYBOARD_MODIFIER_RIGHTSHIFT | KEYBOARD_MODIFIER_LEFTSHIFT,
     .keys           = {HID_KEY_B},
     .key_count      = 1,
     .acknowledge    = true,
     .action_handler = &fw_upgrade_hotkey_handler_B}};

/* ============================================================ *
 * Detect if any hotkeys were pressed
 * ============================================================ */

/* Tries to find if the keyboard report contains key, returns true/false */
bool key_in_report(uint8_t key, const hid_keyboard_report_t *report) {
    for (int j = 0; j < KEYS_IN_USB_REPORT; j++) {
        if (key == report->keycode[j]) {
            return true;
        }
    }

    return false;
}

/* Check if the current report matches a specific hotkey passed on */
bool check_specific_hotkey(hotkey_combo_t keypress, const hid_keyboard_report_t *report) {
    /* We expect all modifiers specified to be detected in the report */
    if (keypress.modifier != (report->modifier & keypress.modifier))
        return false;

    for (int n = 0; n < keypress.key_count; n++) {
        if (!key_in_report(keypress.keys[n], report)) {
            return false;
        }
    }

    /* Getting here means all of the keys were found. */
    return true;
}

/* Go through the list of hotkeys, check if any of them match. */
hotkey_combo_t *check_all_hotkeys(hid_keyboard_report_t *report, device_t *state) {
    for (int n = 0; n < ARRAY_SIZE(hotkeys); n++) {
        if (check_specific_hotkey(hotkeys[n], report)) {
            return &hotkeys[n];
        }
    }

    return NULL;
}

/* ==================================================== *
 * Keyboard State Management
 * ==================================================== */

/* Update the keyboard state for a specific device */
void update_kbd_state(device_t *state, hid_keyboard_report_t *report, uint8_t device_idx) {
    /* Ensure device_idx is within bounds */
    if (device_idx >= MAX_DEVICES)
        return;

    /* Update the keyboard state for this device */
    memcpy(&state->local_kbd_states[device_idx], report, sizeof(hid_keyboard_report_t));

    /* Track the largest keyboard index we have */
    if (state->max_kbd_idx < device_idx)
        state->max_kbd_idx = device_idx;
}

/* Update the struct storing the state of the keyboard(s) connected to the other board */
void update_remote_kbd_state(device_t *state, hid_keyboard_report_t *report) {
    memcpy(&state->remote_kbd_state, report, sizeof(hid_keyboard_report_t));
}

/* Add keys from source to destination, avoiding duplicates */
static void add_keys(hid_keyboard_report_t *dest, const hid_keyboard_report_t *src) {
    for (uint8_t i = 0; i < KEYS_IN_USB_REPORT; i++) {
        uint8_t key = src->keycode[i];
        
        if (key == 0 || key_in_report(key, dest))
            continue;
            
        uint8_t *empty_slot = memchr(dest->keycode, 0, KEYS_IN_USB_REPORT);
        if (empty_slot)
            *empty_slot = key;
    }
}

/* Release all keys */
void release_all_keys(device_t *state) {
    memset(state->local_kbd_states, 0, sizeof(state->local_kbd_states));
    memset(&state->remote_kbd_state, 0, sizeof(hid_keyboard_report_t));
    publish_local_modifiers(state);
    keyboard_sync_publish();
    
    static hid_keyboard_report_t empty_report = {0};
    queue_kbd_report_critical(&empty_report, state);
}


/* Combine all keyboard states into a single report */
void combine_kbd_states(device_t *state, hid_keyboard_report_t *combined_report) {
    memset(combined_report, 0, sizeof(hid_keyboard_report_t));

    /* Combine all local keyboards up to max_kbd_idx */
    for (uint8_t i = 0; i <= state->max_kbd_idx; i++) {
        combined_report->modifier |= state->local_kbd_states[i].modifier;
        add_keys(combined_report, &state->local_kbd_states[i]);
    }
    
    /* A USB reset invalidates the peer cache before core 1's next sync tick. */
    if (state->kbd_remote_generation == state->kbd_host_generation) {
        combined_report->modifier |= state->remote_kbd_state.modifier;
        add_keys(combined_report, &state->remote_kbd_state);
    }
}

/* ==================================================== *
 * Keyboard Queue Section
 * ==================================================== */

/* The short firmware lock serializes the FIFO and durable tail with core 0.
 * USB submission is nonblocking; no polling/wait/flash operation occurs here. */
void process_kbd_queue_task(device_t *state) {
    if (!state->tud_connected)
        return;
    hid_keyboard_report_t waiting;
    if (!queue_try_peek(&state->kbd_queue, &waiting))
        return;
    if (tud_suspended())
        tud_remote_wakeup();
    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    firmware_update_lock();
    hid_keyboard_report_t report;
    if (queue_try_peek(&state->kbd_queue, &report)
        && tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report.modifier, report.keycode))
        queue_try_remove(&state->kbd_queue, &report);
    if (state->kbd_latest_pending && queue_try_add(&state->kbd_queue, &state->kbd_latest))
        state->kbd_latest_pending = false;
    firmware_update_unlock();
}

static void queue_kbd_report_locked(hid_keyboard_report_t *report, device_t *state) {
    if (state->tud_connected) {
        state->kbd_latest = *report;
        /* Once overflowed, retain only the latest tail until a slot opens.
         * Never append newer transitions ahead of that pending tail. */
        if (state->kbd_latest_pending || !queue_try_add(&state->kbd_queue, report))
            state->kbd_latest_pending = true;
    }
}

void queue_kbd_report(hid_keyboard_report_t *report, device_t *state) {
    firmware_update_lock();
    queue_kbd_report_locked(report, state);
    firmware_update_unlock();
}

void keyboard_queue_current(device_t *state) {
    firmware_update_lock();
    hid_keyboard_report_t combined;
    combine_kbd_states(state, &combined);
    queue_kbd_report_locked(&combined, state);
    firmware_update_unlock();
}

bool queue_kbd_report_critical(hid_keyboard_report_t *report, device_t *state) {
    queue_kbd_report(report, state);
    return state->tud_connected;
}

/* Bus reset/unplug cannot retain historical downs for a newly enumerated host.
 * The next accepted source state supplies the current state, without replay. */
void keyboard_host_reset(device_t *state) {
    firmware_update_lock();
    hid_keyboard_report_t discarded;
    while (queue_try_remove(&state->kbd_queue, &discarded)) {}
    state->kbd_latest_pending = false;
    ++state->kbd_host_generation;
    state->peer_modifiers = 0;
    state->kbd_latest = (hid_keyboard_report_t){0};
    firmware_update_unlock();
}

void keyboard_focus_changed(device_t *state) {
    keyboard_host_reset(state);
    release_all_keys(state);
    keyboard_sync_reset();
}

void send_key(hid_keyboard_report_t *report, device_t *state) {
    (void)report;
    keyboard_sync_publish();
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        keyboard_queue_current(state);
}

/* Decide if consumer control reports go local or to the other board */
void send_consumer_control(uint8_t *raw_report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_cc_packet(raw_report, state);
    } else {
        queue_packet((uint8_t *)raw_report, CONSUMER_CONTROL_MSG, CONSUMER_CONTROL_LENGTH);
    }
}

/* Decide if consumer control reports go local or to the other board */
void send_system_control(uint8_t *raw_report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_system_packet(raw_report, state);
    } else {
        queue_packet((uint8_t *)raw_report, SYSTEM_CONTROL_MSG, SYSTEM_CONTROL_LENGTH);
    }
}

/* ==================================================== *
 * Parse and interpret the keys pressed on the keyboard
 * ==================================================== */

void process_keyboard_report(uint8_t *raw_report, int length, uint8_t itf, hid_interface_t *iface) {
    hid_keyboard_report_t new_report = {0};
    hid_keyboard_report_t stored_report;
    device_t *state                  = &global_state;
    hotkey_combo_t *hotkey           = NULL;

    if (length < KBD_REPORT_LENGTH || itf >= MAX_DEVICES)
        return;

    /* No more keys accepted if we're about to reboot */
    if (global_state.reboot_requested)
        return;

    if (extract_kbd_data(raw_report, length, itf, iface, &new_report) < 0)
        return;

    reboot_hotkey_result_t reboot_result = reboot_hotkey_process_report(
        &state->reboot_hotkey_sequence,
        state->reboot_hotkey_source,
        MAX_DEVICES,
        itf,
        new_report.modifier,
        new_report.keycode,
        KEYS_IN_USB_REPORT,
        KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTSHIFT,
        HID_KEY_Q,
        time_us_64());

    /* A swallowed reboot chord must never remain in the stored keyboard state.
       Otherwise a report from another keyboard could recombine and leak the
       hidden Q or modifiers to the active computer before Q is released. */
    stored_report = new_report;
    if (reboot_result == REBOOT_HOTKEY_SWALLOW
        || reboot_result == REBOOT_HOTKEY_TRIGGER) {
        stored_report = (hid_keyboard_report_t){0};
    }

    /* Update the keyboard state for this device */
    update_kbd_state(state, &stored_report, itf);
    publish_local_modifiers(state);
    record_local_activity(state, state->active_output);

    if (reboot_result == REBOOT_HOTKEY_SWALLOW) {
        send_key(&stored_report, state);
        return;
    }

    if (reboot_result == REBOOT_HOTKEY_TAP)
        blink_led(state);

    if (reboot_result == REBOOT_HOTKEY_TRIGGER) {
        blink_led(state);
        reboot_hotkey_handler(state, &stored_report);
        return;
    }

    /* Check if any hotkey was pressed */
    hotkey = check_all_hotkeys(&new_report, state);

    /* ... and take appropriate action */
    if (hotkey != NULL) {
        /* Provide visual feedback we received the action */
        if (hotkey->acknowledge)
            blink_led(state);

        /* Consumed chords must never enter a later source snapshot or another
         * keyboard's aggregate. Also deliver any release hidden by the chord. */
        if (!hotkey->pass_to_os) {
            stored_report = (hid_keyboard_report_t){0};
            update_kbd_state(state, &stored_report, itf);
            publish_local_modifiers(state);
            send_key(&stored_report, state);
        }

        /* Execute the corresponding handler */
        hotkey->action_handler(state, &new_report);

        /* And pass the key to the output PC if configured to do so. */
        if (!hotkey->pass_to_os)
            return;
    }

    /* This method will decide if the key gets queued locally or sent through UART */
    send_key(&new_report, state);
}

void process_consumer_report(uint8_t *raw_report, int length, uint8_t itf, hid_interface_t *iface) {
    /* Consumer interfaces may omit the report ID. */
    int data_len = length - iface->uses_report_id;

    if (data_len <= 0)
        return;

    uint8_t *data = raw_report + iface->uses_report_id;
    uint8_t new_report[CONSUMER_CONTROL_LENGTH] = {0};
    uint16_t *report_ptr = (uint16_t *)new_report;
    device_t *state = &global_state;
    keyboard_t *keyboard = get_keyboard(iface, raw_report[0]);

    /* If consumer control is variable, read the values from cc_array and send as array. */
    if (iface->consumer.is_variable) {
        for (int i = 0; i < MAX_CC_BUTTONS && i < 8 * data_len; i++) {
            int bit_idx = i % 8;
            int byte_idx = i >> 3;

            if ((data[byte_idx] >> bit_idx) & 1) {
                report_ptr[0] = keyboard->cc_array[i];
            }
        }
    }
    else {
        for (int i = 0; i < data_len && i < CONSUMER_CONTROL_LENGTH; i++)
            new_report[i] = data[i];
    }

    record_local_activity(state, state->active_output);
    send_consumer_control(new_report, state);
}

void process_system_report(uint8_t *raw_report, int length, uint8_t itf, hid_interface_t *iface) {
    int data_len = length - iface->uses_report_id;

    if (data_len < SYSTEM_CONTROL_LENGTH)
        return;

    uint8_t *data = raw_report + iface->uses_report_id;
    uint16_t new_report = data[0];
    uint8_t *report_ptr = (uint8_t *)&new_report;
    device_t *state = &global_state;

    record_local_activity(state, state->active_output);
    send_system_control(report_ptr, state);
}

keyboard_t *get_keyboard(hid_interface_t *iface, uint8_t report_id) {
    /* Lookup only: report decoding must never allocate a descriptor slot. */
    if (!iface->uses_report_id)
        return &iface->keyboards[PRIMARY_KEYBOARD];

    /* Go through known keyboards and match on report ID, return pointer to keyboard_t */
    for (int i = 0; i < iface->num_keyboards && i < MAX_KEYBOARDS; i++) {
        if (iface->keyboards[i].report_id == report_id) {
            return &iface->keyboards[i];
        }
    }

    /* Consumer controls retain their primary-slot storage. Keyboard report
       decoding separately rejects IDs that do not own this returned slot. */
    return &iface->keyboards[PRIMARY_KEYBOARD];
}

/* Descriptor parsing only. Existing slots remain writable at capacity; an
   excess report ID must not overwrite a previously registered keyboard. */
keyboard_t *get_or_add_keyboard(hid_interface_t *iface, uint8_t report_id) {
    if (!iface->uses_report_id)
        return &iface->keyboards[PRIMARY_KEYBOARD];

    for (int i = 0; i < iface->num_keyboards && i < MAX_KEYBOARDS; i++) {
        if (iface->keyboards[i].report_id == report_id)
            return &iface->keyboards[i];
    }

    if (iface->num_keyboards >= MAX_KEYBOARDS)
        return NULL;

    keyboard_t *keyboard = &iface->keyboards[iface->num_keyboards];
    keyboard->report_id = report_id;
    keyboard->uses_report_id = true;
    return keyboard;
}
