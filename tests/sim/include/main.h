#pragma once
/* Only the silicon boundary is substituted. All application types are real. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pico/util/queue.h"
#include "tusb.h"
#include "hid_parser.h"
#include "constants.h"
#include "misc.h"
#include "structs.h"
#if SIM_HAS_FW_BATCH
#include "firmware_batch.h"
#endif
#include "config.h"
#include "usb_descriptors.h"
#include "user_config.h"
#include "protocol.h"
#include "dma.h"
#include "firmware.h"
#include "flash.h"
#include "handlers.h"
#include "keyboard.h"
#include "mouse.h"
#include "packet.h"
#include "pinout.h"
#include "screen.h"
#include "screensaver_policy.h"
#include "serial.h"
#include "tasks.h"
#include "watchdog.h"
#include "zoom.h"
#if SIM_HAS_DIAGNOSTIC_VERIFY
#include "diagnostic_verify.h"
#endif
#if SIM_HAS_DIAGNOSTIC_PEER
#include "diagnostic_peer.h"
#endif
#if SIM_HAS_DIAGNOSTIC_HISTORY
#include "diagnostic_history.h"
#endif
#if SIM_HAS_DIAGNOSTIC_PEER_HISTORY
#include "diagnostic_peer_history.h"
/* Observe attempted enqueues, including full-queue refusal, while delegating
   storage/locking semantics to the real SDK implementation in queue.c. */
bool sim_queue_try_add(queue_t *, const void *);
#define queue_try_add(queue, data) sim_queue_try_add((queue), (data))
#endif

#define PICO_DEFAULT_LED_PIN 25
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB 0
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS 3
#define XIP_BASE ((uint32_t)(uintptr_t)sim_flash)
#define PPB_BASE ((uintptr_t)sim_ppb)
extern uint8_t sim_flash[2*1024*1024], sim_ppb[0x10000];
#define ADDR_FW_RUNNING (sim_flash)
#define ADDR_FW_STAGING (sim_flash + STAGING_IMAGE_SIZE)
#define ADDR_FW_METADATA (sim_flash + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE)
#define ADDR_CONFIG ((const config_t *)(sim_flash + 2044 * 1024))
typedef struct { uint32_t scratch[8]; } sim_watchdog_hw_t;
extern sim_watchdog_hw_t *watchdog_hw;
typedef struct { struct { uint32_t ctrl; } io[6]; } sim_ioqspi_hw_t;
extern sim_ioqspi_hw_t *ioqspi_hw;
typedef struct { uint32_t gpio_hi_in; } sim_sio_hw_t;
extern sim_sio_hw_t *sio_hw;
typedef struct { uint32_t fr; } sim_uart_hw_t;
#define uart0 0
#define UART_UARTFR_BUSY_BITS 0x00000008u
sim_uart_hw_t *uart_get_hw(unsigned);
enum gpio_override { GPIO_OVERRIDE_LOW, GPIO_OVERRIDE_NORMAL };
void hw_write_masked(uint32_t *, uint32_t, uint32_t);
uint64_t time_us_64(void);
uint32_t time_us_32(void);
void tight_loop_contents(void);
void sleep_us(uint64_t);
void sleep_ms(uint32_t);
void watchdog_update(void);
void reset_usb_boot(uint32_t, uint32_t);
void gpio_put(uint32_t, bool);
bool gpio_get(uint32_t);
void pico_get_unique_board_id_string(char *, uint32_t);

uint32_t calc_crc32(const uint8_t *, size_t);
