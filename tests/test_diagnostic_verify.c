/* Exact post-scan scheduling seams: production scanner, peer protocol and SDK
 * queues, with deterministic firmware-I/O boundary responses. Real flash-lock
 * acquisition is separately covered by storage tests and paired simulation. */
#include <assert.h>
#include <stdio.h>
#include "../src/diagnostic_verify.c"

static uint64_t clock_us, generation;
static unsigned finish_calls, transmitted;
static firmware_verify_io_t guard_status;
static firmware_metadata_t current_metadata;

uint next_striped_spin_lock_num(void) { return 0; }
uint32_t spin_lock_blocking(spin_lock_t *lock) { assert(!lock->held); lock->held = 1; return 0; }
void spin_unlock(spin_lock_t *lock, uint32_t saved) { (void)saved; assert(lock->held); lock->held = 0; }
void lock_internal_spin_unlock_with_notify(lock_core_t *lock, uint32_t saved) { spin_unlock(lock->spin_lock, saved); }
void lock_internal_spin_unlock_with_wait(lock_core_t *lock, uint32_t saved) {
    (void)lock; (void)saved; assert(!"scanner must not use a blocking queue wait");
}
diagnostic_runtime_snapshot_t diagnostic_runtime_snapshot(void) {
    return (diagnostic_runtime_snapshot_t){.core_valid = 3,
        .core_ticks = {(uint32_t)(clock_us / 1000), (uint32_t)(clock_us / 1000)},
        .total_bytes = VERIFY_IMAGE_BYTES};
}
bool diagnostic_peer_tx_try(uint8_t type, const uint8_t payload[8], uint64_t now_us) {
    (void)type; (void)payload; assert(now_us == clock_us); ++transmitted; return true;
}
/* The checksum is not under test here; supply ordinary CRC32 for zero-filled
 * pages so the actual scanner has to process all 1024 chunks before completion. */
uint32_t crc32_iter(uint32_t crc, const uint8_t byte) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    return crc;
}
firmware_verify_io_t firmware_verify_try_start(uint64_t *out, firmware_metadata_t *metadata) {
    if (guard_status != FIRMWARE_VERIFY_OK) return guard_status;
    *out = generation; *metadata = current_metadata; return FIRMWARE_VERIFY_OK;
}
firmware_verify_io_t firmware_verify_try_read(uint64_t expected, uint32_t offset,
                                             uint8_t *destination, size_t length) {
    assert(offset < VERIFY_IMAGE_BYTES && length == VERIFY_CHUNK_BYTES);
    if (guard_status != FIRMWARE_VERIFY_OK) return guard_status;
    if (expected != generation) return FIRMWARE_VERIFY_CHANGED;
    memset(destination, 0, length); return FIRMWARE_VERIFY_OK;
}
firmware_verify_io_t firmware_verify_try_finish(uint64_t expected, firmware_metadata_t *metadata) {
    ++finish_calls;
    if (guard_status != FIRMWARE_VERIFY_OK) return guard_status;
    if (expected != generation) return FIRMWARE_VERIFY_CHANGED;
    *metadata = current_metadata; return FIRMWARE_VERIFY_OK;
}

static void fresh(void) {
    clock_us = 1000; generation = 7; finish_calls = transmitted = 0;
    guard_status = FIRMWARE_VERIFY_OK;
    current_metadata = (firmware_metadata_t){.magic = 0xf00d, .version = 201, .checksum = 0x12345678};
    peer_status_snapshot_t id = {.role = 0, .major = 0, .minor = 101,
        .boot_session = 1, .image_crc_at_boot = current_metadata.checksum};
    diagnostic_verify_init(&id);
}
static void tick(uint64_t now_us) {
    assert(now_us >= clock_us); clock_us = now_us;
    unsigned before = finish_calls;
    diagnostic_verify_task(clock_us);
    assert(finish_calls - before <= 2); // no retry loop in one task call
}
static void complete_scan(bool remote) {
    scan_start(remote, 123, clock_us);
    for (unsigned count = 0; !scanner.ready; ++count) {
        assert(count < 1030);
        clock_us += 1000;
        scan_task(clock_us);
    }
    assert(scanner.snapshot.outcome == VERIFY_SCAN_COMPLETE);
    assert(scanner.snapshot.bytes_read == VERIFY_IMAGE_BYTES);
}
static void unchanged(const verify_snapshot_t *snapshot, const verify_snapshot_t *expected) {
    assert(memcmp(snapshot, expected, sizeof(*snapshot)) == 0);
}
static void assert_original_interval(const verify_snapshot_t *snapshot, const verify_snapshot_t *expected) {
    assert(snapshot->started_us == expected->started_us && snapshot->completed_us == expected->completed_us);
    assert(snapshot->bytes_read == expected->bytes_read && snapshot->slot_crc32 == expected->slot_crc32);
}

static void local_transient_and_queue_backpressure(void) {
    fresh(); complete_scan(false);
    verify_snapshot_t original = scanner.snapshot;
    guard_status = FIRMWARE_VERIFY_BUSY;
    for (unsigned i = 0; i < 4; ++i) {
        tick(clock_us + 1000);
        assert(scanner.owner == SCAN_LOCAL && !local_result_pending);
        unchanged(&scanner.snapshot, &original);
    }
    guard_status = FIRMWARE_VERIFY_OK;
    tick(clock_us + 1000);
    assert(scanner.owner == SCAN_FREE && local_result_pending);
    verify_result_t filler = {.token = 999, .transport = VERIFY_TRANSPORT_TIMEOUT};
    assert(queue_try_add(&results, &filler) && queue_try_add(&results, &filler));
    tick(clock_us + 1000); // publication guard succeeds, but queue is full
    assert(local_result_pending);
    guard_status = FIRMWARE_VERIFY_BUSY;
    assert(queue_try_remove(&results, &filler));
    tick(clock_us + 1000); // available queue capacity is insufficient without fresh guard
    assert(local_result_pending && queue_get_level(&results) == 1);
    unchanged(&local_result.snapshot, &original);
    guard_status = FIRMWARE_VERIFY_OK;
    ++generation; // flash changed while the completed result was waiting
    tick(clock_us + 1000);
    assert(!local_result_pending);
    verify_result_t result;
    assert(queue_try_remove(&results, &result) && result.token == 999);
    assert(queue_try_remove(&results, &result) && result.token == 123);
    assert(result.snapshot.outcome == VERIFY_SCAN_CHANGED);
    assert_original_interval(&result.snapshot, &original);
}

static void local_stage_deadlines_and_changes(void) {
    for (unsigned publication = 0; publication < 2; ++publication) {
        for (unsigned fault = 0; fault < 5; ++fault) {
            fresh(); complete_scan(false);
            verify_snapshot_t original = scanner.snapshot;
            uint64_t requested = scanner.requested_us;
            if (publication) tick(clock_us + 1000);
            guard_status = FIRMWARE_VERIFY_BUSY;
            tick(clock_us + 1000);
            if (fault == 0) {
                tick(requested + VERIFY_TIMEOUT_US - 1);
                assert(queue_get_level(&results) == 0);
                unsigned before = finish_calls;
                tick(requested + VERIFY_TIMEOUT_US);
                assert(finish_calls == before); // deadline never extends to wait for the lock
            } else {
                guard_status = fault == 1 ? FIRMWARE_VERIFY_UPDATE_ACTIVE : FIRMWARE_VERIFY_OK;
                if (fault == 2) ++generation;
                if (fault == 3) current_metadata.checksum ^= 1;
                tick(clock_us + 1000);
            }
            tick(clock_us + 1000); // local scanner handoff publishes on its next task tick
            verify_result_t result;
            assert(diagnostic_verify_poll(&result));
            verify_scan_outcome_t expected = fault == 0 ? VERIFY_SCAN_TIMEOUT
                : fault == 1 ? VERIFY_SCAN_UPDATE_ACTIVE
                : fault == 4 ? VERIFY_SCAN_COMPLETE : VERIFY_SCAN_CHANGED;
            assert(result.snapshot.outcome == expected);
            assert_original_interval(&result.snapshot, &original);
            assert(scanner.owner == SCAN_FREE && !local_result_pending);
        }
    }
}

static void peer_completion_and_cancellation(void) {
    for (unsigned fault = 0; fault < 5; ++fault) {
        fresh();
        uint8_t request[8] = {123, 0, 0, 0, PEER_VERIFY_PROTOCOL};
        assert(peer_verify_receive_request(&peer, request, clock_us));
        tick(clock_us + 1000); // actual peer provider acquires scanner ownership
        assert(scanner.owner == SCAN_REMOTE);
        while (!scanner.ready) { clock_us += 1000; scan_task(clock_us); }
        verify_snapshot_t original = scanner.snapshot;
        guard_status = FIRMWARE_VERIFY_BUSY;
        tick(clock_us + 1000);
        assert(scanner.owner == SCAN_REMOTE && !transmitted);
        unchanged(&scanner.snapshot, &original);
        if (fault == 0) {
            tick(1000 + VERIFY_TIMEOUT_US - 1);
            assert(scanner.owner == SCAN_REMOTE && !transmitted);
            tick(1000 + VERIFY_TIMEOUT_US);
            assert(scanner.owner == SCAN_FREE && !transmitted); // peer deadline invokes cancel
            guard_status = FIRMWARE_VERIFY_OK;
            request[0] = 124;
            assert(peer_verify_receive_request(&peer, request, clock_us));
            tick(clock_us + 1000);
            assert(scanner.owner == SCAN_REMOTE && scanner.token == 124);
            cancel(NULL, 123); // stale cancellation cannot release the replacement scan
            assert(scanner.owner == SCAN_REMOTE);
            cancel(NULL, 124);
        } else {
            guard_status = fault == 2 ? FIRMWARE_VERIFY_UPDATE_ACTIVE : FIRMWARE_VERIFY_OK;
            if (fault == 3) ++generation;
            if (fault == 4) current_metadata.version++;
            tick(clock_us + 1000);
            assert(scanner.owner == SCAN_FREE && transmitted == 1);
            /* Published peer packet bytes are an immutable scan snapshot, not
             * a claim of continued remote freshness while UART later drains. */
            assert_original_interval(&scanner.snapshot, &original);
            assert(scanner.snapshot.outcome == (fault == 1 ? VERIFY_SCAN_COMPLETE
                : fault == 2 ? VERIFY_SCAN_UPDATE_ACTIVE : VERIFY_SCAN_CHANGED));
        }
    }
}

static void local_presentation_api(void) {
    fresh(); complete_scan(false);
    verify_result_t result = {.transport = VERIFY_TRANSPORT_OK, .snapshot = scanner.snapshot};
    verify_snapshot_t original = result.snapshot;
    guard_status = FIRMWARE_VERIFY_BUSY;
    assert(!diagnostic_verify_recheck_local(&result));
    unchanged(&result.snapshot, &original);
    guard_status = FIRMWARE_VERIFY_OK;
    assert(diagnostic_verify_recheck_local(&result));
    unchanged(&result.snapshot, &original);
    guard_status = FIRMWARE_VERIFY_BUSY;
    assert(!diagnostic_verify_recheck_local(&result));
    guard_status = FIRMWARE_VERIFY_UPDATE_ACTIVE;
    assert(diagnostic_verify_recheck_local(&result));
    assert(result.snapshot.outcome == VERIFY_SCAN_UPDATE_ACTIVE);
    guard_status = FIRMWARE_VERIFY_OK;
    assert(diagnostic_verify_recheck_local(&result));
    assert(result.snapshot.outcome == VERIFY_SCAN_UPDATE_ACTIVE); // no terminal recovery/cached PASS
    result.remote = true; result.snapshot = original;
    guard_status = FIRMWARE_VERIFY_BUSY;
    assert(diagnostic_verify_recheck_local(&result));
    unchanged(&result.snapshot, &original);
}

int main(void) {
    local_transient_and_queue_backpressure();
    local_stage_deadlines_and_changes();
    peer_completion_and_cancellation();
    local_presentation_api();
    diagnostic_verify_shutdown();
    puts("Diagnostic verifier BUSY tests passed (scanner/queue/peer/presentation, original deadlines, immutable evidence, changes/cancel)");
    return 0;
}
