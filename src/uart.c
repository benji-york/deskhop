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
#include "diagnostic_peer.h"
#include "diagnostic_peer_history.h"
#include "diagnostic_history.h"
#include "diagnostic_verify.h"
#include "maintenance.h"

/* ================================================== *
 * ===============  Sending Packets  ================ *
 * ================================================== */

/* UART v1: bounded nibble encoding excludes delimiters and the old AA55
   preamble, even when arbitrary payload/CRC bytes contain that sequence.
   All meaningful fields have one CRC32; no unprotected command/type path. */
void write_raw_packet(uint8_t *dst, uart_packet_t *packet) {
    uint8_t body[UART_FRAME_BODY_LENGTH] = {UART_FRAME_VERSION, PACKET_DATA_LENGTH, packet->type};
    memcpy(body + 3, packet->data, PACKET_DATA_LENGTH);
    uint32_t crc = calc_packet_checksum(packet);
    for (unsigned i = 0; i < 4; ++i)
        body[11 + i] = (uint8_t)(crc >> (8 * i));
    dst[0] = UART_FRAME_START;
    for (unsigned i = 0; i < sizeof(body); ++i) {
        dst[1 + 2 * i] = 0x40 | (body[i] >> 4);
        dst[2 + 2 * i] = 0x40 | (body[i] & 0xf);
    }
    dst[RAW_PACKET_LENGTH - 1] = UART_FRAME_END;
}

/* Schedule packet for sending to the other box */
bool queue_packet_try(const uint8_t *data, enum packet_type_e packet_type, int length) {
    if (length < 0 || length > PACKET_DATA_LENGTH || (length && !data)
        || packet_type <= 0 || packet_type > UINT8_MAX)
        return false;
    uart_packet_t packet = {.type = packet_type};
    if (length)
        memcpy(packet.data, data, length);

    return queue_try_add(&global_state.uart_tx_queue, &packet);
}

void queue_packet(const uint8_t *data, enum packet_type_e packet_type, int length) {
    if (!queue_packet_try(data, packet_type, length))
        diagnostic_history_record(HISTORY_UART_DROPPED, 0, 0, packet_type);
}

/* Preserve source-response backpressure. This helper sends protected frames
   only; it does not enable communication with legacy UART receivers. */
void queue_packet_blocking(const uint8_t *data, enum packet_type_e packet_type, int length) {
    if (length < 0 || length > PACKET_DATA_LENGTH || (length && !data)
        || packet_type <= 0 || packet_type > UINT8_MAX)
        return;
    uart_packet_t packet = {.type = packet_type};
    if (length)
        memcpy(packet.data, data, length);

    queue_add_blocking(&global_state.uart_tx_queue, &packet);
}

/* Sends just one byte of a certain packet type to the other box. */
void send_value(const uint8_t value, enum packet_type_e packet_type) {
    queue_packet(&value, packet_type, sizeof(uint8_t));
}

/* Progress only WebHID-originated maintenance on the same core as its callback.
   A full queue retains the peer request for another poll; normal TX below keeps
   making room. Never arm the watchdog while waiting for actual wire drain. */
static bool process_config_bootloader_request(device_t *state) {
    if (!state->config_bootloader_peer_pending && !state->config_bootloader_local_pending)
        return false;

    firmware_update_lock();
    if (state->maintenance_reserved) {
        firmware_update_unlock();
        return false;
    }
    if (state->fw.upgrade_in_progress || state->fw.image_dirty || state->batch.tx.active) {
        state->config_bootloader_peer_pending = false;
        state->config_bootloader_local_pending = false;
        firmware_update_unlock();
        return false;
    }

    /* The maintenance command has no payload semantics. Serialize a canonical
       empty payload using the same protected UART frame as normal dispatch. */
    if (state->config_bootloader_peer_pending
        && queue_packet_try(NULL, FIRMWARE_UPGRADE_MSG, 0))
        state->config_bootloader_peer_pending = false;

    bool ready = state->config_bootloader_local_pending
        && !state->config_bootloader_peer_pending
        && queue_is_empty(&state->uart_tx_queue)
        && !dma_channel_is_busy(state->dma_tx_channel)
        && !(uart_get_hw(SERIAL_UART)->fr & UART_UARTFR_BUSY_BITS);
    if (ready) {
        state->config_bootloader_local_pending = false;
        /* Keep the updater excluded through ROM entry, closing the race between
           the final active/dirty check and reset. Bit 0 disables the ROM disk. */
        reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 1);
    }
    /* ROM entry does not return on silicon; native reset doubles do. */
    firmware_update_unlock();
    return ready;
}

/* Process outgoing config report messages. */
void process_uart_tx_task(device_t *state) {
    _Static_assert(RAW_PACKET_LENGTH <= DMA_TX_BUFFER_SIZE, "UART frame exceeds DMA buffer");
    uart_packet_t packet = {0};

    if (maintenance_task(time_us_64()))
        return;

    if (process_config_bootloader_request(state))
        return;

    if (dma_channel_is_busy(state->dma_tx_channel))
        return;

    /* Page service never fills the normal queue. Pending input/control traffic
     * always wins; at most the one DMA frame already selected can precede new HID. */
    if (!queue_try_remove(&state->uart_tx_queue, &packet)
        && !firmware_batch_next_tx(state, &packet))
        return;

    if (!maintenance_packet_allowed(packet.type, packet.data, time_us_64()))
        return;

    write_raw_packet(uart_txbuf, &packet);
    dma_channel_transfer_from_buffer_now(state->dma_tx_channel, uart_txbuf, RAW_PACKET_LENGTH);
}

/* ================================================== *
 * ===============  Parsing Packets  ================ *
 * ================================================== */

static void handle_verify_request(uart_packet_t *packet, device_t *state) {
    (void)state;
    diagnostic_verify_receive(false, packet->data, time_us_64());
}

static void handle_maintenance(uart_packet_t *packet, device_t *state) {
    (void)state;
    maintenance_receive(packet->type, packet->data, time_us_64());
}

static void handle_verify_response(uart_packet_t *packet, device_t *state) {
    (void)state;
    diagnostic_verify_receive(true, packet->data, time_us_64());
}

static void handle_diagnostic_request(uart_packet_t *packet, device_t *state) {
    (void)state;
    diagnostic_peer_receive(false, packet->data, time_us_64());
}

static void handle_diagnostic_response(uart_packet_t *packet, device_t *state) {
    (void)state;
    diagnostic_peer_receive(true, packet->data, time_us_64());
}

static void handle_history_request(uart_packet_t *packet, device_t *state) {
    (void)state;
    diagnostic_peer_history_receive(false, packet->data, time_us_64());
}

static void handle_history_response(uart_packet_t *packet, device_t *state) {
    (void)state;
    diagnostic_peer_history_receive(true, packet->data, time_us_64());
}

const uart_handler_t uart_handler[] = {
    /* Core functions */
    {.type = KEYBOARD_STATE_RESET_MSG, .handler = keyboard_sync_receive},
    {.type = KEYBOARD_STATE_REQUEST_MSG, .handler = keyboard_sync_receive},
    {.type = KEYBOARD_STATE_0_MSG, .handler = keyboard_sync_receive},
    {.type = KEYBOARD_STATE_1_MSG, .handler = keyboard_sync_receive},
    {.type = KEYBOARD_STATE_2_MSG, .handler = keyboard_sync_receive},
    {.type = KEYBOARD_STATE_3_MSG, .handler = keyboard_sync_receive},
    {.type = KEYBOARD_STATE_4_MSG, .handler = keyboard_sync_receive},

    {.type = KEYBOARD_REPORT_MSG, .handler = handle_keyboard_uart_msg},
    {.type = MOUSE_REPORT_MSG, .handler = handle_mouse_abs_uart_msg},
    {.type = MOUSE_SOURCE_REPORT_MSG, .handler = handle_mouse_source_uart_msg},
    {.type = MOUSE_SYNTHETIC_REPORT_MSG, .handler = handle_mouse_synthetic_uart_msg},
    {.type = MOUSE_BUTTONS_SYNC_MSG, .handler = handle_mouse_buttons_sync_msg},
    {.type = MOUSE_NONMOTION_MSG, .handler = handle_mouse_nonmotion_uart_msg},
    {.type = OUTPUT_SELECT_MSG, .handler = handle_output_select_msg},
    {.type = OUTPUT_SELECT_SYNC_MSG, .handler = handle_output_select_sync_msg},
    {.type = POINTER_SYNC_MSG, .handler = handle_pointer_sync_msg},
    {.type = MODIFIER_STATE_MSG, .handler = handle_modifier_state_msg},
    {.type = ZOOM_ASSIST_MSG, .handler = handle_zoom_assist_msg},
    {.type = ZOOM_ASSIST_CLEAR_MSG, .handler = handle_zoom_assist_clear_msg},
    {.type = ACTIVITY_MSG, .handler = handle_activity_msg},

    /* Box control */
    {.type = MOUSE_ZOOM_MSG, .handler = handle_mouse_zoom_msg},
    {.type = KBD_SET_REPORT_MSG, .handler = handle_set_report_msg},
    {.type = SWITCH_LOCK_MSG, .handler = handle_switch_lock_msg},
    {.type = SYNC_BORDERS_MSG, .handler = handle_sync_borders_msg},
    {.type = FLASH_LED_MSG, .handler = handle_flash_led_msg},
    {.type = GAMING_MODE_MSG, .handler = handle_toggle_gaming_msg},
    {.type = CONSUMER_CONTROL_MSG, .handler = handle_consumer_control_msg},
    {.type = SYSTEM_CONTROL_MSG, .handler = handle_system_control_msg},
    {.type = SCREENSAVER_MSG, .handler = handle_screensaver_msg},

    /* Config */
    {.type = WIPE_CONFIG_MSG, .handler = handle_wipe_config_msg},
    {.type = SAVE_CONFIG_MSG, .handler = handle_save_config_msg},
    {.type = REBOOT_MSG, .handler = handle_reboot_msg},
    {.type = GET_VAL_MSG, .handler = handle_api_msgs},
    {.type = GET_ALL_VALS_MSG, .handler = handle_api_read_all_msg},
    {.type = SET_VAL_MSG, .handler = handle_api_msgs},
    {.type = CONFIG_CONFIRM_META_MSG, .handler = handle_config_confirm_msg},
    {.type = CONFIG_CONFIRM_LO_MSG, .handler = handle_config_confirm_msg},
    {.type = CONFIG_CONFIRM_HI_MSG, .handler = handle_config_confirm_msg},
    {.type = CONFIG_CONFIRM_EXEC_MSG, .handler = handle_config_confirm_msg},
    {.type = CONFIG_CONFIRM_ACK_META_MSG, .handler = handle_config_confirm_msg},
    {.type = CONFIG_CONFIRM_ACK_VALUE_MSG, .handler = handle_config_confirm_msg},

    /* Firmware */
    {.type = REQUEST_BYTE_MSG, .handler = handle_request_byte_msg},
    {.type = RESPONSE_BYTE_MSG, .handler = handle_response_byte_msg},
    {.type = FW_BATCH_CAPS_REQUEST_MSG, .handler = firmware_batch_packet},
    {.type = FW_BATCH_CAPS_RESPONSE_MSG, .handler = firmware_batch_packet},
    {.type = FW_BATCH_PAGE_REQUEST_MSG, .handler = firmware_batch_packet},
    {.type = FW_BATCH_PAGE_DATA_MSG, .handler = firmware_batch_packet},
    {.type = FW_BATCH_PAGE_END_MSG, .handler = firmware_batch_packet},
    {.type = DIAGNOSTIC_STATUS_REQUEST_MSG, .handler = handle_diagnostic_request},
    {.type = DIAGNOSTIC_STATUS_RESPONSE_MSG, .handler = handle_diagnostic_response},
    {.type = DIAGNOSTIC_HISTORY_REQUEST_MSG, .handler = handle_history_request},
    {.type = DIAGNOSTIC_HISTORY_RESPONSE_MSG, .handler = handle_history_response},
    {.type = DIAGNOSTIC_VERIFY_REQUEST_MSG, .handler = handle_verify_request},
    {.type = DIAGNOSTIC_VERIFY_RESPONSE_MSG, .handler = handle_verify_response},
    {.type = FIRMWARE_UPGRADE_MSG, .handler = handle_fw_upgrade_msg},
    {.type = MAINTENANCE_BOOTLOADER_REQUEST_MSG, .handler = handle_maintenance},
    {.type = MAINTENANCE_BOOTLOADER_ACK_MSG, .handler = handle_maintenance},

    {.type = HEARTBEAT_MSG, .handler = handle_heartbeat_msg},
    {.type = PROXY_PACKET_MSG, .handler = handle_proxy_msg},
};

void process_packet(uart_packet_t *packet, device_t *state) {
    if (!verify_checksum(packet)) {
        diagnostic_history_record(HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, packet->type);
        return;
    }

    /* A proxy is a configuration-only envelope. In particular it may not
       recursively unwrap another proxy or tunnel arbitrary input/update types. */
    if (packet->type == PROXY_PACKET_MSG && !validate_packet(packet))
        return;

    for (int i = 0; i < ARRAY_SIZE(uart_handler); i++) {
        if (uart_handler[i].type == packet->type) {
            uart_handler[i].handler(packet, state);
            return;
        }
    }
}
