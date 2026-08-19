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

_Static_assert(sizeof(config_t) <= FLASH_PAGE_SIZE,
               "config_t has grown beyond the configuration flash page");
_Static_assert(sizeof(config_t) == CONFIG_V8_SIZE_BYTES,
               "v8-to-v9 migration requires the persisted config layout to remain unchanged");
_Static_assert(offsetof(config_t, screensaver_system_timeout_sec) == CONFIG_V8_RESERVED_OFFSET,
               "system timeout must occupy the v8 reserved configuration word");

/* Firmware state is shared by the TinyUSB device task on core 0 and the peer
   puller on core 1. Flash commands and XIP data reads also need their own
   cross-core exclusion; disabling interrupts protects only the calling core. */
static critical_section_t firmware_update_critical_section;
static critical_section_t flash_access_critical_section;

void firmware_sync_init(void) {
    critical_section_init(&firmware_update_critical_section);
    critical_section_init(&flash_access_critical_section);
}

void firmware_update_lock(void) {
    critical_section_enter_blocking(&firmware_update_critical_section);
}

void firmware_update_unlock(void) {
    critical_section_exit(&firmware_update_critical_section);
}

/* ================================================== *
 * ==============  Checksum Functions  ============== *
 * ================================================== */

uint8_t calc_checksum(const uint8_t *data, int length) {
    uint8_t checksum = 0;

    for (int i = 0; i < length; i++) {
        checksum ^= data[i];
    }

    return checksum;
}

bool verify_checksum(const uart_packet_t *packet) {
    uint8_t checksum = calc_checksum(packet->data, PACKET_DATA_LENGTH);
    return checksum == packet->checksum;
}

uint32_t crc32_iter(uint32_t crc, const uint8_t byte) {
    return crc32_lookup_table[(byte ^ crc) & 0xff] ^ (crc >> 8);
}

/* TODO - use DMA sniffer's built-in CRC32 */
uint32_t calc_crc32(const uint8_t *s, size_t n) {
    uint32_t crc = 0xffffffff;

    for(size_t i=0; i < n; i++) {
        crc = crc32_iter(crc, s[i]);
    }

    return ~crc;
}

static uint32_t calculate_firmware_crc32_unlocked(void) {
    return calc_crc32(ADDR_FW_RUNNING, STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE);
}

uint32_t calculate_firmware_crc32(void) {
    critical_section_enter_blocking(&flash_access_critical_section);
    uint32_t checksum = calculate_firmware_crc32_unlocked();
    critical_section_exit(&flash_access_critical_section);
    return checksum;
}

bool firmware_image_is_valid(uint16_t expected_version,
                             uint32_t transferred_checksum,
                             bool check_transferred_checksum) {
    firmware_metadata_t metadata;
    uint32_t legacy_checksum;

    critical_section_enter_blocking(&flash_access_critical_section);
    uint32_t calculated_checksum = calculate_firmware_crc32_unlocked();
    memcpy(&metadata, ADDR_FW_METADATA, sizeof(metadata));
    /* Firmware through v0.84 packed checksum at byte 6. Keep direct rollback
       possible while requiring the aligned format for peer propagation. */
    memcpy(&legacy_checksum, ADDR_FW_METADATA + 6, sizeof(legacy_checksum));
    critical_section_exit(&flash_access_critical_section);

    bool metadata_valid = fw_update_metadata_matches(metadata.magic,
                                                     metadata.version,
                                                     metadata.checksum,
                                                     calculated_checksum,
                                                     expected_version,
                                                     expected_version != 0);
    bool legacy_direct_image = !check_transferred_checksum
        && expected_version == 0
        && fw_update_metadata_matches(metadata.magic,
                                      metadata.version,
                                      legacy_checksum,
                                      calculated_checksum,
                                      0,
                                      false);

    return (!check_transferred_checksum || transferred_checksum == calculated_checksum)
        && (metadata_valid || legacy_direct_image);
}

bool read_running_firmware_word(uint32_t address, uint32_t *word) {
    if (address > STAGING_IMAGE_SIZE - sizeof(*word)
        || address % sizeof(*word) != 0)
        return false;

    critical_section_enter_blocking(&flash_access_critical_section);
    memcpy(word, &ADDR_FW_RUNNING[address], sizeof(*word));
    critical_section_exit(&flash_access_critical_section);
    return true;
}

void read_flash_bytes(const uint8_t *source, void *destination, size_t length) {
    critical_section_enter_blocking(&flash_access_critical_section);
    memcpy(destination, source, length);
    critical_section_exit(&flash_access_critical_section);
}

/* Never attempt a normal reboot with a known-partial image. Invalidating its
   first sector makes the ROM USB bootloader the deterministic recovery path. */
void enter_firmware_recovery(void) {
    critical_section_enter_blocking(&flash_access_critical_section);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase((uint32_t)ADDR_FW_RUNNING - XIP_BASE, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    critical_section_exit(&flash_access_critical_section);
    reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
}

/* ================================================== *
 * Flash and config functions
 * ================================================== */

void wipe_config(void) {
    firmware_update_lock();
    if (global_state.fw.upgrade_in_progress) {
        firmware_update_unlock();
        return;
    }

    critical_section_enter_blocking(&flash_access_critical_section);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase((uint32_t)ADDR_CONFIG - XIP_BASE, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    critical_section_exit(&flash_access_critical_section);
    firmware_update_unlock();
}

void write_flash_page_erasing(uint32_t target_addr, uint8_t *buffer, bool erase_sector) {
    critical_section_enter_blocking(&flash_access_critical_section);
    uint32_t ints = save_and_disable_interrupts();
    if (erase_sector)
        flash_range_erase(target_addr & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);

    flash_range_program(target_addr, buffer, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    critical_section_exit(&flash_access_critical_section);
}

void write_flash_page(uint32_t target_addr, uint8_t *buffer) {
    /* Sequential writers encounter the first page of each sector first. */
    write_flash_page_erasing(target_addr, buffer, (target_addr & 0xf00) == 0);
}

void load_config(device_t *state) {
    const config_t *config   = ADDR_CONFIG;
    config_t *running_config = &state->config;

    /* Load the flash config first, including the checksum */
    critical_section_enter_blocking(&flash_access_critical_section);
    memcpy(running_config, config, sizeof(config_t));
    critical_section_exit(&flash_access_critical_section);

    /* Calculate and update checksum, size without checksum */
    uint32_t checksum = calc_crc32((uint8_t *)running_config, offsetof(config_t, checksum));

    /* We expect a certain byte to start the config header */
    bool magic_header_fail = (running_config->magic_header != 0xB00B1E5);

    /* We expect the checksum to match */
    bool checksum_fail = (running_config->checksum != checksum);

    bool config_valid = !magic_header_fail && !checksum_fail;

    /* Version 8 reserved exactly the word now used by the global timeout, so
       migrate it in place without disturbing the user's HID, LED, OS, or
       screensaver configuration. Persist once so subsequent boots read v9. */
    if (config_valid && running_config->version == PREVIOUS_CONFIG_VERSION) {
        running_config->version = CURRENT_CONFIG_VERSION;
        running_config->screensaver_system_timeout_sec = SCREENSAVER_SYSTEM_TIMEOUT_SEC;
        save_config(state);
        return;
    }

    /* On any condition failing, we fall back to default config */
    if (!config_valid || running_config->version != CURRENT_CONFIG_VERSION)
        memcpy(running_config, &default_config, sizeof(config_t));
}

void save_config(device_t *state) {
    firmware_update_lock();
    if (state->fw.upgrade_in_progress) {
        firmware_update_unlock();
        return;
    }

    uint8_t *raw_config = (uint8_t *)&state->config;

    /* Calculate and update checksum, size without checksum */
    uint32_t checksum       = calc_crc32(raw_config, offsetof(config_t, checksum));
    state->config.checksum = checksum;

    /* Copy the config to buffer and pad the rest with zeros */
    memcpy(state->page_buffer, raw_config, sizeof(config_t));
    memset(state->page_buffer + sizeof(config_t), 0, FLASH_PAGE_SIZE - sizeof(config_t));

    /* Write the new config to flash */
    write_flash_page((uint32_t)ADDR_CONFIG - XIP_BASE, state->page_buffer);
    firmware_update_unlock();
}

void reset_config_timer(device_t *state) {
    /* Once this is reached, we leave the config mode */
    state->config_mode_timer = time_us_64() + CONFIG_MODE_TIMEOUT;
}

void _configure_flash_cs(enum gpio_override gpo, uint pin_index) {
  hw_write_masked(&ioqspi_hw->io[pin_index].ctrl,
                  gpo << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
}

bool is_bootsel_pressed(void) {
  const uint CS_PIN_INDEX = 1;
  critical_section_enter_blocking(&flash_access_critical_section);
  uint32_t flags = save_and_disable_interrupts();

  /* Set chip select to high impedance */
  _configure_flash_cs(GPIO_OVERRIDE_LOW, CS_PIN_INDEX);
  sleep_us(20);

  /* Button pressed pulls pin DOWN, so invert */
  bool button_pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

  /* Restore chip select state */
  _configure_flash_cs(GPIO_OVERRIDE_NORMAL, CS_PIN_INDEX);
  restore_interrupts(flags);
  critical_section_exit(&flash_access_critical_section);

  return button_pressed;
}

bool request_byte(device_t *state, uint32_t address) {
    uart_packet_t packet = {
        .data32[0] = address,
        .type = REQUEST_BYTE_MSG,
    };

    if (!queue_try_add(&state->uart_tx_queue, &packet))
        return false;

    state->fw.byte_done = false;
    state->fw.request_pending = true;
    state->fw.requested_at_us = time_us_32();
    return true;
}

void reboot(void) {
    *((volatile uint32_t*)(PPB_BASE + 0x0ED0C)) = 0x5FA0004;
}

bool is_start_of_packet(device_t *state) {
    return (uart_rxbuf[state->dma_ptr] == START1 && uart_rxbuf[NEXT_RING_IDX(state->dma_ptr)] == START2);
}

uint32_t get_ptr_delta(uint32_t current_pointer, device_t *state) {
    uint32_t delta;

    if (current_pointer >= state->dma_ptr)
        delta = current_pointer - state->dma_ptr;
    else
        delta = DMA_RX_BUFFER_SIZE - state->dma_ptr + current_pointer;

    /* Clamp to 12 bits since it can never be bigger */
    delta = delta & 0x3FF;

    return delta;
}

void fetch_packet(device_t *state) {
    uint8_t *dst = (uint8_t *)&state->in_packet;

    for (int i = 0; i < RAW_PACKET_LENGTH; i++) {
        /* Skip the header preamble */
        if (i >= START_LENGTH)
            dst[i - START_LENGTH] = uart_rxbuf[state->dma_ptr];

        state->dma_ptr = NEXT_RING_IDX(state->dma_ptr);
    }
}

/* Validating any input is mandatory. Only packets of these type are allowed
   to be sent to the device over configuration endpoint. */
bool validate_packet(uart_packet_t *packet) {
    const enum packet_type_e ALLOWED_PACKETS[] = {
        FLASH_LED_MSG,
        GET_VAL_MSG,
        GET_ALL_VALS_MSG,
        SET_VAL_MSG,
        WIPE_CONFIG_MSG,
        SAVE_CONFIG_MSG,
        REBOOT_MSG,
        PROXY_PACKET_MSG,
    };
    uint8_t packet_type = packet->type;

    /* Proxied packets are encapsulated in the data field, but same rules apply */
    if (packet->type == PROXY_PACKET_MSG)
        packet_type = packet->data[0];

    for (int i = 0; i < ARRAY_SIZE(ALLOWED_PACKETS); i++) {
        if (ALLOWED_PACKETS[i] == packet_type)
            return true;
    }
    return false;
}


/* ================================================== *
 * Debug functions
 * ================================================== */
#ifdef DH_DEBUG

// Based on: https://github.com/raspberrypi/pico-sdk/blob/a1438dff1d38bd9c65dbd693f0e5db4b9ae91779/src/rp2_common/pico_stdio_usb/stdio_usb.c#L100-L130
static void cdc_write_str(const char *str) {
    int str_len = strlen(str);

    if (!tud_cdc_connected())
        return;

    uint64_t last_write_time = time_us_64();

    for (int bytes_written = 0; bytes_written < str_len;) {
        int bytes_remaining = str_len - bytes_written;
        int available_space = (int)tud_cdc_write_available();
        int chunk_size      = (bytes_remaining < available_space) ? bytes_remaining : available_space;

        if (chunk_size > 0) {
            int written = (int)tud_cdc_write(str + bytes_written, (uint32_t)chunk_size);
            tud_task();
            tud_cdc_write_flush();

            bytes_written += written;
            last_write_time = time_us_64();
        } else {
            tud_task();
            tud_cdc_write_flush();

            /* Timeout after 1ms if buffer stays full or connection lost */
            if (!tud_cdc_connected() || (time_us_64() > last_write_time + 1000))
                break;
        }
    }
}


int dh_debug_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[512];

    int string_len = vsnprintf(buffer, 512, format, args);

    cdc_write_str(buffer);
    tud_cdc_write_flush();

    va_end(args);
    return string_len;
}
#else

int dh_debug_printf(const char *format, ...) {
    return 0;
}

#endif
