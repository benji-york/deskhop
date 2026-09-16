#pragma once
#include "structs.h"

/* Initialize before launching either core, using this boot's random session. */
void firmware_batch_init(device_t *, uint64_t boot_session);
/* Caller holds firmware_update_lock. No operation blocks on UART capacity. */
void firmware_batch_begin_locked(device_t *);
bool firmware_batch_request_locked(device_t *, uint32_t address);
bool firmware_batch_accepts_word(const device_t *);
/* Protected UART dispatch on core 1. */
void firmware_batch_packet(uart_packet_t *, device_t *);
/* Core 0: called only with idle DMA and empty ordinary queue. Single frame.
 * Uses try-lock and never waits behind core-1 flash operations. */
bool firmware_batch_next_tx(device_t *, uart_packet_t *);
