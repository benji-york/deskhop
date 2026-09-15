#pragma once

/* Native boundary for the production HID translation units. HID structs,
 * protocol constants, TinyUSB HID declarations, and reboot logic remain real;
 * only hardware-facing state, queues and callbacks are replaced here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tusb.h"
#include "hid_parser.h"
#include "constants.h"
#include "packet.h"
#include "reboot_hotkey.h"
#include "usb_descriptors.h"
#include "user_config.h"

typedef struct { unsigned unused; } queue_t;
typedef struct {
    uint8_t kbd_dev_addr, kbd_instance;
    uint8_t active_output, board_role, max_kbd_idx, peer_modifiers;
    uint8_t keyboard_leds_desired[NUM_SCREENS];
    bool tud_connected, keyboard_connected, mouse_connected;
    bool reboot_requested, config_mode_active;
    bool config_bootloader_peer_pending, config_bootloader_local_pending;
    struct { bool upgrade_in_progress, image_dirty; } fw;
    hid_keyboard_report_t local_kbd_states[MAX_DEVICES], remote_kbd_state;
    reboot_hotkey_sequence_t reboot_hotkey_sequence;
    reboot_hotkey_source_t reboot_hotkey_source[MAX_DEVICES];
    hid_interface_t iface[MAX_DEVICES][MAX_INTERFACES];
    queue_t kbd_queue;
    hid_keyboard_report_t kbd_latest;
    bool kbd_latest_pending;
    uint32_t kbd_host_generation;
    uint32_t kbd_remote_generation;
    struct {
        bool enforce_ports, force_kbd_boot_protocol, force_mouse_boot_mode;
    } config;
} device_t;

typedef struct {
    uint8_t modifier, keys[KEYS_IN_USB_REPORT], key_count;
    void (*action_handler)(device_t *, hid_keyboard_report_t *);
    bool pass_to_os, acknowledge;
} hotkey_combo_t;

#define BOARD_ROLE (global_state.board_role)
extern device_t global_state;

void parse_report_descriptor(hid_interface_t *, const uint8_t *, int);
void extract_data(hid_interface_t *, report_val_t *);
int32_t get_report_value(uint8_t *, int, report_val_t *);
int32_t extract_kbd_data(uint8_t *, int, uint8_t, hid_interface_t *, hid_keyboard_report_t *);
int32_t extract_bit_variable(nkro_block_t *, uint8_t *, int, uint8_t *, int);
keyboard_t *get_keyboard(hid_interface_t *, uint8_t);
void process_mouse_report(uint8_t *, int, uint8_t, hid_interface_t *);
void mouse_interface_removed(hid_interface_t *, device_t *);
void process_keyboard_report(uint8_t *, int, uint8_t, hid_interface_t *);
void process_consumer_report(uint8_t *, int, uint8_t, hid_interface_t *);
void process_system_report(uint8_t *, int, uint8_t, hid_interface_t *);
void combine_kbd_states(device_t *, hid_keyboard_report_t *);
void send_key(hid_keyboard_report_t *, device_t *);
bool queue_kbd_report_critical(hid_keyboard_report_t *, device_t *);
void queue_cc_packet(uint8_t *, device_t *);
void queue_system_packet(uint8_t *, device_t *);
void queue_packet(uint8_t *, uint8_t, uint8_t);
void record_local_activity(device_t *, uint8_t);
void publish_local_modifiers(device_t *);
void restore_leds(device_t *);
void blink_led(device_t *);
void send_value(uint8_t, uint8_t);
bool validate_packet(uart_packet_t *);
uint32_t calc_packet_checksum(const uart_packet_t *);
void process_packet(uart_packet_t *, device_t *);
uint64_t time_us_64(void);
void tight_loop_contents(void);
bool queue_try_peek(queue_t *, void *);
bool queue_try_add(queue_t *, const void *);
bool queue_try_remove(queue_t *, void *);
bool tud_suspended(void);
bool tud_remote_wakeup(void);
bool tud_hid_n_ready(uint8_t);
bool tud_hid_keyboard_report(uint8_t, uint8_t, const uint8_t *);
uint8_t tuh_hid_interface_protocol(uint8_t, uint8_t);
uint8_t tuh_hid_get_protocol(uint8_t, uint8_t);
bool tuh_hid_set_protocol(uint8_t, uint8_t, uint8_t);
bool tuh_hid_receive_report(uint8_t, uint8_t);
void tuh_hid_report_received_cb(uint8_t, uint8_t, const uint8_t *, uint16_t);

#define HID_TEST_HOTKEY_HANDLERS(X) \
    X(output_toggle_hotkey_handler) \
    X(mouse_zoom_hotkey_handler) \
    X(switchlock_hotkey_handler) \
    X(screenlock_hotkey_handler) \
    X(toggle_gaming_mode_handler) \
    X(clear_zoom_assist_hotkey_handler) \
    X(enable_screensaver_pong_hotkey_handler) \
    X(enable_screensaver_jitter_hotkey_handler) \
    X(disable_screensaver_hotkey_handler) \
    X(wipe_config_hotkey_handler) \
    X(screen_border_hotkey_handler) \
    X(config_enable_hotkey_handler) \
    X(fw_upgrade_hotkey_handler_A) \
    X(fw_upgrade_hotkey_handler_B) \
    X(reboot_hotkey_handler)
#define DECLARE_HANDLER(name) void name(device_t *, hid_keyboard_report_t *);
HID_TEST_HOTKEY_HANDLERS(DECLARE_HANDLER)
#undef DECLARE_HANDLER

void keyboard_host_reset(device_t *);
void keyboard_sync_publish(void);
void keyboard_sync_reset(void);
void firmware_update_lock(void);
void firmware_update_unlock(void);
