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
#include "diagnostic_transport.h"
#include "critical_try.h"
#include "diagnostic_history.h"
#include "ram_clear.h"

_Static_assert(sizeof(config_t) <= FLASH_PAGE_SIZE,
               "config_t has grown beyond the configuration flash page");
_Static_assert(sizeof(config_t) == CONFIG_V8_SIZE_BYTES,
               "v8-to-v10 migration requires the persisted config layout to remain unchanged");
_Static_assert(offsetof(config_t, screensaver_system_timeout_sec) == CONFIG_V8_RESERVED_OFFSET,
               "system timeout must occupy the v8 reserved configuration word");

/* Firmware state is shared by the TinyUSB device task on core 0 and the peer
   puller on core 1. Flash commands and XIP data reads also need their own
   cross-core exclusion; disabling interrupts protects only the calling core. */
static critical_section_t firmware_update_critical_section;
static critical_section_t flash_access_critical_section;
static critical_section_t config_critical_section;
/* Protected by flash_access_critical_section; UINT64_MAX permanently expires
   all generations until startup rather than allowing an ABA through wrap. */
static uint64_t firmware_flash_generation;

void firmware_sync_init(void) {
    critical_section_init(&firmware_update_critical_section);
    critical_section_init(&flash_access_critical_section);
    critical_section_init(&config_critical_section);
    firmware_flash_generation = 0;
}

void firmware_update_lock(void) {
    critical_section_enter_blocking(&firmware_update_critical_section);
}

bool firmware_update_try_lock(void) {
    return dh_critical_section_try_enter(&firmware_update_critical_section);
}

void firmware_update_unlock(void) {
    critical_section_exit(&firmware_update_critical_section);
}

/* This lock only covers the small RAM snapshot/mutations. Keep it separate
   from firmware ownership so API SETs never wait through a flash erase. */
void config_lock(void) {
    critical_section_enter_blocking(&config_critical_section);
}

void config_unlock(void) {
    critical_section_exit(&config_critical_section);
}

/* Readers copy under the same lock as publishers, then release it before
 * queues, USB, flash, or other locks. A mutex around writers alone cannot
 * protect a 64-bit timeout or the relationship between two borders. */
void config_snapshot(const device_t *state, config_t *snapshot) {
    config_lock();
    memcpy(snapshot, &state->config, sizeof(*snapshot));
    config_unlock();
}

static size_t config_field_size(const field_map_t *map) {
    return map->type == UINT64 ? sizeof(uint64_t) : map->len;
}

static uint8_t *config_field(config_t *config, const field_map_t *map) {
    return (uint8_t *)config + map->offset - offsetof(device_t, config);
}

static bool config_value_valid(uint8_t index, uint64_t value) {
    if (index >= 40 && index <= 52)
        index -= 30;
    switch (index) {
        case 11: return value >= 1 && value <= CONFIG_SCREEN_COUNT_MAX;
        case 12: case 13: return value >= 1 && value <= CONFIG_SPEED_MAX;
        case 14: case 15: return value <= MAX_SCREEN_COORD;
        case 16: return value == LINUX || value == MACOS || value == WINDOWS
                       || value == ANDROID || value == OTHER;
        case 17: return value == LEFT || value == RIGHT;
        case 18: return value == 0 || value == 1 || value == 3;
        case 19: return value <= MAX_SS_VAL;
        case 20: case 71: case 72: case 73: case 75: case 76: return value <= 1;
        /* All persisted uint64 durations are meaningful. The legacy API can
         * only SET 48 bits, but loading or editing a different field must not
         * silently replace a larger existing duration. */
        case 21: case 22: return true;
        /* The persisted hotkey byte is currently dormant: the hotkey table
         * uses HOTKEY_TOGGLE at compile time. Preserve every legacy byte. */
        case 74: return value <= UINT8_MAX;
        case 77: return value <= UINT16_MAX;
        case 83: return value <= UINT32_MAX;
        default: return false;
    }
}

static bool config_border_valid(const border_size_t *border) {
    return border->top >= 0 && border->top < border->bottom
        && border->bottom <= MAX_SCREEN_COORD;
}

bool config_validate(const config_t *config) {
    if (config->magic_header != default_config.magic_header
        || config->version != CURRENT_CONFIG_VERSION)
        return false;
    for (size_t i = 0; i < get_field_map_length(); ++i) {
        const field_map_t *map = get_field_map_index(i);
        if (map->readonly)
            continue;
        uint64_t value = 0;
        memcpy(&value, (const uint8_t *)config + map->offset - offsetof(device_t, config),
               config_field_size(map));
        if (!config_value_valid(map->idx, value))
            return false;
    }
    for (unsigned i = 0; i < NUM_SCREENS; ++i) {
        const output_t *out = &config->output[i];
        if (out->number != i || !config_border_valid(&out->border)
            || out->screen_index < 1 || out->screen_index > out->screen_count)
            return false;
    }
    return true;
}

/* After integrity/version checks and migration, salvage independent settings.
 * Invalid scalar fields use their own defaults; an invalid border relationship
 * resets only that pair. A reduced monitor count clamps the runtime index. */
bool config_repair(config_t *config) {
    bool repaired = false;
    for (size_t i = 0; i < get_field_map_length(); ++i) {
        const field_map_t *map = get_field_map_index(i);
        if (map->readonly)
            continue;
        size_t size = config_field_size(map);
        uint64_t value = 0;
        memcpy(&value, config_field(config, map), size);
        if (!config_value_valid(map->idx, value)) {
            memcpy(config_field(config, map),
                   (const uint8_t *)&default_config + map->offset - offsetof(device_t, config), size);
            repaired = true;
        }
    }
    for (unsigned i = 0; i < NUM_SCREENS; ++i) {
        output_t *out = &config->output[i];
        if (out->number != i) {
            out->number = i;
            repaired = true;
        }
        if (!config_border_valid(&out->border)) {
            out->border = default_config.output[i].border;
            repaired = true;
        }
        uint32_t index = out->screen_index;
        if (index < 1) index = 1;
        if (index > out->screen_count) index = out->screen_count;
        if (out->screen_index != index) {
            out->screen_index = index;
            repaired = true;
        }
    }
    return repaired;
}

bool config_set_value(device_t *state, uint8_t index, const uint8_t value[7]) {
    const field_map_t *map = get_field_map_entry(index);
    if (!map || map->readonly)
        return false;
    /* All unused bytes must be zero, including high bits callers could have
     * accidentally truncated to a narrow integer. Never silently discard. */
    for (unsigned i = map->len; i < 7; ++i)
        if (value[i]) return false;
    uint64_t decoded = 0;
    for (unsigned i = 0; i < map->len; ++i)
        decoded |= (uint64_t)value[i] << (8 * i);
    return config_set_value64(state, index, decoded);
}

bool config_set_value64(device_t *state, uint8_t index, uint64_t decoded) {
    const field_map_t *map = get_field_map_entry(index);
    if (!map || map->readonly)
        return false;
    if (map->type == UINT64 && decoded > CONFIG_TIMEOUT_MAX_US)
        return false;
    if (!config_value_valid(index, decoded))
        return false;
    config_lock();
    config_t candidate = state->config;
    /* UINT64 writes replace all eight storage bytes, never stale upper bits. */
    memcpy(config_field(&candidate, map), &decoded, config_field_size(map));
    if (index == 11 || index == 41) {
        output_t *out = &candidate.output[index == 41];
        if (out->screen_index > out->screen_count)
            out->screen_index = out->screen_count;
    }
    bool valid = config_validate(&candidate);
    if (valid) memcpy(&state->config, &candidate, sizeof(candidate));
    config_unlock();
    return valid;
}

bool config_set_border(device_t *state, uint8_t output, const border_size_t *border) {
    if (output >= NUM_SCREENS || !config_border_valid(border))
        return false;
    config_lock();
    config_t candidate = state->config;
    candidate.output[output].border = *border;
    bool valid = config_validate(&candidate);
    if (valid) memcpy(&state->config, &candidate, sizeof(candidate));
    config_unlock();
    return valid;
}

bool config_set_screensaver_mode(device_t *state, uint8_t output, uint8_t mode) {
    if (output >= NUM_SCREENS) return false;
    const uint8_t value[7] = {mode};
    return config_set_value(state, output == OUTPUT_A ? 19 : 49, value);
}

void config_set_screen_index(device_t *state, uint8_t output, uint32_t index) {
    if (output >= NUM_SCREENS) return;
    config_lock();
    /* The count may have shrunk since the mouse consumer's snapshot. */
    uint32_t count = state->config.output[output].screen_count;
    if (count < 1 || count > CONFIG_SCREEN_COUNT_MAX) {
        config_unlock();
        return;
    }
    if (index < 1) index = 1;
    if (index > count) index = count;
    state->config.output[output].screen_index = index;
    config_unlock();
}

/* ================================================== *
 * ==============  Checksum Functions  ============== *
 * ================================================== */

uint32_t calc_packet_checksum(const uart_packet_t *packet) {
    uint8_t body[11] = {UART_FRAME_VERSION, PACKET_DATA_LENGTH, packet->type};
    memcpy(body + 3, packet->data, PACKET_DATA_LENGTH);
    uint32_t crc = calc_crc32(body, sizeof(body));
    ram_clear(body, sizeof(body));
    return crc;
}

bool verify_checksum(const uart_packet_t *packet) {
    return calc_packet_checksum(packet) == packet->checksum;
}

bool read_raw_packet(const uint8_t *raw, uart_packet_t *packet) {
    uint8_t body[UART_FRAME_BODY_LENGTH];
    bool valid = false;
    if (raw[0] != UART_FRAME_START || raw[RAW_PACKET_LENGTH - 1] != UART_FRAME_END)
        goto done;
    for (unsigned i = 0; i < sizeof(body); ++i) {
        uint8_t hi = raw[1 + 2 * i], lo = raw[2 + 2 * i];
        if ((hi & 0xf0) != 0x40 || (lo & 0xf0) != 0x40)
            goto done;
        body[i] = ((hi & 0xf) << 4) | (lo & 0xf);
    }
    if (body[0] != UART_FRAME_VERSION || body[1] != PACKET_DATA_LENGTH)
        goto done;
    uint32_t crc = 0;
    for (unsigned i = 0; i < 4; ++i)
        crc |= (uint32_t)body[11 + i] << (8 * i);
    if (calc_crc32(body, 11) != crc)
        goto done;
    packet->type = body[2];
    memcpy(packet->data, body + 3, PACKET_DATA_LENGTH);
    packet->checksum = crc;
    valid = true;
done:
    ram_clear(body, sizeof(body));
    return valid;
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

/* Layout-independent checksum of user settings, not of C padding or the
 * currently selected monitor. Version, then writable API fields in API order:
 * each entry is id8, storage-width8, little-endian value (UINT64 really is 8).
 * CRC32 is an accidental-change check, not an authentication mechanism. */
static uint32_t config_digest_snapshot(const config_t *config) {
    uint32_t crc = UINT32_MAX;
    crc = crc32_iter(crc, 70);
    crc = crc32_iter(crc, 4);
    for (unsigned b = 0; b < 4; ++b)
        crc = crc32_iter(crc, (uint8_t)(config->version >> (8 * b)));
    for (size_t i = 0; i < get_field_map_length(); ++i) {
        const field_map_t *map = get_field_map_index(i);
        if (map->readonly) continue;
        size_t size = config_field_size(map);
        uint64_t value = 0;
        memcpy(&value, (const uint8_t *)config + map->offset - offsetof(device_t, config), size);
        crc = crc32_iter(crc, (uint8_t)map->idx);
        crc = crc32_iter(crc, (uint8_t)size);
        for (unsigned b = 0; b < size; ++b)
            crc = crc32_iter(crc, (uint8_t)(value >> (8 * b)));
    }
    return ~crc;
}

uint32_t config_digest(const device_t *state) {
    config_t snapshot;
    config_snapshot(state, &snapshot);
    return config_digest_snapshot(&snapshot);
}

/* Read-only expected-value proof. Compare the full storage width, including
 * all eight timer bytes, and bind the result to that same coherent snapshot.
 * A series of checks with one digest establishes all intended field values
 * even when a peer rebooted between earlier independent SET operations. */
config_confirm_status_t config_check_value64(const device_t *state, uint8_t index,
                                             uint64_t expected, uint32_t *digest) {
    if (!digest) return CONFIG_CONFIRM_INVALID;
    *digest = 0;
    const field_map_t *map = get_field_map_entry(index);
    if (!map || map->readonly || !config_value_valid(index, expected)
        || (map->type == UINT64 && expected > CONFIG_TIMEOUT_MAX_US))
        return CONFIG_CONFIRM_INVALID;
    config_t snapshot;
    config_snapshot(state, &snapshot);
    uint64_t actual = 0;
    memcpy(&actual, config_field(&snapshot, map), config_field_size(map));
    *digest = config_digest_snapshot(&snapshot);
    return actual == expected ? CONFIG_CONFIRM_OK : CONFIG_CONFIRM_CONFLICT;
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

static void firmware_verify_unlock(void) {
    critical_section_exit(&flash_access_critical_section);
    firmware_update_unlock();
}

/* On success both locks remain held for one bounded copy. Never wait behind
   flash erase or the updater's longer ownership scope on the other core. */
static firmware_verify_io_t firmware_verify_try_lock(uint64_t generation, bool compare) {
    if (!dh_critical_section_try_enter(&firmware_update_critical_section))
        return FIRMWARE_VERIFY_BUSY;
    if (global_state.fw.upgrade_in_progress || global_state.fw.image_dirty
        || global_state.reboot_requested) {
        firmware_update_unlock();
        return FIRMWARE_VERIFY_UPDATE_ACTIVE;
    }
    if (!dh_critical_section_try_enter(&flash_access_critical_section)) {
        firmware_update_unlock();
        return FIRMWARE_VERIFY_BUSY;
    }
    if (firmware_flash_generation == UINT64_MAX
        || (compare && generation != firmware_flash_generation)) {
        firmware_verify_unlock();
        return FIRMWARE_VERIFY_CHANGED;
    }
    return FIRMWARE_VERIFY_OK;
}

firmware_verify_io_t firmware_verify_try_start(uint64_t *generation,
                                               firmware_metadata_t *metadata) {
    if (!generation || !metadata)
        return FIRMWARE_VERIFY_BAD_ARGUMENT;
    firmware_verify_io_t result = firmware_verify_try_lock(0, false);
    if (result != FIRMWARE_VERIFY_OK)
        return result;
    memcpy(metadata, ADDR_FW_METADATA, sizeof(*metadata));
    *generation = firmware_flash_generation;
    firmware_verify_unlock();
    return FIRMWARE_VERIFY_OK;
}

firmware_verify_io_t firmware_verify_try_read(uint64_t generation, uint32_t offset,
                                              uint8_t *destination, size_t length) {
    if (!destination || !length || length > FLASH_PAGE_SIZE
        || offset > STAGING_IMAGE_SIZE - length)
        return FIRMWARE_VERIFY_BAD_ARGUMENT;
    firmware_verify_io_t result = firmware_verify_try_lock(generation, true);
    if (result != FIRMWARE_VERIFY_OK)
        return result;
    memcpy(destination, ADDR_FW_RUNNING + offset, length);
    firmware_verify_unlock();
    return FIRMWARE_VERIFY_OK;
}

firmware_verify_io_t firmware_verify_try_finish(uint64_t generation,
                                                firmware_metadata_t *metadata) {
    if (!metadata)
        return FIRMWARE_VERIFY_BAD_ARGUMENT;
    firmware_verify_io_t result = firmware_verify_try_lock(generation, true);
    if (result != FIRMWARE_VERIFY_OK)
        return result;
    memcpy(metadata, ADDR_FW_METADATA, sizeof(*metadata));
    firmware_verify_unlock();
    return FIRMWARE_VERIFY_OK;
}

/* Call under the flash lock, before the first byte may change. Include the
   metadata sector as well as executable/disk data; exclude saved settings. */
static void firmware_verify_invalidate_range(uint32_t offset, uint32_t length) {
    uint32_t slot_start = (uint32_t)(uintptr_t)ADDR_FW_RUNNING - XIP_BASE;
    if (length && (uint64_t)offset < (uint64_t)slot_start + STAGING_IMAGE_SIZE
        && (uint64_t)offset + length > slot_start
        && firmware_flash_generation != UINT64_MAX)
        ++firmware_flash_generation;
}

/* Never attempt a normal reboot with a known-partial image. Invalidating its
   first sector makes the ROM USB bootloader the deterministic recovery path. */
void enter_firmware_recovery(void) {
    critical_section_enter_blocking(&flash_access_critical_section);
    uint32_t ints = save_and_disable_interrupts();
    firmware_verify_invalidate_range((uint32_t)ADDR_FW_RUNNING - XIP_BASE, FLASH_SECTOR_SIZE);
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
    if (erase_sector) {
        firmware_verify_invalidate_range(target_addr & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
        flash_range_erase(target_addr & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
    }

    firmware_verify_invalidate_range(target_addr, FLASH_PAGE_SIZE);
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
    config_t loaded_config;
    config_t *running_config = &loaded_config;

    /* Validate/migrate privately; publish one complete configuration below. */
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

    /* Version 8 reserved exactly the word now used by the global timeout.
       Versions 8 and 9 share the current on-flash layout, so migrate them in
       place without disturbing HID, LED, OS, calibration, or timeout values.

       Version 10 makes Jitter the startup mode on both outputs. This one-time
       migration enables the requested keep-awake behavior for existing
       installations; Web Config can subsequently disable it by saving both
       output modes as Disabled. */
    bool migrated = config_valid
        && migrate_config_to_current(
            running_config->version,
            &running_config->version,
            &running_config->screensaver_system_timeout_sec,
            &running_config->output[OUTPUT_A].screensaver.mode,
            &running_config->output[OUTPUT_B].screensaver.mode,
            SCREENSAVER_SYSTEM_TIMEOUT_SEC,
            JITTER);

    /* On any condition failing, we fall back to default config */
    if (!config_valid || running_config->version != CURRENT_CONFIG_VERSION)
        memcpy(running_config, &default_config, sizeof(config_t));

    /* Current-format repairs remain in RAM until an explicit SAVE. Migration
     * retains its existing one-time persistence, after semantic repair. */
    config_repair(running_config);

    config_lock();
    memcpy(&state->config, running_config, sizeof(config_t));
    config_unlock();
    if (migrated)
        save_config(state);
}

/* Called by the deferred core-0 command task, never from USB callbacks.
 * Retain firmware ownership through readback, but do not hold the config lock
 * across flash: a concurrent edit survives and changes the outcome to CONFLICT.
 * This confirms persistence, not atomicity across boards or power-loss safety. */
config_confirm_status_t config_save_confirmed(device_t *state, uint32_t expected,
                                               uint32_t *actual) {
    if (!actual) return CONFIG_CONFIRM_INVALID;
    *actual = 0;
    if (!firmware_update_try_lock()) return CONFIG_CONFIRM_BUSY;
    if (state->fw.upgrade_in_progress || state->fw.image_dirty || state->reboot_requested
        || state->maintenance_reserved || state->config_bootloader_local_pending
        || state->config_bootloader_peer_pending) {
        firmware_update_unlock();
        return CONFIG_CONFIRM_BUSY;
    }
    config_t snapshot;
    config_snapshot(state, &snapshot);
    *actual = config_digest_snapshot(&snapshot);
    if (!config_validate(&snapshot) || *actual != expected) {
        firmware_update_unlock();
        return *actual != expected ? CONFIG_CONFIRM_CONFLICT : CONFIG_CONFIRM_INVALID;
    }
    snapshot.checksum = calc_crc32((const uint8_t *)&snapshot, offsetof(config_t, checksum));
    memset(state->page_buffer, 0, FLASH_PAGE_SIZE);
    memcpy(state->page_buffer, &snapshot, sizeof(snapshot));

    critical_section_enter_blocking(&flash_access_critical_section);
    bool identical = memcmp(ADDR_CONFIG, state->page_buffer, FLASH_PAGE_SIZE) == 0;
    critical_section_exit(&flash_access_critical_section);
    if (!identical)
        write_flash_page((uint32_t)ADDR_CONFIG - XIP_BASE, state->page_buffer);
    critical_section_enter_blocking(&flash_access_critical_section);
    bool verified = memcmp(ADDR_CONFIG, state->page_buffer, FLASH_PAGE_SIZE) == 0;
    critical_section_exit(&flash_access_critical_section);

    config_lock();
    *actual = config_digest_snapshot(&state->config);
    bool unchanged = *actual == expected;
    if (verified && unchanged) state->config.checksum = snapshot.checksum;
    config_unlock();
    firmware_update_unlock();
    if (!verified) return CONFIG_CONFIRM_FLASH_MISMATCH;
    return unchanged ? CONFIG_CONFIRM_OK : CONFIG_CONFIRM_CONFLICT;
}

void save_config(device_t *state) {
    firmware_update_lock();
    if (state->fw.upgrade_in_progress) {
        firmware_update_unlock();
        return;
    }

    /* Snapshot and checksum the same bytes. SET_VAL on the other core may
       update live config after this short section, but cannot tear this save. */
    config_lock();
    if (!config_validate(&state->config)) {
        config_unlock();
        firmware_update_unlock();
        return;
    }
    memcpy(state->page_buffer, &state->config, sizeof(config_t));
    uint32_t checksum = calc_crc32(state->page_buffer, offsetof(config_t, checksum));
    memcpy(state->page_buffer + offsetof(config_t, checksum), &checksum, sizeof(checksum));
    state->config.checksum = checksum;
    config_unlock();

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
    return uart_rxbuf[state->dma_ptr] == UART_FRAME_START;
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

bool fetch_packet(device_t *state) {
    uint8_t raw[RAW_PACKET_LENGTH];
    uint32_t cursor = state->dma_ptr;
    for (unsigned i = 0; i < sizeof(raw); ++i) {
        raw[i] = uart_rxbuf[cursor];
        cursor = NEXT_RING_IDX(cursor);
    }
    bool valid = read_raw_packet(raw, &state->in_packet);
    ram_clear(raw, sizeof(raw));
    if (!valid) {
        diagnostic_history_record(HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, 0);
        return false;
    }
    state->dma_ptr = cursor;
    return true;
}

volatile uint32_t diagnostic_rx_cleanup_phase;

/* Core 1 owns parsing; the DMA owns the producer side. No live DMA destination
 * may be erased. Disable the reload channel first, then abort both channels
 * using the SDK's BUSY-drain barrier (including already issued bus writes).
 * Active normal frames incur no pause: callers erase clipboard frames,
 * discarded corrupt/partial bytes, and stable idle ring slices. The pause
 * contains at most 63 byte stores and the SDK's DMA BUSY-drain barriers;
 * it performs no parsing, allocation, lock acquisition or dispatch.
 *
 * The reload channel writes 1024 to TRANS_COUNT_TRIG, rather than resetting the
 * write address. Therefore restarting this exact remainder does not change the
 * next full-ring reload count. No completion IRQ is installed for these two
 * channels; RP2040-E13's spurious completion IRQ cannot run a handler.
 */
void uart_rx_erase_consumed(device_t *state, uint32_t start, uint32_t length) {
    if (!length || length >= 2 * RAW_PACKET_LENGTH || start >= DMA_RX_BUFFER_SIZE) return;
    uint32_t interrupts = save_and_disable_interrupts();
    dma_channel_config control = dma_get_channel_config(state->dma_control_channel);
    dma_channel_config paused = control;
    channel_config_set_enable(&paused, false);
    dma_channel_set_config(state->dma_control_channel, &paused, false);
    diagnostic_rx_cleanup_phase = DIAGNOSTIC_RX_STOP_CONTROL;
    dma_channel_abort(state->dma_control_channel);
    diagnostic_rx_cleanup_phase = DIAGNOSTIC_RX_STOP_DATA;
    dma_channel_abort(state->dma_rx_channel);
    diagnostic_rx_cleanup_phase = DIAGNOSTIC_RX_CLEAR;

    /* CHAN_ABORT clears TRANS_COUNT (RP2040 datasheet 2.5.5.3). After the
     * SDK's BUSY-drain barrier, WRITE_ADDR is stable with no in-flight address
     * adjustment (E12). Recover the remainder from that actual ring position;
     * reading the cleared count would falsely rewind the logical producer. */
    uint32_t writer = ((uintptr_t)dma_channel_hw_addr(state->dma_rx_channel)->write_addr
                       - (uintptr_t)uart_rxbuf) & (DMA_RX_BUFFER_SIZE - 1u);
    uint32_t remaining = DMA_RX_BUFFER_SIZE - writer;
    uint32_t unread = get_ptr_delta(writer, state);
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t index = (start + i) & (DMA_RX_BUFFER_SIZE - 1u);
        uint32_t distance = (index - state->dma_ptr) & (DMA_RX_BUFFER_SIZE - 1u);
        /* A producer wrap after the parser snapshot may have replaced an old
         * consumed slot with new unread input. That new input retains ownership. */
        if (distance >= unread) *(volatile uint8_t *)&uart_rxbuf[index] = 0;
    }
    diagnostic_rx_cleanup_phase = DIAGNOSTIC_RX_RESTART;
    dma_channel_set_trans_count(state->dma_control_channel, 1, false);
    dma_channel_set_config(state->dma_control_channel, &control, false);
    dma_channel_set_trans_count(state->dma_rx_channel,
                               remaining, true);
    diagnostic_rx_cleanup_phase = DIAGNOSTIC_RX_IDLE;
    restore_interrupts(interrupts);
}

/* Validating any input is mandatory. Only packets of these type are allowed
   to be sent to the device over configuration endpoint. */
bool validate_packet(uart_packet_t *packet) {
    /* Multipart confirmed commands must never use the lossy legacy proxy
     * envelope, and ACK packets are never accepted from WebHID. */
    if (packet->type >= CONFIG_CONFIRM_META_MSG && packet->type <= CONFIG_CONFIRM_EXEC_MSG)
        return true;
    const enum packet_type_e ALLOWED_PACKETS[] = {
        FLASH_LED_MSG,
        GET_VAL_MSG,
        GET_ALL_VALS_MSG,
        SET_VAL_MSG,
        WIPE_CONFIG_MSG,
        SAVE_CONFIG_MSG,
        REBOOT_MSG,
        FIRMWARE_UPGRADE_MSG,
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
#if defined(DH_DEBUG) && !DH_CONSOLE

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
