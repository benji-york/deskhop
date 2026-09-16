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
#include "handlers.h"
#include "usb_descriptors.h"
#include "user_config.h"
#include "pinout.h"
#include "hid_report.h"
extern device_t global_state;
void config_snapshot(const device_t *, config_t *);
void restore_leds(device_t *);
void send_value(uint8_t, enum packet_type_e);
bool validate_packet(uart_packet_t *);
uint32_t calc_packet_checksum(const uart_packet_t *);
void process_packet(uart_packet_t *, device_t *);
void publish_local_modifiers(device_t *);
void blink_led(device_t *);
void record_local_activity(device_t *, uint8_t);
void queue_packet(const uint8_t *, enum packet_type_e, int);
uint64_t time_us_64(void);
void tight_loop_contents(void);
bool queue_try_add(queue_t *, const void *);
bool queue_try_remove(queue_t *, void *);
bool queue_try_peek(queue_t *, void *);
/* Device task isn't compiled; declarations permit unused keyboard queue code. */
bool tud_suspended(void);
bool tud_remote_wakeup(void);
bool tud_hid_n_ready(uint8_t);
bool tud_hid_keyboard_report(uint8_t, uint8_t, uint8_t *);

void firmware_update_lock(void);
void firmware_update_unlock(void);
