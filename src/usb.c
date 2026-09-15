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
#include "console.h"
#include "hid_report.h"
#include "diagnostic_history.h"
#include "config_packet.h"

_Static_assert(MAX_DEVICES <= CFG_TUH_DEVICE_MAX,
               "MAX_DEVICES must not exceed CFG_TUH_DEVICE_MAX");

/* ================================================== *
 * ===========  TinyUSB Device Callbacks  =========== *
 * ================================================== */

/* Invoked when we get GET_REPORT control request.
 * We are expected to fill buffer with the report content, update reqlen
 * and return its length. We return 0 to STALL the request. */
uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t request_len) {
    return 0;
}

/**
 * Computer controls our LEDs by sending USB SetReport messages with a payload
 * of just 1 byte and report type output. It's type 0x21 (USB_REQ_DIR_OUT |
 * USB_REQ_TYP_CLASS | USB_REQ_REC_IFACE) Request code for SetReport is 0x09,
 * report type is 0x02 (HID_REPORT_TYPE_OUTPUT). We get a set_report callback
 * from TinyUSB device HID and then figure out what to do with the LEDs.
 */
void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize) {

    /* We received a report on the config report ID */
    if (instance == ITF_NUM_HID_VENDOR && report_id == REPORT_ID_VENDOR) {
        /* Security - only if config mode is enabled are we allowed to do anything. While the report_id
           isn't even advertised when not in config mode, security must always be explicit and never assume */
        if (!global_state.config_mode_active)
            return;

        /* We insist on a fixed size packet. No overflows. */
        if (report_type != HID_REPORT_TYPE_OUTPUT || bufsize != CONFIG_PACKET_LENGTH)
            return;

        uart_packet_t packet;
        if (!read_config_packet(buffer, bufsize, &packet)) {
            diagnostic_history_record(HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, buffer[2]);
            return;
        }

        /* Only a certain packet types are accepted */
        if (!validate_packet(&packet))
            return;

        /* The dispatcher accepts normalized packets only. The USB report's
           integrity was checked before constructing this internal checksum. */
        packet.checksum = calc_packet_checksum(&packet);

        /* The Web Config button sends peer then local requests. USB completion
           does not mean core 0 has drained its UART queue, so latch these two
           validated commands for the TX task instead of resetting here. UART
           reception and keyboard maintenance retain their existing handlers. */
        bool local_bootloader = packet.type == FIRMWARE_UPGRADE_MSG;
        bool peer_bootloader = packet.type == PROXY_PACKET_MSG
            && packet.data[0] == FIRMWARE_UPGRADE_MSG;
        if (local_bootloader || peer_bootloader) {
            firmware_update_lock();
            if (!global_state.fw.upgrade_in_progress && !global_state.fw.image_dirty) {
                if (peer_bootloader)
                    global_state.config_bootloader_peer_pending = true;
                else
                    global_state.config_bootloader_local_pending = true;
            }
            firmware_update_unlock();
            return;
        }

        process_packet(&packet, &global_state);
        return;
    }

    /* Only other set report we care about is LED state change, and that's exactly 1 byte long */
    if (report_id != REPORT_ID_KEYBOARD || bufsize != 1 || report_type != HID_REPORT_TYPE_OUTPUT)
        return;

    uint8_t leds = buffer[0];

    /* Cache the host's unmodified state. The focus indicator is applied when
       the selected output's LEDs are sent to the physical keyboard. */
    global_state.keyboard_leds_desired[BOARD_ROLE] = leds;

    /* If the board has a keyboard connected directly, restore those leds. */
    if (global_state.keyboard_connected && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        restore_leds(&global_state);

    /* Always send to the other one, so it is aware of the change */
    send_value(leds, KBD_SET_REPORT_MSG);
}

/* Invoked when device is mounted */
void tud_mount_cb(void) {
    keyboard_host_reset(&global_state);
    global_state.tud_connected = true;
    diagnostic_history_record(HISTORY_USB_MOUNT, 0, 0, 0);
#if DH_CONSOLE && CFG_TUD_CDC
    /* A USB bus reset need not call unmount, and re-enumeration can finish
     * before the console's next poll. Never retain the old partial command. */
    console_disconnect();
#endif
}

/* Invoked when device is unmounted */
void tud_umount_cb(void) {
    global_state.tud_connected = false;
    keyboard_host_reset(&global_state);
    diagnostic_history_record(HISTORY_USB_UNMOUNT, 0, 0, 0);
#if DH_CONSOLE && CFG_TUD_CDC
    console_disconnect();
#endif
}

#ifdef DH_DEBUG_CDC_FLASH
void tud_cdc_rx_cb(uint8_t itf) {
    char buf[64];
    uint32_t count = tud_cdc_n_available(itf);

    if (count == 0)
        return;

    if (count > sizeof(buf))
        count = sizeof(buf);

    tud_cdc_n_read(itf, buf, count);

    if (count >= 5 && memcmp(buf, "flash", 5) == 0) {
        reset_usb_boot(0, 0);
    }
}
#endif

/* ================================================== *
 * ===============  USB HOST Section  =============== *
 * ================================================== */

static uint8_t get_device_index(uint8_t dev_addr, uint8_t instance, uint8_t itf_protocol) {
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        if (dev_addr == global_state.kbd_dev_addr && instance == global_state.kbd_instance)
            return 0;

        return MAX_DEVICES - 2;
    }

    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE)
        return 1;

    return (dev_addr - 1) % (MAX_DEVICES - 1);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (dev_addr == 0 || dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];
    bool contains_keyboard = itf_protocol == HID_ITF_PROTOCOL_KEYBOARD
                             || iface->num_keyboards > 0;
    bool contains_mouse = itf_protocol == HID_ITF_PROTOCOL_MOUSE
                          || iface->mouse.is_found;

    diagnostic_history_record(HISTORY_HID_UNMOUNT, dev_addr, instance,
                              itf_protocol | (contains_keyboard ? 256u : 0)
                              | (contains_mouse ? 512u : 0));

    if (contains_mouse)
        mouse_interface_removed(iface, &global_state);

    if (contains_keyboard) {
        if (dev_addr == global_state.kbd_dev_addr
            && instance == global_state.kbd_instance)
            global_state.keyboard_connected = false;

        /* Clear only this device. Keeping the other local and remote keyboard
           states avoids spuriously releasing their held keys. */
        uint8_t device_idx = get_device_index(dev_addr, instance, itf_protocol);
        memset(&global_state.local_kbd_states[device_idx], 0,
               sizeof(hid_keyboard_report_t));
        reboot_hotkey_reset(&global_state.reboot_hotkey_sequence);
        memset(global_state.reboot_hotkey_source, 0,
               sizeof(global_state.reboot_hotkey_source));
        publish_local_modifiers(&global_state);

        /* Route the recombined state to whichever output is active. This also
           prevents an unplugged Command key from classifying later scrolls as
           macOS Zoom gestures. */
        hid_keyboard_report_t combined_report;
        combine_kbd_states(&global_state, &combined_report);
        send_key(&combined_report, &global_state);
    }

    /* Also clear the interface structure, otherwise plugging something else later
       might be a fun (and confusing) experience */
    memset(iface, 0, sizeof(hid_interface_t));

    if (contains_mouse) {
        global_state.mouse_connected = false;
        for (uint8_t dev = 0; dev < MAX_DEVICES; dev++) {
            for (uint8_t itf = 0; itf < MAX_INTERFACES; itf++) {
                if (global_state.iface[dev][itf].mouse.is_found) {
                    global_state.mouse_connected = true;
                    return;
                }
            }
        }
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (dev_addr == 0 || dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    /* Get interface information */
    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    iface->protocol = tuh_hid_get_protocol(dev_addr, instance);

    /* Parse the report descriptor into our internal structure. */
    parse_report_descriptor(iface, desc_report, desc_len);

    /* Record enumeration even when enforced-port policy later ignores it.
     * This describes the interface, not a guarantee that reports are accepted. */
    diagnostic_history_record(HISTORY_HID_MOUNT, dev_addr, instance,
                              itf_protocol
                              | ((itf_protocol == HID_ITF_PROTOCOL_KEYBOARD
                                  || iface->num_keyboards > 0) ? 256u : 0)
                              | ((itf_protocol == HID_ITF_PROTOCOL_MOUSE
                                  || iface->mouse.is_found) ? 512u : 0));
    if (iface->descriptor_invalid)
        diagnostic_history_record(HISTORY_DESCRIPTOR_REJECTED, dev_addr, instance, 0);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (global_state.config.enforce_ports && BOARD_ROLE == OUTPUT_B)
                return;

            if (global_state.config.force_kbd_boot_protocol)
                tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

            /* Keeping this is required for setting leds from device set_report callback */
            global_state.kbd_dev_addr       = dev_addr;
            global_state.kbd_instance       = instance;
            global_state.keyboard_connected = true;
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            if (global_state.config.enforce_ports && BOARD_ROLE == OUTPUT_A)
                return;

            if (global_state.config.force_mouse_boot_mode) {
                /* User requested boot mode - simpler protocol for compatibility.
                   Note: many mice still send wheel data even in boot mode. */
                tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
            } else {
                /* Switch to using report protocol instead of boot, it's more complicated but
                   at least we get all the information we need (looking at you, mouse wheel) */
                if (tuh_hid_get_protocol(dev_addr, instance) == HID_PROTOCOL_BOOT) {
                    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);
                }
            }
            global_state.mouse_connected = true;
            break;

        case HID_ITF_PROTOCOL_NONE:
            break;
    }

    /* Also set mouse_connected if report descriptor contains mouse, even if interface
       protocol says keyboard. This handles composite devices like QMK. */
    if (iface->mouse.is_found) {
        global_state.mouse_connected = true;
    }

    /* Flash local led to indicate a device was connected */
    blink_led(&global_state);

    /* Also signal the other board to flash LED, to enable easy verification if serial works */
    send_value(ENABLE, FLASH_LED_MSG);

    /* Kick off the report querying */
    tuh_hid_receive_report(dev_addr, instance);
}

/* Invoked when received report from device via interrupt endpoint */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    if (dev_addr == 0 || dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    /* No report ID or payload is available yet. Keep polling this valid endpoint. */
    if (len == 0) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    /* A rejected descriptor cannot safely select the report-protocol fallback
       layout, even on a boot-capable keyboard interface. Explicitly negotiated
       boot protocol has its own fixed layout and remains available. */
    if (iface->descriptor_invalid && iface->protocol != HID_PROTOCOL_BOOT) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    /* Keep report routing and unmount cleanup on the same device-state slot. */
    uint8_t device_idx = get_device_index(dev_addr, instance, itf_protocol);

    if (iface->uses_report_id || itf_protocol == HID_ITF_PROTOCOL_NONE) {
        uint8_t report_id = 0;

        if (iface->uses_report_id)
            report_id = report[0];

        process_report_f receiver = report_receivers[iface->report_handler[report_id]];

        if (receiver != NULL)
            receiver((uint8_t *)report, len, device_idx, iface);
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        process_keyboard_report((uint8_t *)report, len, device_idx, iface);
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        process_mouse_report((uint8_t *)report, len, device_idx, iface);
    }

    /* Continue requesting reports */
    tuh_hid_receive_report(dev_addr, instance);
}

/* Set protocol in a callback. This is tied to an interface, not a specific report ID */
void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol) {
    if (dev_addr == 0 || dev_addr > MAX_DEVICES || idx >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][idx];
    iface->protocol = protocol;
}
