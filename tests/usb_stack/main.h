#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "tusb.h"
#include "constants.h"
#include "hid_parser.h"
typedef struct { unsigned unused; } queue_t;
#include "structs.h"
#include "keyboard.h"
#include "mouse.h"
#include "usb_descriptors.h"
#include "user_config.h"
#include "pinout.h"
#include "hid_report.h"
#include "console.h"
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
void pico_get_unique_board_id_string(char *, unsigned);
extern device_t global_state;
void restore_leds(device_t *);
void send_value(uint8_t, enum packet_type_e);
bool validate_packet(uart_packet_t *);
void process_packet(uart_packet_t *, device_t *);
void publish_local_modifiers(device_t *);
void blink_led(device_t *);
uint8_t tuh_hid_interface_protocol(uint8_t, uint8_t);
uint8_t tuh_hid_get_protocol(uint8_t, uint8_t);
bool tuh_hid_set_protocol(uint8_t, uint8_t, uint8_t);
bool tuh_hid_receive_report(uint8_t, uint8_t);
