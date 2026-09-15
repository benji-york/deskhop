/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"

#define SNAPSHOT_US 100000u
#define LEASE_US 500000u
#define SOURCE_QUEUE_SIZE 128u

/* All mutable protocol state belongs to core 1. Initialization precedes launch.
 * The USB callbacks communicate through kbd_host_generation under the RAM lock. */
static struct {
    uint64_t boot, nonce, nonce_counter, request_due, nonce_started;
    uint64_t target, peer_boot, peer_seen, synthetic_deadline;
    uint32_t serial, peer_serial, host_generation;
    bool initialized, target_valid, peer_valid, peer_serial_valid, epoch_pending;
    hid_keyboard_report_t latest, fifo[SOURCE_QUEUE_SIZE];
    unsigned head, count;
    bool tail_pending, synthetic_pending, target_known;
    uint8_t incoming[40];
    unsigned fragment;
} sync;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const uint8_t *p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }
static void write32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8 * i);
}
static void write64(uint8_t *p, uint64_t v) { write32(p, v); write32(p + 4, v >> 32); }
static uint32_t state_crc(const uint8_t *p) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 36; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static void rotate_nonce(uint64_t now) {
    sync.nonce = sync.boot + (++sync.nonce_counter * UINT64_C(0x9e3779b97f4a7c15));
    sync.nonce_started = now;
    sync.fragment = 0;
}

void keyboard_sync_init(uint64_t boot_session) {
    memset(&sync, 0, sizeof(sync));
    sync.boot = boot_session;
    sync.initialized = sync.epoch_pending = true;
    rotate_nonce(time_us_64());
}

void keyboard_sync_reset(void) {
    if (!sync.initialized) return;
    sync.latest = (hid_keyboard_report_t){0};
    sync.head = sync.count = 0;
    sync.tail_pending = sync.target_valid = sync.peer_valid = sync.target_known = false;
    sync.synthetic_pending = false;
    sync.peer_serial_valid = false;
    sync.epoch_pending = true;
    sync.request_due = 0;
    rotate_nonce(time_us_64());
}

void keyboard_sync_publish(void) {
    if (!sync.initialized) return;
    device_t *state = &global_state;
    hid_keyboard_report_t local = {0};
    /* Only physical local contributions; never echo remote state. */
    for (unsigned i = 0; i <= state->max_kbd_idx; ++i) {
        const hid_keyboard_report_t *src = &state->local_kbd_states[i];
        local.modifier |= src->modifier;
        for (unsigned k = 0; k < KEYS_IN_USB_REPORT; ++k) {
            uint8_t key = src->keycode[k];
            if (!key || key_in_report(key, &local)) continue;
            uint8_t *slot = memchr(local.keycode, 0, KEYS_IN_USB_REPORT);
            if (slot) *slot = key;
        }
    }
    if (memcmp(&sync.latest, &local, sizeof(local)) == 0) return;
    sync.latest = local;
    if (sync.target_known) sync.target_valid = true;
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) return;
    if (sync.tail_pending || sync.count == SOURCE_QUEUE_SIZE) {
        sync.tail_pending = true;
        return;
    }
    sync.fifo[(sync.head + sync.count++) % SOURCE_QUEUE_SIZE] = local;
}

static void publish_peer(device_t *state, const hid_keyboard_report_t *report) {
    firmware_update_lock();
    bool changed = memcmp(&state->remote_kbd_state, report, sizeof(*report)) != 0;
    state->remote_kbd_state = *report;
    state->kbd_remote_generation = sync.host_generation;
    if (state->kbd_host_generation == sync.host_generation) {
        state->peer_modifiers = report->modifier;
        state->peer_modifiers_last_seen = time_us_64();
    }
    firmware_update_unlock();
    if (changed && CURRENT_BOARD_IS_ACTIVE_OUTPUT && !state->reboot_requested)
        keyboard_queue_current(state);
}

static void synthetic_release(device_t *state) {
    sync.synthetic_pending = false;
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) keyboard_queue_current(state);
    else {
        hid_keyboard_report_t empty = {0};
        queue_kbd_report(&empty, state);
    }
}

/* ID1 is reserved for the explicit lock-both-hosts synthetic chord. It never
 * owns physical source state. A lost all-up is repaired after 20 ms without
 * releasing a different keyboard's current physical hold. */
void keyboard_synthetic_receive(uart_packet_t *packet, device_t *state) {
    if (!sync.initialized || state->reboot_requested) return;
    hid_keyboard_report_t report;
    memcpy(&report, packet->data, sizeof(report));
    bool empty = true;
    for (unsigned i = 0; i < sizeof(report); ++i) empty &= packet->data[i] == 0;
    if (empty) {
        if (sync.synthetic_pending) synthetic_release(state);
        return;
    }
    bool lock = report.reserved == 0
        && ((report.modifier == KEYBOARD_MODIFIER_LEFTGUI && report.keycode[0] == HID_KEY_L)
            || (report.modifier == (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTGUI)
                && report.keycode[0] == HID_KEY_Q));
    for (unsigned i = 1; i < KEYS_IN_USB_REPORT; ++i) lock &= report.keycode[i] == 0;
    if (!lock || sync.synthetic_pending) return;
    queue_kbd_report(&report, state);
    sync.synthetic_pending = true;
    sync.synthetic_deadline = time_us_64() + 20000;
}

void keyboard_sync_task(device_t *state) {
    if (!sync.initialized || state->reboot_requested) return;
    uint64_t now = time_us_64();
    if (sync.synthetic_pending && now >= sync.synthetic_deadline) synthetic_release(state);
    firmware_update_lock();
    uint32_t generation = state->kbd_host_generation;
    firmware_update_unlock();
    if (generation != sync.host_generation) {
        sync.host_generation = generation;
        /* A synthetic down may have arrived after the bus reset, before this
         * task. Its release deadline must survive generation reconciliation. */
        sync.peer_valid = false;
        sync.epoch_pending = true;
        firmware_update_lock();
        state->remote_kbd_state = (hid_keyboard_report_t){0};
        state->kbd_remote_generation = generation;
        state->peer_modifiers = 0;
        firmware_update_unlock();
        rotate_nonce(now);
        sync.request_due = 0;
        if (CURRENT_BOARD_IS_ACTIVE_OUTPUT && state->tud_connected) {
            /* Explicitly reconcile a reset bus, including all-up. Only the
             * first empty cold mount can skip its redundant initial report. */
            hid_keyboard_report_t combined, empty = {0};
            combine_kbd_states(state, &combined);
            if (generation > 1 || memcmp(&combined, &empty, sizeof(combined)) != 0)
                keyboard_queue_current(state);
        }
    }
    if (sync.peer_valid && now - sync.peer_seen >= LEASE_US) {
        sync.peer_valid = false;
        sync.epoch_pending = true;
        rotate_nonce(now);
        sync.request_due = 0;
        hid_keyboard_report_t empty = {0};
        publish_peer(state, &empty);
    }
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT && state->tud_connected && now >= sync.request_due) {
        /* Commit a new challenge only if it entered the FIFO. Full queues retry
         * next tick; rotating an unsent challenge could prevent convergence. */
        uint64_t candidate = sync.boot + ((sync.nonce_counter + 1) * UINT64_C(0x9e3779b97f4a7c15));
        uint8_t payload[8];
        write64(payload, candidate);
        if (queue_packet_try(payload, sync.epoch_pending ? KEYBOARD_STATE_RESET_MSG
                                                            : KEYBOARD_STATE_REQUEST_MSG, sizeof(payload))) {
            rotate_nonce(now);
            sync.request_due = now + SNAPSHOT_US;
        }
    }
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT || !sync.target_valid) return;
    if (queue_get_level(&state->uart_tx_queue) > UART_QUEUE_LENGTH - 5) return;

    uint8_t payload[40] = {0};
    write64(payload, sync.target);
    write64(payload + 8, sync.boot);
    write32(payload + 16, state->selection.counter);
    write32(payload + 20, ++sync.serial);
    memcpy(payload + 24, sync.count ? &sync.fifo[sync.head] : &sync.latest, 8);
    payload[32] = state->selection.origin;
    payload[33] = state->active_output;
    payload[34] = 0x4b; payload[35] = 1;
    write32(payload + 36, state_crc(payload));
    for (unsigned i = 0; i < 5; ++i)
        if (!queue_packet_try(payload + i * 8, KEYBOARD_STATE_0_MSG + i, 8)) return;
    if (sync.count) {
        sync.head = (sync.head + 1) % SOURCE_QUEUE_SIZE;
        --sync.count;
    }
    if (sync.tail_pending && sync.count < SOURCE_QUEUE_SIZE) {
        sync.fifo[(sync.head + sync.count++) % SOURCE_QUEUE_SIZE] = sync.latest;
        sync.tail_pending = false;
    }
    /* Events continue on the next tick; an empty FIFO sends one snapshot per
     * request. A changed local source can send again using the current nonce. */
    sync.target_valid = sync.count != 0;
}

void keyboard_sync_receive(uart_packet_t *packet, device_t *state) {
    if (!sync.initialized || state->reboot_requested) return;
    uint64_t now = time_us_64();
    if (packet->type == KEYBOARD_STATE_REQUEST_MSG || packet->type == KEYBOARD_STATE_RESET_MSG) {
        if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
            if (packet->type == KEYBOARD_STATE_RESET_MSG && read64(packet->data) != sync.target) {
                /* New receiver bus/focus/lease: discard historical source events.
                 * Only the authoritative current state may cross this boundary. */
                sync.head = sync.count = 0;
                sync.tail_pending = false;
            }
            sync.target = read64(packet->data);
            sync.target_valid = sync.target_known = true;
        }
        return;
    }
    unsigned fragment = packet->type - KEYBOARD_STATE_0_MSG;
    if (sync.fragment && fragment == sync.fragment - 1
        && memcmp(sync.incoming + fragment * 8, packet->data, 8) == 0) return;
    if (fragment == 0) sync.fragment = 0;
    if (fragment >= 5 || fragment != sync.fragment) {
        sync.fragment = 0;
        return;
    }
    memcpy(sync.incoming + fragment * 8, packet->data, 8);
    if (++sync.fragment != 5) return;
    sync.fragment = 0;
    const uint8_t *p = sync.incoming;
    if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT || !state->tud_connected
        || state->kbd_host_generation != sync.host_generation
        || now - sync.nonce_started >= SNAPSHOT_US
        || read64(p) != sync.nonce || read32(p + 36) != state_crc(p)
        || read32(p + 16) != state->selection.counter
        || p[32] != state->selection.origin || p[33] != state->active_output
        || p[34] != 0x4b || p[35] != 1 || p[25] != 0) return;
    uint64_t boot = read64(p + 8);
    uint32_t serial = read32(p + 20);
    if (sync.peer_serial_valid && boot == sync.peer_boot) {
        uint32_t distance = serial - sync.peer_serial;
        if (distance == 0 || distance >= UINT32_C(0x80000000)) return;
        sync.epoch_pending = false;
    } else if (sync.peer_serial_valid) {
        sync.epoch_pending = true;
        /* A newly observed source boot retires this challenge immediately, so
         * queued old-session traffic cannot switch the accepted session back. */
        rotate_nonce(now);
        sync.request_due = 0;
    } else {
        sync.epoch_pending = false;
    }
    sync.peer_boot = boot;
    sync.peer_serial = serial;
    sync.peer_serial_valid = sync.peer_valid = true;
    sync.peer_seen = now;
    hid_keyboard_report_t report;
    memcpy(&report, p + 24, sizeof(report));
    publish_peer(state, &report);
    /* A snapshot is not evidence of new physical activity, including after
     * lease expiry/reboot. ACTIVITY_MSG carries the source's actual input age. */
}
