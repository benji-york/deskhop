/* Execute production update/config/MSC units against observable NOR/SDK edges. */
#include "main.h"

/* Clipboard runtime is covered by paired tests, outside this storage boundary. */
void clipboard_task(device_t *state) { (void)state; }
#undef memcpy
#include <stdio.h>
#include <setjmp.h>

static const char *scenario;
static uint64_t now;
/* IDs 1/2/3 remain firmware/flash/config; startup then initializes history/runtime. */
static unsigned current_core, lock_owner[6], lock_depth[6], next_lock;
static unsigned interrupts[2], erases, programs, resets, watchdog_kicks;
static bool try_owned[6], verification_call;
static unsigned try_saved_irq[6], try_attempts[6], blocking_entries;
static unsigned verification_copies, verification_bytes;
static uint32_t reset_disable_mask;
static unsigned sector_erases[STORAGE_SIZE / FLASH_SECTOR_SIZE];
static unsigned page_programs[STAGING_PAGES_CNT];
static unsigned active_operation, cut_at_operation, cut_bytes;
static jmp_buf power_cut;
static uint32_t seed = 1, replay_seed = 1;
static FILE *trace;
static unsigned trace_step;
static bool config_set_on_copy, config_set_pending;
static bool batch_flash_order;
static unsigned config_set_invoked;
static bool config_set_active;
static bool config_set_on_read, config_read_on_copy, config_read_active, config_read_pending;
static unsigned config_read_invoked;
static bool config_write_border, config_write_opposite_edge;
/* Exercise the confirmed-save contract at real NOR boundaries. The competing
   operation is the production SET handler, not a direct config assignment. */
static bool config_set_on_settings_program, corrupt_settings_program;
static size_t config_copy_split;
static config_t observed_config;
static jmp_buf config_set_blocked;
static void interleaved_config_set(void);
static void interleaved_config_read(void);
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
    uint64_t idle = UINT64_C(0x0000778899aabbcc);
    if (config_write_opposite_edge) {
        packet.data[0] = 15;
        idle = 6000;
    }
    memcpy(&packet.data[1], &idle, 7);
    config_set_active = true;
    if (setjmp(config_set_blocked) == 0) {
        if (config_write_border) {
            const border_size_t border = {5000, 6000};
            CHECK(config_set_border(&global_state, 0, &border));
        } else handle_api_msgs(&packet, &global_state);
        ++config_set_invoked;
    }
    config_set_active = false;
    current_core = saved_core;
}
static void interleaved_config_read(void) {
    unsigned saved_core = current_core;
    current_core = 1;
    config_read_active = true;
    if (setjmp(config_set_blocked) == 0) {
        config_snapshot(&global_state, &observed_config);
        ++config_read_invoked;
    }
    config_read_active = false;
    current_core = saved_core;
}
void *storage_memcpy(void *destination, const void *source, size_t length) {
    if (verification_call) {
        uintptr_t start = (uintptr_t)storage_flash, address = (uintptr_t)source;
        CHECK(length && length <= FLASH_PAGE_SIZE);
        CHECK(address >= start && address - start <= STAGING_IMAGE_SIZE - length);
        CHECK(lock_depth[1] == 1 && lock_owner[1] == current_core);
        CHECK(lock_depth[2] == 1 && lock_owner[2] == current_core);
        CHECK(interrupts[current_core]);
        CHECK(++verification_copies == 1);
        verification_bytes += (unsigned)length;
    }
    if (((config_set_on_copy && destination == global_state.page_buffer) || config_set_on_read)
        && source == &global_state.config && length == sizeof(config_t)) {
        config_set_on_copy = false;
        config_set_on_read = false;
        /* Preempt the native copy inside one multibyte config field. This
           models a legal copy interleaving; the actual setter must acquire its
           own lock to defer. Without serialization the saved field is torn. */
        size_t split = config_copy_split ? config_copy_split
            : offsetof(config_t, output[0].screensaver.idle_time_us) + 4;
        memcpy(destination, source, split);
        interleaved_config_set();
        memcpy((uint8_t *)destination + split, (const uint8_t *)source + split, length - split);
        return destination;
    }
    if (config_read_on_copy && destination == &global_state.config && length == sizeof(config_t)) {
        config_read_on_copy = false;
        size_t split = config_copy_split ? config_copy_split
            : offsetof(config_t, output[0].screensaver.idle_time_us) + 4;
        memcpy(destination, source, split);
        interleaved_config_read();
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
    ++blocking_entries;
    CHECK(cs->id && cs->id <= 5);
    if (cs->id <= 3) CHECK(!lock_depth[4] && !lock_depth[5]);
    if (cs->id >= 4) {
        /* Updater hooks may hold firmware, but never flash/config or each other. */
        CHECK(!lock_depth[2] && !lock_depth[3]);
        CHECK(!lock_depth[4] && !lock_depth[5]);
        /* The SDK firmware critical section masks IRQs, including try-entry.
           Sparse source END hooks may record history under that existing lock.
           Flash/config and reverse/nested diagnostic locks remain forbidden. */
        CHECK(!interrupts[current_core]
              || (lock_depth[1] == 1 && lock_owner[1] == current_core && try_owned[1]));
    }
    if (lock_depth[cs->id]) {
        /* Run the real setter up to its blocked SDK acquisition, then resume
           that operation from its side-effect-free entry when the lock opens. */
        CHECK((config_set_active || config_read_active) && cs->id == 3 && lock_owner[cs->id] != current_core);
        if (config_set_active) config_set_pending = true;
        if (config_read_active) config_read_pending = true;
        longjmp(config_set_blocked, 1);
    }
    lock_owner[cs->id] = current_core;
    lock_depth[cs->id]++;
}
bool dh_critical_section_try_enter(critical_section_t *cs) {
    CHECK(cs->id && cs->id <= 5);
    ++try_attempts[cs->id];
    uint32_t saved_irq = save_and_disable_interrupts();
    if (lock_depth[cs->id]) {
        restore_interrupts(saved_irq);
        return false;
    }
    CHECK(!lock_depth[4] && !lock_depth[5]);
    lock_owner[cs->id] = current_core;
    lock_depth[cs->id] = 1;
    try_owned[cs->id] = true;
    try_saved_irq[cs->id] = saved_irq;
    return true;
}
void critical_section_exit(critical_section_t *cs) {
    CHECK(lock_depth[cs->id] == 1 && lock_owner[cs->id] == current_core);
    lock_depth[cs->id]--;
    if (try_owned[cs->id]) {
        try_owned[cs->id] = false;
        restore_interrupts(try_saved_irq[cs->id]);
    }
    if (cs->id == 3 && config_set_pending) {
        config_set_pending = false;
        interleaved_config_set();
    }
    if (cs->id == 3 && config_read_pending) {
        config_read_pending = false;
        interleaved_config_read();
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
    if (batch_flash_order && global_state.batch.mode == FW_BATCH_PAGES
        && global_state.fw.source == FW_UPDATE_SOURCE_PULL) {
        /* A complete page burst exceeds the RX ring. The source must not see
           the next request while this receiver is unavailable inside NOR I/O. */
        for (unsigned i = 0; i < global_state.uart_tx_queue.used; ++i) {
            uart_packet_t queued;
            memcpy(&queued, global_state.uart_tx_queue.bytes[i], sizeof(queued));
            CHECK(queued.type != FW_BATCH_PAGE_REQUEST_MSG
                  || queued.data32[1] != global_state.fw.address);
        }
    }
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
    if (offset == STORAGE_CONFIG_OFFSET && config_set_on_settings_program) {
        config_set_on_settings_program = false;
        interleaved_config_set();
        CHECK(config_set_invoked == 1 && !config_set_pending);
        event("config-set-before-settings-program", config_set_invoked);
    }
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
    if (offset == STORAGE_CONFIG_OFFSET && corrupt_settings_program) {
        corrupt_settings_program = false;
        /* Model a silent NOR program failure by clearing an additional set
           bit. Read-back must catch it; the SDK itself has no result code. */
        size_t changed = offsetof(config_t, output[0].speed_x);
        uint8_t value = storage_flash[offset + changed];
        CHECK(value != 0);
        storage_flash[offset + changed] = value & (uint8_t)(value - 1);
        event("settings-program-corrupted", (uint32_t)changed);
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
bool queue_packet_try(const uint8_t *data, enum packet_type_e type, int length) {
    CHECK(length >= 0 && (size_t)length <= sizeof(((uart_packet_t *)0)->data));
    uart_packet_t packet = {.type = type};
    memcpy(packet.data, data, (size_t)length);
    bool admitted = queue_try_add(uart_sink, &packet);
    if (admitted && batch_flash_order && uart_sink == &global_state.uart_tx_queue
        && type == FW_BATCH_PAGE_REQUEST_MSG && packet.data32[1] != 0) {
        /* Observe program completion, not merely entry to its callback. This
           also catches prefetch if a concurrent UART drain hid the queue item. */
        unsigned previous = packet.data32[1] / FLASH_PAGE_SIZE - 1;
        CHECK(previous < STAGING_PAGES_CNT && page_programs[previous] == 1);
        event("batch-next-page-admitted-after-program", packet.data32[1]);
    }
    return admitted;
}
void queue_packet(const uint8_t *data, enum packet_type_e type, int length) {
    /* This storage-boundary call observes publication of the calibrated pair;
       protected UART encoding/delivery remains in the paired simulator. */
    CHECK(queue_packet_try(data, type, length));
}

static void fresh(const char *name) {
    scenario = name;
    batch_flash_order = false;
    config_set_on_copy = config_set_pending = config_set_active = false;
    config_set_invoked = 0;
    config_set_on_read = config_read_on_copy = config_read_active = config_read_pending = false;
    config_read_invoked = 0;
    config_write_border = config_write_opposite_edge = false;
    config_set_on_settings_program = corrupt_settings_program = false;
    config_copy_split = 0;
    event("scenario-start", 0);
    memset(&global_state, 0, sizeof(global_state));
    storage_flash = receiver_flash;
    memset(storage_flash, 0xff, STORAGE_SIZE);
    memset(&source_state, 0, sizeof(source_state));
    source_state.uart_tx_queue.capacity = 256;
    uart_sink = &global_state.uart_tx_queue;
    memset(lock_depth, 0, sizeof(lock_depth));
    memset(try_owned, 0, sizeof(try_owned));
    memset(try_attempts, 0, sizeof(try_attempts));
    verification_call = false;
    verification_copies = verification_bytes = blocking_entries = 0;
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
    /* This harness runs both physical roles against one history store. Keep
       the exact receiver contract while allowing independently checked source
       profiling records to interleave, as two real rings would not. */
    history_window_t window = diagnostic_history_window(HISTORY_CAPACITY);
    unsigned receiver_indices[7], receiver_count = 0;
    CHECK(!window.overwritten);
    for (unsigned i = 0; i < window.count; ++i) {
        history_event_t event;
        CHECK(diagnostic_history_read(window.first_seq + i, &event));
        if (event.type == HISTORY_TRANSFER_SOURCE || event.type == HISTORY_TRANSFER_TIMING
            || event.type == HISTORY_TRANSFER_COUNT)
            continue;
        CHECK(receiver_count < 7);
        receiver_indices[receiver_count++] = i;
    }
    CHECK(receiver_count == 7);
    expect_history(receiver_indices[0], HISTORY_UPDATE_BEGIN, source, 0, target);
    for (unsigned quarter = 1; quarter <= 4; ++quarter)
        expect_history(receiver_indices[quarter], HISTORY_UPDATE_PROGRESS, source, quarter * 25,
                       quarter * STAGING_IMAGE_SIZE / 4);
    expect_history(receiver_indices[5], HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_VALIDATING, source, target);
    expect_history(receiver_indices[6], HISTORY_UPDATE_PHASE, phase, source, target);
}

/* Drive the production burst negotiator/source/receiver/task, not just its
 * pure collector. Storage and queue admission remain the observable oracles. */
static uart_packet_t pop_batch_packet(queue_t *queue, enum packet_type_e type) {
    uart_packet_t packet;
    CHECK(queue_try_remove(queue, &packet));
    CHECK(packet.type == type);
    return packet;
}
static void batch_receive(uart_packet_t packet) {
    owner(1);
    firmware_batch_packet(&packet, &global_state);
    CHECK(!lock_depth[1] && !lock_depth[2]);
}
static void batch_source_request(uart_packet_t packet) {
    owner(1);
    storage_flash = image;
    uart_sink = &source_state.uart_tx_queue;
    firmware_batch_packet(&packet, &source_state);
    storage_flash = receiver_flash;
    uart_sink = &global_state.uart_tx_queue;
    CHECK(!lock_depth[1] && !lock_depth[2]);
}
static uart_packet_t batch_start(uint32_t crc) {
    batch_flash_order = true;
    firmware_batch_init(&global_state, UINT64_C(0x123400));
    firmware_batch_init(&source_state, UINT64_C(0x987600));
    source_state._running_fw = (firmware_metadata_t){
        .magic = FIRMWARE_METADATA_MAGIC, .version = 193, .checksum = crc,
    };
    heartbeat(193, crc, true);
    upgrade_tick();
    CHECK(!programs && !erases && global_state.batch.mode == FW_BATCH_WAIT_CAPS);
    batch_source_request(pop_batch_packet(&global_state.uart_tx_queue, FW_BATCH_CAPS_REQUEST_MSG));
    batch_receive(pop_batch_packet(&source_state.uart_tx_queue, FW_BATCH_CAPS_RESPONSE_MSG));
    CHECK(global_state.batch.mode == FW_BATCH_PAGES);
    upgrade_tick();
    CHECK(!programs && !erases && global_state.fw.request_pending);
    return pop_batch_packet(&global_state.uart_tx_queue, FW_BATCH_PAGE_REQUEST_MSG);
}
static void batch_source_page(uart_packet_t request, uart_packet_t packets[FW_BATCH_WORDS + 1]) {
    batch_source_request(request);
    CHECK(source_state.batch.tx.active);
    owner(0);
    for (unsigned word = 0; word <= FW_BATCH_WORDS; ++word) {
        CHECK(firmware_batch_next_tx(&source_state, &packets[word]));
        CHECK(packets[word].type == (word == FW_BATCH_WORDS ? FW_BATCH_PAGE_END_MSG : FW_BATCH_PAGE_DATA_MSG));
        CHECK(!lock_depth[1] && !lock_depth[2] && !interrupts[current_core]);
    }
    uart_packet_t unused;
    CHECK(!firmware_batch_next_tx(&source_state, &unused));
    CHECK(!source_state.uart_tx_queue.used); /* A page never floods ordinary TX. */
}
static uart_packet_t batch_retry(void) {
    now += FW_UPDATE_RESPONSE_TIMEOUT_US;
    upgrade_tick();
    return pop_batch_packet(&global_state.uart_tx_queue, FW_BATCH_PAGE_REQUEST_MSG);
}
static void batch_expect_uncommitted(uint32_t address, uint32_t checksum, unsigned committed) {
    CHECK(global_state.fw.address == address && global_state.fw.checksum == checksum);
    CHECK(global_state.fw.request_pending && !global_state.fw.byte_done);
    CHECK(programs == committed && !resets && !global_state.reboot_requested);
}

static void batch_complete_unique_page(void) {
    fresh("batch_only_64_unique_words_plus_valid_crc_can_commit");
    uint32_t crc = make_image(image, 193, 0xb1);
    uart_packet_t request = batch_start(crc), packets[FW_BATCH_WORDS + 1];
    CHECK(request.data32[1] == 0);
    batch_source_page(request, packets);
    CHECK(packets[FW_BATCH_WORDS].data32[1] == oracle_crc32(image, FLASH_PAGE_SIZE));
    /* END may arrive first. Repeated words are not distinct progress. */
    batch_receive(packets[FW_BATCH_WORDS]);
    unsigned order[FW_BATCH_WORDS];
    for (unsigned i = 0; i < FW_BATCH_WORDS; ++i) order[i] = i;
    for (unsigned i = FW_BATCH_WORDS - 1; i; --i) {
        unsigned j = random32() % (i + 1), old = order[i]; order[i] = order[j]; order[j] = old;
    }
    for (unsigned i = 0; i < FW_BATCH_WORDS - 1; ++i) {
        batch_receive(packets[order[i]]);
        batch_receive(packets[order[i]]);
        upgrade_tick();
        batch_expect_uncommitted(0, UINT32_MAX, 0);
        CHECK(!erases);
    }
    batch_receive(packets[order[FW_BATCH_WORDS - 1]]);
    CHECK(global_state.fw.address == FLASH_PAGE_SIZE && global_state.fw.byte_done);
    CHECK(global_state.fw.checksum == ~oracle_crc32(image, FLASH_PAGE_SIZE));
    CHECK(!programs && !erases); /* Receiver assembles; task owns NOR commit. */
    global_state.uart_tx_queue.capacity = 0;
    upgrade_tick();
    /* Batch mode commits before attempting to enqueue the next request. Full
       ordinary TX must neither postpone this write nor make retry repeat it. */
    CHECK(programs == 1 && erases == 1 && page_programs[0] == 1);
    CHECK(memcmp(storage_flash, image, FLASH_PAGE_SIZE) == 0);
    fw_upgrade_state_t saved = global_state.fw;
    for (unsigned retry = 0; retry < 4; ++retry) {
        upgrade_tick();
        expect_no_change(saved);
        CHECK(programs == 1 && erases == 1 && page_programs[0] == 1);
    }
    global_state.uart_tx_queue.capacity = 256;
    upgrade_tick();
    CHECK(programs == 1 && erases == 1 && page_programs[0] == 1);
    CHECK(memcmp(storage_flash, image, FLASH_PAGE_SIZE) == 0);
    uart_packet_t next = pop_batch_packet(&global_state.uart_tx_queue, FW_BATCH_PAGE_REQUEST_MSG);
    CHECK(next.data32[1] == FLASH_PAGE_SIZE && next.data32[0] != request.data32[0]);
    saved = global_state.fw;
    for (unsigned i = 0; i <= FW_BATCH_WORDS; ++i) batch_receive(packets[i]);
    upgrade_tick();
    expect_no_change(saved); /* Late whole page must not replay checksum/write. */
    CHECK(programs == 1 && page_programs[0] == 1);
}

static void batch_invalid_pages_and_fallback(void) {
    for (unsigned fault = 0; fault < 4; ++fault) {
        fresh("batch_partial_corrupt_conflicting_and_missing_end_retry");
        uint32_t crc = make_image(image, 193, (uint8_t)(0xb2 + fault));
        uart_packet_t old_request = batch_start(crc), packets[FW_BATCH_WORDS + 1];
        batch_source_page(old_request, packets);
        for (unsigned i = 0; i < FW_BATCH_WORDS; ++i) {
            if (fault == 0 && i == 17) continue;
            batch_receive(packets[i]);
            if (fault == 2 && i == 0) {
                uart_packet_t conflict = packets[i];
                conflict.data[4] ^= 1;
                batch_receive(conflict);
            }
        }
        if (fault != 3) {
            uart_packet_t end = packets[FW_BATCH_WORDS];
            if (fault == 1) end.data32[1] ^= 1;
            batch_receive(end);
        }
        upgrade_tick();
        batch_expect_uncommitted(0, UINT32_MAX, 0);
        CHECK(!erases);
        uart_packet_t retry = batch_retry();
        CHECK(retry.data32[0] != old_request.data32[0] && retry.data32[1] == 0);
        for (unsigned i = 0; i <= FW_BATCH_WORDS; ++i) batch_receive(packets[i]);
        batch_expect_uncommitted(0, UINT32_MAX, 0);
        batch_source_page(retry, packets);
        for (unsigned i = 0; i <= FW_BATCH_WORDS; ++i) batch_receive(packets[i]);
        upgrade_tick();
        CHECK(programs == 1 && erases == 1 && page_programs[0] == 1);
        CHECK(memcmp(storage_flash, image, FLASH_PAGE_SIZE) == 0);
    }

    fresh("batch_retry_exhaustion_restarts_only_uncommitted_page_as_words");
    uint32_t crc = make_image(image, 193, 0xb7);
    uart_packet_t request = batch_start(crc), packets[FW_BATCH_WORDS + 1];
    batch_source_page(request, packets);
    for (unsigned i = 0; i <= FW_BATCH_WORDS; ++i) batch_receive(packets[i]);
    upgrade_tick();
    CHECK(programs == 1);
    request = pop_batch_packet(&global_state.uart_tx_queue, FW_BATCH_PAGE_REQUEST_MSG);
    CHECK(request.data32[1] == FLASH_PAGE_SIZE);
    batch_source_page(request, packets);
    batch_receive(packets[0]);
    request = batch_retry();
    CHECK(request.data32[1] == FLASH_PAGE_SIZE);
    request = batch_retry();
    CHECK(request.data32[1] == FLASH_PAGE_SIZE);
    /* Every retry has its own collector. Leave the final one partly filled
       with page 1 when fallback hits a full ordinary TX queue. */
    batch_source_page(request, packets);
    batch_receive(packets[0]);
    CHECK(memcmp(global_state.page_buffer, image + FLASH_PAGE_SIZE, 4) == 0);
    global_state.uart_tx_queue.capacity = 0;
    now += FW_UPDATE_RESPONSE_TIMEOUT_US;
    upgrade_tick();
    CHECK(global_state.batch.mode == FW_BATCH_LEGACY && programs == 1);
    CHECK(!global_state.fw.byte_done && !global_state.fw.request_pending);
    CHECK(memcmp(storage_flash, image, FLASH_PAGE_SIZE) == 0);
    upgrade_tick();
    CHECK(programs == 1 && page_programs[0] == 1);
    CHECK(memcmp(storage_flash, image, FLASH_PAGE_SIZE) == 0);
    global_state.uart_tx_queue.capacity = 256;
    upgrade_tick();
    CHECK(programs == 1 && page_programs[0] == 1);
    CHECK(memcmp(storage_flash, image, FLASH_PAGE_SIZE) == 0);
    for (unsigned word = 0; word < FW_BATCH_WORDS; ++word) {
        request = pop_request();
        CHECK(request.data32[0] == FLASH_PAGE_SIZE + word * 4);
        /* A late batch frame must not contaminate the legacy page buffer. */
        fw_upgrade_state_t saved = global_state.fw;
        batch_receive(packets[word]);
        expect_no_change(saved);
        response(request.data32[0], image);
        upgrade_tick();
    }
    CHECK(programs == 2 && page_programs[0] == 1 && page_programs[1] == 1);
    CHECK(memcmp(storage_flash, image, 2 * FLASH_PAGE_SIZE) == 0);
}

static void actual_batch_transfer(unsigned failure) {
    fresh(failure == 0 ? "batch_full_image_preserves_settings" : "batch_full_image_crc_or_metadata_failure");
    uint32_t crc = make_image(image, 193, 0xb8);
    config_t settings = global_state.config;
    uint8_t config_sector[FLASH_SECTOR_SIZE];
    for (unsigned i = 0; i < sizeof(config_sector); ++i) config_sector[i] = (uint8_t)(i ^ 0x5a);
    memcpy(storage_flash + STORAGE_CONFIG_OFFSET, config_sector, sizeof(config_sector));
    /* Each individual page remains valid; only final validation rejects these. */
    if (failure == 1) image[33001] ^= 0x20;
    if (failure == 2) image[STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE] ^= 1;
    if (failure == 3) image[STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE + offsetof(firmware_metadata_t, version)] ^= 1;
    uart_packet_t request = batch_start(crc), packets[FW_BATCH_WORDS + 1];
    for (unsigned page = 0; page < STAGING_PAGES_CNT; ++page) {
        CHECK(request.data32[1] == page * FLASH_PAGE_SIZE);
        batch_source_page(request, packets);
        for (unsigned i = 0; i < FW_BATCH_WORDS; ++i) batch_receive(packets[i]);
        CHECK(global_state.fw.address == page * FLASH_PAGE_SIZE);
        CHECK(programs == page); /* 64 DATA frames alone cannot commit. */
        batch_receive(packets[FW_BATCH_WORDS]);
        CHECK(global_state.fw.address == (page + 1) * FLASH_PAGE_SIZE);
        CHECK(programs == page);
        fw_upgrade_state_t completed = global_state.fw;
        batch_receive(packets[FW_BATCH_WORDS]);
        expect_no_change(completed);
        now += 100;
        upgrade_tick();
        CHECK(programs == page + 1 && page_programs[page] == 1);
        if (page + 1 < STAGING_PAGES_CNT)
            request = pop_batch_packet(&global_state.uart_tx_queue, FW_BATCH_PAGE_REQUEST_MSG);
    }
    CHECK(!global_state.uart_tx_queue.used && !global_state.fw.upgrade_in_progress);
    CHECK(memcmp(&settings, &global_state.config, sizeof(settings)) == 0);
    CHECK(memcmp(storage_flash + STORAGE_CONFIG_OFFSET, config_sector, sizeof(config_sector)) == 0);
    CHECK(!sector_erases[STORAGE_CONFIG_OFFSET / FLASH_SECTOR_SIZE]);
    for (unsigned i = 0; i < STAGING_PAGES_CNT; ++i) CHECK(page_programs[i] == 1);
    if (failure) {
        CHECK(resets == 1 && reset_disable_mask == 0 && global_state.fw.image_dirty);
        CHECK(!global_state.reboot_requested);
        for (unsigned i = 0; i < FLASH_SECTOR_SIZE; ++i) CHECK(storage_flash[i] == 0xff);
    } else {
        CHECK(!resets && global_state.reboot_requested && !global_state.fw.image_dirty);
        CHECK(global_state._running_fw.version == 193 && global_state._running_fw.checksum == crc);
        CHECK(memcmp(storage_flash, image, STAGING_IMAGE_SIZE) == 0);
        CHECK(erases == STAGING_IMAGE_SIZE / FLASH_SECTOR_SIZE);
    }
    expect_completed_history(DIAGNOSTIC_SOURCE_PEER, 193, failure != 0);
    CHECK(source_state.batch.profile.finished);
    CHECK(source_state.batch.profile.mode == TRANSFER_MODE_PAGES);
    CHECK(source_state.batch.profile.page_requests == STAGING_PAGES_CNT);
    CHECK(!source_state.batch.profile.word_requests && !source_state.batch.profile.retries);
    /* Four milestones + negotiation/path/end + seven summary rows. The
       receiver shares this test store but has exactly seven separate rows. */
    CHECK(diagnostic_history_window(HISTORY_CAPACITY).count == 21);
}

static unsigned profile_events(history_type_t type, uint8_t kind, history_event_t *last) {
    history_window_t window = diagnostic_history_window(HISTORY_CAPACITY);
    unsigned found = 0;
    CHECK(!window.overwritten);
    for (uint64_t seq = window.first_seq; seq < window.end_seq; ++seq) {
        history_event_t event;
        CHECK(diagnostic_history_read(seq, &event));
        if (event.type == type && event.a == kind) {
            ++found;
            if (last) *last = event;
        }
    }
    return found;
}

static void profile_word(uint32_t address) {
    owner(1);
    firmware_update_lock();
    firmware_source_word_locked(&source_state, address);
    firmware_update_unlock();
}

static void source_profile_boundaries(void) {
    fresh("source_profile_timing_retry_fallback_and_final_snapshot");
    uint32_t crc = make_image(image, 193, 0xb9);
    uart_packet_t request = batch_start(crc), packet;
    uint64_t started = now;
    batch_source_request(request);
    owner(0);
    for (unsigned word = 0; word <= FW_BATCH_WORDS; ++word) {
        now += 10;
        CHECK(firmware_batch_next_tx(&source_state, &packet));
    }
    CHECK(source_state.batch.profile.page_service_us == 650);
    CHECK(source_state.batch.profile.page_max_us == 650);
    now += 200;
    request.data32[0] += FW_BATCH_WORDS;
    batch_source_request(request); /* Same address, newer token = retry. */
    CHECK(source_state.batch.profile.page_gap_us == 200);
    CHECK(source_state.batch.profile.retries == 1);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_RETRY, NULL) == 1);
    fw_source_profile_t saved = source_state.batch.profile;
    batch_source_request(request); /* Same token is rejected, cannot count. */
    CHECK(memcmp(&saved, &source_state.batch.profile, sizeof(saved)) == 0);
    uart_packet_t malformed = request;
    malformed.data32[0] += FW_BATCH_WORDS;
    malformed.data32[1] = 3;
    batch_source_request(malformed);
    CHECK(memcmp(&saved, &source_state.batch.profile, sizeof(saved)) == 0);
    now += 50;
    profile_word(0); /* Fallback cancels profiler's unfinished page interval. */
    CHECK(source_state.batch.profile.mode == TRANSFER_MODE_MIXED);
    CHECK(source_state.batch.profile.page_service_us == 700);
    CHECK(!source_state.batch.profile.page_active);
    saved = source_state.batch.profile;
    profile_word(3);
    profile_word(STAGING_IMAGE_SIZE);
    CHECK(memcmp(&saved, &source_state.batch.profile, sizeof(saved)) == 0);
    now += 100;
    profile_word(STAGING_IMAGE_SIZE - 4);
    CHECK(source_state.batch.profile.finished);
    history_event_t event;
    CHECK(profile_events(HISTORY_TRANSFER_TIMING, TRANSFER_ELAPSED_US, &event) == 1);
    CHECK(event.value == now - started && event.b == TRANSFER_MODE_MIXED);
    CHECK(profile_events(HISTORY_TRANSFER_COUNT, TRANSFER_WORD_REQUESTS, &event) == 1);
    CHECK(event.value == 2); /* Sparse requests cannot claim all words served. */
    saved = source_state.batch.profile;
    profile_word(STAGING_IMAGE_SIZE - 4);
    CHECK(memcmp(&saved, &source_state.batch.profile, sizeof(saved)) == 0);
    now += 100;
    profile_word(0); /* Legacy restart from zero after progress is a new run. */
    CHECK(!source_state.batch.profile.finished && source_state.batch.profile.word_requests == 1);
    CHECK(!source_state.batch.profile.page_requests && source_state.batch.profile.started_us == now);

    fresh("source_profile_final_page_retry_visible_after_summary");
    crc = make_image(image, 193, 0xba);
    request = batch_start(crc);
    request.data32[1] = STAGING_IMAGE_SIZE - FLASH_PAGE_SIZE;
    uart_packet_t packets[FW_BATCH_WORDS + 1];
    batch_source_page(request, packets);
    CHECK(source_state.batch.profile.finished);
    request.data32[0] += FW_BATCH_WORDS;
    batch_source_page(request, packets);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_RETRY, &event) == 1);
    CHECK(event.value == STAGING_IMAGE_SIZE - FLASH_PAGE_SIZE);
    CHECK(profile_events(HISTORY_TRANSFER_COUNT, TRANSFER_PAGE_REQUESTS, &event) == 1);
    CHECK(event.value == 1); /* Snapshot is through first end, not final delivery. */
    profile_word(STAGING_IMAGE_SIZE - FLASH_PAGE_SIZE);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_WORDS_BEGIN, &event) == 1);
    CHECK(event.b == TRANSFER_MODE_MIXED);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_BATCH_END, NULL) == 1);

    fresh("source_profile_source_changes_saturation_and_caps_backpressure");
    crc = make_image(image, 193, 0xbb);
    request = batch_start(crc);
    source_state.batch.profile.page_service_us = UINT32_MAX - 1;
    source_state.batch.profile.page_requests = UINT32_MAX;
    batch_source_request(request);
    owner(0);
    for (unsigned word = 0; word <= FW_BATCH_WORDS; ++word) {
        now += 10;
        CHECK(firmware_batch_next_tx(&source_state, &packet));
    }
    CHECK(source_state.batch.profile.page_service_us == UINT32_MAX);
    CHECK(source_state.batch.profile.page_requests == UINT32_MAX);
    source_state._running_fw.checksum ^= 1;
    CHECK(!firmware_batch_next_tx(&source_state, &packet));
    CHECK(!source_state.batch.profile.active);
    profile_word(0);
    CHECK(source_state.batch.profile.mode == TRANSFER_MODE_WORDS);
    CHECK(source_state.batch.profile.checksum == source_state._running_fw.checksum);
    owner(1);
    firmware_update_lock();
    firmware_batch_begin_locked(&source_state);
    firmware_update_unlock();
    CHECK(!source_state.batch.profile.active);
    uart_packet_t caps = {.type = FW_BATCH_CAPS_REQUEST_MSG};
    CHECK(fw_batch_caps_encode_request(0x1000, 193, caps.data));
    unsigned prior = profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_CAPS_QUEUED, NULL);
    source_state.uart_tx_queue.capacity = 0;
    batch_source_request(caps);
    CHECK(!source_state.batch.profile.active);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_CAPS_QUEUED, NULL) == prior);
    source_state.uart_tx_queue.capacity = 256;
    batch_source_request(caps);
    CHECK(source_state.batch.profile.active && !source_state.batch.profile.mode);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_CAPS_QUEUED, NULL) == prior + 1);
    pop_batch_packet(&source_state.uart_tx_queue, FW_BATCH_CAPS_RESPONSE_MSG);
    saved = source_state.batch.profile;
    batch_source_request(caps); /* Duplicate probe response need not flood ring. */
    CHECK(memcmp(&saved, &source_state.batch.profile, sizeof(saved)) == 0);
    CHECK(profile_events(HISTORY_TRANSFER_SOURCE, TRANSFER_CAPS_QUEUED, NULL) == prior + 1);
}

static void batch_source_ownership(void) {
    for (unsigned condition = 0; condition < 5; ++condition) {
        fresh("batch_source_refuses_conflicting_ownership_and_cancels_burst");
        uint32_t crc = make_image(image, 193, 0xbc);
        uart_packet_t request = batch_start(crc);
        source_state.fw.upgrade_in_progress = condition < 2;
        source_state.fw.source = condition == 0 ? FW_UPDATE_SOURCE_DROP
                                : condition == 1 ? FW_UPDATE_SOURCE_PULL : FW_UPDATE_SOURCE_NONE;
        source_state.fw.image_dirty = condition == 2;
        source_state.maintenance_reserved = condition == 3;
        source_state.reboot_requested = condition == 4;
        batch_source_request(request);
        CHECK(!source_state.batch.tx.active && !source_state.uart_tx_queue.used);
        uart_packet_t caps = {.type = FW_BATCH_CAPS_REQUEST_MSG};
        CHECK(fw_batch_caps_encode_request(request.data32[0] + FW_BATCH_WORDS, 193, caps.data));
        batch_source_request(caps);
        CHECK(!source_state.uart_tx_queue.used);
        source_state.fw = (fw_upgrade_state_t){0};
        source_state.maintenance_reserved = source_state.reboot_requested = false;
        batch_source_request(request);
        CHECK(source_state.batch.tx.active);
        source_state.fw.upgrade_in_progress = condition < 2;
        source_state.fw.image_dirty = condition == 2;
        source_state.maintenance_reserved = condition == 3;
        source_state.reboot_requested = condition == 4;
        uart_packet_t next;
        owner(0);
        CHECK(!firmware_batch_next_tx(&source_state, &next));
        CHECK(!source_state.batch.tx.active && !source_state.batch.source_enabled);
        CHECK(!programs && !erases);
    }
}

static void batch_receiver_ownership(void) {
    for (unsigned condition = 0; condition < 4; ++condition) {
        fresh("batch_receiver_rejects_late_burst_after_owner_change");
        uint32_t crc = make_image(image, 193, 0xbd);
        uart_packet_t request = batch_start(crc), packets[FW_BATCH_WORDS + 1];
        batch_source_page(request, packets);
        batch_receive(packets[0]);
        if (condition == 0) host_block(31, image);
        if (condition == 1) global_state.fw.source = FW_UPDATE_SOURCE_PULL_PAUSED;
        if (condition == 2) global_state.maintenance_reserved = true;
        if (condition == 3) global_state.reboot_requested = true;
        fw_upgrade_state_t saved = global_state.fw;
        uint8_t page[FLASH_PAGE_SIZE];
        memcpy(page, global_state.page_buffer, sizeof(page));
        unsigned old_programs = programs, old_erases = erases;
        for (unsigned i = 0; i <= FW_BATCH_WORDS; ++i) batch_receive(packets[i]);
        expect_no_change(saved);
        CHECK(memcmp(page, global_state.page_buffer, sizeof(page)) == 0);
        CHECK(programs == old_programs && erases == old_erases);
    }
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

    fresh("maintenance_reservation_rejects_update_work_until_release");
    crc = make_image(image, 193, 0x73);
    global_state.maintenance_reserved = true;
    host_block(12, image);
    heartbeat(193, crc, true);
    upgrade_tick();
    CHECK(!programs && !erases && !watchdog_kicks && !resets);
    CHECK(!global_state.reboot_requested && !global_state.fw.upgrade_in_progress);
    CHECK(!global_state.fw.image_dirty && !global_state.uf2_blocks_received_count);
    CHECK(!diagnostic_runtime_snapshot().update_seen);
    CHECK(!diagnostic_history_window(HISTORY_CAPACITY).count);
    global_state.maintenance_reserved = false;
    host_block(12, image);
    CHECK(programs == 1 && erases == 1 && global_state.uf2_blocks_received_count == 1);
    CHECK(global_state.fw.upgrade_in_progress && global_state.fw.image_dirty);
    CHECK(global_state.fw.source == FW_UPDATE_SOURCE_DROP);
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
    global_state.config.output[0].screensaver.idle_time_us = UINT64_C(0x0000223344556677);
    config_set_on_copy = true;
    save_config(&global_state);
    CHECK(config_set_invoked == 1);
    config_t persisted;
    memcpy(&persisted, ADDR_CONFIG, sizeof(persisted));
    CHECK(persisted.checksum == oracle_crc32((const uint8_t *)&persisted, offsetof(config_t, checksum)));
    CHECK(persisted.output[0].screensaver.idle_time_us == UINT64_C(0x0000223344556677)
          || persisted.output[0].screensaver.idle_time_us == UINT64_C(0x0000778899aabbcc));
    CHECK(global_state.config.output[0].screensaver.idle_time_us == UINT64_C(0x0000778899aabbcc));
    /* The later SET was not lost: a subsequent explicit save persists it. */
    save_config(&global_state);
    memset(&global_state.config, 0, sizeof(config_t));
    load_config(&global_state);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == UINT64_C(0x0000778899aabbcc));
}

static config_t customized_config(void) {
    config_t config = default_config;
    config.force_mouse_boot_mode = 1;
    config.force_kbd_boot_protocol = 1;
    config.kbd_led_as_indicator = 0;
    config.hotkey_toggle = 0xa5;
    config.enable_acceleration = 0;
    config.enforce_ports = 0;
    config.jump_threshold = 987;
    config.screensaver_system_timeout_sec = 123456789;
    for (unsigned i = 0; i < 2; ++i) {
        config.output[i].screen_count = 7 + i;
        config.output[i].screen_index = 3 + i;
        config.output[i].speed_x = 47 + i;
        config.output[i].speed_y = 59 + i;
        config.output[i].border = (border_size_t){901 + (int)i, 31000 - (int)i};
        config.output[i].os = i ? LINUX : MACOS;
        config.output[i].mouse_park_pos = 3;
        config.output[i].screensaver.mode = i ? PONG : DISABLED;
        config.output[i].screensaver.only_if_inactive = 1;
        config.output[i].screensaver.idle_time_us = UINT64_C(7200000000) + i;
        config.output[i].screensaver.max_time_us = UINT64_C(281474976710655) - i;
    }
    return config;
}
static void install_config(config_t *config) {
    config->checksum = oracle_crc32((const uint8_t *)config, offsetof(config_t, checksum));
    memcpy(storage_flash + STORAGE_CONFIG_OFFSET, config, sizeof(*config));
}
static void same_settings(const config_t *left, const config_t *right) {
    CHECK(memcmp(left, right, offsetof(config_t, checksum)) == 0);
}
static void config_semantic_persistence(void) {
    /* Every scalar with an invalid representable storage value is repaired
       independently. Full-width byte/uint16/uint32 fields have no such value. */
    /* Locate the actual persisted members, not the production field map.
       A mistaken API mapping must not move both fixture and repair oracle. */
#define INVALID(MEMBER, VALUE) {offsetof(config_t, MEMBER), sizeof(((config_t *)0)->MEMBER), VALUE}
    static const struct { size_t offset, width; uint64_t invalid; } invalid[] = {
        INVALID(output[0].screen_count, 0), INVALID(output[0].speed_x, 0),
        INVALID(output[0].speed_y, 129), INVALID(output[0].border.top, UINT32_MAX),
        INVALID(output[0].border.bottom, 32768), INVALID(output[0].os, 0),
        INVALID(output[0].pos, 3), INVALID(output[0].mouse_park_pos, 2),
        INVALID(output[0].screensaver.mode, 3), INVALID(output[0].screensaver.only_if_inactive, 2),
        INVALID(output[1].screen_count, UINT32_MAX), INVALID(output[1].speed_x, UINT32_MAX),
        INVALID(output[1].speed_y, 129), INVALID(output[1].border.top, 32768),
        INVALID(output[1].border.bottom, UINT32_MAX), INVALID(output[1].os, 5),
        INVALID(output[1].pos, 0), INVALID(output[1].mouse_park_pos, 2),
        INVALID(output[1].screensaver.mode, 255), INVALID(output[1].screensaver.only_if_inactive, 255),
        INVALID(force_mouse_boot_mode, 2), INVALID(force_kbd_boot_protocol, 2),
        INVALID(kbd_led_as_indicator, 255), INVALID(enable_acceleration, 255),
        INVALID(enforce_ports, 255),
    };
#undef INVALID
    for (unsigned test = 0; test < sizeof(invalid) / sizeof(invalid[0]); ++test) {
        fresh("valid_crc_invalid_scalar_repairs_only_affected_setting");
        config_t stored = customized_config(), expected = stored;
        size_t offset = invalid[test].offset, size = invalid[test].width;
        memcpy((uint8_t *)&stored + offset, &invalid[test].invalid, size);
        memcpy((uint8_t *)&expected + offset, (const uint8_t *)&default_config + offset, size);
        for (unsigned output = 0; output < 2; ++output) {
            if (expected.output[output].screen_index > expected.output[output].screen_count)
                expected.output[output].screen_index = expected.output[output].screen_count;
        }
        install_config(&stored);
        global_state.config = stored;
        save_config(&global_state);
        CHECK(!programs && !erases);
        CHECK(memcmp(&global_state.config, &stored, sizeof(stored)) == 0);
        load_config(&global_state);
        same_settings(&global_state.config, &expected);
        CHECK(!programs && !erases); /* Current format repairs are RAM-only. */
        CHECK(memcmp(ADDR_CONFIG, &stored, sizeof(stored)) == 0);
        save_config(&global_state);
        CHECK(programs == 1 && erases == 1);
        config_t saved;
        memcpy(&saved, ADDR_CONFIG, sizeof(saved));
        CHECK(saved.checksum == oracle_crc32((const uint8_t *)&saved, offsetof(config_t, checksum)));
        same_settings(&saved, &expected);
        memset(&global_state.config, 0, sizeof(config_t));
        load_config(&global_state);
        same_settings(&global_state.config, &expected);
    }
    for (unsigned version = 8; version <= 10; ++version) {
        fresh("identity_border_and_runtime_index_repair_after_migration");
        config_t stored = customized_config(), expected = stored;
        stored.version = version;
        stored.output[0].number = 3;
        stored.output[1].number = UINT32_MAX;
        stored.output[0].border = (border_size_t){16384, 16384};
        stored.output[1].border = (border_size_t){25000, 12000};
        stored.output[0].screen_index = 0;
        stored.output[1].screen_index = UINT32_MAX;
        expected.output[0].border = default_config.output[0].border;
        expected.output[1].border = default_config.output[1].border;
        expected.output[0].screen_index = 1;
        expected.output[1].screen_index = expected.output[1].screen_count;
        if (version < 10) {
            expected.output[0].screensaver.mode = JITTER;
            expected.output[1].screensaver.mode = JITTER;
        }
        if (version == 8) expected.screensaver_system_timeout_sec = SCREENSAVER_SYSTEM_TIMEOUT_SEC;
        install_config(&stored);
        load_config(&global_state);
        same_settings(&global_state.config, &expected);
        CHECK(programs == (version < 10 ? 1u : 0u));
        if (version < 10) same_settings(ADDR_CONFIG, &expected);
        else CHECK(memcmp(ADDR_CONFIG, &stored, sizeof(stored)) == 0);
    }
    fresh("save_refuses_invalid_snapshot_without_flash_or_ram_mutation");
    global_state.config = customized_config();
    save_config(&global_state);
    uint8_t sector[FLASH_SECTOR_SIZE];
    memcpy(sector, ADDR_CONFIG, sizeof(sector));
    global_state.config.output[0].border = (border_size_t){16384, 16384};
    config_t invalid_ram = global_state.config;
    save_config(&global_state);
    CHECK(programs == 1 && erases == 1);
    CHECK(memcmp(sector, ADDR_CONFIG, sizeof(sector)) == 0);
    CHECK(memcmp(&invalid_ram, &global_state.config, sizeof(invalid_ram)) == 0);
    const unsigned bad_versions[] = {0, 7, 11, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(bad_versions) / sizeof(bad_versions[0]); ++i) {
        fresh("unsupported_persisted_version_uses_defaults");
        config_t stored = customized_config();
        stored.version = bad_versions[i];
        install_config(&stored);
        load_config(&global_state);
        same_settings(&global_state.config, &default_config);
        CHECK(!programs && !erases);
    }
    fresh("valid_crc_bad_magic_uses_defaults");
    config_t wrong_magic = customized_config();
    wrong_magic.magic_header ^= 1;
    install_config(&wrong_magic);
    load_config(&global_state);
    same_settings(&global_state.config, &default_config);
    CHECK(!programs && !erases);
}
static void config_publication_schedules(void) {
    fresh("set_preempts_real_reader_snapshot_mid_timeout");
    const uint64_t before = UINT64_C(0x0000223344556677);
    const uint64_t after = UINT64_C(0x0000778899aabbcc);
    global_state.config.output[0].screensaver.idle_time_us = before;
    config_set_on_read = true;
    config_t snapshot;
    config_snapshot(&global_state, &snapshot);
    CHECK(config_set_invoked == 1);
    CHECK(snapshot.output[0].screensaver.idle_time_us == before);
    CHECK(global_state.config.output[0].screensaver.idle_time_us == after);
    fresh("reader_preempts_set_publication_mid_timeout");
    global_state.config.output[0].screensaver.idle_time_us = before;
    config_read_on_copy = true;
    uart_packet_t packet = {.type = SET_VAL_MSG, .data = {21}};
    memcpy(packet.data + 1, &after, 7);
    handle_api_msgs(&packet, &global_state);
    CHECK(config_read_invoked == 1);
    CHECK(observed_config.output[0].screensaver.idle_time_us == after);
    same_settings(&observed_config, &global_state.config);
    fresh("reader_preempts_load_publication_mid_timeout");
    global_state.config.output[0].screensaver.idle_time_us = before;
    config_t stored = customized_config();
    install_config(&stored);
    config_read_on_copy = true;
    load_config(&global_state);
    CHECK(config_read_invoked == 1);
    same_settings(&observed_config, &stored);
    fresh("border_pair_writer_preempts_reader_between_top_and_bottom");
    global_state.config.output[0].border = (border_size_t){100, 1000};
    config_set_on_read = config_write_border = true;
    config_copy_split = offsetof(config_t, output[0].border.bottom);
    config_snapshot(&global_state, &snapshot);
    CHECK(config_set_invoked == 1);
    CHECK(snapshot.output[0].border.top == 100 && snapshot.output[0].border.bottom == 1000);
    CHECK(global_state.config.output[0].border.top == 5000 && global_state.config.output[0].border.bottom == 6000);
    fresh("reader_preempts_border_pair_publication_between_top_and_bottom");
    global_state.config.output[0].border = (border_size_t){100, 1000};
    config_read_on_copy = true;
    config_copy_split = offsetof(config_t, output[0].border.bottom);
    const border_size_t border = {5000, 6000};
    CHECK(config_set_border(&global_state, 0, &border));
    CHECK(config_read_invoked == 1);
    CHECK(observed_config.output[0].border.top == 5000 && observed_config.output[0].border.bottom == 6000);
    fresh("calibration_preserves_concurrent_opposite_edge_set");
    global_state.active_output = global_state.board_role = 0;
    global_state.pointer_y = 200;
    global_state.config.output[0].border = (border_size_t){100, 30000};
    config_set_on_read = config_write_opposite_edge = true;
    screen_border_hotkey_handler(&global_state, NULL);
    CHECK(config_set_invoked == 1);
    CHECK(global_state.config.output[0].border.top == 200);
    CHECK(global_state.config.output[0].border.bottom == 6000);
    config_t calibrated;
    memcpy(&calibrated, ADDR_CONFIG, sizeof(calibrated));
    CHECK(calibrated.output[0].border.top == 200 && calibrated.output[0].border.bottom == 6000);
    CHECK(calibrated.checksum == oracle_crc32((const uint8_t *)&calibrated, offsetof(config_t, checksum)));
    fresh("stale_consumer_index_publication_after_count_reduction");
    global_state.config.output[0].screen_count = 10;
    global_state.config.output[0].screen_index = 7;
    config_snapshot(&global_state, &snapshot);
    packet = (uart_packet_t){.type = SET_VAL_MSG, .data = {11, 2}};
    handle_api_msgs(&packet, &global_state);
    config_set_screen_index(&global_state, 0, snapshot.output[0].screen_index + 1);
    CHECK(global_state.config.output[0].screen_count == 2);
    CHECK(global_state.config.output[0].screen_index == 2);
}

static void config_full_width_legacy_timers(void) {
    /* SET's 48-bit transport limit must not become a persisted-data limit.
       Exercise all four fields across current and migrated formats. */
    const uint64_t durations[] = {
        UINT64_C(281474976710656), UINT64_C(0x00ffffffffffffff),
        UINT64_C(0x8000000000000000), UINT64_MAX,
    };
    for (unsigned version = 8; version <= 10; ++version) {
        fresh("full64_legacy_timers_preserved_on_load_edit_save_and_reload");
        config_t stored = customized_config(), expected = stored;
        for (unsigned output = 0; output < 2; ++output) {
            stored.output[output].screensaver.idle_time_us = durations[output * 2];
            stored.output[output].screensaver.max_time_us = durations[output * 2 + 1];
            expected.output[output].screensaver = stored.output[output].screensaver;
            if (version < 10) expected.output[output].screensaver.mode = JITTER;
        }
        stored.version = version;
        if (version == 8) expected.screensaver_system_timeout_sec = SCREENSAVER_SYSTEM_TIMEOUT_SEC;
        install_config(&stored);
        load_config(&global_state);
        same_settings(&global_state.config, &expected);
        CHECK(programs == (version < 10 ? 1u : 0u));
        CHECK(erases == programs);
        /* Unrelated valid edit must neither be rejected nor wipe the timers. */
        uart_packet_t packet = {.type = SET_VAL_MSG, .data = {12, 63}};
        handle_api_msgs(&packet, &global_state);
        expected.output[0].speed_x = 63;
        same_settings(&global_state.config, &expected);
        save_config(&global_state);
        config_t saved;
        memcpy(&saved, ADDR_CONFIG, sizeof(saved));
        CHECK(saved.checksum == oracle_crc32((const uint8_t *)&saved, offsetof(config_t, checksum)));
        same_settings(&saved, &expected);
        memset(&global_state.config, 0, sizeof(config_t));
        load_config(&global_state);
        same_settings(&global_state.config, &expected);
        /* An explicit narrow timer SET replaces all eight bytes by request. */
        packet = (uart_packet_t){.type = SET_VAL_MSG, .data = {21, 1}};
        handle_api_msgs(&packet, &global_state);
        expected.output[0].screensaver.idle_time_us = 1;
        same_settings(&global_state.config, &expected);
    }
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

/* Probe the actual guard API; only its one-read lock primitive is modeled.
   Existing locks owned by either core and both IRQ masks must survive a call. */
static firmware_verify_io_t verification_io(unsigned operation, uint64_t *generation,
                                             uint32_t offset, uint8_t *destination, size_t length,
                                             firmware_metadata_t *metadata) {
    unsigned depths[6], owners[6], irq[2], blocked = blocking_entries;
    memcpy(depths, lock_depth, sizeof(depths));
    memcpy(owners, lock_owner, sizeof(owners));
    memcpy(irq, interrupts, sizeof(irq));
    verification_copies = verification_bytes = 0;
    verification_call = true;
    firmware_verify_io_t result;
    switch (operation) {
    case 0: result = firmware_verify_try_start(generation, metadata); break;
    case 1: result = firmware_verify_try_read(*generation, offset, destination, length); break;
    default: result = firmware_verify_try_finish(*generation, metadata); break;
    }
    verification_call = false;
    CHECK(blocking_entries == blocked);
    CHECK(memcmp(depths, lock_depth, sizeof(depths)) == 0);
    CHECK(memcmp(irq, interrupts, sizeof(irq)) == 0);
    for (unsigned id = 1; id <= 5; ++id)
        if (depths[id]) CHECK(owners[id] == lock_owner[id]);
    CHECK(verification_copies <= 1 && verification_bytes <= FLASH_PAGE_SIZE);
    if (result != FIRMWARE_VERIFY_OK) CHECK(!verification_copies && !verification_bytes);
    return result;
}

static void verify_full_slot_reads(void) {
    fresh("verify_full_slot_with_live_metadata");
    uint32_t payload_crc = make_image(image, 201, 0xa1);
    memcpy(storage_flash, image, STAGING_IMAGE_SIZE);
    /* The updater's mutable RAM metadata is deliberately unrelated. */
    global_state._running_fw = (firmware_metadata_t){.magic=0xf00d, .version=202, .checksum=0x12345678};
    uint64_t generation = UINT64_MAX;
    firmware_metadata_t metadata, final;
    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    CHECK(verification_bytes == sizeof(metadata) && generation != UINT64_MAX);
    CHECK(metadata.magic == FIRMWARE_METADATA_MAGIC && metadata.version == 201);
    CHECK(metadata.checksum == payload_crc);
    uint8_t page[FLASH_PAGE_SIZE];
    uint32_t crc = UINT32_MAX;
    unsigned pages = 0;
    for (uint32_t offset = 0; offset < STAGING_IMAGE_SIZE; offset += sizeof(page)) {
        CHECK(verification_io(1, &generation, offset, page, sizeof(page), NULL) == FIRMWARE_VERIFY_OK);
        CHECK(verification_bytes == sizeof(page));
        CHECK(memcmp(page, image + offset, sizeof(page)) == 0);
        /* Incremental production CRC runs after both guard locks have closed. */
        CHECK(!lock_depth[1] && !lock_depth[2]);
        for (unsigned byte = 0; byte < sizeof(page); ++byte) crc = crc32_iter(crc, page[byte]);
        ++pages;
        now += 1000;
    }
    CHECK(pages == 1024);
    CHECK(verification_io(2, &generation, 0, NULL, 0, &final) == FIRMWARE_VERIFY_OK);
    CHECK(memcmp(&metadata, &final, sizeof(metadata)) == 0);
    CHECK(~crc == oracle_crc32(image, STAGING_IMAGE_SIZE));
    CHECK(~crc != payload_crc); /* Full-slot CRC includes the metadata sector. */
}

static void verify_nonblocking_and_bounds(void) {
    fresh("verify_one_shot_locks_and_range_rejection");
    make_image(image, 201, 0xa2);
    memcpy(storage_flash, image, STAGING_IMAGE_SIZE);
    uint64_t generation = UINT64_MAX;
    firmware_metadata_t metadata, saved;
    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    uint8_t page[FLASH_PAGE_SIZE], old_page[FLASH_PAGE_SIZE];
    memset(page, 0x7d, sizeof(page));
    memcpy(old_page, page, sizeof(page));
    saved = metadata;

    for (unsigned held = 1; held <= 2; ++held) {
        /* Model the other core owning firmware or flash before this tick. */
        lock_depth[held] = 1; lock_owner[held] = 1;
        for (unsigned operation = 0; operation < 3; ++operation) {
            unsigned fw_attempts = try_attempts[1], flash_attempts = try_attempts[2];
            uint64_t old_generation = generation;
            CHECK(verification_io(operation, &generation, 0, page, sizeof(page), &metadata)
                  == FIRMWARE_VERIFY_BUSY);
            CHECK(try_attempts[1] == fw_attempts + 1);
            CHECK(try_attempts[2] == flash_attempts + (held == 2));
            CHECK(generation == old_generation && memcmp(&metadata, &saved, sizeof(metadata)) == 0);
            CHECK(memcmp(page, old_page, sizeof(page)) == 0);
        }
        lock_depth[held] = 0;
    }
    /* An existing IRQ mask remains disabled after successful and refused tries. */
    interrupts[0] = 1;
    CHECK(verification_io(1, &generation, 0, page, sizeof(page), NULL) == FIRMWARE_VERIFY_OK);
    lock_depth[2] = 1; lock_owner[2] = 1;
    CHECK(verification_io(1, &generation, 0, page, sizeof(page), NULL) == FIRMWARE_VERIFY_BUSY);
    lock_depth[2] = 0; interrupts[0] = 0;

    /* A RAM-only config lock is unrelated to the actual flash snapshot. */
    lock_depth[3] = 1; lock_owner[3] = 1;
    CHECK(verification_io(1, &generation, 0, page, sizeof(page), NULL) == FIRMWARE_VERIFY_OK);
    lock_depth[3] = 0;
    CHECK(verification_io(0, NULL, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_BAD_ARGUMENT);
    CHECK(verification_io(0, &generation, 0, NULL, 0, NULL) == FIRMWARE_VERIFY_BAD_ARGUMENT);
    CHECK(verification_io(2, &generation, 0, NULL, 0, NULL) == FIRMWARE_VERIFY_BAD_ARGUMENT);
    const struct { uint32_t offset; size_t length; } invalid[] = {
        {0, 0}, {0, FLASH_PAGE_SIZE + 1}, {0, SIZE_MAX},
        {STAGING_IMAGE_SIZE, 1}, {STAGING_IMAGE_SIZE - 1, 2}, {UINT32_MAX, 1},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(verification_io(1, &generation, invalid[i].offset, page, invalid[i].length, NULL)
              == FIRMWARE_VERIFY_BAD_ARGUMENT);
    CHECK(verification_io(1, &generation, 0, NULL, FLASH_PAGE_SIZE, NULL) == FIRMWARE_VERIFY_BAD_ARGUMENT);
    CHECK(verification_io(1, &generation, STAGING_IMAGE_SIZE - 1, page, 1, NULL) == FIRMWARE_VERIFY_OK);
    CHECK(page[0] == image[STAGING_IMAGE_SIZE - 1]);
    uint64_t wrong_generation = generation + 1;
    CHECK(verification_io(1, &wrong_generation, 0, page, sizeof(page), NULL) == FIRMWARE_VERIFY_CHANGED);
    CHECK(verification_io(2, &wrong_generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_CHANGED);

    for (unsigned condition = 0; condition < 3; ++condition) {
        global_state.fw.upgrade_in_progress = condition == 0;
        global_state.fw.image_dirty = condition == 1;
        global_state.reboot_requested = condition == 2;
        for (unsigned operation = 0; operation < 3; ++operation) {
            unsigned attempted = try_attempts[2];
            CHECK(verification_io(operation, &generation, 0, page, sizeof(page), &metadata)
                  == FIRMWARE_VERIFY_UPDATE_ACTIVE);
            CHECK(try_attempts[2] == attempted); /* No reason to try flash yet. */
        }
    }
    global_state.reboot_requested = false;
}

static void verify_flash_mutation_generations(void) {
    fresh("verify_generation_invalidation_including_identical_and_metadata_writes");
    make_image(image, 201, 0xa3);
    memcpy(storage_flash, image, STAGING_IMAGE_SIZE);
    uint64_t generation;
    firmware_metadata_t metadata;
    uint8_t page[FLASH_PAGE_SIZE];

    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    CHECK(verification_io(1, &generation, 0, page, sizeof(page), NULL) == FIRMWARE_VERIFY_OK);
    /* An already-read page is programmed with exactly the same bytes. The
       generation still expires even though a CRC-only ABA check would pass. */
    owner(1);
    write_flash_page_erasing(0, page, false);
    owner(0);
    CHECK(verification_io(1, &generation, FLASH_PAGE_SIZE, page, sizeof(page), NULL) == FIRMWARE_VERIFY_CHANGED);
    CHECK(verification_io(2, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_CHANGED);
    CHECK(memcmp(storage_flash, image, STAGING_IMAGE_SIZE) == 0);

    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    uint32_t last_page = STAGING_IMAGE_SIZE - FLASH_PAGE_SIZE;
    memcpy(page, image + last_page, sizeof(page));
    page[0] = 0; /* Reserved metadata bytes are still part of the verified slot. */
    uint32_t before = oracle_crc32(storage_flash, STAGING_IMAGE_SIZE);
    owner(1);
    write_flash_page_erasing(last_page, page, false);
    owner(0);
    CHECK(verification_io(2, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_CHANGED);
    CHECK(oracle_crc32(storage_flash, STAGING_IMAGE_SIZE) != before);

    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    before = oracle_crc32(storage_flash, STAGING_IMAGE_SIZE);
    /* Settings and unused staging writes share flash exclusion but do not
       overlap the running slot, so they cannot expire its generation. */
    owner(1);
    memset(page, 0xa5, sizeof(page));
    write_flash_page(STAGING_IMAGE_SIZE, page);
    owner(0);
    save_config(&global_state);
    wipe_config();
    CHECK(verification_io(2, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    CHECK(oracle_crc32(storage_flash, STAGING_IMAGE_SIZE) == before);

    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    owner(1);
    diagnostic_update_begin(DIAGNOSTIC_SOURCE_PEER, 201);
    diagnostic_update_phase(DIAGNOSTIC_UPDATE_FAILED);
    enter_firmware_recovery();
    owner(0);
    CHECK(resets == 1);
    CHECK(verification_io(2, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_CHANGED);
}

/* run.py builds a separate fixture with only the private startup generation
   initialized near exhaustion. The actual guard and mutation logic is unchanged;
   no test setter or configurable generation is shipped in production. */
static void verify_generation_saturation(void) {
    fresh("verify_generation_saturates_instead_of_wrapping");
    make_image(image, 201, 0xa4);
    memcpy(storage_flash, image, STAGING_IMAGE_SIZE);
    uint64_t generation;
    firmware_metadata_t metadata;
    CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_OK);
    CHECK(generation == UINT64_MAX - 1);
    for (unsigned write = 0; write < 3; ++write) {
        write_flash_page_erasing(0, image, false);
        CHECK(verification_io(2, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_CHANGED);
        CHECK(verification_io(0, &generation, 0, NULL, 0, &metadata) == FIRMWARE_VERIFY_CHANGED);
        CHECK(generation == UINT64_MAX - 1);
    }
}

#include "test_confirmed_config.h"

int main(int argc, char **argv) {
    if (argc > 1) seed = (uint32_t)strtoul(argv[1], NULL, 0);
    if (!seed) seed = 1;
    replay_seed = seed;
    const char *trace_path = getenv("DESKHOP_STORAGE_TRACE");
    if (trace_path) { trace = fopen(trace_path, "w"); if (!trace) { perror(trace_path); return 2; } }
    if (argc > 2 && strcmp(argv[2], "generation-saturation") == 0) {
        verify_generation_saturation();
        if (trace) fclose(trace);
        puts("storage verification generation saturation fixture passed");
        return 0;
    }
    uf2_reordering_and_duplicate();
    uf2_reject_invalid_and_mixed();
    actual_peer_transfer(false, true);
    actual_peer_transfer(true, false);
    peer_stall_pause_and_restart();
    peer_update_source_transitions();
    host_peer_config_serializations();
    source_reads_and_metadata();
    batch_complete_unique_page();
    batch_invalid_pages_and_fallback();
    batch_source_ownership();
    batch_receiver_ownership();
    for (unsigned failure = 0; failure < 4; ++failure) actual_batch_transfer(failure);
    source_profile_boundaries();
    config_persistence_and_migration();
    config_set_during_save();
    config_semantic_persistence();
    config_publication_schedules();
    config_full_width_legacy_timers();
    confirmed_config_persistence();
    power_cut_observation();
    watchdog_contract();
    diagnostic_task_checkpoints();
    verify_full_slot_reads();
    verify_nonblocking_and_bounds();
    verify_flash_mutation_generations();
    if (trace) fclose(trace);
    printf("storage production-boundary tests passed (seed=%u; 2 word/4 batch images, batch faults/ownership/fallback, shuffled UF2, 6 serializations, confirmed config persistence/readback/conflicts, 8 power cuts)\n", replay_seed);
    return 0;
}
