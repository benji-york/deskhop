/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "clipboard.h"
#include "clipboard_diagnostics.h"
#include "diagnostic_history.h"
#include <limits.h>

#define HELPER_LEASE_US 3000000ull
#define PEER_LEASE_US 750000ull
#define HEARTBEAT_US 250000ull
#define CONTROL_SIZE 54u
#define CONTROL_FRAGMENTS 9u
enum { C_TRIGGER = 1, C_GRANT, C_BEGIN, C_RELEASE, C_CANCEL, C_DONE, C_ALIVE,
       C_COMMIT, C_DATA, C_PULL };

/* A single bounded allocation serves helper assembly on the source and
 * full-message validation/injection on the focused target. All access after initialization uses the RAM lock.
 * Transport bypasses the general UART FIFO: real input and updates win. */
static struct {
    clipboard_text_t text;
    clipboard_request_t request;
    uint8_t tx[CONTROL_SIZE], rx[CONTROL_SIZE];
    uint64_t boot, helper, helper_seen, peer_seen, heartbeat_due, deadline;
    uint64_t nonce_counter, raw_busy, peer_boot, last_trigger_boot;
    uint64_t diagnostic_f23_down;
    hid_interface_t *trigger_iface;
    uint32_t trigger_counter, trigger_id, host_generation, alive_serial, peer_alive_serial;
    uint32_t last_trigger_id;
    uint16_t data_offset;
    uint8_t tx_kind, tx_index, rx_kind, rx_index;
    bool initialized, active, waiting_grant, helper_pending, helper_started;
    bool source_ready, begin_sent, commit_sent, release_sent, raw_released;
    bool trigger_latched, host_leds_known, target;
} clip;

_Static_assert(MAX_DEVICES * MAX_INTERFACES <= 64, "raw keyboard mask capacity");
_Static_assert(sizeof(clip) <= 1536, "clipboard runtime RAM budget exceeded");

static void trace(clipboard_diagnostic_phase_t phase, clipboard_diagnostic_reason_t reason) {
    diagnostic_history_record(HISTORY_CLIPBOARD, phase, reason, 0);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const uint8_t *p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }
static void write32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8 * i);
}
static void write64(uint8_t *p, uint64_t n) { write32(p, n); write32(p + 4, n >> 32); }
static bool request_equal(const clipboard_request_t *a, const clipboard_request_t *b) {
    return a->boot_source == b->boot_source && a->boot_target == b->boot_target
        && a->helper_session == b->helper_session && a->nonce == b->nonce
        && a->focus == b->focus;
}
static void encode_request(uint8_t *p, const clipboard_request_t *r) {
    write64(p, r->boot_source); write64(p + 8, r->boot_target);
    write64(p + 16, r->helper_session); write64(p + 24, r->nonce);
    write64(p + 32, r->focus);
}
static clipboard_request_t decode_request(const uint8_t *p) {
    return (clipboard_request_t){read64(p), read64(p + 8), read64(p + 16),
                                 read64(p + 24), read64(p + 32)};
}
static uint32_t data_crc(uint16_t offset, uint8_t byte) {
    uint8_t bound[43];
    encode_request(bound, &clip.request);
    bound[40] = offset;
    bound[41] = offset >> 8;
    bound[42] = byte;
    uint32_t crc = clipboard_crc32(bound, sizeof(bound));
    *(volatile uint8_t *)&bound[42] = 0;
    return crc;
}
/* Low bits bind selection origin, destination role and physical trigger owner.
 * The remaining 29 bits bind the target USB keyboard generation. */
static uint64_t focus_base(const device_t *state) {
    return (uint64_t)state->selection.counter << 32 | state->selection.origin
        | (uint64_t)state->active_output << 1;
}
static bool trigger_here(const device_t *state) {
    return ((clip.request.focus >> 2) & 1u) == state->board_role;
}
static bool focus_matches(const device_t *state, uint64_t focus) {
    return state->active_output == ((focus >> 1) & 1u) && (uint32_t)(focus >> 32) == state->selection.counter
        && (focus & 1u) == state->selection.origin;
}
static bool physical_idle(const device_t *state) {
    hid_keyboard_report_t combined;
    combine_kbd_states((device_t *)state, &combined);
    const uint8_t *p = (const uint8_t *)&combined;
    for (unsigned i = 0; i < sizeof(combined); ++i) if (p[i]) return false;
    return combined_mouse_buttons(state) == 0;
}
static bool helper_live(uint64_t now) {
    return clip.helper && now >= clip.helper_seen && now - clip.helper_seen < HELPER_LEASE_US;
}

/* Same availability gates as admission; report the first blocker only. */
static clipboard_diagnostic_reason_t unavailable_reason(const device_t *state) {
    if (!clip.initialized) return CLIP_REASON_UNINITIALIZED;
    if (!state->tud_connected) return CLIP_REASON_HOST_DISCONNECTED;
    if (state->config_mode_active) return CLIP_REASON_CONFIG_MODE;
    if (tud_suspended()) return CLIP_REASON_SUSPENDED;
    if (state->reboot_requested || state->maintenance_reserved
        || state->config_bootloader_local_pending || state->config_bootloader_peer_pending)
        return CLIP_REASON_MAINTENANCE;
    if (state->fw.upgrade_in_progress || state->fw.image_dirty || state->batch.tx.active)
        return CLIP_REASON_UPDATE_ACTIVE;
    if (state->active_output >= 2) return CLIP_REASON_FOCUS_INVALID;
    /* Hosts/remappers may omit LED output reports entirely. Use Caps-off as
     * the startup default; only a report from this USB session can override
     * it. An old session's cached Caps bit must not block a new host. */
    if (state->board_role == state->active_output && clip.host_leds_known
        && (state->keyboard_leds_desired[state->board_role] & 2u))
        return CLIP_REASON_CAPS_ON;
    return CLIP_REASON_NONE;
}
static bool available(const device_t *state) {
    return unavailable_reason(state) == CLIP_REASON_NONE;
}
static uint32_t control_crc(const uint8_t *data, uint8_t kind) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i <= 48; ++i) {
        crc ^= i < 48 ? data[i] : kind;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static void control(uint8_t kind, uint32_t argument, uint32_t crc) {
    memset(clip.tx, 0, sizeof(clip.tx));
    encode_request(clip.tx, &clip.request);
    write32(clip.tx + 40, argument);
    write32(clip.tx + 44, crc);
    write32(clip.tx + 48, control_crc(clip.tx, kind));
    clip.tx_kind = kind;
    clip.tx_index = 0;
}
static void cancel_locked(bool notify, clipboard_diagnostic_reason_t reason) {
    bool active = clip.active;
    if (active && reason != CLIP_REASON_NONE) trace(CLIP_DIAG_CANCELLED, reason);
    clip.active = clip.waiting_grant = clip.helper_pending = clip.helper_started = false;
    clip.source_ready = clip.begin_sent = clip.commit_sent = clip.release_sent = false;
    clip.data_offset = clip.rx_index = clip.rx_kind = 0;
    memset(clip.rx, 0, sizeof(clip.rx));
    clipboard_text_cancel(&clip.text);
    if (notify && active) control(C_CANCEL, 0, 0);
    else if (!(notify && clip.tx_kind == C_CANCEL)) {
        memset(clip.tx, 0, sizeof(clip.tx)); clip.tx_kind = clip.tx_index = 0;
    }
    /* The exact F23 latch intentionally survives cancellation until raw key-up. */
}
static void check_locked(device_t *state, uint64_t now) {
    if (clip.helper && !helper_live(now)) {
        clip.helper = 0;
        if (clip.active && !clip.target) cancel_locked(true, CLIP_REASON_HELPER_EXPIRED);
    }
    if (!clip.active) return;
    bool invalid = !available(state) || !focus_matches(state, clip.request.focus)
        || state->kbd_host_generation != clip.host_generation;
    if (!clip.target) invalid |= !helper_live(now)
        || (!clip.waiting_grant && now - clip.peer_seen >= PEER_LEASE_US);
    else invalid |= (clip.waiting_grant ? false : (uint32_t)clip.request.focus >> 3 != state->kbd_host_generation)
                  || (!clip.waiting_grant && now - clip.peer_seen >= PEER_LEASE_US);
    if (now >= clip.deadline) invalid = true;
    if (clip.target && clip.text.phase == CLIPBOARD_READY
        && !clip.raw_released && now >= clip.text.deadline) invalid = true;
    if (invalid) {
        clipboard_diagnostic_reason_t reason = unavailable_reason(state);
        if (reason == CLIP_REASON_NONE) {
            if (!focus_matches(state, clip.request.focus)) reason = CLIP_REASON_FOCUS_CHANGED;
            else if (state->kbd_host_generation != clip.host_generation
                || (clip.target && !clip.waiting_grant
                    && (uint32_t)clip.request.focus >> 3 != state->kbd_host_generation))
                reason = CLIP_REASON_USB_CHANGED;
            else if (!clip.target && !helper_live(now)) reason = CLIP_REASON_HELPER_EXPIRED;
            else if (!clip.waiting_grant && now - clip.peer_seen >= PEER_LEASE_US)
                reason = CLIP_REASON_PEER_EXPIRED;
            else reason = CLIP_REASON_DEADLINE;
        }
        cancel_locked(true, reason);
    }
}

void clipboard_init(uint64_t boot_session) {
    memset(&clip, 0, sizeof(clip));
    clip.boot = boot_session;
    clip.initialized = true;
}
void clipboard_cancel(void) {
    firmware_update_lock(); cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT); firmware_update_unlock();
}
void clipboard_task(device_t *state) {
    firmware_update_lock(); check_locked(state, time_us_64()); firmware_update_unlock();
}
void clipboard_host_reset(device_t *state) {
    (void)state; firmware_update_lock();
    cancel_locked(true, CLIP_REASON_HOST_RESET); firmware_update_unlock();
}
void clipboard_usb_session_reset(device_t *state) {
    firmware_update_lock();
    clip.host_leds_known = false;
    cancel_locked(true, CLIP_REASON_USB_CHANGED);
    firmware_update_unlock();
}
void clipboard_host_led_report(device_t *state) {
    firmware_update_lock();
    clip.host_leds_known = true;
    check_locked(state, time_us_64());
    firmware_update_unlock();
}
void clipboard_remote_input(device_t *state) { (void)state; clipboard_cancel(); }
void clipboard_peer_session(uint64_t boot_session) {
    firmware_update_lock();
    if (boot_session) {
        if (clip.peer_boot && clip.peer_boot != boot_session) cancel_locked(true, CLIP_REASON_PEER_RESTARTED);
        clip.peer_boot = boot_session;
    }
    firmware_update_unlock();
}

static unsigned interface_index(const hid_interface_t *iface, const device_t *state) {
    for (unsigned d = 0; d < MAX_DEVICES; ++d)
        for (unsigned i = 0; i < MAX_INTERFACES; ++i)
            if (iface == &state->iface[d][i]) return d * MAX_INTERFACES + i;
    return 64;
}
/* Observe only F23 presence, once per press. No other key identity or report
 * enters history, including incomplete decoded snapshots. This mask never
 * participates in release proof or transaction admission. */
static bool observe_trigger(uint64_t bit, bool present, clipboard_diagnostic_reason_t reason) {
    bool edge = present && bit && !(clip.diagnostic_f23_down & bit);
    if (present) clip.diagnostic_f23_down |= bit;
    else clip.diagnostic_f23_down &= ~bit;
    if (edge) {
        trace(CLIP_DIAG_TRIGGER, CLIP_REASON_NONE);
        if (reason != CLIP_REASON_NONE) trace(CLIP_DIAG_REJECTED, reason);
    }
    return edge;
}
void clipboard_keyboard_incomplete(hid_interface_t *iface, const hid_keyboard_report_t *report,
                                   device_t *state) {
    firmware_update_lock();
    unsigned index = interface_index(iface, state);
    bool present = false;
    for (unsigned i = 0; i < KEYS_IN_USB_REPORT; ++i) present |= report->keycode[i] == HID_KEY_F23;
    observe_trigger(index < 64 ? UINT64_C(1) << index : 0, present, CLIP_REASON_INCOMPLETE_REPORT);
    firmware_update_unlock();
}
/* Classification after a failed admission, with protocol validation as the
 * fallback for request bindings. Never record any binding or payload value. */
static clipboard_diagnostic_reason_t rejected_reason(device_t *state, uint64_t now,
                                                     bool needs_helper, bool ignore_raw) {
    clipboard_diagnostic_reason_t reason = unavailable_reason(state);
    if (reason != CLIP_REASON_NONE) return reason;
    if (needs_helper && !helper_live(now)) return CLIP_REASON_HELPER_MISSING;
    if ((!ignore_raw && clip.raw_busy) || !physical_idle(state)) return CLIP_REASON_INPUT_HELD;
    if ((clip.active && !ignore_raw) || clip.tx_kind || clip.text.phase != CLIPBOARD_IDLE)
        return CLIP_REASON_BUSY;
    return CLIP_REASON_BINDING;
}
bool clipboard_keyboard_raw(hid_interface_t *iface, const hid_keyboard_report_t *report,
                            device_t *state) {
    firmware_update_lock();
    uint64_t now = time_us_64();
    check_locked(state, now);
    unsigned index = interface_index(iface, state);
    bool empty = report->modifier == 0;
    bool trigger = report->modifier == 0;
    unsigned trigger_keys = 0;
    for (unsigned i = 0; i < KEYS_IN_USB_REPORT; ++i) {
        empty &= report->keycode[i] == 0;
        if (report->keycode[i] == HID_KEY_F23) ++trigger_keys;
        else trigger &= report->keycode[i] == 0;
    }
    trigger &= trigger_keys == 1;
    bool swallow = false;
    uint64_t bit = index < 64 ? UINT64_C(1) << index : 0;
    bool edge = observe_trigger(bit, trigger_keys != 0,
                                trigger ? CLIP_REASON_NONE : CLIP_REASON_NON_BARE);
    if (clip.trigger_latched && clip.trigger_iface == iface) {
        if (empty) {
            clip.trigger_latched = false;
            clip.raw_released = true;
            if (clip.active) trace(CLIP_DIAG_RELEASE, CLIP_REASON_NONE);
            if (clip.active && clip.target) clipboard_text_release(&clip.text);
            swallow = true;
        } else if (trigger) swallow = true;
        else cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT);
    } else if (trigger) {
        if (clip.active) cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT);
        swallow = true;
        clip.trigger_latched = true;
        clip.trigger_iface = iface;
        if (!clip.active && !clip.tx_kind && clip.text.phase == CLIPBOARD_IDLE && bit
            && available(state)
            && (state->board_role == state->active_output || helper_live(now))
            && !clip.raw_busy && physical_idle(state)
            && clip.trigger_counter != UINT32_MAX) {
            clip.trigger_id = ++clip.trigger_counter;
            clip.target = state->board_role == state->active_output;
            clip.request = (clipboard_request_t){clip.target ? 0 : clip.boot,
                clip.target ? clip.boot : 0, clip.target ? 0 : clip.helper,
                clip.trigger_id, focus_base(state) | (uint64_t)state->board_role << 2};
            clip.active = clip.waiting_grant = true;
            clip.raw_released = clip.helper_pending = false;
            clip.host_generation = state->kbd_host_generation;
            clip.alive_serial = clip.peer_alive_serial = 0;
            clip.deadline = now + CLIPBOARD_RESPONSE_TIMEOUT_US;
            control(clip.target ? C_PULL : C_TRIGGER, clip.trigger_id, 0);
            trace(CLIP_DIAG_ADMITTED, CLIP_REASON_NONE);
        } else if (edge) {
            trace(CLIP_DIAG_REJECTED, clip.trigger_counter == UINT32_MAX
                ? CLIP_REASON_COUNTER_LIMIT : rejected_reason(state, now,
                    state->board_role != state->active_output, false));
        }
    } else if (!empty && clip.active) cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT);
    if (empty) clip.raw_busy &= ~bit;
    else clip.raw_busy |= bit;
    firmware_update_unlock();
    return swallow;
}
void clipboard_physical_disconnect(hid_interface_t *iface, device_t *state) {
    firmware_update_lock();
    unsigned index = interface_index(iface, state);
    if (index < 64) {
        clip.raw_busy &= ~(UINT64_C(1) << index);
        clip.diagnostic_f23_down &= ~(UINT64_C(1) << index);
    }
    if (clip.trigger_iface == iface) clip.trigger_latched = false;
    cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT);
    firmware_update_unlock();
}
void clipboard_physical_unknown(hid_interface_t *iface, device_t *state) {
    firmware_update_lock();
    unsigned index = interface_index(iface, state);
    if (index < 64) clip.raw_busy |= UINT64_C(1) << index;
    cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT);
    firmware_update_unlock();
}
void clipboard_mouse_buttons(hid_interface_t *iface, uint8_t old_buttons,
                             uint8_t new_buttons, device_t *state) {
    (void)iface; (void)state;
    if (old_buttons != new_buttons || new_buttons) clipboard_cancel();
}

bool clipboard_helper_open(uint64_t session, uint64_t now) {
    firmware_update_lock();
    device_t *state = &global_state;
    bool ok = clip.initialized && session && state->board_role < 2
        && state->tud_connected && !state->config_mode_active && !state->reboot_requested
        && !state->maintenance_reserved && !state->fw.upgrade_in_progress
        && !state->fw.image_dirty;
    if (clip.active && !clip.target) cancel_locked(true, CLIP_REASON_HELPER_CLOSED);
    if (ok) trace(CLIP_DIAG_HELPER_OPEN, CLIP_REASON_NONE);
    clip.helper = ok ? session : 0;
    clip.helper_seen = now;
    firmware_update_unlock(); return ok;
}
void clipboard_helper_close(uint64_t now) {
    (void)now; firmware_update_lock();
    if (clip.helper) trace(CLIP_DIAG_HELPER_CLOSE, CLIP_REASON_NONE);
    clip.helper = 0;
    if (clip.active && !clip.target) cancel_locked(true, CLIP_REASON_HELPER_CLOSED);
    firmware_update_unlock();
}
void clipboard_helper_keepalive(uint64_t now) {
    firmware_update_lock();
    if (helper_live(now)) clip.helper_seen = now;
    else { clip.helper = 0; if (clip.active && !clip.target) cancel_locked(true, CLIP_REASON_HELPER_EXPIRED); }
    firmware_update_unlock();
}
bool clipboard_helper_active(uint64_t now) {
    firmware_update_lock();
    check_locked(&global_state, now);
    bool live = helper_live(now);
    firmware_update_unlock(); return live;
}
bool clipboard_helper_poll(clipboard_request_t *request) {
    firmware_update_lock();
    check_locked(&global_state, time_us_64());
    bool result = clip.active && !clip.target && clip.helper_pending;
    if (result) {
        *request = clip.request; clip.helper_pending = false; clip.helper_started = true;
        trace(CLIP_DIAG_HELPER_REQUEST, CLIP_REASON_NONE);
    }
    firmware_update_unlock(); return result;
}
bool clipboard_helper_reply_begin(const clipboard_request_t *request, uint8_t status,
                                  uint16_t length, uint32_t crc, uint64_t now) {
    firmware_update_lock(); check_locked(&global_state, now);
    bool valid = clip.active && !clip.target && clip.helper_started && !clip.waiting_grant
        && !clip.source_ready && clip.text.phase == CLIPBOARD_IDLE
        && request_equal(request, &clip.request);
    if (valid && status >= CLIPBOARD_EMPTY && status <= CLIPBOARD_UNAVAILABLE
        && length == 0 && crc == 0) {
        const clipboard_diagnostic_reason_t reasons[] = {CLIP_REASON_EMPTY, CLIP_REASON_NON_TEXT,
            CLIP_REASON_OVERSIZE, CLIP_REASON_UNSUPPORTED, CLIP_REASON_UNAVAILABLE};
        trace(CLIP_DIAG_HELPER_RESULT, reasons[status - CLIPBOARD_EMPTY]);
        cancel_locked(true, reasons[status - CLIPBOARD_EMPTY]);
        firmware_update_unlock(); return true;
    }
    if (valid) valid = clipboard_text_begin(&clip.text, status, length, crc, now);
    if (valid) trace(CLIP_DIAG_HELPER_RESULT, CLIP_REASON_NONE);
    if (!valid) cancel_locked(true, CLIP_REASON_PAYLOAD_INVALID);
    firmware_update_unlock(); return valid;
}
bool clipboard_helper_reply_chunk(uint16_t offset, const uint8_t *data, size_t length, uint64_t now) {
    firmware_update_lock(); check_locked(&global_state, now);
    bool valid = clip.active && !clip.target && clip.helper_started
        && clipboard_text_append(&clip.text, offset, data, length, now);
    if (!valid) cancel_locked(true, CLIP_REASON_PAYLOAD_INVALID);
    firmware_update_unlock(); return valid;
}
bool clipboard_helper_reply_commit(uint64_t now) {
    firmware_update_lock(); check_locked(&global_state, now);
    bool valid = clip.active && !clip.target && clip.helper_started && clipboard_text_commit(&clip.text, now);
    if (valid) {
        trace(CLIP_DIAG_PAYLOAD_READY, CLIP_REASON_NONE);
        clip.source_ready = true;
        clip.deadline = now + CLIPBOARD_TYPING_TIMEOUT_US + CLIPBOARD_RESPONSE_TIMEOUT_US;
    } else cancel_locked(true, CLIP_REASON_PAYLOAD_INVALID);
    firmware_update_unlock(); return valid;
}

/* Every control is nine strictly ordered fragments and binds all five context
 * fields under CRC32. Each data byte carries its exact offset and a CRC over
 * the complete context, offset and byte. COMMIT validates the entire payload. */
bool clipboard_next_tx(uart_packet_t *packet, uint64_t now) {
    firmware_update_lock();
    device_t *state = &global_state;
    check_locked(state, now);
    if (!clip.tx_kind && clip.active && !clip.target && !clip.waiting_grant) {
        if (clip.source_ready && !clip.begin_sent) {
            control(C_BEGIN, clip.text.length, clip.text.expected_crc);
            clip.begin_sent = true;
        } else if (clip.source_ready && clip.data_offset < clip.text.length) {
            *packet = (uart_packet_t){.type = CLIPBOARD_MSG};
            packet->data[0] = C_DATA;
            packet->data[1] = clip.data_offset;
            packet->data[2] = clip.data_offset >> 8;
            packet->data[3] = clip.text.text[clip.data_offset];
            write32(packet->data + 4, data_crc(clip.data_offset, packet->data[3]));
            ++clip.data_offset;
            firmware_update_unlock(); return true;
        } else if (clip.source_ready && !clip.commit_sent) {
            control(C_COMMIT, 0, 0); clip.commit_sent = true;
        } else if (trigger_here(state) && clip.raw_released && !clip.release_sent) {
            control(C_RELEASE, 0, 0); clip.release_sent = true;
        } else if (now >= clip.heartbeat_due) {
            if (clip.alive_serial == UINT32_MAX) cancel_locked(true, CLIP_REASON_COUNTER_LIMIT);
            else control(C_ALIVE, ++clip.alive_serial, 0);
            clip.heartbeat_due = now + HEARTBEAT_US;
        }
    }
    if (!clip.tx_kind && clip.active && clip.target && !clip.waiting_grant && now >= clip.heartbeat_due) {
        if (clip.alive_serial == UINT32_MAX) cancel_locked(true, CLIP_REASON_COUNTER_LIMIT);
        else control(C_ALIVE, ++clip.alive_serial, 0);
        clip.heartbeat_due = now + HEARTBEAT_US;
    }
    bool ready = clip.tx_kind != 0;
    if (ready) {
        *packet = (uart_packet_t){.type = CLIPBOARD_MSG};
        packet->data[0] = clip.tx_kind;
        packet->data[1] = clip.tx_index;
        memcpy(packet->data + 2, clip.tx + 6 * clip.tx_index, 6);
        if (++clip.tx_index == CONTROL_FRAGMENTS) {
            clip.tx_kind = clip.tx_index = 0;
            memset(clip.tx, 0, sizeof(clip.tx));
        }
    }
    firmware_update_unlock(); return ready;
}

static void receive_control(uint8_t kind, device_t *state, uint64_t now) {
    if (clip.rx[52] || clip.rx[53] || control_crc(clip.rx, kind) != read32(clip.rx + 48)) {
        cancel_locked(true, CLIP_REASON_PROTOCOL); return;
    }
    clipboard_request_t request = decode_request(clip.rx);
    uint32_t argument = read32(clip.rx + 40), crc = read32(clip.rx + 44);
    if (kind == C_PULL && state->board_role != state->active_output) {
        /* A key on the destination asks the opposite, currently live helper.
         * No helper advertisement or clipboard value is cached. */
        trace(CLIP_DIAG_PULL, CLIP_REASON_NONE);
        if (clip.active || clip.tx_kind || clip.text.phase != CLIPBOARD_IDLE
            || !available(state) || !helper_live(now) || !focus_matches(state, request.focus)
            || request.boot_source || !request.boot_target || request.helper_session
            || !argument || request.nonce != argument || crc || (request.focus & 0xfffffff8u)
            || ((request.focus >> 2) & 1u) != state->active_output
            || (clip.peer_boot && request.boot_target != clip.peer_boot)
            || (request.boot_target == clip.last_trigger_boot && argument <= clip.last_trigger_id)
            || clip.raw_busy || !physical_idle(state)) {
            trace(CLIP_DIAG_REJECTED, rejected_reason(state, now, true, false)); return;
        }
        trace(CLIP_DIAG_ADMITTED, CLIP_REASON_NONE);
        clip.last_trigger_boot = request.boot_target; clip.last_trigger_id = argument;
        clip.request = request; clip.request.boot_source = clip.boot;
        clip.request.helper_session = clip.helper;
        clip.target = false; clip.active = clip.waiting_grant = true;
        clip.trigger_id = argument; clip.raw_released = false;
        clip.host_generation = state->kbd_host_generation;
        clip.alive_serial = clip.peer_alive_serial = 0;
        clip.deadline = now + CLIPBOARD_RESPONSE_TIMEOUT_US;
        control(C_TRIGGER, argument, 0);
        return;
    }
    if (kind == C_TRIGGER && state->board_role == state->active_output) {
        trace(CLIP_DIAG_OFFER, CLIP_REASON_NONE);
        bool local = ((request.focus >> 2) & 1u) == state->board_role;
        if ((local ? (!clip.active || !clip.target || !clip.waiting_grant
                       || request.boot_target != clip.boot || argument != clip.trigger_id
                       || request.focus != clip.request.focus)
                   : (clip.active || request.boot_target != 0))
            || clip.tx_kind || clip.text.phase != CLIPBOARD_IDLE || !available(state)
            || !focus_matches(state, request.focus) || !request.boot_source || !request.helper_session
            || (clip.peer_boot && request.boot_source != clip.peer_boot)
            || (!local && request.boot_source == clip.last_trigger_boot && argument <= clip.last_trigger_id)
            || !argument || request.nonce != argument || crc || (request.focus & 0xfffffff8u)
            || (!local && clip.raw_busy) || !physical_idle(state)
            || state->kbd_host_generation > (UINT32_MAX >> 3) || clip.nonce_counter == UINT64_MAX) {
            trace(CLIP_DIAG_REJECTED, rejected_reason(state, now, false, local)); return;
        }
        trace(CLIP_DIAG_ADMITTED, CLIP_REASON_NONE);
        clip.request = request;
        if (!local) { clip.last_trigger_boot = request.boot_source; clip.last_trigger_id = argument; }
        clip.request.boot_target = clip.boot;
        clip.request.nonce = ++clip.nonce_counter;
        clip.request.focus |= (uint64_t)state->kbd_host_generation << 3;
        clip.host_generation = state->kbd_host_generation;
        clip.alive_serial = clip.peer_alive_serial = 0;
        clip.target = clip.active = true; clip.waiting_grant = false;
        if (!local) { clip.raw_released = false; clip.text.released = false; }
        clip.peer_seen = now; clip.heartbeat_due = now + HEARTBEAT_US;
        clip.deadline = now + CLIPBOARD_RESPONSE_TIMEOUT_US;
        control(C_GRANT, argument, 0);
        return;
    }
    if (kind == C_GRANT && state->board_role != state->active_output) {
        trace(CLIP_DIAG_GRANT, CLIP_REASON_NONE);
        if (!clip.active || clip.target || !clip.waiting_grant || argument != clip.trigger_id || crc
            || request.boot_source != clip.boot || !request.boot_target || !request.nonce
            || (clip.request.boot_target && request.boot_target != clip.request.boot_target)
            || (clip.peer_boot && request.boot_target != clip.peer_boot)
            || (request.focus & 7) != (clip.request.focus & 7)
            || request.helper_session != clip.helper || !focus_matches(state, request.focus)) {
            trace(CLIP_DIAG_REJECTED, CLIP_REASON_BINDING); return;
        }
        clip.request = request; clip.waiting_grant = false; clip.helper_pending = true;
        clip.peer_seen = now; clip.heartbeat_due = 0;
        return;
    }
    if (!clip.active || !request_equal(&request, &clip.request)) return;
    if (kind == C_CANCEL || kind == C_DONE) {
        if (kind == C_DONE) trace(CLIP_DIAG_DONE, CLIP_REASON_NONE);
        cancel_locked(false, kind == C_DONE ? CLIP_REASON_NONE : CLIP_REASON_PEER_CANCELLED);
        return;
    }
    if (kind == C_ALIVE) {
        if (!argument || argument <= clip.peer_alive_serial || crc) cancel_locked(true, CLIP_REASON_PROTOCOL);
        else { clip.peer_alive_serial = argument; clip.peer_seen = now; }
        return;
    }
    if (!clip.target) return;
    clip.peer_seen = now;
    switch (kind) {
    case C_BEGIN:
        if (argument > UINT16_MAX || !clipboard_text_begin(&clip.text, CLIPBOARD_OK,
                argument, crc, now)) cancel_locked(true, CLIP_REASON_PROTOCOL);
        break;
    case C_RELEASE:
        if (argument || crc || trigger_here(state) || clip.raw_released) { cancel_locked(true, CLIP_REASON_PROTOCOL); break; }
        trace(CLIP_DIAG_RELEASE, CLIP_REASON_NONE);
        clip.raw_released = true;
        clipboard_text_release(&clip.text);
        break;
    case C_COMMIT:
        if (argument || crc || !clipboard_text_commit(&clip.text, now)) {
            cancel_locked(true, CLIP_REASON_PROTOCOL); break;
        }
        trace(CLIP_DIAG_PAYLOAD_READY, CLIP_REASON_NONE);
        if (clip.raw_released) clipboard_text_release(&clip.text);
        clip.deadline = now + CLIPBOARD_TYPING_TIMEOUT_US;
        break;
    case C_ALIVE:
        if (argument || crc) cancel_locked(true, CLIP_REASON_PROTOCOL);
        break;
    default: cancel_locked(true, CLIP_REASON_PROTOCOL); break;
    }
}

void clipboard_receive(uint8_t type, const uint8_t payload[8], uint64_t now) {
    if (type != CLIPBOARD_MSG) return;
    firmware_update_lock();
    device_t *state = &global_state;
    check_locked(state, now);
    uint8_t kind = payload[0], index = payload[1];
    if (kind == C_DATA) {
        if (clip.target && clip.active && !clip.waiting_grant) {
            uint16_t offset = payload[1] | (uint16_t)payload[2] << 8;
            bool valid = clip.rx_index == 0 && clip.text.phase == CLIPBOARD_RECEIVING
                && offset == clip.text.received && offset < clip.text.length
                && data_crc(offset, payload[3]) == read32(payload + 4);
            if (!valid || !clipboard_text_append(&clip.text, offset, payload + 3, 1, now))
                cancel_locked(true, CLIP_REASON_PROTOCOL);
            else clip.peer_seen = now;
        }
    } else if (((kind >= C_TRIGGER && kind <= C_COMMIT) || kind == C_PULL) && index < CONTROL_FRAGMENTS) {
        if (index == 0 && clip.rx_index == 0) clip.rx_kind = kind;
        if (kind != clip.rx_kind || index != clip.rx_index) cancel_locked(true, CLIP_REASON_PROTOCOL);
        else {
            memcpy(clip.rx + 6 * index, payload + 2, 6);
            if (++clip.rx_index == CONTROL_FRAGMENTS) {
                clip.rx_index = 0;
                receive_control(kind, state, now);
                memset(clip.rx, 0, sizeof(clip.rx));
            }
        }
    } else cancel_locked(true, CLIP_REASON_PROTOCOL);
    firmware_update_unlock();
}

void clipboard_usb_task(device_t *state) {
    firmware_update_lock();
    uint64_t now = time_us_64();
    check_locked(state, now);
    if (!clip.target || !state->tud_connected || tud_suspended()
        || !queue_is_empty(&state->kbd_queue) || state->kbd_latest_pending
        || !tud_hid_n_ready(ITF_NUM_HID)) {
        firmware_update_unlock(); return;
    }
    if (clip.active && !physical_idle(state)) cancel_locked(true, CLIP_REASON_PHYSICAL_INPUT);
    uint8_t bytes[8];
    if (clipboard_text_report(&clip.text, now, bytes)) {
        hid_keyboard_report_t report;
        memcpy(&report, bytes, sizeof(report));
        /* A cancellation release must preserve real keys that won the race. */
        if (clip.text.phase == CLIPBOARD_RELEASING && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
            combine_kbd_states(state, &report);
        if (tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report.modifier, report.keycode)) {
            if (clip.active && clip.text.phase == CLIPBOARD_TYPING && !clip.text.index && !clip.text.down)
                trace(CLIP_DIAG_TYPING, CLIP_REASON_NONE);
            clipboard_text_accepted(&clip.text, now);
            if (clip.active && clip.text.phase == CLIPBOARD_IDLE) {
                trace(CLIP_DIAG_DONE, CLIP_REASON_NONE);
                clip.active = false;
                control(C_DONE, 0, 0);
            }
        }
    }
    firmware_update_unlock();
}
