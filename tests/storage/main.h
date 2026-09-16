#pragma once
/* Real firmware types and translation units; only SDK/hardware boundaries differ. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tusb.h"
#include "constants.h"
#include "hid_parser.h"
typedef struct { unsigned used, capacity; uint8_t bytes[256][32]; } queue_t;
#include "structs.h"
#include "config.h"
#include "firmware.h"
#include "firmware_batch.h"
#include "handlers.h"
#include "mouse.h"
#include "tasks.h"
#include "usb_descriptors.h"
#include "user_config.h"
#include "screensaver_policy.h"
#include "zoom.h"
#include "pinout.h"
#include "diagnostic_history.h"
#include "diagnostic_runtime.h"
#define CORE1_HANG_TIMEOUT_US 500000u
#define MAGIC_WORD_1 0xdeadf00fu
#define MAGIC_WORD_2 0x00c0ffeeu
#define DMA_RX_BUFFER_SIZE 1024
#define NEXT_RING_IDX(x) (((x) + 1) & 0x3ff)
extern uint8_t uart_rxbuf[DMA_RX_BUFFER_SIZE];
#define PICO_DEFAULT_LED_PIN 25
#define SCSI_SENSE_ILLEGAL_REQUEST 5
#define PPB_BASE 0u
#define STORAGE_SIZE (2048u * 1024u)
#define STORAGE_CONFIG_OFFSET (STORAGE_SIZE - FLASH_SECTOR_SIZE)
#define STORAGE_DISK_OFFSET (188u * 1024u)
extern uint8_t *storage_flash;
/* Keep host pointers full width on reads. Production offset arithmetic truncates
 * both sides modulo 2^32, reproducing RP2040 relative offsets without fixed mmap. */
#define XIP_BASE ((uint32_t)(uintptr_t)storage_flash)
#define ADDR_FW_RUNNING storage_flash
#define ADDR_FW_STAGING (storage_flash + STAGING_IMAGE_SIZE)
#define ADDR_FW_METADATA (storage_flash + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE)
#define ADDR_CONFIG ((const config_t *)(storage_flash + STORAGE_CONFIG_OFFSET))
#define ADDR_DISK_IMAGE (storage_flash + STORAGE_DISK_OFFSET)
typedef struct { unsigned id; } critical_section_t;
void critical_section_init(critical_section_t *);
void critical_section_enter_blocking(critical_section_t *);
void critical_section_exit(critical_section_t *);
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t);
uint32_t time_us_32(void);
uint64_t time_us_64(void);
void sleep_us(uint64_t);
void tight_loop_contents(void);
void watchdog_update(void);
void reset_usb_boot(uint32_t, uint32_t);
bool queue_try_add(queue_t *, const void *);
bool queue_try_remove(queue_t *, void *);
bool queue_try_peek(queue_t *, void *);
void queue_packet(const uint8_t *, enum packet_type_e, int);
void queue_packet_blocking(const uint8_t *, enum packet_type_e, int);
bool queue_packet_try(const uint8_t *, enum packet_type_e, int);
void send_value(uint8_t, enum packet_type_e);
void process_packet(uart_packet_t *, device_t *);
void tud_task(void);
void tuh_task(void);
bool tuh_inited(void);
bool tud_ready(void);
bool tud_hid_n_ready(uint8_t);
bool tud_hid_n_report(uint8_t, uint8_t, const void *, uint16_t);
void tud_msc_set_sense(uint8_t, uint8_t, uint8_t, uint8_t);
uint32_t calc_crc32(const uint8_t *, size_t);
int32_t tud_msc_write10_cb(uint8_t, uint32_t, uint32_t, uint8_t *, uint32_t);
int32_t tud_msc_read10_cb(uint8_t, uint32_t, uint32_t, void *, uint32_t);
void kick_watchdog_task(device_t *);
typedef unsigned uint;
enum gpio_override { GPIO_OVERRIDE_NORMAL, GPIO_OVERRIDE_LOW };
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB 0
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS 3
struct storage_ioqspi { struct { uint32_t ctrl; } io[2]; };
struct storage_sio { uint32_t gpio_hi_in; };
struct storage_watchdog { uint32_t scratch[8]; };
struct storage_dma { uint32_t transfer_count; };
extern struct storage_ioqspi *ioqspi_hw;
extern struct storage_sio *sio_hw;
extern struct storage_watchdog *watchdog_hw;
void hw_write_masked(uint32_t *, uint32_t, uint32_t);
struct storage_dma *dma_channel_hw_addr(uint32_t);

/* Instrument native memcpy boundaries without replacing production logic. */
void *storage_memcpy(void *, const void *, size_t);
void write_raw_packet(uint8_t *, uart_packet_t *);
#define memcpy storage_memcpy
