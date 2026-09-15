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
#include "diagnostic_history.h"

/* =================================================== *
 * ============  Hotkey Handler Routines  ============ *
 * =================================================== */

/* This is the main hotkey for switching outputs */
void output_toggle_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    /* If switching explicitly disabled, return immediately */
    if (state->switch_lock)
        return;

    state->active_output ^= 1;
    set_active_output(state, state->active_output);
};

/* Request a graceful reboot only while the currently executing image is
   intact. Both cores continue running until the watchdog expires, leaving
   enough time for queued all-up and UART reports to be delivered. */
static bool request_graceful_reboot(device_t *state, bool notify_peer) {
    uint8_t request = ENABLE;

    while (true) {
        firmware_update_lock();

        if (state->fw.upgrade_in_progress || state->fw.image_dirty || state->maintenance_reserved) {
            firmware_update_unlock();
            return false;
        }

        /* Never wait on a queue while holding the firmware lock: core 0 may
           need that lock before it can drain the queue. Retrying outside the
           lock preserves the peer-message-before-local-reboot ordering. */
        if (notify_peer
            && !queue_packet_try(&request, REBOOT_MSG, sizeof(request))) {
            firmware_update_unlock();
            tight_loop_contents();
            continue;
        }

        state->reboot_requested = true;
        firmware_update_unlock();
        break;
    }

    /* Queue host all-up after the reboot decision. Core 0 continues servicing
       HID and UART queues throughout the watchdog's 500 ms grace period. */
    keyboard_focus_changed(state);
    return true;
}

/* The third completed Ctrl+Right Shift+Q tap reboots both DeskHop boards. */
void reboot_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    request_graceful_reboot(state, true);
}

void _get_border_position(device_t *state, border_size_t *border) {
    /* To avoid having 2 different keys, if we're above half, it's the top coord */
    if (state->pointer_y > (MAX_SCREEN_COORD / 2))
        border->bottom = state->pointer_y;
    else
        border->top = state->pointer_y;
}

void _screensaver_set(device_t *state, uint8_t value) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        config_lock();
        state->config.output[BOARD_ROLE].screensaver.mode = value;
        config_unlock();
    } else
        send_value(value, SCREENSAVER_MSG);
};

/* This key combo records switch y top coordinate for different-size monitors  */
void screen_border_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    bool save = CURRENT_BOARD_IS_ACTIVE_OUTPUT;
    border_size_t snapshot;
    config_lock();
    border_size_t *border = &state->config.output[state->active_output].border;
    if (save)
        _get_border_position(state, border);
    snapshot = *border;
    config_unlock();
    if (save)
        save_config(state);

    queue_packet((uint8_t *)&snapshot, SYNC_BORDERS_MSG, sizeof(snapshot));
};

/* Routine maintenance uses PICOBOOT only: bit 0 disables the ROM USB disk.
   Physical BOOTSEL still provides the usual UF2 recovery drive. */
void fw_upgrade_hotkey_handler_A(device_t *state, hid_keyboard_report_t *report) {
    firmware_update_lock();
    if (state->maintenance_reserved) {
        firmware_update_unlock();
        return;
    }
    reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 1);
    firmware_update_unlock();
};

/* This key combo puts board B in firmware upgrade mode */
void fw_upgrade_hotkey_handler_B(device_t *state, hid_keyboard_report_t *report) {
    firmware_update_lock();
    if (state->maintenance_reserved) {
        firmware_update_unlock();
        return;
    }
    send_value(ENABLE, FIRMWARE_UPGRADE_MSG);
    firmware_update_unlock();
};

/* This key combo prevents mouse from switching outputs */
void switchlock_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    state->switch_lock ^= 1;
    send_value(state->switch_lock, SWITCH_LOCK_MSG);
}

/* This key combo toggles gaming mode */
void toggle_gaming_mode_handler(device_t *state, hid_keyboard_report_t *report) {
    state->gaming_mode ^= 1;
    send_value(state->gaming_mode, GAMING_MODE_MSG);
};

/* Clear inferred zoom assist without changing manually selected gaming mode. */
void clear_zoom_assist_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        clear_zoom_assist(state, state->active_output, true);
    else
        send_value(state->active_output, ZOOM_ASSIST_CLEAR_MSG);
}

/* This key combo locks both outputs simultaneously */
void screenlock_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    hid_keyboard_report_t lock_report = {0}, release_keys = {0};

    for (int out = 0; out < NUM_SCREENS; out++) {
        switch (state->config.output[out].os) {
            case WINDOWS:
            case LINUX:
                lock_report.modifier   = KEYBOARD_MODIFIER_LEFTGUI;
                lock_report.keycode[0] = HID_KEY_L;
                break;
            case MACOS:
                lock_report.modifier   = KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTGUI;
                lock_report.keycode[0] = HID_KEY_Q;
                break;
            default:
                break;
        }

        if (BOARD_ROLE == out) {
            queue_kbd_report(&lock_report, state);
            release_all_keys(state);
        } else {
            queue_packet((uint8_t *)&lock_report, KEYBOARD_REPORT_MSG, KBD_REPORT_LENGTH);
            queue_packet((uint8_t *)&release_keys, KEYBOARD_REPORT_MSG, KBD_REPORT_LENGTH);
        }
    }
}

/* When pressed, erases stored config in flash and loads defaults on both boards */
void wipe_config_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    wipe_config();
    load_config(state);
    send_value(ENABLE, WIPE_CONFIG_MSG);
}

/* When pressed, toggles the current mouse zoom mode state */
void mouse_zoom_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    state->mouse_zoom ^= 1;
    send_value(state->mouse_zoom, MOUSE_ZOOM_MSG);
};

/* When pressed, enables the pong screensaver on active output */
void enable_screensaver_pong_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    uint8_t desired_mode = state->config.output[BOARD_ROLE].screensaver.mode;

    /* If the user explicitly asks for pong screensaver to be active, ignore config and turn it on */
    if (desired_mode == DISABLED || desired_mode == JITTER)
        desired_mode = PONG;

    _screensaver_set(state, desired_mode);
}

/* When pressed, enables the jitter screensaver on active output */
void enable_screensaver_jitter_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    uint8_t desired_mode = state->config.output[BOARD_ROLE].screensaver.mode;

    /* If the user explicitly asks for jitter screensaver to be active, ignore config and turn it on */
    if (desired_mode == DISABLED || desired_mode == PONG)
        desired_mode = JITTER;

    _screensaver_set(state, desired_mode);
}

/* When pressed, disables the screensaver on active output */
void disable_screensaver_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    _screensaver_set(state, DISABLED);
}

/* Put the device into a special configuration mode */
void config_enable_hotkey_handler(device_t *state, hid_keyboard_report_t *report) {
    firmware_update_lock();
    if (state->maintenance_reserved) {
        firmware_update_unlock();
        return;
    }
    /* If config mode is already active, skip this and reboot to return to normal mode */
    if (!state->config_mode_active) {
        watchdog_hw->scratch[5] = MAGIC_WORD_1;
        watchdog_hw->scratch[6] = MAGIC_WORD_2;
    }

    state->reboot_requested = true;
    firmware_update_unlock();
    keyboard_focus_changed(state);
};


/* ==================================================== *
 * ==========  UART Message Handling Routines  ======== *
 * ==================================================== */

/* Function handles received keypresses from the other board */
void handle_keyboard_uart_msg(uart_packet_t *packet, device_t *state) {
    keyboard_synthetic_receive(packet, state);
}

/* Function handles received mouse moves from the other board */
static void handle_mouse_uart_report(uart_packet_t *packet, device_t *state, bool source_report) {
    mouse_report_t report = *(mouse_report_t *)packet->data;
    if (source_report) {
        state->peer_mouse_buttons = report.buttons;
        state->mouse_buttons = combined_mouse_buttons(state);
        /* A packet already in flight can still target the previous output.
         * Retain its source state without re-pressing the released old host. */
        if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT)
            return;
        report.buttons = combined_mouse_buttons(state);
    }
    bool activating_zoom = is_macos_zoom_scroll(state, report.wheel);
    observe_zoom_scroll(state, report.wheel);

    /* If the source had not yet received mirrored Command state, it may have
       encoded this first combined wheel+motion event as absolute. The owner
       knows the reported logical position, but emitting it as relative motion
       would treat coordinates as deltas. Retain the logical position and send
       a zero-motion relative wheel report instead; the next input carries the
       ordinary movement after state synchronization. */
    if (activating_zoom && report.mode != RELATIVE) {
        state->pointer_x = report.x;
        state->pointer_y = report.y;
        report.x = 0;
        report.y = 0;
        report.mode = RELATIVE;
    }

    queue_mouse_report(&report, state);

    /* A relative report carries deltas, not authoritative coordinates. Do not
       mistake those small deltas for an absolute cursor position. */
    if (report.mode != RELATIVE) {
        state->pointer_x = report.x;
        state->pointer_y = report.y;
    }
    state->mouse_buttons = combined_mouse_buttons(state);

    record_remote_activity(state, BOARD_ROLE);
}

void handle_mouse_abs_uart_msg(uart_packet_t *packet, device_t *state) {
    /* Capability learning can be asymmetric while old-format reports remain
     * in flight. Never reinterpret those physical reports as synthetic. */
    handle_mouse_uart_report(packet, state, true);
}

void handle_mouse_source_uart_msg(uart_packet_t *packet, device_t *state) {
    handle_mouse_uart_report(packet, state, true);
}

void handle_mouse_synthetic_uart_msg(uart_packet_t *packet, device_t *state) {
    handle_mouse_uart_report(packet, state, false);
}

void handle_mouse_buttons_sync_msg(uart_packet_t *packet, device_t *state) {
    if (packet->data[1] > 1)
        return;
    uint8_t before = combined_mouse_buttons(state);
    state->peer_mouse_state_known = true;
    state->peer_mouse_buttons = packet->data[0];
    state->mouse_buttons = combined_mouse_buttons(state);
    /* A detach must release only that source. Periodic state mirrors do not
     * manufacture input/activity or replay an ordinary physical report. */
    if (packet->data[1] && before != state->mouse_buttons && CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        bool relative = mouse_uses_relative_mode(state);
        mouse_report_t report = {.buttons = state->mouse_buttons,
            .x = relative ? 0 : state->pointer_x, .y = relative ? 0 : state->pointer_y,
            .mode = relative ? RELATIVE : ABSOLUTE};
        queue_mouse_report_critical(&report, state);
    }
}

/* Apply position-neutral mouse input at the active board's authoritative
   coordinates. This prevents button-only reports from an inactive board from
   reasserting a stale absolute position. */
void handle_mouse_nonmotion_uart_msg(uart_packet_t *packet, device_t *state) {
    mouse_nonmotion_report_t *input = (mouse_nonmotion_report_t *)packet->data;
    state->peer_mouse_buttons = input->buttons;
    state->mouse_buttons = combined_mouse_buttons(state);
    if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        return;
    observe_zoom_scroll(state, input->wheel);
    uint8_t mode = mouse_uses_relative_mode(state) || input->mode == RELATIVE
                       ? RELATIVE
                       : ABSOLUTE;
    mouse_report_t report = {
        .buttons = state->mouse_buttons,
        .x       = mode == RELATIVE ? 0 : state->pointer_x,
        .y       = mode == RELATIVE ? 0 : state->pointer_y,
        .wheel   = input->wheel,
        .pan     = input->pan,
        .mode    = mode,
    };

    queue_mouse_report(&report, state);
    record_remote_activity(state, BOARD_ROLE);
}

/* Merge source-authoritative activity ages into this Pico's monotonic clock.
   Unknown entries never erase previously received history after a peer reboot. */
void handle_activity_msg(uart_packet_t *packet, device_t *state) {
    uint64_t now = time_us_64();

    for (uint8_t output = 0; output < NUM_SCREENS; output++) {
        if (activity_merge_age_seconds(now, packet->data32[output],
                                       &state->peer_activity[output]))
            state->peer_activity_valid |= (1u << output);
    }
}

/* Adopt the authoritative cursor position from the other board.

   Both boards track the cursor independently, but there is only one cursor. After an
   output switch, the board that performed the switch knows where the cursor ended up,
   so it tells us. Without this, our stale coordinates - in particular the parking
   position from the hidden_pointer report in switch_to_another_pc - would be used the
   next time a pointing device on THIS board moves, making the cursor jump in from a
   screen corner. Worse, since the parked X sits right on the screen edge, the smallest
   movement would immediately trigger an unwanted switch back. */
void handle_pointer_sync_msg(uart_packet_t *packet, device_t *state) {
    state->pointer_x = (int16_t)packet->data16[0];
    state->pointer_y = (int16_t)packet->data16[1];
}

/* Versioned selection updates are idempotent. A newer token selecting the same
 * output must not repeatedly release held keys during heartbeat reconciliation. */
static void receive_output_selection(uart_packet_t *packet, device_t *state, bool allow_legacy) {
    selection_state_t incoming;
    bool legacy = allow_legacy && selection_is_legacy(packet->data);
    if (!legacy && !selection_decode(packet->data, &incoming))
        return;

    firmware_update_lock();
    uint8_t previous_output = state->active_output;
    bool accepted;
    if (legacy) {
        selection_legacy_request(&state->selection, packet->data[0], 1 - state->board_role);
        accepted = true;
    } else {
        accepted = selection_merge(&state->selection, &incoming, state->board_role);
    }
    if (accepted)
        state->active_output = state->selection.output;
    uint8_t selected_output = state->active_output;
    bool changed = previous_output != selected_output;
    firmware_update_unlock();

    if (!accepted)
        return;
    if (changed)
        diagnostic_history_record(HISTORY_OUTPUT_PEER, previous_output, selected_output, 0);
    /* Queue and USB operations can wait; never perform them under the state lock. */
    if (changed || legacy) {
        keyboard_focus_changed(state);
        if (changed)
            release_mouse_host_buttons(state);
        restore_leds(state);
    }
    if (!legacy)
        sync_output_selection(state);
}

void handle_output_select_msg(uart_packet_t *packet, device_t *state) {
    receive_output_selection(packet, state, true);
}

void handle_output_select_sync_msg(uart_packet_t *packet, device_t *state) {
    receive_output_selection(packet, state, false);
}

/* Apply the same disk-free maintenance mode to the peer request. */
void handle_fw_upgrade_msg(uart_packet_t *packet, device_t *state) {
    firmware_update_lock();
    if (state->maintenance_reserved) {
        firmware_update_unlock();
        return;
    }
    reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 1);
    firmware_update_unlock();
}

/* Comply with request to turn mouse zoom mode on/off  */
void handle_mouse_zoom_msg(uart_packet_t *packet, device_t *state) {
    state->mouse_zoom = packet->data[0];
}

/* Process request to update keyboard LEDs */
void handle_set_report_msg(uart_packet_t *packet, device_t *state) {
    /* We got this via serial, so it's stored to the opposite of our board role */
    state->keyboard_leds_desired[OTHER_ROLE] = packet->data[0];

    /* If we have a keyboard we can control leds on, restore state if active */
    if (global_state.keyboard_connected && !CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        restore_leds(state);
}

/* Process request to block mouse from switching, update internal state */
void handle_switch_lock_msg(uart_packet_t *packet, device_t *state) {
    state->switch_lock = packet->data[0];
}

/* Handle border syncing message that lets the other device know about monitor height offset */
void handle_sync_borders_msg(uart_packet_t *packet, device_t *state) {
    bool notify = CURRENT_BOARD_IS_ACTIVE_OUTPUT;
    border_size_t snapshot;
    config_lock();
    border_size_t *border = &state->config.output[state->active_output].border;
    if (notify)
        _get_border_position(state, border);
    else
        memcpy(border, packet->data, sizeof(border_size_t));
    snapshot = *border;
    config_unlock();
    if (notify)
        queue_packet((uint8_t *)&snapshot, SYNC_BORDERS_MSG, sizeof(snapshot));

    save_config(state);
}

/* When this message is received, flash the locally attached LED to verify serial comms */
void handle_flash_led_msg(uart_packet_t *packet, device_t *state) {
    blink_led(state);
}

/* When this message is received, wipe the local flash config */
void handle_wipe_config_msg(uart_packet_t *packet, device_t *state) {
    wipe_config();
    load_config(state);
}

/* Update screensaver state after received message */
void handle_screensaver_msg(uart_packet_t *packet, device_t *state) {
    config_lock();
    state->config.output[BOARD_ROLE].screensaver.mode = packet->data[0];
    config_unlock();
}

/* Process consumer control message */
void handle_consumer_control_msg(uart_packet_t *packet, device_t *state) {
    queue_cc_packet(packet->data, state);
    record_remote_activity(state, BOARD_ROLE);
}

/* Process system control message */
void handle_system_control_msg(uart_packet_t *packet, device_t *state) {
    queue_system_packet(packet->data, state);
    record_remote_activity(state, BOARD_ROLE);
}

/* Process request to store config to flash */
void handle_save_config_msg(uart_packet_t *packet, device_t *state) {
    save_config(state);
}

/* Process request to reboot the board */
void handle_reboot_msg(uart_packet_t *packet, device_t *state) {
    /* Do not echo: the initiating board has already scheduled its own reboot. */
    request_graceful_reboot(state, false);
}

/* Decapsulate and send to the other box */
void handle_proxy_msg(uart_packet_t *packet, device_t *state) {
    queue_packet(&packet->data[1], (enum packet_type_e)packet->data[0], PACKET_DATA_LENGTH - 1);
}

/* Process relative mouse command */
void handle_toggle_gaming_msg(uart_packet_t *packet, device_t *state) {
    state->gaming_mode = packet->data[0];
}

/* Process api communication messages */
void handle_api_msgs(uart_packet_t *packet, device_t *state) {
    uint8_t value_idx = packet->data[0];
    const field_map_t *map = get_field_map_entry(value_idx);

    /* If we don't have a valid map entry, return immediately */
    if (map == NULL)
        return;

    /* Create a pointer to the offset into the structure we need to access */
    uint8_t *ptr = (((uint8_t *)&global_state) + map->offset);

    if (packet->type == SET_VAL_MSG) {
        /* Not allowing writes to objects defined as read-only */
        if (map->readonly)
            return;

        config_lock();
        memcpy(ptr, &packet->data[1], map->len);
        config_unlock();
    }
    else if (packet->type == GET_VAL_MSG) {
        uart_packet_t response = {.type=GET_VAL_MSG, .data={[0] = value_idx}};
        config_lock();
        memcpy(&response.data[1], ptr, map->len);
        config_unlock();
        queue_cfg_packet(&response, state);
    }

    /* With each GET/SET message, we reset the configuration mode timeout */
    reset_config_timer(state);
}

/* Handle the "read all" message by calling our "read one" handler for each type */
void handle_api_read_all_msg(uart_packet_t *packet, device_t *state) {
    uart_packet_t result = {.type=GET_VAL_MSG};

    for (int i = 0; i < get_field_map_length(); i++) {
        result.data[0] = get_field_map_index(i)->idx;
        handle_api_msgs(&result, state);
    }
}

/* Process request packet and create a response */
void handle_request_byte_msg(uart_packet_t *packet, device_t *state) {
    uint32_t address = packet->data32[0];

    firmware_update_lock();

    /* Never expose a slot while this board is replacing or repairing it. */
    if (state->reboot_requested || state->fw.upgrade_in_progress || state->maintenance_reserved) {
        firmware_update_unlock();
        return;
    }

    if (address > STAGING_IMAGE_SIZE) {
        firmware_update_unlock();
        return;
    }

    /* Legacy updaters request one sentinel word at the exact end of the image
       after committing the final page. Acknowledge it without reading beyond
       the image so they can enter their completion branch and reboot. */
    uint32_t data = 0;
    if (address < STAGING_IMAGE_SIZE) {
        if (!read_running_firmware_word(address, &data)) {
            firmware_update_unlock();
            return;
        }
    }

    state->maintenance_source_seen = true;
    state->maintenance_source_last_us = time_us_64();
    firmware_update_unlock();

    /* Add requested data to bytes 4-7 in the packet and return it with a different type */
    packet->data32[1] = data;

    queue_packet_blocking(packet->data, RESPONSE_BYTE_MSG, PACKET_DATA_LENGTH);
}

/* Process response message following a request we sent to read a byte */
/* state->page_offset and state->page_number are kept locally and compared to returned values */
static void handle_response_byte_msg_locked(uart_packet_t *packet, device_t *state) {
    if (state->reboot_requested)
        return;

    uint32_t address = packet->data32[0];

    /* Retransmission can produce a late duplicate response. Only the one exact
       response currently outstanding may touch the page buffer or checksum. */
    if (!fw_update_response_expected(state->fw.source,
                                     state->fw.request_pending,
                                     state->fw.address,
                                     address))
        return;

    if (address >= STAGING_IMAGE_SIZE || address % sizeof(uint32_t) != 0)
        return;

    /* Provide visual feedback of the ongoing copy by toggling LED for every sector */
    if((address & 0xfff) == 0x000)
        toggle_led();

    /* Update checksum as we receive each byte */
    if (address < STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE)
        for (int i=0; i<4; i++)
            state->fw.checksum = crc32_iter(state->fw.checksum, packet->data[4 + i]);

    memcpy(state->page_buffer + (uint8_t)address, &packet->data32[1], sizeof(uint32_t));

    /* Neeeeeeext byte, please! */
    state->fw.address += sizeof(uint32_t);
    if (state->fw.address % FLASH_PAGE_SIZE == 0)
        diagnostic_update_progress(state->fw.address);
    state->fw.progressed_at_us = time_us_32();
    state->fw.request_pending = false;
    state->fw.byte_done = true;
}

void handle_response_byte_msg(uart_packet_t *packet, device_t *state) {
    firmware_update_lock();
    handle_response_byte_msg_locked(packet, state);
    firmware_update_unlock();
}

static void begin_firmware_pull(device_t *state,
                                uint16_t version,
                                uint32_t peer_checksum,
                                bool image_dirty,
                                uint32_t now,
                                bool drain_old_responses) {
    state->fw = (fw_upgrade_state_t) {
        .upgrade_in_progress = true,
        .byte_done = !drain_old_responses,
        .image_dirty = image_dirty,
        .source = FW_UPDATE_SOURCE_PULL,
        .version = version,
        .peer_checksum = peer_checksum,
        .checksum = 0xffffffff,
        .requested_at_us = now,
        .progressed_at_us = now,
    };
    diagnostic_update_begin(DIAGNOSTIC_SOURCE_PEER, version);
}

/* Process a request to read a firmware package from flash */
static void handle_heartbeat_msg_locked(uart_packet_t *packet, device_t *state) {
    if (state->reboot_requested || state->maintenance_reserved)
        return;

    uint16_t other_running_version = packet->data16[0];
    uint32_t other_running_checksum = packet->data32[1];
    uint32_t now = time_us_32();

    /* Firmware before v0.85 used bytes 4-5 for unrelated state and supplied no
       build checksum. Old receivers can consume our extended heartbeat, but a
       new receiver must not pull from an unmarked, unverifiable source. */
    if (!fw_update_peer_compatible(packet->data16[1])) {
        if (state->fw.source == FW_UPDATE_SOURCE_PULL) {
            if (state->fw.image_dirty) {
                state->fw.source = FW_UPDATE_SOURCE_PULL_PAUSED;
                state->fw.request_pending = false;
                state->fw.byte_done = false;
                diagnostic_update_phase(DIAGNOSTIC_UPDATE_PAUSED);
            }
            else {
                diagnostic_update_phase(DIAGNOSTIC_UPDATE_ABANDONED);
                state->fw = (fw_upgrade_state_t){0};
            }
        }
        return;
    }

    state->peer_fw_last_seen_us = now;

    /* A dirty transfer whose peer disappeared remains alive in RAM. A newer
       heartbeat resumes its repair, after a short stale-response drain. */
    if (state->fw.source == FW_UPDATE_SOURCE_PULL_PAUSED) {
        if (other_running_version >= state->_running_fw.version)
            begin_firmware_pull(state, other_running_version, other_running_checksum,
                                state->fw.image_dirty, now, true);
        return;
    }

    /* Pin the source version so a peer that changes image midway cannot create
       a hybrid image or silently downgrade this board. */
    if (state->fw.source == FW_UPDATE_SOURCE_PULL) {
        if (other_running_version != state->fw.version
            || other_running_checksum != state->fw.peer_checksum) {
            if (other_running_version > state->_running_fw.version
                || (state->fw.image_dirty
                    && other_running_version == state->_running_fw.version)) {
                begin_firmware_pull(state, other_running_version, other_running_checksum,
                                    state->fw.image_dirty, now, true);
            }
            else if (state->fw.image_dirty) {
                state->fw.source = FW_UPDATE_SOURCE_PULL_PAUSED;
                state->fw.request_pending = false;
                state->fw.byte_done = false;
                diagnostic_update_phase(DIAGNOSTIC_UPDATE_PAUSED);
            }
            else {
                diagnostic_update_phase(DIAGNOSTIC_UPDATE_ABANDONED);
                state->fw = (fw_upgrade_state_t){0};
            }
        }
        return;
    }

    if (state->fw.upgrade_in_progress)
        return;

    /* If the other board isn't running a newer version, we are done */
    if (other_running_version <= state->_running_fw.version)
        return;

    /* It is? Ok, kick off the firmware upgrade */
    begin_firmware_pull(state, other_running_version, other_running_checksum,
                        false, now, false);
}

void handle_heartbeat_msg(uart_packet_t *packet, device_t *state) {
    firmware_update_lock();
    handle_heartbeat_msg_locked(packet, state);
    firmware_update_unlock();
}


/* ==================================================== *
 * ==============  Output Switch Routines  ============ *
 * ==================================================== */

/* Update output variable, set LED on/off and notify the other board so they are in sync. */
void set_active_output(device_t *state, uint8_t new_output) {
    if (new_output >= NUM_SCREENS)
        return;
    uint8_t payload[PACKET_DATA_LENGTH];
    firmware_update_lock();
    /* output_toggle_hotkey_handler historically updates active_output first;
     * the accepted token still records the previous selection here. */
    uint8_t previous_output = state->selection.output;
    bool changed = previous_output != new_output;
    selection_request(&state->selection, new_output, state->board_role);
    state->active_output = new_output;
    selection_encode(&state->selection, payload);
    firmware_update_unlock();
    if (changed)
        diagnostic_history_record(HISTORY_OUTPUT_LOCAL, previous_output, new_output, 0);
    restore_leds(state);

    /* A dropped selection packet leaves the two Picos routing reports to
       different hosts. This queue is drained by the other core, so wait for a
       slot rather than silently losing a switch-critical state transition. */
    queue_packet_blocking(payload, OUTPUT_SELECT_MSG, sizeof(payload));

    /* If we were holding a key down and drag the mouse to another screen, the key gets stuck.
       Changing outputs = no more keypresses on the previous system. */
    keyboard_focus_changed(state);
    if (changed)
        release_mouse_host_buttons(state);
}

/* Announce the initialized state without treating the boot default as a new
 * human selection. A surviving upgraded peer can restore its accepted token. */
void announce_initial_output(device_t *state) {
    uint8_t payload[PACKET_DATA_LENGTH];
    firmware_update_lock();
    selection_encode(&state->selection, payload);
    firmware_update_unlock();
    restore_leds(state);
    queue_packet_blocking(payload, OUTPUT_SELECT_MSG, sizeof(payload));
    /* Core 1 is already live. Its source/protocol state was initialized before
     * launch; the core-0 announcement must not mutate its ring or snapshots. */
    keyboard_host_reset(state);
}

/* Also sent after an accepted merge for prompt joining. Dropped/full-queue
 * attempts are retried by the existing 1 Hz heartbeat; old peers ignore ID32. */
void sync_output_selection(device_t *state) {
    uint8_t payload[PACKET_DATA_LENGTH];
    firmware_update_lock();
    selection_encode(&state->selection, payload);
    firmware_update_unlock();
    queue_packet_try(payload, OUTPUT_SELECT_SYNC_MSG, sizeof(payload));
}
