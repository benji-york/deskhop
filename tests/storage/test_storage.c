/* Execute production update/config/MSC units against observable NOR/SDK edges. */
#include "main.h"
#undef memcpy
#include <stdio.h>
#include <setjmp.h>

static const char *scenario;
static uint64_t now;
/* IDs 1/2/3 remain firmware/flash/config; startup then initializes history/runtime. */
static unsigned current_core, lock_owner[6], lock_depth[6], next_lock;
static unsigned interrupts[2], erases, programs, resets, watchdog_kicks;
static uint32_t reset_disable_mask;
static unsigned sector_erases[STORAGE_SIZE / FLASH_SECTOR_SIZE];
static unsigned page_programs[STAGING_PAGES_CNT];
static unsigned active_operation, cut_at_operation, cut_bytes;
static jmp_buf power_cut;
static uint32_t seed = 1, replay_seed = 1;
static FILE *trace;
static unsigned trace_step;
static bool config_set_on_copy, config_set_pending;
static unsigned config_set_invoked;
static bool config_set_active;
static jmp_buf config_set_blocked;
static void interleaved_config_set(void);
static void event(const char *kind, uint32_t value) {
    if (trace) fprintf(trace, "{\"scenario\":\"%s\",\"seed\":%u,\"step\":%u,\"time_us\":%llu,\"core\":%u,\"event\":\"%s\",\"value\":%u}\n",
                       scenario, replay_seed, trace_step++, (unsigned long long)now, current_core, kind, value);
}
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "storage failure: %s seed=%u time=%llu core=%u %s:%d: %s\n", \
    scenario, replay_seed, (unsigned long long)now, current_core, __FILE__, __LINE__, #x); \
    exit(1); } } while (0)

_Alignas(8) static uint8_t receiver_flash[STORAGE_SIZE];
uint8_t *storage_flash = receiver_flash;
device_t global_state;
static device_t source_state;
static queue_t *uart_sink;
uint8_t uart_rxbuf[DMA_RX_BUFFER_SIZE];
static uint8_t image[STAGING_IMAGE_SIZE];
static uint8_t alternate_image[STAGING_IMAGE_SIZE];

/* This deliberately differs from the firmware's table implementation. */
static uint32_t oracle_crc32(const uint8_t *data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}
static uint32_t random32(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static uint32_t make_image(uint8_t *bytes, uint16_t version, uint8_t salt) {
    for (unsigned i = 0; i < STAGING_IMAGE_SIZE; ++i)
        bytes[i] = (uint8_t)((i * 73u + i / 127u) ^ salt);
    uint32_t crc = oracle_crc32(bytes, STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE);
    firmware_metadata_t metadata = {
        .magic = FIRMWARE_METADATA_MAGIC, .version = version, .checksum = crc,
    };
    memcpy(bytes + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE, &metadata, sizeof(metadata));
    return crc;
}
static void interleaved_config_set(void) {
    unsigned saved_core = current_core;
    current_core = 1;
    uart_packet_t packet = {.type = SET_VAL_MSG, .data = {21}};
    uint64_t idle = UINT64_C(0x0066778899aabbcc);
    memcpy(&packet.data[1], &idle, 7);
    config_set_active = true;
    if (setjmp(config_set_blocked) == 0) {
        handle_api_msgs(&packet, &global_state);
        ++config_set_invoked;
    }
    config_set_active = false;
    current_core = saved_core;
}
void *storage_memcpy(void *destination, const void *source, size_t length) {
    if (config_set_on_copy && destination == global_state.page_buffer
        && source == &global_state.config && length == sizeof(config_t)) {
        config_set_on_copy = false;
        /* Preempt the native copy inside one multibyte config field. This
           models a legal copy interleaving; the actual setter must acquire its
           own lock to defer. Without serialization the saved field is torn. */
        size_t split = offsetof(config_t, output[0].screensaver.idle_time_us) + 4;
        memcpy(destination, source, split);
        interleaved_config_set();
        memcpy((uint8_t *)destination + split, (const uint8_t *)source + split, length - split);
        return destination;
    }
    return memcpy(destination, source, length);
}
void write_raw_packet(uint8_t *bytes, uart_packet_t *packet) { CHECK(false); }
void critical_section_init(critical_section_t *cs) {
    if (!cs->id) cs->id = ++next_lock;
    CHECK(cs->id <= 5);
}
void critical_section_enter_blocking(critical_section_t *cs) {
    CHECK(cs->id && cs->id <= 5);
    if (cs->id <= 3) CHECK(!lock_depth[4] && !lock_depth[5]);
    if (cs->id >= 4) {
        /* Updater hooks may hold firmware, but never flash/config or each other. */
        CHECK(!lock_depth[2] && !lock_depth[3]);
        CHECK(!lock_depth[4] && !lock_depth[5]);
        CHECK(!interrupts[current_core]);
    }
    if (lock_depth[cs->id]) {
        /* Run the real setter up to its blocked SDK acquisition, then resume
           that operation from its side-effect-free entry when the lock opens. */
        CHECK(config_set_active && cs->id == 3 && lock_owner[cs->id] != current_core);
        config_set_pending = true;
        longjmp(config_set_blocked, 1);
    }
    lock_owner[cs->id] = current_core;
    lock_depth[cs->id]++;
}
void critical_section_exit(critical_section_t *cs) {
    CHECK(lock_depth[cs->id] == 1 && lock_owner[cs->id] == current_core);
    lock_depth[cs->id]--;
    if (cs->id == 3 && config_set_pending) {
        config_set_pending = false;
        interleaved_config_set();
    }
}
uint32_t save_and_disable_interrupts(void) {
    unsigned was = interrupts[current_core];
    interrupts[current_core] = 1;
    return was;
}
void restore_interrupts(uint32_t previous) { interrupts[current_core] = previous; }
uint32_t time_us_32(void) { return (uint32_t)now; }
uint64_t time_us_64(void) { return now; }
void watchdog_update(void) { ++watchdog_kicks; event("watchdog-kick", watchdog_kicks); }
uint8_t toggle_led(void) { return 0; }
void reset_usb_boot(uint32_t gpio, uint32_t disable) {
    /* ROM reset does not return on hardware. Failure must already be visible. */
    diagnostic_runtime_snapshot_t snapshot = diagnostic_runtime_snapshot();
    CHECK(snapshot.phase == DIAGNOSTIC_UPDATE_FAILED);
    history_window_t window = diagnostic_history_window(HISTORY_CAPACITY);
    history_event_t last;
    CHECK(window.count && diagnostic_history_read(window.end_seq - 1, &last));
    CHECK(last.type == HISTORY_UPDATE_PHASE && last.a == DIAGNOSTIC_UPDATE_FAILED);
    ++resets;
    reset_disable_mask = disable;
    event("rom-recovery-request", resets);
    event("rom-interface-disable-mask", disable);
}

static void before_flash(uint32_t offset, size_t length) {
    CHECK(!lock_depth[3]); /* RAM snapshots never hold their lock through flash. */
    CHECK(!lock_depth[4] && !lock_depth[5]);
    CHECK(lock_depth[2] == 1 && lock_owner[2] == current_core);
    CHECK(interrupts[current_core]);
    CHECK(offset <= STORAGE_SIZE && length <= STORAGE_SIZE - offset);
    ++active_operation;
    event("flash-operation", offset);
}
void flash_range_erase(uint32_t offset, size_t length) {
    before_flash(offset, length);
    CHECK(offset % FLASH_SECTOR_SIZE == 0 && length % FLASH_SECTOR_SIZE == 0);
    if (active_operation == cut_at_operation) {
        event("power-cut", cut_bytes);
        memset(storage_flash + offset, 0xff, cut_bytes < length ? cut_bytes : length);
        longjmp(power_cut, 1);
    }
    memset(storage_flash + offset, 0xff, length);
    ++erases;
    ++sector_erases[offset / FLASH_SECTOR_SIZE];
}
void flash_range_program(uint32_t offset, const uint8_t *bytes, size_t length) {
    before_flash(offset, length);
    CHECK(offset % FLASH_PAGE_SIZE == 0 && length % FLASH_PAGE_SIZE == 0);
    if (active_operation == cut_at_operation) {
        event("power-cut", cut_bytes);
        size_t changed = cut_bytes < length ? cut_bytes : length;
        for (size_t i = 0; i < changed; ++i) storage_flash[offset + i] &= bytes[i];
        longjmp(power_cut, 1);
    }
    for (size_t i = 0; i < length; ++i) {
        CHECK((storage_flash[offset + i] & bytes[i]) == bytes[i]);
        storage_flash[offset + i] &= bytes[i];
    }
    ++programs;
    if (offset < STAGING_IMAGE_SIZE) ++page_programs[offset / FLASH_PAGE_SIZE];
}
bool queue_try_add(queue_t *queue, const void *packet) {
    if (queue->used == queue->capacity) return false;
    CHECK(queue->capacity <= 256);
    memcpy(queue->bytes[queue->used++], packet, sizeof(uart_packet_t));
    return true;
}
bool queue_try_remove(queue_t *queue, void *packet) {
    if (!queue->used) return false;
    memcpy(packet, queue->bytes[0], sizeof(uart_packet_t));
    --queue->used;
    memmove(queue->bytes[0], queue->bytes[1], queue->used * sizeof(queue->bytes[0]));
    return true;
}
void queue_packet_blocking(const uint8_t *data, enum packet_type_e type, int length) {
    /* Real request handlers must release the shared firmware lock before a
       blocking UART send, otherwise core 0 cannot acquire it to drain TX. */
    CHECK(!lock_depth[1]);
    uart_packet_t packet = {.type = type};
    memcpy(packet.data, data, (size_t)length);
    CHECK(queue_try_add(uart_sink, &packet));
}

static void fresh(const char *name) {
    scenario = name;
    config_set_on_copy = config_set_pending = config_set_active = false;
    config_set_invoked = 0;
    event("scenario-start", 0);
    memset(&global_state, 0, sizeof(global_state));
    storage_flash = receiver_flash;
    memset(storage_flash, 0xff, STORAGE_SIZE);
    memset(&source_state, 0, sizeof(source_state));
    source_state.uart_tx_queue.capacity = 256;
    uart_sink = &global_state.uart_tx_queue;
    memset(lock_depth, 0, sizeof(lock_depth));
    memset(interrupts, 0, sizeof(interrupts));
    memset(sector_erases, 0, sizeof(sector_erases));
    memset(page_programs, 0, sizeof(page_programs));
    erases = programs = resets = watchdog_kicks = active_operation = cut_at_operation = 0;
    reset_disable_mask = UINT32_MAX;
    current_core = 0;
    now = 1000000;
    global_state.config = default_config;
    global_state._running_fw.version = 192;
    global_state.uart_tx_queue.capacity = 256;
    firmware_sync_init();
    diagnostic_history_init();
    diagnostic_runtime_init();
}
static void owner(unsigned core) {
    for (unsigned id = 1; id <= 5; ++id) CHECK(!lock_depth[id]);
    CHECK(!interrupts[0] && !interrupts[1]);
    current_core = core;
    event("core-dispatch", core);
}
static uf2_t block_for(uint32_t block, const uint8_t *source) {
    uf2_t uf2 = {
        .magicStart0 = UF2_MAGIC_START0, .magicStart1 = UF2_MAGIC_START1,
        .targetAddr = XIP_BASE + block * FLASH_PAGE_SIZE,
        .payloadSize = FLASH_PAGE_SIZE, .blockNo = block,
        .numBlocks = STAGING_PAGES_CNT, .magicEnd = UF2_MAGIC_END,
    };
    memcpy(uf2.data, source + block * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE);
    return uf2;
}
static void host_block(uint32_t block, const uint8_t *source) {
    owner(0);
    uf2_t uf2 = block_for(block, source);
    CHECK(tud_msc_write10_cb(0, block, 0, (uint8_t *)&uf2, sizeof(uf2)) == sizeof(uf2));
    CHECK(!lock_depth[1] && !lock_depth[2]);
}
static void heartbeat(uint16_t version, uint32_t crc, bool compatible) {
    owner(1);
    uart_packet_t packet = {.type = HEARTBEAT_MSG};
    packet.data16[0] = version;
    packet.data16[1] = compatible ? FW_UPDATE_PROTOCOL_MARKER : 0;
    packet.data32[1] = crc;
    handle_heartbeat_msg(&packet, &global_state);
}
static void upgrade_tick(void) {
    owner(1);
    firmware_upgrade_task(&global_state);
    CHECK(!lock_depth[1] && !lock_depth[2]);
}
static uart_packet_t pop_request(void) {
    uart_packet_t request;
    CHECK(queue_try_remove(&global_state.uart_tx_queue, &request));
    CHECK(request.type == REQUEST_BYTE_MSG);
    CHECK(request.data32[0] < STAGING_IMAGE_SIZE);
    return request;
}
static void response(uint32_t address, const uint8_t *source) {
    uart_packet_t packet = {.type = RESPONSE_BYTE_MSG};
    packet.data32[0] = address;
    CHECK(address <= STAGING_IMAGE_SIZE - 4);
    memcpy(&packet.data32[1], source + address, 4);
    owner(1);
    handle_response_byte_msg(&packet, &global_state);
}
static void source_reply(uart_packet_t request, uint8_t *source) {
    /* Route the emitted request through the real source handler and guarded
       flash reader. The source owns separate state, flash bytes and TX queue;
       this adapter switches the current modeled SDK address space serially. */
    owner(1);
    storage_flash = source;
    uart_sink = &source_state.uart_tx_queue;
    handle_request_byte_msg(&request, &source_state);
    storage_flash = receiver_flash;
    uart_sink = &global_state.uart_tx_queue;
    uart_packet_t reply;
    CHECK(queue_try_remove(&source_state.uart_tx_queue, &reply));
    CHECK(reply.type == RESPONSE_BYTE_MSG);
    event("peer-word-delivery", reply.data32[0]);
    handle_response_byte_msg(&reply, &global_state);
}
static void expect_no_change(fw_upgrade_state_t old) {
    CHECK(memcmp(&old, &global_state.fw, sizeof(old)) == 0);
}

static void expect_history(unsigned index, history_type_t type,
                           uint8_t a, uint8_t b, uint32_t value) {
    history_window_t window = diagnostic_history_window(HISTORY_CAPACITY);
    history_event_t observed;
    CHECK(!window.overwritten && index < window.count);
    CHECK(diagnostic_history_read(window.first_seq + index, &observed));
    CHECK(observed.type == type && observed.a == a && observed.b == b);
    CHECK(observed.value == value && observed.reserved == 0);
}

static void expect_completed_history(diagnostic_update_source_t source,
                                     uint16_t target, bool failed) {
    diagnostic_runtime_snapshot_t snapshot = diagnostic_runtime_snapshot();
    diagnostic_update_phase_t phase = failed ? DIAGNOSTIC_UPDATE_FAILED
                                             : DIAGNOSTIC_UPDATE_REBOOT_PENDING;
    CHECK(snapshot.update_seen && snapshot.update_attempt == 1);
    CHECK(snapshot.phase == phase && snapshot.source == source);
    CHECK(snapshot.target_version == target && snapshot.received_bytes == STAGING_IMAGE_SIZE);
    CHECK(snapshot.total_bytes == STAGING_IMAGE_SIZE);
    CHECK(diagnostic_history_window(HISTORY_CAPACITY).count == 7);
    expect_history(0, HISTORY_UPDATE_BEGIN, source, 0, target);
    for (unsigned quarter = 1; quarter <= 4; ++quarter)
        expect_history(quarter, HISTORY_UPDATE_PROGRESS, source, quarter * 25,
                       quarter * STAGING_IMAGE_SIZE / 4);
    expect_history(5, HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_VALIDATING, source, target);
    expect_history(6, HISTORY_UPDATE_PHASE, phase, source, target);
}

static void uf2_reordering_and_duplicate(void) {
    fresh("uf2_reordering_and_duplicate");
    uint32_t crc = make_image(image, 193, 0x31);
    unsigned order[STAGING_PAGES_CNT];
    for (unsigned i = 0; i < STAGING_PAGES_CNT; ++i) order[i] = i;
    for (unsigned i = STAGING_PAGES_CNT - 1; i; --i) {
        unsigned j = random32() % (i + 1), temp = order[i]; order[i] = order[j]; order[j] = temp;
    }
    for (unsigned i = 0; i < STAGING_PAGES_CNT; ++i) {
        host_block(order[i], image);
        if (i + 1 < STAGING_PAGES_CNT) CHECK(!global_state.reboot_requested);
        unsigned committed = programs;
        host_block(order[i], image);
        CHECK(programs == committed);
        CHECK(global_state.uf2_blocks_received_count == i + 1);
    }
    CHECK(programs == STAGING_PAGES_CNT && erases == STAGING_IMAGE_SIZE / FLASH_SECTOR_SIZE);
    CHECK(memcmp(storage_flash, image, sizeof(image)) == 0);
    CHECK(calculate_firmware_crc32() == crc);
    CHECK(global_state.reboot_requested && !global_state.fw.image_dirty && !resets);
    for (unsigned i = 0; i < STAGING_PAGES_CNT; ++i) CHECK(page_programs[i] == 1);
    for (unsigned i = 0; i < STAGING_IMAGE_SIZE / FLASH_SECTOR_SIZE; ++i) CHECK(sector_erases[i] == 1);
    expect_completed_history(DIAGNOSTIC_SOURCE_USB, 0, false);
}
static void uf2_reject_invalid_and_mixed(void) {
    fresh("uf2_reject_invalid");
    make_image(image, 193, 0x41);
    uf2_t valid = block_for(0, image);
    for (unsigned field = 0; field < 8; ++field) {
        uf2_t invalid = valid;
        switch (field) {
        case 0: invalid.magicStart0 ^= 1; break;
        case 1: invalid.magicStart1 ^= 1; break;
        case 2: invalid.magicEnd ^= 1; break;
        case 3: invalid.payloadSize--; break;
        case 4: invalid.numBlocks--; break;
        case 5: invalid.blockNo = STAGING_PAGES_CNT; break;
        case 6: invalid.targetAddr++; break;
        case 7: break;
        }
        unsigned size = field == 7 ? sizeof(invalid) - 1 : sizeof(invalid);
        CHECK(tud_msc_write10_cb(0, 1, 0, (uint8_t *)&invalid, size) == (int32_t)size);
        CHECK(!programs && !erases && !global_state.fw.upgrade_in_progress && !watchdog_kicks);
        CHECK(!diagnostic_runtime_snapshot().update_seen);
        CHECK(!diagnostic_history_window(HISTORY_CAPACITY).count);
    }
    CHECK(tud_msc_write10_cb(0, 4096, 0, (uint8_t *)&valid, sizeof(valid)) == -1);
    scenario = "uf2_complete_mixed_image_enters_recovery";
    make_image(alternate_image, 193, 0x42);
    for (unsigned i = 0; i < STAGING_PAGES_CNT; ++i) host_block(i, i == 333 ? alternate_image : image);
    CHECK(resets == 1 && !global_state.reboot_requested);
    CHECK(reset_disable_mask == 0); /* Invalid-image recovery keeps UF2 and PICOBOOT. */
    CHECK(global_state.fw.image_dirty);
    for (unsigned i = 0; i < FLASH_SECTOR_SIZE; ++i) CHECK(storage_flash[i] == 0xff);
    expect_completed_history(DIAGNOSTIC_SOURCE_USB, 0, true);
}

static void actual_peer_transfer(bool corrupt, bool intermittent_loss) {
    fresh(corrupt ? "peer_corrupt_word_recovery" : "peer_transfer_queue_loss_duplicate_wraparound");
    uint32_t crc = make_image(image, 193, 0x51);
    now = UINT32_MAX - 20000u;
    heartbeat(193, crc, true);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_PULL);
    upgrade_tick();
    CHECK(!programs); /* Address zero is not a completed page. */
    for (unsigned word = 0; word < STAGING_IMAGE_SIZE / 4; ++word) {
        uart_packet_t request = pop_request();
        CHECK(request.data32[0] == word * 4);
        if (intermittent_loss && word % 1009 == 0) {
            fw_upgrade_state_t saved = global_state.fw;
            now += FW_UPDATE_RESPONSE_TIMEOUT_US - 1;
            upgrade_tick();
            CHECK(!global_state.uart_tx_queue.used);
            expect_no_change(saved);
            now++;
            upgrade_tick();
            request = pop_request();
            CHECK(request.data32[0] == word * 4);
        }
        if (word % 257 == 0 && word + 1 < STAGING_IMAGE_SIZE / 4) {
            fw_upgrade_state_t saved = global_state.fw;
            response(word * 4 + 4, image); /* Future/out-of-order word. */
            expect_no_change(saved);
        }
        if (corrupt && word == 1000) image[word * 4] ^= 0x80;
        source_reply(request, image);
        fw_upgrade_state_t saved = global_state.fw;
        response(request.data32[0], image); /* Duplicate must not advance checksum. */
        expect_no_change(saved);
        if (word == FLASH_PAGE_SIZE / 4 - 1) {
            now += 5000;
            response(request.data32[0], image);
            diagnostic_runtime_snapshot_t snapshot = diagnostic_runtime_snapshot();
            CHECK(snapshot.received_bytes == FLASH_PAGE_SIZE && snapshot.progress_age_ms == 5);
        }
        if (intermittent_loss && (word + 1) % 4096 == 0 && word + 1 < STAGING_IMAGE_SIZE / 4) {
            unsigned old_programs = programs;
            global_state.uart_tx_queue.capacity = 0;
            upgrade_tick();
            CHECK(programs == old_programs);
            expect_no_change(saved); /* Failed enqueue must leave completed word live. */
            global_state.uart_tx_queue.capacity = 256;
        }
        now += 7;
        if (word % 1000 == 0) heartbeat(193, crc, true);
        upgrade_tick();
    }
    CHECK(global_state.fw.address == STAGING_IMAGE_SIZE);
    CHECK(!global_state.uart_tx_queue.used); /* No out-of-range sentinel request. */
    CHECK(!global_state.fw.upgrade_in_progress && programs == STAGING_PAGES_CNT);
    if (corrupt) {
        CHECK(resets == 1 && !global_state.reboot_requested);
        CHECK(reset_disable_mask == 0);
        CHECK(global_state.fw.image_dirty);
    } else {
        CHECK(!resets && global_state.reboot_requested && !global_state.fw.image_dirty);
        CHECK(global_state._running_fw.version == 193 && global_state._running_fw.checksum == crc);
        CHECK(memcmp(storage_flash, image, sizeof(image)) == 0);
    }
    expect_completed_history(DIAGNOSTIC_SOURCE_PEER, 193, corrupt);
}

static void peer_stall_pause_and_restart(void) {
    fresh("peer_stall_pause_and_restart");
    uint32_t crc = make_image(image, 193, 0x61);
    heartbeat(193, crc, true);
    upgrade_tick();
    now += FW_UPDATE_STALL_TIMEOUT_US;
    upgrade_tick();
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_NONE && !programs && !resets);
    CHECK(diagnostic_runtime_snapshot().phase == DIAGNOSTIC_UPDATE_ABANDONED);

    global_state.uart_tx_queue.used = 0;
    heartbeat(193, crc, true);
    upgrade_tick();
    for (unsigned word = 0; word < FLASH_PAGE_SIZE / 4; ++word) {
        uart_packet_t request = pop_request();
        response(request.data32[0], image);
        upgrade_tick();
    }
    CHECK(global_state.fw.image_dirty && programs == 1);
    now += FW_UPDATE_STALL_TIMEOUT_US;
    upgrade_tick();
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_PULL_PAUSED);
    CHECK(global_state.fw.upgrade_in_progress && !resets && !global_state.reboot_requested);
    diagnostic_runtime_snapshot_t paused = diagnostic_runtime_snapshot();
    CHECK(paused.phase == DIAGNOSTIC_UPDATE_PAUSED && paused.received_bytes == FLASH_PAGE_SIZE);
    fw_upgrade_state_t saved = global_state.fw;
    response(FLASH_PAGE_SIZE, image);
    expect_no_change(saved);

    global_state.uart_tx_queue.used = 0;
    heartbeat(193, crc, true);
    CHECK(global_state.fw.address == 0 && global_state.fw.image_dirty && !global_state.fw.request_pending);
    diagnostic_runtime_snapshot_t resumed = diagnostic_runtime_snapshot();
    CHECK(resumed.phase == DIAGNOSTIC_UPDATE_RECEIVING && resumed.received_bytes == 0);
    CHECK(resumed.update_attempt == 3);
    response(0, image);
    CHECK(global_state.fw.address == 0);
    now += FW_UPDATE_RESPONSE_TIMEOUT_US - 1;
    upgrade_tick();
    CHECK(!global_state.uart_tx_queue.used);
    now++;
    upgrade_tick();
    CHECK(pop_request().data32[0] == 0);

    /* A same-version new checksum is still a different source generation. */
    heartbeat(193, crc ^ 1u, true);
    CHECK(global_state.fw.peer_checksum == (crc ^ 1u) && !global_state.fw.request_pending);
    heartbeat(193, crc, false);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_PULL_PAUSED);
    CHECK(diagnostic_runtime_snapshot().update_attempt == 4);
    CHECK(diagnostic_runtime_snapshot().phase == DIAGNOSTIC_UPDATE_PAUSED);
    CHECK(diagnostic_history_window(HISTORY_CAPACITY).count == 7);
    expect_history(0, HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 193);
    expect_history(1, HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_ABANDONED, DIAGNOSTIC_SOURCE_PEER, 193);
    expect_history(2, HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 193);
    expect_history(3, HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_PAUSED, DIAGNOSTIC_SOURCE_PEER, 193);
    expect_history(4, HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 193);
    expect_history(5, HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 193);
    expect_history(6, HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_PAUSED, DIAGNOSTIC_SOURCE_PEER, 193);
}

static void peer_update_source_transitions(void) {
    fresh("peer_source_changes_and_live_stall_observations");
    uint32_t crc = make_image(image, 193, 0x62);
    heartbeat(193, crc, true);
    heartbeat(194, crc, true);
    CHECK(diagnostic_runtime_snapshot().target_version == 194);
    CHECK(diagnostic_runtime_snapshot().update_attempt == 2);
    heartbeat(194, crc, false);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_NONE);
    CHECK(diagnostic_runtime_snapshot().phase == DIAGNOSTIC_UPDATE_ABANDONED);
    heartbeat(193, crc, true);
    heartbeat(192, crc, true);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_NONE);
    CHECK(diagnostic_runtime_snapshot().phase == DIAGNOSTIC_UPDATE_ABANDONED);

    heartbeat(193, crc, true);
    upgrade_tick();
    for (unsigned word = 0; word < FLASH_PAGE_SIZE / 4; ++word) {
        uart_packet_t request = pop_request();
        response(request.data32[0], image);
        upgrade_tick();
    }
    CHECK(global_state.fw.image_dirty);
    heartbeat(191, crc, true);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_PULL_PAUSED);
    CHECK(diagnostic_runtime_snapshot().phase == DIAGNOSTIC_UPDATE_PAUSED);
    unsigned paused_events = diagnostic_history_window(HISTORY_CAPACITY).count;
    heartbeat(191, crc, true); /* An unusable peer cannot produce a false restart. */
    CHECK(diagnostic_history_window(HISTORY_CAPACITY).count == paused_events);
    heartbeat(193, crc, true);
    diagnostic_runtime_snapshot_t before = diagnostic_runtime_snapshot();
    now += FW_UPDATE_STALL_TIMEOUT_US;
    heartbeat(193, crc, true); /* A live source makes the dirty stall restart. */
    upgrade_tick();
    diagnostic_runtime_snapshot_t after = diagnostic_runtime_snapshot();
    CHECK(after.phase == DIAGNOSTIC_UPDATE_RECEIVING && after.received_bytes == 0);
    CHECK(after.update_attempt == before.update_attempt + 1);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_PULL && global_state.fw.image_dirty);
    history_window_t window = diagnostic_history_window(HISTORY_CAPACITY);
    expect_history(window.count - 1, HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 193);
}

static void host_peer_config_serializations(void) {
    /* Enumerate all six orders of host UF2 claim, core1 peer response, core0
       save. These operations serialize on the actual production fw lock. */
    static const unsigned permutations[6][3] = {
        {0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0},
    };
    for (unsigned order = 0; order < 6; ++order) {
        fresh("host_peer_config_serializations");
        uint32_t crc = make_image(image, 193, 0x71);
        heartbeat(193, crc, true);
        upgrade_tick();
        pop_request();
        for (unsigned step = 0; step < 3; ++step) {
            switch (permutations[order][step]) {
            case 0: host_block(31, image); break;
            case 1: response(0, image); break;
            case 2: owner(0); save_config(&global_state); break;
            }
        }
        CHECK(global_state.fw.source == FW_UPDATE_SOURCE_DROP && programs == 1);
        CHECK(!sector_erases[STORAGE_CONFIG_OFFSET / FLASH_SECTOR_SIZE]);
        CHECK(global_state.uf2_blocks_received_count == 1 && global_state.fw.address == 0);
        diagnostic_runtime_snapshot_t snapshot = diagnostic_runtime_snapshot();
        CHECK(snapshot.source == DIAGNOSTIC_SOURCE_USB && snapshot.update_attempt == 2);
        CHECK(snapshot.received_bytes == FLASH_PAGE_SIZE && snapshot.target_version == 0);
        fw_upgrade_state_t saved = global_state.fw;
        heartbeat(200, crc, true);
        upgrade_tick();
        response(0, image);
        expect_no_change(saved);
        CHECK(diagnostic_history_window(HISTORY_CAPACITY).count == 2);
        owner(0);
        wipe_config();
        CHECK(erases == 1);
    }
    fresh("reboot_reservation_rejects_update_work");
    uint32_t crc = make_image(image, 193, 0x72);
    global_state.reboot_requested = true;
    host_block(12, image);
    heartbeat(193, crc, true);
    upgrade_tick();
    CHECK(!programs && !erases && !watchdog_kicks && !global_state.fw.upgrade_in_progress);
    CHECK(!diagnostic_runtime_snapshot().update_seen);
    CHECK(!diagnostic_history_window(HISTORY_CAPACITY).count);
}

static void source_reads_and_metadata(void) {
    fresh("source_reads_and_metadata");
    uint32_t crc = make_image(image, 193, 0x81);
    memcpy(storage_flash, image, sizeof(image));
    CHECK(firmware_image_is_valid(193, crc, true));
    CHECK(!firmware_image_is_valid(194, crc, true));
    CHECK(!firmware_image_is_valid(193, crc ^ 1u, true));
    uint32_t word;
    CHECK(read_running_firmware_word(0, &word));
    CHECK(!read_running_firmware_word(1, &word));
    CHECK(!read_running_firmware_word(STAGING_IMAGE_SIZE, &word));
    CHECK(!read_running_firmware_word(UINT32_MAX, &word));
    uart_packet_t request = {.type = REQUEST_BYTE_MSG};
    request.data32[0] = STAGING_IMAGE_SIZE;
    handle_request_byte_msg(&request, &global_state);
    uart_packet_t reply;
    CHECK(queue_try_remove(&global_state.uart_tx_queue, &reply));
    CHECK(reply.type == RESPONSE_BYTE_MSG && reply.data32[1] == 0);
    global_state.fw.upgrade_in_progress = true;
    request.data32[0] = 0;
    handle_request_byte_msg(&request, &global_state);
    CHECK(!global_state.uart_tx_queue.used);
    global_state.fw.upgrade_in_progress = false;
    global_state.reboot_requested = true;
    handle_request_byte_msg(&request, &global_state);
    CHECK(!global_state.uart_tx_queue.used);

    /* Packed legacy metadata is accepted only for direct host rollback. */
    memset(storage_flash + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE + 6, 0xff, 6);
    memcpy(storage_flash + STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE + 6, &crc, 4);
    CHECK(firmware_image_is_valid(0, 0, false));
    CHECK(!firmware_image_is_valid(193, crc, true));
}

static void config_persistence_and_migration(void) {
    for (unsigned version = 8; version <= 10; ++version) {
        fresh("config_persistence_and_migration");
        config_t historical = default_config;
        historical.version = version;
        historical.output[0].speed_x = 47;
        historical.output[1].border.top = 901;
        historical.output[0].screensaver.mode = DISABLED;
        historical.output[1].screensaver.mode = PONG;
        historical.screensaver_system_timeout_sec = version == 8 ? 0x12345678 : 91;
        historical.checksum = oracle_crc32((const uint8_t *)&historical, offsetof(config_t, checksum));
        memcpy(storage_flash + STORAGE_CONFIG_OFFSET, &historical, sizeof(historical));
        load_config(&global_state);
        CHECK(global_state.config.version == 10);
        CHECK(global_state.config.output[0].speed_x == 47);
        CHECK(global_state.config.output[1].border.top == 901);
        CHECK(global_state.config.screensaver_system_timeout_sec == (version == 8 ? SCREENSAVER_SYSTEM_TIMEOUT_SEC : 91));
        CHECK(global_state.config.output[0].screensaver.mode == (version < 10 ? JITTER : DISABLED));
        CHECK(global_state.config.output[1].screensaver.mode == (version < 10 ? JITTER : PONG));
        CHECK(programs == (version < 10 ? 1u : 0u));
        if (version < 10) CHECK(memcmp(ADDR_CONFIG, &global_state.config, sizeof(config_t)) == 0);
        global_state.config.output[0].screensaver.mode = DISABLED;
        global_state.config.output[1].screensaver.mode = DISABLED;
        save_config(&global_state);
        memset(&global_state.config, 0, sizeof(config_t));
        load_config(&global_state);
        CHECK(global_state.config.output[0].screensaver.mode == DISABLED);
        CHECK(global_state.config.output[1].screensaver.mode == DISABLED);
        uint8_t *stored = storage_flash + STORAGE_CONFIG_OFFSET;
        stored[20] ^= 1; /* Independent corruption after a valid persisted save. */
        load_config(&global_state);
        CHECK(memcmp(&global_state.config, &default_config, sizeof(config_t)) == 0);
    }
}

static void config_set_during_save(void) {
    fresh("config_set_during_save_keeps_persisted_crc_coherent");
    global_state.config.output[0].screensaver.idle_time_us = UINT64_C(0x0011223344556677);
    config_set_on_copy = true;
    save_config(&global_state);
    CHECK(config_set_invoked == 1);
    config_t persisted;
    memcpy(&persisted, ADDR_CONFIG, sizeof(persisted));
    CHECK(persisted.checksum == oracle_crc32((const uint8_t *)&persisted, offsetof(config_t, checksum)));
    CHECK(persisted.output[0].screensaver.idle_time_us == UINT64_C(0x0011223344556677)
          || persisted.output[0].screensaver.idle_time_us == UINT64_C(0x0066778899aabbcc));
    CHECK(global_state.config.output[0].screensaver.idle_time_us == UINT64_C(0x0066778899aabbcc));
    /* The later SET was not lost: a subsequent explicit save persists it. */
    save_config(&global_state);
    memset(&global_state.config, 0, sizeof(config_t));
    load_config(&global_state);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == UINT64_C(0x0066778899aabbcc));
}

static void power_cut_observation(void) {
    static const unsigned cuts[] = {1, 2, 3, 17, 18, 63, 127};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        fresh("power_cut_preserves_nor_but_loses_ram_ownership");
        make_image(alternate_image, 192, 0x91);
        make_image(image, 193, 0x92);
        memcpy(storage_flash, alternate_image, sizeof(image));
        cut_at_operation = cuts[i];
        cut_bytes = 123;
        if (setjmp(power_cut) == 0) {
            for (unsigned block = 0; block < STAGING_PAGES_CNT; ++block) host_block(block, image);
            CHECK(false);
        }
        /* Power removes both core states and locks, while NOR bytes persist.
           No simulated ROM success is inferred: there is no whole-image boot
           gate in these production units. This is a tested limitation. */
        memset(&global_state, 0, sizeof(global_state));
        memset(lock_depth, 0, sizeof(lock_depth));
        memset(interrupts, 0, sizeof(interrupts));
        diagnostic_history_init();
        diagnostic_runtime_init();
        cut_at_operation = 0;
        CHECK(!global_state.fw.image_dirty);
        CHECK(!firmware_image_is_valid(0, 0, false));
        CHECK(!resets); /* No software recovery ran after asynchronous power loss. */
        CHECK(!diagnostic_runtime_snapshot().update_seen);
        CHECK(!diagnostic_history_window(HISTORY_CAPACITY).count);
    }
    fresh("config_power_cut_falls_back_to_defaults");
    global_state.config.output[0].speed_x = 101;
    cut_at_operation = 2; cut_bytes = 64;
    if (setjmp(power_cut) == 0) { save_config(&global_state); CHECK(false); }
    memset(&global_state, 0, sizeof(global_state));
    memset(lock_depth, 0, sizeof(lock_depth));
    memset(interrupts, 0, sizeof(interrupts));
    cut_at_operation = 0;
    load_config(&global_state);
    CHECK(memcmp(&global_state.config, &default_config, sizeof(config_t)) == 0);
}

static void watchdog_contract(void) {
    fresh("watchdog_deadline_and_reboot");
    global_state.core1_last_loop_pass = (uint32_t)now;
    now += CORE1_HANG_TIMEOUT_US - 1;
    kick_watchdog_task(&global_state);
    CHECK(watchdog_kicks == 1);
    now++;
    kick_watchdog_task(&global_state);
    CHECK(watchdog_kicks == 1);
    global_state.core1_last_loop_pass = (uint32_t)now;
    global_state.reboot_requested = true;
    kick_watchdog_task(&global_state);
    CHECK(watchdog_kicks == 1);
}

/* The peer transport is an explicit boundary here; its separate suites exercise
 * UART behavior. This assertion checks the task publishes before entering it. */
void diagnostic_peer_task(uint64_t now_us) {
    diagnostic_runtime_snapshot_t snapshot = diagnostic_runtime_snapshot();
    CHECK(now_us == now && (snapshot.core_valid & 2) && snapshot.core_ticks[1] != 0);
}

static void diagnostic_task_checkpoints(void) {
    fresh("diagnostic_task_checkpoints_with_console_disabled");
    diagnostic_runtime_snapshot_t initial = diagnostic_runtime_snapshot();
    CHECK(!initial.core_valid && initial.core_age_ms[0] == UINT32_MAX);
    CHECK(initial.core_age_ms[1] == UINT32_MAX);
    owner(0);
    diagnostic_console_task(&global_state);
    now += 2000;
    owner(1);
    diagnostic_peer_status_task(&global_state);
    diagnostic_runtime_snapshot_t snapshot = diagnostic_runtime_snapshot();
    CHECK(snapshot.core_valid == 3 && snapshot.core_ticks[0] == 1 && snapshot.core_ticks[1] == 1);
    CHECK(snapshot.core_age_ms[0] == 2 && snapshot.core_age_ms[1] == 0);
    CHECK(!diagnostic_history_window(HISTORY_CAPACITY).count);
    diagnostic_runtime_checkpoint(2);
    diagnostic_runtime_checkpoint(UINT32_MAX);
    diagnostic_runtime_snapshot_t unchanged = diagnostic_runtime_snapshot();
    CHECK(unchanged.core_valid == 3 && unchanged.core_ticks[0] == 1 && unchanged.core_ticks[1] == 1);

    /* Exercise runtime input rejection separately from the actual updater
       scenarios above. No invalid update observation may refresh progress. */
    firmware_update_lock();
    diagnostic_update_begin(DIAGNOSTIC_SOURCE_PEER, 193);
    diagnostic_update_progress(FLASH_PAGE_SIZE);
    firmware_update_unlock();
    now += 7000;
    firmware_update_lock();
    diagnostic_update_progress(FLASH_PAGE_SIZE);
    diagnostic_update_progress(0);
    diagnostic_update_progress(STAGING_IMAGE_SIZE + 1);
    diagnostic_update_phase(DIAGNOSTIC_UPDATE_PAUSED);
    diagnostic_update_phase(DIAGNOSTIC_UPDATE_PAUSED);
    diagnostic_update_progress(2 * FLASH_PAGE_SIZE);
    firmware_update_unlock();
    snapshot = diagnostic_runtime_snapshot();
    CHECK(snapshot.core_ticks[0] == 1 && snapshot.core_ticks[1] == 1 && snapshot.core_valid == 3);
    CHECK(snapshot.core_age_ms[0] == 9 && snapshot.core_age_ms[1] == 7);
    CHECK(snapshot.received_bytes == FLASH_PAGE_SIZE && snapshot.progress_age_ms == 7);
    CHECK(snapshot.phase == DIAGNOSTIC_UPDATE_PAUSED && snapshot.update_attempt == 1);
    CHECK(diagnostic_history_window(HISTORY_CAPACITY).count == 2);
    expect_history(0, HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 193);
    expect_history(1, HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_PAUSED, DIAGNOSTIC_SOURCE_PEER, 193);
}

int main(int argc, char **argv) {
    if (argc > 1) seed = (uint32_t)strtoul(argv[1], NULL, 0);
    if (!seed) seed = 1;
    replay_seed = seed;
    const char *trace_path = getenv("DESKHOP_STORAGE_TRACE");
    if (trace_path) { trace = fopen(trace_path, "w"); if (!trace) { perror(trace_path); return 2; } }
    uf2_reordering_and_duplicate();
    uf2_reject_invalid_and_mixed();
    actual_peer_transfer(false, true);
    actual_peer_transfer(true, false);
    peer_stall_pause_and_restart();
    peer_update_source_transitions();
    host_peer_config_serializations();
    source_reads_and_metadata();
    config_persistence_and_migration();
    config_set_during_save();
    power_cut_observation();
    watchdog_contract();
    diagnostic_task_checkpoints();
    if (trace) fclose(trace);
    printf("storage production-boundary tests passed (seed=%u; 2 full peer images, shuffled UF2, 6 serializations, 8 power cuts)\n", replay_seed);
    return 0;
}
