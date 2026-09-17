/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "clipboard_state.h"

typedef struct {
    uint64_t boot_source, boot_target, helper_session, nonce, focus;
} clipboard_request_t;

/* Firmware integration includes main.h before this header. All functions own
 * the firmware RAM lock; callers must not already hold it. */
void clipboard_init(uint64_t boot_session);
void clipboard_task(device_t *);
bool clipboard_next_tx(uart_packet_t *, uint64_t now);
void clipboard_receive(uint8_t type, const uint8_t payload[8], uint64_t now);
void clipboard_cancel(void);
bool clipboard_keyboard_raw(hid_interface_t *, const hid_keyboard_report_t *, device_t *);
void clipboard_keyboard_incomplete(hid_interface_t *, const hid_keyboard_report_t *, device_t *);
void clipboard_physical_disconnect(hid_interface_t *, device_t *);
void clipboard_physical_unknown(hid_interface_t *, device_t *);
void clipboard_mouse_buttons(hid_interface_t *, uint8_t old_buttons, uint8_t new_buttons, device_t *);
void clipboard_remote_input(device_t *);
/* Peer keyboard-state source boot observed after a challenge on either board. */
void clipboard_peer_session(uint64_t boot_session);
void clipboard_host_reset(device_t *);
/* Actual upstream USB sessions invalidate LED knowledge; focus changes do not. */
void clipboard_usb_session_reset(device_t *);
void clipboard_host_led_report(device_t *);
void clipboard_usb_task(device_t *);

bool clipboard_helper_open(uint64_t helper_session, uint64_t now);
void clipboard_helper_close(uint64_t now);
void clipboard_helper_keepalive(uint64_t now);
bool clipboard_helper_active(uint64_t now);
bool clipboard_helper_poll(clipboard_request_t *);
bool clipboard_helper_reply_begin(const clipboard_request_t *, uint8_t status,
                                  uint16_t length, uint32_t crc, uint64_t now);
bool clipboard_helper_reply_chunk(uint16_t offset, const uint8_t *, size_t length, uint64_t now);
bool clipboard_helper_reply_commit(uint64_t now);
