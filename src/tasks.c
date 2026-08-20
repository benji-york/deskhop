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

_Static_assert(ACTIVITY_OUTPUT_COUNT == NUM_SCREENS,
               "activity synchronization must cover every DeskHop output");

void task_scheduler(device_t *state, task_t *task) {
    uint64_t current_time = time_us_64();

    if (current_time < task->next_run)
        return;

    task->next_run = current_time + task->frequency;
    task->exec(state);
}

/* ================================================== *
 * ==============  Watchdog Functions  ============== *
 * ================================================== */

void kick_watchdog_task(device_t *state) {
    /* Read the timer AFTER duplicating the core1 timestamp,
       so it doesn't get updated in the meantime. */
    uint32_t core1_last_loop_pass = state->core1_last_loop_pass;
    uint32_t current_time         = time_us_32();

    /* If a reboot is requested, we'll stop updating watchdog */
    if (state->reboot_requested)
        return;

    /* If core1 stops updating the timestamp, we'll stop kicking the watchog and reboot */
    if ((uint32_t)(current_time - core1_last_loop_pass) < CORE1_HANG_TIMEOUT_US)
        watchdog_update();
}

/* ================================================== *
 * ===============  USB Device / Host  ============== *
 * ================================================== */

void usb_device_task(device_t *state) {
    tud_task();
}

void usb_host_task(device_t *state) {
    if (tuh_inited())
        tuh_task();
}

/* Record real input at its physical source. The separate direct timestamp is
   authoritative for this Pico and is the only activity we broadcast, which
   prevents timestamps from echoing back and becoming artificially newer. */
void record_local_activity(device_t *state, uint8_t output) {
    if (output >= NUM_SCREENS)
        return;

    uint64_t now = time_us_64();
    state->last_activity[output] = now;
    state->direct_activity[output] = now;
    state->direct_activity_valid |= (1u << output);
}

/* A routed HID report is proof of peer-originated activity. Keep it out of the
   direct array so the next activity packet never rebroadcasts it. */
void record_remote_activity(device_t *state, uint8_t output) {
    if (output >= NUM_SCREENS)
        return;

    uint64_t now = time_us_64();
    state->last_activity[output] = now;
    state->peer_activity[output] = now;
    state->peer_activity_valid |= (1u << output);
}

static void sync_activity(device_t *state) {
    uint64_t now = time_us_64();
    uart_packet_t packet = {.type = ACTIVITY_MSG};

    for (uint8_t output = 0; output < NUM_SCREENS; output++) {
        uint8_t mask = 1u << output;
        packet.data32[output] = activity_age_seconds(
            now, state->direct_activity[output], state->direct_activity_valid & mask);
    }

    /* This repeats every second, so a full queue or a corrupt UART packet only
       delays convergence; it cannot permanently lose the activity update. */
    queue_try_add(&state->uart_tx_queue, &packet);
}

mouse_report_t *screensaver_pong(device_t *state) {
    static mouse_report_t report = {0};
    static int dx = 20, dy = 25;

    /* Check if we are bouncing off the walls and reverse direction in that case. */
    if (report.x + dx < MIN_SCREEN_COORD || report.x + dx > MAX_SCREEN_COORD)
        dx = -dx;

    if (report.y + dy < MIN_SCREEN_COORD || report.y + dy > MAX_SCREEN_COORD)
        dy = -dy;

    report.x += dx;
    report.y += dy;

    return &report;
}

mouse_report_t *screensaver_jitter(device_t *state) {
    static mouse_report_t report = {
        .y = JITTER_DISTANCE,
        .mode = RELATIVE,
    };
    report.y = -report.y;

    return &report;
}

/* Have something fun and entertaining when idle. */
void screensaver_task(device_t *state) {
    const uint32_t delays[] = {
        0,        /* DISABLED, unused index 0 */
        5000,     /* PONG, move mouse every 5 ms for a high framerate */
        10000000, /* JITTER, once every 10 sec is more than enough */
    };
    static uint32_t last_pointer_move = 0;
    uint64_t now = time_us_64();
    screensaver_t *screensaver = &state->config.output[BOARD_ROLE].screensaver;
    uint64_t inactivity_period = now - state->last_activity[BOARD_ROLE];

    /* If we're not enabled, nothing to do here. */
    if (screensaver->mode == DISABLED)
        return;

    /* System is still not idle for long enough to activate or screensaver mode is not supported */
    if (inactivity_period < screensaver->idle_time_us || screensaver->mode > MAX_SS_VAL)
        return;

    /* We exceeded the maximum permitted screensaver runtime */
    if (screensaver->max_time_us
        && inactivity_period > (screensaver->max_time_us + screensaver->idle_time_us))
        return;

    /* If we're the selected output and we can only run on inactive output, nothing to do here. */
    if (screensaver->only_if_inactive && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        return;

    /* Keep both computers awake while either is genuinely in use, but stop all
       synthetic motion after the configured system-wide idle period. */
    if (screensaver_system_idle_timed_out(
            now,
            activity_latest_timestamp(state->direct_activity,
                                      state->direct_activity_valid,
                                      state->peer_activity,
                                      state->peer_activity_valid),
            state->config.screensaver_system_timeout_sec))
        return;

    /* We're active! Now check if it's time to move the cursor yet. */
    if (time_us_32() - last_pointer_move < delays[screensaver->mode])
        return;

    /* Return, if we're not connected or the host is suspended */
    if(!tud_ready()) {
        return;
    }

    mouse_report_t *report;
    switch (screensaver->mode) {
        case PONG:
            report = screensaver_pong(state);
            break;

        case JITTER:
            report = screensaver_jitter(state);
            break;

        default:
            return;
    }

    /* Move mouse pointer */
    queue_mouse_report(report, state);

    /* Update timer of the last pointer move */
    last_pointer_move = time_us_32();
}

/* Periodically emit heartbeat packets */
void heartbeat_output_task(device_t *state) {
    firmware_update_lock();

    /* A host UF2 drop is actively replacing the advertised image. Remain silent
       until reboot so the peer cannot start pulling a changing flash slot. */
    if (state->fw.source == FW_UPDATE_SOURCE_DROP) {
        firmware_update_unlock();
        return;
    }

    /* Config-mode timeout and BOOTSEL probing touch flash state. Heartbeats do
       not, and must continue during a pull so a stalled peer can recover. */
    if (!state->fw.upgrade_in_progress && state->config_mode_active) {
        /* Leave config mode if timeout expired and user didn't click exit */
        if (time_us_64() > state->config_mode_timer)
            reboot();

        /* Keep notifying the user we're still in config mode */
        blink_led(state);
    }

#ifdef DH_DEBUG
    /* Holding the button invokes bootsel firmware upgrade */
    if (!state->fw.upgrade_in_progress && is_bootsel_pressed())
        reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
#endif

    uint16_t running_version = state->_running_fw.version;
    uint32_t running_checksum = state->_running_fw.checksum;
    firmware_update_unlock();

    uart_packet_t packet = {.type = HEARTBEAT_MSG};
    packet.data16[0] = running_version;
    packet.data16[1] = FW_UPDATE_PROTOCOL_MARKER;
    packet.data32[1] = running_checksum;

    queue_try_add(&global_state.uart_tx_queue, &packet);
    sync_activity(state);
    sync_owned_zoom_assist(state);
}


/* Process other outgoing hid report messages. */
void process_hid_queue_task(device_t *state) {
    hid_generic_pkt_t packet;

    if (!queue_try_peek(&state->hid_queue_out, &packet))
        return;

    if (!tud_hid_n_ready(packet.instance))
        return;

    /* ... try sending it to the host, if it's successful */
    bool succeeded = tud_hid_n_report(packet.instance, packet.report_id, packet.data, packet.len);

    /* ... then we can remove it from the queue. Race conditions shouldn't happen [tm] */
    if (succeeded)
        queue_try_remove(&state->hid_queue_out, &packet);
}

/* Task that handles copying firmware from the other device to ours */
static void firmware_upgrade_task_locked(device_t *state) {
    if (state->reboot_requested || !state->fw.upgrade_in_progress)
        return;

    uint32_t now = time_us_32();
    fw_update_action_t action = fw_update_next_action(state->fw.source,
                                                      state->fw.byte_done,
                                                      state->fw.image_dirty,
                                                      now,
                                                      state->fw.requested_at_us,
                                                      state->fw.progressed_at_us,
                                                      state->peer_fw_last_seen_us);

    if (action == FW_UPDATE_WAIT)
        return;

    if (action == FW_UPDATE_ABANDON) {
        state->fw = (fw_upgrade_state_t){0};
        return;
    }

    if (action == FW_UPDATE_RESTART) {
        bool image_dirty = state->fw.image_dirty;
        uint16_t version = state->fw.version;
        uint32_t peer_checksum = state->fw.peer_checksum;
        state->fw = (fw_upgrade_state_t) {
            .upgrade_in_progress = true,
            /* Drain replies from the old transfer generation before requesting
               address zero. request_pending=false rejects them in the meantime. */
            .byte_done = false,
            .image_dirty = image_dirty,
            .source = FW_UPDATE_SOURCE_PULL,
            .version = version,
            .peer_checksum = peer_checksum,
            .checksum = 0xffffffff,
            .requested_at_us = now,
            .progressed_at_us = now,
        };
        return;
    }

    if (action == FW_UPDATE_PAUSE) {
        /* Keep executing safely from RAM and advertising heartbeats. When the
           peer returns, its heartbeat will restart the repair from address 0. */
        state->fw.source = FW_UPDATE_SOURCE_PULL_PAUSED;
        state->fw.request_pending = false;
        state->fw.byte_done = false;
        return;
    }

    /* A timeout asks for the same address again. Do not run the completed-word
       page/finalization logic until a response has actually advanced it. */
    if (!state->fw.byte_done) {
        request_byte(state, state->fw.address);
        return;
    }

    /* Queue the next read before writing a completed page. Other cores can also
       produce UART traffic, so request_byte must atomically succeed before it
       marks this word consumed. The response cannot update page_buffer until this
       task returns because packet_receiver_task runs on the same core. */
    bool transfer_complete = state->fw.address >= STAGING_IMAGE_SIZE;
    if (!transfer_complete && !request_byte(state, state->fw.address))
        return;

    /* A response advances address past the four bytes it supplied. At each page
       boundary, commit the page that has just finished. Address zero is the start
       of the transfer, not a completed page. */
    if (state->fw.address != 0 && TU_U32_BYTE0(state->fw.address) == 0x00) {
        uint32_t page_start_addr = state->fw.address - FLASH_PAGE_SIZE;
        write_flash_page((uint32_t)ADDR_FW_RUNNING + page_start_addr - XIP_BASE, state->page_buffer);
        state->fw.image_dirty = true;
    }

    /* The final response leaves address exactly at the image size. Finalize now;
       requesting that out-of-range address would never receive a response. */
    if (transfer_complete) {
        state->fw.upgrade_in_progress = 0;
        state->fw.source = FW_UPDATE_SOURCE_NONE;
        state->fw.checksum = ~state->fw.checksum;

        /* Checksum mismatch, we wipe the stage 2 bootloader and rely on ROM recovery */
        if(state->fw.checksum != state->fw.peer_checksum
           || !firmware_image_is_valid(state->fw.version, state->fw.checksum, true)) {
            enter_firmware_recovery();
        }

        else {
            state->fw.image_dirty = false;
            state->_running_fw = (firmware_metadata_t) {
                .magic = FIRMWARE_METADATA_MAGIC,
                .version = state->fw.version,
                .checksum = state->fw.checksum,
            };
            global_state.reboot_requested = true;
        }

        return;
    }
}

void firmware_upgrade_task(device_t *state) {
    firmware_update_lock();
    firmware_upgrade_task_locked(state);
    firmware_update_unlock();
}

void packet_receiver_task(device_t *state) {
    uint32_t current_pointer
        = (uint32_t)DMA_RX_BUFFER_SIZE - dma_channel_hw_addr(state->dma_rx_channel)->transfer_count;
    uint32_t delta = get_ptr_delta(current_pointer, state);

    /* If we don't have enough characters for a packet, skip loop and return immediately */
    while (delta >= RAW_PACKET_LENGTH) {
        if (is_start_of_packet(state)) {
            fetch_packet(state);
            process_packet(&state->in_packet, state);
            return;
        }

        /* No packet found, advance to next position and decrement delta */
        state->dma_ptr = NEXT_RING_IDX(state->dma_ptr);
        delta--;
    }
}
