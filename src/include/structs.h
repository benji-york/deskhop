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
#pragma once

#include <stdint.h>
#include "flash.h"
#include "fw_update.h"
#include "packet.h"
#include "reboot_hotkey.h"
#include "screen.h"
#include "zoom_tracker.h"

typedef void (*action_handler_t)();

typedef struct { // Maps message type -> message handler function
    enum packet_type_e type;
    action_handler_t handler;
} uart_handler_t;

typedef struct {
    uint8_t modifier;                 // Which modifier is pressed
    uint8_t keys[KEYS_IN_USB_REPORT]; // Which keys need to be pressed
    uint8_t key_count;                // How many keys are pressed
    action_handler_t action_handler;  // What to execute when the key combination is detected
    bool pass_to_os;                  // True if we are to pass the key to the OS too
    bool acknowledge;                 // True if we are to notify the user about registering keypress
} hotkey_combo_t;

typedef struct TU_ATTR_PACKED {
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t wheel;
    int8_t pan;
    uint8_t mode;
} mouse_report_t;

typedef struct TU_ATTR_PACKED {
    uint8_t buttons;
    int8_t wheel;
    int8_t pan;
    uint8_t mode;
} mouse_nonmotion_report_t;

typedef struct {
    uint8_t tip_pressure;
    uint8_t buttons; // Digitizer buttons
    uint16_t x;      // X coordinate (0-32767)
    uint16_t y;      // Y coordinate (0-32767)
} touch_report_t;

typedef struct {
    uint8_t instance;
    uint8_t report_id;
    uint8_t type;
    uint8_t len;
    uint8_t data[RAW_PACKET_LENGTH];
} hid_generic_pkt_t;

typedef enum { IDLE, READING_PACKET, PROCESSING_PACKET } receiver_state_t;

typedef struct {
    uint32_t address;         // Address we're sending to the other box
    uint32_t checksum;
    uint32_t peer_checksum;   // Build-time checksum advertised by the source
    uint32_t requested_at_us; // When the outstanding word was last requested
    uint32_t progressed_at_us;// When the last valid word was received
    uint16_t version;
    fw_update_source_t source;// Peer pull and host UF2 drop must never be mixed
    bool image_dirty;         // At least one page of the running image was overwritten
    bool request_pending;     // Only an outstanding request may accept a response
    bool byte_done;           // Has the byte been successfully transferred
    bool upgrade_in_progress; // True if firmware transfer from the other box is in progress
} fw_upgrade_state_t;

typedef struct {
    uint32_t magic_header;
    uint32_t version;

    uint8_t force_mouse_boot_mode;
    uint8_t force_kbd_boot_protocol;

    uint8_t kbd_led_as_indicator;
    uint8_t hotkey_toggle;
    uint8_t enable_acceleration;

    uint8_t enforce_ports;
    uint16_t jump_threshold;

    output_t output[NUM_SCREENS];
    uint32_t screensaver_system_timeout_sec;

    // Keep checksum at the end of the struct
    uint32_t checksum;
} config_t;


/*==============================================================================
 *  Device State
 *==============================================================================*/
typedef struct {
    uint8_t kbd_dev_addr; // Address of the Keyboard device
    uint8_t kbd_instance; // Keyboard instance (d'uh - isn't this a useless comment)

    uint8_t keyboard_leds_desired[NUM_SCREENS];  // Raw host LED state (index 0 = A, index 1 = B)
    uint8_t keyboard_leds_actual[NUM_SCREENS];   // Actual state of keyboard LEDs
    uint64_t last_activity[NUM_SCREENS];   // Activity delivered to each output
    uint64_t direct_activity[NUM_SCREENS]; // Physical input originating on this Pico
    uint64_t peer_activity[NUM_SCREENS];   // Physical input originating on the peer Pico
    uint8_t direct_activity_valid;          // Bitmask for direct_activity
    uint8_t peer_activity_valid;            // Bitmask for peer_activity
    uint32_t core1_last_loop_pass;       // Timestamp of last core1 loop execution
    uint8_t active_output;               // Currently selected output (0 = A, 1 = B)
    uint8_t board_role;                  // Which board are we running on? (0 = A, 1 = B, etc.)

    hid_keyboard_report_t local_kbd_states[MAX_DEVICES]; // Store keyboard states
    hid_keyboard_report_t remote_kbd_state;              // Store combined remote keyboard state
    reboot_hotkey_sequence_t reboot_hotkey_sequence;     // Guarded reboot tap progress
    reboot_hotkey_source_t reboot_hotkey_source[MAX_DEVICES]; // Per-keyboard held-Q state
    uint8_t max_kbd_idx;                                 // Store largest kbd_idx seen
    uint8_t local_modifiers;                             // Modifier state from locally attached keyboards
    uint8_t peer_modifiers;                              // Modifier state mirrored by the other Pico
    uint64_t peer_modifiers_last_seen;                   // Local receipt time of the peer's latest heartbeat

    int16_t pointer_x; // Store and update the location of our mouse pointer
    int16_t pointer_y;
    int16_t mouse_buttons; // Store and update the state of mouse buttons

    config_t config;       // Device configuration, loaded from flash or defaults used
    queue_t hid_queue_out; // Queue that stores outgoing hid messages
    queue_t kbd_queue;     // Queue that stores keyboard reports
    queue_t mouse_queue;   // Queue that stores mouse reports
    queue_t uart_tx_queue; // Queue that stores outgoing packets

    hid_interface_t iface[MAX_DEVICES][MAX_INTERFACES]; // Store info about HID interfaces
    uart_packet_t in_packet;

    /* DMA */
    uint32_t dma_ptr;             // Stores info about DMA ring buffer last checked position
    uint32_t dma_rx_channel;      // DMA RX channel we're using to receive
    uint32_t dma_control_channel; // DMA channel that controls the RX transfer channel
    uint32_t dma_tx_channel;      // DMA TX channel we're using to send

    /* Firmware */
    fw_upgrade_state_t fw;           // State of the firmware upgrader
    firmware_metadata_t _running_fw; // RAM copy of running fw metadata
    uint32_t peer_fw_last_seen_us;    // Last heartbeat, used for stalled-pull recovery
    bool reboot_requested;           // If set, stop updating watchdog
    uint64_t config_mode_timer;      // Counts how long are we to remain in config mode

    uint8_t page_buffer[FLASH_PAGE_SIZE]; // For firmware-over-serial upgrades
    uint32_t uf2_blocks_received[STAGING_PAGES_CNT / 32];
    uint32_t uf2_sectors_erased[STAGING_PAGES_CNT / 16 / 32];
    uint16_t uf2_blocks_received_count;

    /* Connection status flags */
    bool tud_connected;      // True when TinyUSB device successfully connects
    bool keyboard_connected; // True when our keyboard is connected locally
    bool mouse_connected;    // True when our mouse is connected locally

    /* Feature flags */
    bool mouse_zoom;         // True when "mouse zoom" is enabled
    bool switch_lock;        // True when device is prevented from switching
    bool onboard_led_state;  // True when LED is ON
    bool relative_mouse;     // True when relative mouse mode is used
    bool gaming_mode;        // True when gaming mode is on (relative passthru + lock)
    bool config_mode_active; // True when config mode is active
    bool digitizer_active;   // True when digitizer Win/Mac workaround is active

    /* Runtime-only inferred macOS Zoom state, kept independently per output. */
    zoom_tracker_t zoom_assist[NUM_SCREENS];
    bool zoom_activation_pending[NUM_SCREENS]; // Non-owner waits for authoritative state sync

    /* Onboard LED blinky (provide feedback when e.g. mouse connected) */
    int32_t  blinks_left;     // How many blink transitions are left
    uint32_t last_led_change; // Timestamp of the last time led state transitioned
} device_t;
/*==============================================================================*/


typedef struct {
    void (*exec)(device_t *state);
    uint64_t frequency;
    uint64_t next_run;
    bool *enabled;
} task_t;

enum os_type_e {
    LINUX   = 1,
    MACOS   = 2,
    WINDOWS = 3,
    ANDROID = 4,
    OTHER   = 255,
};

enum screen_pos_e {
    NONE   = 0,
    LEFT   = 1,
    RIGHT  = 2,
    MIDDLE = 3,
};

enum screensaver_mode_e {
    DISABLED   = 0,
    PONG       = 1,
    JITTER     = 2,
    MAX_SS_VAL = JITTER,
};

extern const config_t default_config;
extern const config_t ADDR_CONFIG[];
extern const uint8_t ADDR_FW_METADATA[];
extern const uint8_t ADDR_FW_RUNNING[];
extern const uint8_t ADDR_FW_STAGING[];
extern const uint8_t ADDR_DISK_IMAGE[];
