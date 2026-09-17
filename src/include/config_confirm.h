/* Confirmed configuration transactions. Additive to the legacy config API. */
#pragma once

#include "structs.h"

enum config_confirm_operation {
    CONFIG_CONFIRM_CAP = 1,
    CONFIG_CONFIRM_SET = 2,
    CONFIG_CONFIRM_BORDER_PAIR = 3,
    CONFIG_CONFIRM_QUERY_DIGEST = 4,
    CONFIG_CONFIRM_SAVE = 5,
    CONFIG_CONFIRM_CHECK_VALUE = 6,
};

typedef enum {
    CONFIG_CONFIRM_OK = 0,
    CONFIG_CONFIRM_INVALID = 1,
    CONFIG_CONFIRM_BUSY = 2,
    CONFIG_CONFIRM_CONFLICT = 3,
    CONFIG_CONFIRM_FLASH_MISMATCH = 4,
    CONFIG_CONFIRM_EXPIRED = 5,
    CONFIG_CONFIRM_INCOMPLETE = 6,
} config_confirm_status_t;

#define CONFIG_CONFIRM_VERSION 1u
#define CONFIG_CONFIRM_TIMEOUT_US UINT64_C(2000000)

/* META: token32, target-role8, operation8, key8, reserved0.
 * LO/HI: token32, value32. EXEC: token32, zero32. All integers little-endian.
 * ACK_META: token32, physical-role8, operation8, key8, status8.
 * ACK_VALUE: token32, result32. Both ACKs are required for confirmation.
 * One serialized transaction; no automatic replay after an unknown outcome.
 * Exact duplicate EXEC replays the most recent result, never the write.
 * Admission to a transport queue is not delivery or peer execution. */
void config_confirm_init(void);
void config_confirm_shutdown(void);
void config_confirm_usb_disconnect(void);
void config_confirm_task(device_t *);
bool config_confirm_usb_request(const uart_packet_t *, device_t *);
void handle_config_confirm_msg(uart_packet_t *, device_t *);
