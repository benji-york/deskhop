/* Included after the native NOR/config fixtures in test_storage.c. All tested
   operations below execute the production config helpers. Reproduce with:
       python3 tests/storage/run.py --seeds 3
   Protocol transport/retry/session coverage belongs to the paired simulator. */

/* Deliberately independent of api_field_map: a production mapping omission or
   wrong offset must not move the fixture and its oracle together. */
#define CONFIRMED_FIELD(ID, MEMBER) \
    {ID, sizeof(((config_t *)0)->MEMBER), offsetof(config_t, MEMBER)}
static const struct {
    uint8_t id, width;
    size_t offset;
} confirmed_digest_fields[] = {
    CONFIRMED_FIELD(70, version),
    CONFIRMED_FIELD(11, output[0].screen_count),
    CONFIRMED_FIELD(12, output[0].speed_x),
    CONFIRMED_FIELD(13, output[0].speed_y),
    CONFIRMED_FIELD(14, output[0].border.top),
    CONFIRMED_FIELD(15, output[0].border.bottom),
    CONFIRMED_FIELD(16, output[0].os),
    CONFIRMED_FIELD(17, output[0].pos),
    CONFIRMED_FIELD(18, output[0].mouse_park_pos),
    CONFIRMED_FIELD(19, output[0].screensaver.mode),
    CONFIRMED_FIELD(20, output[0].screensaver.only_if_inactive),
    CONFIRMED_FIELD(21, output[0].screensaver.idle_time_us),
    CONFIRMED_FIELD(22, output[0].screensaver.max_time_us),
    CONFIRMED_FIELD(41, output[1].screen_count),
    CONFIRMED_FIELD(42, output[1].speed_x),
    CONFIRMED_FIELD(43, output[1].speed_y),
    CONFIRMED_FIELD(44, output[1].border.top),
    CONFIRMED_FIELD(45, output[1].border.bottom),
    CONFIRMED_FIELD(46, output[1].os),
    CONFIRMED_FIELD(47, output[1].pos),
    CONFIRMED_FIELD(48, output[1].mouse_park_pos),
    CONFIRMED_FIELD(49, output[1].screensaver.mode),
    CONFIRMED_FIELD(50, output[1].screensaver.only_if_inactive),
    CONFIRMED_FIELD(51, output[1].screensaver.idle_time_us),
    CONFIRMED_FIELD(52, output[1].screensaver.max_time_us),
    CONFIRMED_FIELD(71, force_mouse_boot_mode),
    CONFIRMED_FIELD(72, force_kbd_boot_protocol),
    CONFIRMED_FIELD(73, kbd_led_as_indicator),
    CONFIRMED_FIELD(74, hotkey_toggle),
    CONFIRMED_FIELD(75, enable_acceleration),
    CONFIRMED_FIELD(76, enforce_ports),
    CONFIRMED_FIELD(77, jump_threshold),
    CONFIRMED_FIELD(83, screensaver_system_timeout_sec),
};
#undef CONFIRMED_FIELD

static uint32_t confirmed_digest_oracle(const config_t *config) {
    uint8_t bytes[256];
    size_t used = 0;
    for (size_t i = 0; i < ARRAY_SIZE(confirmed_digest_fields); ++i) {
        unsigned width = confirmed_digest_fields[i].width;
        CHECK(used + 2 + width <= sizeof(bytes));
        bytes[used++] = confirmed_digest_fields[i].id;
        bytes[used++] = (uint8_t)width;
        uint64_t value = 0;
        memcpy(&value, (const uint8_t *)config + confirmed_digest_fields[i].offset, width);
        for (unsigned byte = 0; byte < width; ++byte)
            bytes[used++] = (uint8_t)(value >> (8 * byte));
    }
    return oracle_crc32(bytes, used);
}

static void confirmed_digest_covers_settings_only(void) {
    fresh("confirmed_digest_independent_oracle_includes_all_field_bytes_not_padding");
    global_state.config = customized_config();
    uint32_t expected = confirmed_digest_oracle(&global_state.config);
    CHECK(config_digest(&global_state) == expected);
    bool included[sizeof(config_t)] = {false};
    uint8_t *config_bytes = (uint8_t *)&global_state.config;
    for (size_t i = 0; i < ARRAY_SIZE(confirmed_digest_fields); ++i) {
        for (unsigned byte = 0; byte < confirmed_digest_fields[i].width; ++byte) {
            size_t offset = confirmed_digest_fields[i].offset + byte;
            included[offset] = true;
            config_bytes[offset] ^= 0x80;
            CHECK(config_digest(&global_state) == confirmed_digest_oracle(&global_state.config));
            CHECK(config_digest(&global_state) != expected);
            config_bytes[offset] ^= 0x80;
        }
    }
    /* Includes C padding, checksum, magic, output identities and monitor index.
       Save separately validates invariants; these are not editable settings. */
    for (size_t byte = 0; byte < sizeof(config_t); ++byte) {
        if (included[byte]) continue;
        config_bytes[byte] ^= 0xa5;
        CHECK(config_digest(&global_state) == expected);
        config_bytes[byte] ^= 0xa5;
    }
    global_state.config.output[0].screen_index = 1;
    CHECK(config_validate(&global_state.config));
    CHECK(config_digest(&global_state) == expected);
    uint32_t actual;
    CHECK(config_save_confirmed(&global_state, expected, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == expected);
}

static void confirmed_save_rejects_conflicts(void) {
    fresh("confirmed_save_crc_conflict_does_not_erase");
    config_t previously_saved = customized_config();
    install_config(&previously_saved);
    uint8_t previous_sector[FLASH_SECTOR_SIZE];
    memcpy(previous_sector, ADDR_CONFIG, sizeof(previous_sector));
    config_t before = global_state.config;
    uint32_t digest = config_digest(&global_state), actual = ~digest;
    CHECK(config_save_confirmed(&global_state, digest ^ 1u, &actual) == CONFIG_CONFIRM_CONFLICT);
    CHECK(actual == digest && !erases && !programs);
    CHECK(memcmp(&global_state.config, &before, sizeof(before)) == 0);
    CHECK(memcmp(ADDR_CONFIG, previous_sector, sizeof(previous_sector)) == 0);

    fresh("confirmed_save_invalid_ram_does_not_erase");
    install_config(&previously_saved);
    memcpy(previous_sector, ADDR_CONFIG, sizeof(previous_sector));
    global_state.config.output[0].border = (border_size_t){200, 100};
    before = global_state.config;
    digest = config_digest(&global_state);
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_INVALID);
    CHECK(!erases && !programs);
    CHECK(memcmp(&global_state.config, &before, sizeof(before)) == 0);
    CHECK(memcmp(ADDR_CONFIG, previous_sector, sizeof(previous_sector)) == 0);
}

static void confirmed_save_busy_ownership(void) {
    for (unsigned condition = 0; condition < 6; ++condition) {
        fresh("confirmed_save_busy_ownership_preserves_flash_and_ram");
        config_t previously_saved = customized_config();
        install_config(&previously_saved);
        uint8_t previous_sector[FLASH_SECTOR_SIZE];
        memcpy(previous_sector, ADDR_CONFIG, sizeof(previous_sector));
        global_state.fw.upgrade_in_progress = condition == 0;
        global_state.fw.image_dirty = condition == 1;
        global_state.maintenance_reserved = condition == 2;
        global_state.reboot_requested = condition == 3;
        global_state.config_bootloader_local_pending = condition == 4;
        global_state.config_bootloader_peer_pending = condition == 5;
        config_t before = global_state.config;
        uint32_t digest = config_digest(&global_state), actual = ~digest;
        CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_BUSY);
        CHECK(!erases && !programs);
        CHECK(memcmp(&global_state.config, &before, sizeof(before)) == 0);
        CHECK(memcmp(ADDR_CONFIG, previous_sector, sizeof(previous_sector)) == 0);
        owner(1); /* Every refusal released all ownership and restored IRQ state. */
    }
    fresh("confirmed_save_does_not_wait_behind_other_core_firmware_ownership");
    uint32_t digest = config_digest(&global_state), actual = ~digest;
    lock_owner[1] = 1;
    lock_depth[1] = 1;
    unsigned before = blocking_entries;
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_BUSY);
    CHECK(blocking_entries == before && try_attempts[1] == 1);
    CHECK(lock_depth[1] == 1 && lock_owner[1] == 1);
    CHECK(!interrupts[0] && !interrupts[1] && !erases && !programs);
    lock_depth[1] = 0;
    CHECK(config_save_confirmed(&global_state, digest, NULL) == CONFIG_CONFIRM_INVALID);
    CHECK(!erases && !programs);
    owner(1);
}

static void confirmed_save_readback_and_idempotence(void) {
    fresh("confirmed_save_verifies_exact_page_and_skips_identical_image");
    global_state.config = customized_config();
    config_t expected = global_state.config;
    expected.checksum = oracle_crc32((const uint8_t *)&expected, offsetof(config_t, checksum));
    uint8_t page[FLASH_PAGE_SIZE] = {0};
    memcpy(page, &expected, sizeof(expected));
    uint32_t digest = config_digest(&global_state), actual = ~digest;
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == digest && erases == 1 && programs == 1);
    CHECK(memcmp(ADDR_CONFIG, page, sizeof(page)) == 0);
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == digest && erases == 1 && programs == 1);
    /* Even a corrupted page-tail byte is not an identical saved image. */
    storage_flash[STORAGE_CONFIG_OFFSET + FLASH_PAGE_SIZE - 1] = 1;
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == digest && erases == 2 && programs == 2);
    CHECK(memcmp(ADDR_CONFIG, page, sizeof(page)) == 0);
    memset(&global_state.config, 0, sizeof(config_t));
    load_config(&global_state);
    CHECK(memcmp(&global_state.config, &expected, sizeof(expected)) == 0);

    fresh("confirmed_save_silent_nor_corruption_is_explicit_failure");
    digest = config_digest(&global_state);
    corrupt_settings_program = true;
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_FLASH_MISMATCH);
    CHECK(!corrupt_settings_program && erases == 1 && programs == 1);
    config_t stored;
    memcpy(&stored, ADDR_CONFIG, sizeof(stored));
    CHECK(stored.checksum != oracle_crc32((const uint8_t *)&stored, offsetof(config_t, checksum)));
    /* A new explicit attempt repairs the failed image and may then confirm. */
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == digest && erases == 2 && programs == 2);
    owner(1);
}

static void confirmed_save_concurrent_mutation(void) {
    const uint64_t old_idle = UINT64_C(0x0000223344556677);
    const uint64_t new_idle = UINT64_C(0x0000778899aabbcc);
    fresh("confirmed_save_real_set_at_program_reports_conflict_and_preserves_edit");
    global_state.config.output[0].screensaver.idle_time_us = old_idle;
    uint32_t digest = config_digest(&global_state), actual = ~digest;
    config_set_on_settings_program = true;
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_CONFLICT);
    CHECK(config_set_invoked == 1 && !config_set_on_settings_program);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == new_idle);
    CHECK(actual == config_digest(&global_state) && actual != digest);
    config_t stored;
    memcpy(&stored, ADDR_CONFIG, sizeof(stored));
    CHECK(stored.output[0].screensaver.idle_time_us == old_idle);
    CHECK(stored.checksum == oracle_crc32((const uint8_t *)&stored, offsetof(config_t, checksum)));
    CHECK(erases == 1 && programs == 1);
    CHECK(config_save_confirmed(&global_state, actual, &digest) == CONFIG_CONFIRM_OK);
    CHECK(digest == actual && erases == 2 && programs == 2);
    memset(&global_state.config, 0, sizeof(config_t));
    load_config(&global_state);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == new_idle);

    fresh("confirmed_save_snapshot_preemption_never_accepts_torn_u64");
    global_state.config.output[0].screensaver.idle_time_us = old_idle;
    digest = config_digest(&global_state);
    config_set_on_read = true;
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_CONFLICT);
    CHECK(config_set_invoked == 1);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == new_idle);
    CHECK(actual == config_digest(&global_state) && actual != digest);
    if (programs) {
        memcpy(&stored, ADDR_CONFIG, sizeof(stored));
        CHECK(stored.output[0].screensaver.idle_time_us == old_idle);
        CHECK(stored.checksum == oracle_crc32((const uint8_t *)&stored, offsetof(config_t, checksum)));
    }
    CHECK(erases == programs && programs <= 1);
}

static void confirmed_save_full_width_timers(void) {
    fresh("confirmed_save_unrelated_edit_preserves_all_legacy_u64_timer_bits");
    config_t expected = customized_config();
    const uint64_t timers[] = {
        UINT64_C(281474976710656), UINT64_C(0x00ffffffffffffff),
        UINT64_C(0x8000000000000000), UINT64_MAX,
    };
    for (unsigned output = 0; output < 2; ++output) {
        expected.output[output].screensaver.idle_time_us = timers[output * 2];
        expected.output[output].screensaver.max_time_us = timers[output * 2 + 1];
    }
    install_config(&expected);
    load_config(&global_state);
    CHECK(config_set_value64(&global_state, 12, 63));
    expected.output[0].speed_x = 63;
    same_settings(&global_state.config, &expected);
    uint32_t actual, digest = config_digest(&global_state);
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == digest);
    memset(&global_state.config, 0, sizeof(config_t));
    load_config(&global_state);
    same_settings(&global_state.config, &expected);
    const uint8_t timer_fields[] = {21, 22, 51, 52};
    for (unsigned i = 0; i < sizeof(timer_fields); ++i) {
        CHECK(!config_set_value64(&global_state, timer_fields[i], CONFIG_TIMEOUT_MAX_US + 1));
        same_settings(&global_state.config, &expected);
    }
    CHECK(!config_set_value64(&global_state, 12, UINT64_C(0x10000003f)));
    CHECK(!config_set_value64(&global_state, 10, 1));
    CHECK(!config_set_value64(&global_state, 255, 1));
    same_settings(&global_state.config, &expected);
    CHECK(config_set_value64(&global_state, 21, CONFIG_TIMEOUT_MAX_US));
    expected.output[0].screensaver.idle_time_us = CONFIG_TIMEOUT_MAX_US;
    same_settings(&global_state.config, &expected);
    digest = config_digest(&global_state);
    CHECK(config_save_confirmed(&global_state, digest, &actual) == CONFIG_CONFIRM_OK);
    memset(&global_state.config, 0, sizeof(config_t));
    load_config(&global_state);
    same_settings(&global_state.config, &expected);
}

static void confirmed_check_value_snapshots(void) {
    fresh("confirmed_value_checks_cover_writable_fields_without_mutation_or_flash");
    global_state.config = customized_config();
    const config_t original = global_state.config;
    uint32_t expected_digest = confirmed_digest_oracle(&original), actual = 0;
    for (size_t i = 1; i < ARRAY_SIZE(confirmed_digest_fields); ++i) {
        uint64_t value = 0;
        memcpy(&value, (const uint8_t *)&original + confirmed_digest_fields[i].offset,
               confirmed_digest_fields[i].width);
        CHECK(config_check_value64(&global_state, confirmed_digest_fields[i].id,
                                   value, &actual) == CONFIG_CONFIRM_OK);
        CHECK(actual == expected_digest);
    }
    /* These expectations are individually valid, but differ from RAM. */
    const struct { uint8_t key; uint64_t value; } differing[] = {
        {12, 48}, {71, 0}, {14, 900}, {15, 30999}, {44, 900}, {45, 30998},
        {21, 1}, {22, 1}, {51, 1}, {52, 1},
    };
    for (size_t i = 0; i < ARRAY_SIZE(differing); ++i) {
        CHECK(config_check_value64(&global_state, differing[i].key, differing[i].value,
                                   &actual) == CONFIG_CONFIRM_CONFLICT);
        CHECK(actual == expected_digest);
    }
    const struct { uint8_t key; uint64_t value; } invalid[] = {
        {0, 0}, {10, 0}, {40, 1}, {70, 10}, {255, 1},
        {12, UINT64_C(0x10000002f)}, {71, 256}, {14, UINT32_MAX},
        {21, CONFIG_TIMEOUT_MAX_US + 1}, {22, UINT64_MAX},
        {51, CONFIG_TIMEOUT_MAX_US + 1}, {52, UINT64_MAX},
    };
    for (size_t i = 0; i < ARRAY_SIZE(invalid); ++i)
        CHECK(config_check_value64(&global_state, invalid[i].key, invalid[i].value,
                                   &actual) == CONFIG_CONFIRM_INVALID);
    CHECK(config_check_value64(&global_state, 12, 47, NULL) == CONFIG_CONFIRM_INVALID);
    CHECK(!erases && !programs && memcmp(&global_state.config, &original, sizeof(original)) == 0);

    fresh("confirmed_value_check_does_not_truncate_legacy_u64_high_bits");
    const uint8_t fields[] = {21, 22, 51, 52};
    for (unsigned output = 0; output < 2; ++output) {
        global_state.config.output[output].screensaver.idle_time_us = UINT64_C(0x8000000000000001);
        global_state.config.output[output].screensaver.max_time_us = UINT64_MAX;
    }
    for (size_t i = 0; i < ARRAY_SIZE(fields); ++i) {
        uint64_t low = (i & 1) ? CONFIG_TIMEOUT_MAX_US : 1;
        CHECK(config_check_value64(&global_state, fields[i], low, &actual) == CONFIG_CONFIRM_CONFLICT);
        CHECK(actual == confirmed_digest_oracle(&global_state.config));
    }
    CHECK(!erases && !programs);

    fresh("confirmed_value_and_digest_come_from_same_coherent_snapshot");
    const uint64_t before = UINT64_C(0x0000223344556677);
    const uint64_t after = UINT64_C(0x0000778899aabbcc);
    global_state.config.output[0].screensaver.idle_time_us = before;
    expected_digest = confirmed_digest_oracle(&global_state.config);
    config_set_on_read = true;
    CHECK(config_check_value64(&global_state, 21, before, &actual) == CONFIG_CONFIRM_OK);
    CHECK(config_set_invoked == 1 && actual == expected_digest);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == after);
    CHECK(config_check_value64(&global_state, 21, before, &actual) == CONFIG_CONFIRM_CONFLICT);
    CHECK(actual == confirmed_digest_oracle(&global_state.config) && actual != expected_digest);
    CHECK(config_check_value64(&global_state, 21, after, &actual) == CONFIG_CONFIRM_OK);
    CHECK(actual == confirmed_digest_oracle(&global_state.config));
    CHECK(!erases && !programs);
    owner(1);
}

static void confirmed_config_persistence(void) {
    confirmed_digest_covers_settings_only();
    confirmed_save_rejects_conflicts();
    confirmed_save_busy_ownership();
    confirmed_save_readback_and_idempotence();
    confirmed_save_concurrent_mutation();
    confirmed_save_full_width_timers();
    confirmed_check_value_snapshots();
}
