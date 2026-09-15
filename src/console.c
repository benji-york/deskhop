/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console.h"
#include "diagnostic_history.h"
#include "diagnostic_peer.h"
#include "diagnostic_peer_history.h"
#include "diagnostic_runtime.h"
#include "tusb.h"

#if DH_CONSOLE && CFG_TUD_CDC
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CONSOLE_RX_BUDGET 32u
#define CONSOLE_TX_BUDGET 64u
#define CONSOLE_LINE_SIZE 64u
#define CONSOLE_TX_SIZE 1024u
#define CONSOLE_PEER_TIMEOUT_US UINT64_C(600000)
#define CONSOLE_HISTORY_TIMEOUT_US UINT64_C(3500000)

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
static const char build[] = STRINGIFY(VERSION_MAJOR) "." STRINGIFY(VERSION_MINOR);
static const char prompt[] = "deskhop> ";

/* No other core or IRQ accesses this state; TinyUSB callbacks run in core 0's
 * tud_task(). One pending response applies USB backpressure to further input. */
static struct {
    char board, board_id[17];
    uint64_t boot_session;
    uint32_t image_crc_at_boot;
    bool connected, previous_cr;
    bool peer_waiting, peer_ready;
    bool history_active, history_metadata_sent;
    unsigned history_count, history_captured, history_local_next, history_peer_next;
    const char *history_outcome;
    peer_history_snapshot_t history_local;
    const peer_history_result_t *history_peer;
    uint32_t query_token;
    uint64_t query_started_us;
    peer_status_result_t peer_result;
    enum { LINE_OK, LINE_TOO_LONG, LINE_INVALID } line_error;
    char line[CONSOLE_LINE_SIZE], tx[CONSOLE_TX_SIZE];
    unsigned line_used, tx_used, tx_sent;
} console;

static void release_history_peer(void) {
    if (console.history_peer) {
        diagnostic_peer_history_release();
        console.history_peer = NULL;
    }
}

void console_disconnect(void) {
    console.connected = false;
    console.previous_cr = false;
    console.line_error = LINE_OK;
    console.line_used = console.tx_used = console.tx_sent = 0;
    console.peer_waiting = console.peer_ready = false;
    console.history_active = false;
    release_history_peer();
    /* TinyUSB resets CDC endpoints BEFORE the unmount callback. read_flush()
     * rearms OUT, so it must never run after that reset (ep_out is then zero).
     * USB reset already clears those FIFOs; a DTR drop still needs a flush. */
    if (tud_mounted()) {
        tud_cdc_read_flush();
        tud_cdc_write_clear();
    }
}

void console_init(uint8_t board_role, const char *board_id,
                  uint64_t boot_session, uint32_t image_crc_at_boot) {
    memset(&console, 0, sizeof(console));
    console.board = board_role == 0 ? 'A' : 'B';
    snprintf(console.board_id, sizeof(console.board_id), "%s", board_id);
    console.boot_session = boot_session;
    console.image_crc_at_boot = image_crc_at_boot;
    console.query_token = (uint32_t)boot_session;
}

static void append(const char *text) {
    size_t count = strlen(text);
    size_t available = sizeof(console.tx) - console.tx_used;
    /* Responses below are fixed and fit alongside one RX budget of echo.
     * Retain a bound here as well so future text edits cannot corrupt memory. */
    if (count > available)
        count = available;
    memcpy(console.tx + console.tx_used, text, count);
    console.tx_used += count;
}

static void appendf(const char *format, ...) {
    unsigned available = sizeof(console.tx) - console.tx_used;
    if (!available)
        return;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(console.tx + console.tx_used, available, format, args);
    va_end(args);
    if (count > 0)
        console.tx_used += (unsigned)count < available ? (unsigned)count : available - 1;
}

static void finish_status(const char *outcome) {
    appendf("peer=%s\r\nverification=not_implemented\r\nEND status\r\n", outcome);
    console.peer_waiting = console.peer_ready = false;
}

static const char *update_source_name(unsigned source) {
    switch (source) {
    case DIAGNOSTIC_SOURCE_NONE: return "none";
    case DIAGNOSTIC_SOURCE_PEER: return "peer";
    case DIAGNOSTIC_SOURCE_USB: return "usb";
    default: return "unknown";
    }
}

static const char *update_phase_name(unsigned phase) {
    switch (phase) {
    case DIAGNOSTIC_UPDATE_IDLE: return "idle";
    case DIAGNOSTIC_UPDATE_RECEIVING: return "receiving";
    case DIAGNOSTIC_UPDATE_PAUSED: return "paused";
    case DIAGNOSTIC_UPDATE_VALIDATING: return "validating";
    case DIAGNOSTIC_UPDATE_REBOOT_PENDING: return "reboot_pending";
    case DIAGNOSTIC_UPDATE_FAILED: return "failed";
    case DIAGNOSTIC_UPDATE_ABANDONED: return "abandoned";
    default: return "unknown";
    }
}

static const char *peer_boot_name(unsigned boot) {
    switch (boot) {
    case DIAGNOSTIC_PEER_UNOBSERVED: return "unobserved";
    case DIAGNOSTIC_PEER_FIRST_SEEN: return "first_seen";
    case DIAGNOSTIC_PEER_SAME_BOOT: return "same_boot";
    case DIAGNOSTIC_PEER_NEW_BOOT: return "new_boot";
    case DIAGNOSTIC_PEER_IDENTITY_CHANGED: return "identity_changed";
    default: return "unknown";
    }
}

static const char *progress_name(unsigned progress) {
    switch (progress) {
    case DIAGNOSTIC_PROGRESS_UNAVAILABLE: return "unavailable";
    case DIAGNOSTIC_PROGRESS_BASELINE: return "baseline";
    case DIAGNOSTIC_PROGRESS_ADVANCING: return "advancing";
    case DIAGNOSTIC_PROGRESS_NOT_ADVANCING: return "not_advancing";
    default: return "unknown";
    }
}

static const char *execution_name(unsigned execution) {
    switch (execution) {
    case DIAGNOSTIC_EXECUTION_NOT_OBSERVED: return "not_observed";
    case DIAGNOSTIC_EXECUTION_PENDING_REBOOT: return "pending_reboot";
    case DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS: return "awaiting_progress";
    case DIAGNOSTIC_EXECUTION_CONFIRMED: return "confirmed";
    case DIAGNOSTIC_EXECUTION_UNEXPECTED_BOOT: return "unexpected_boot";
    default: return "unknown";
    }
}

static void append_version(unsigned encoded_version) {
    if (encoded_version < 100) {
        append("unknown");
    } else {
        unsigned version = encoded_version - 100;
        appendf("%u.%u", version / 1000, version % 1000);
    }
}

static void append_runtime(char board, const diagnostic_runtime_snapshot_t *runtime) {
    for (unsigned core = 0; core < 2; ++core) {
        if (runtime->core_valid & (1u << core)) {
            appendf("board=%c core=%u checkpoints=%lu age_ms=%lu\r\n", board, core,
                    (unsigned long)runtime->core_ticks[core],
                    (unsigned long)runtime->core_age_ms[core]);
        } else {
            appendf("board=%c core=%u checkpoints=unavailable age_ms=unavailable\r\n", board, core);
        }
    }
    appendf("board=%c update seen=%u source=%s phase=%s received=%lu total=%lu progress_age_ms=",
            board, (unsigned)runtime->update_seen, update_source_name(runtime->source),
            update_phase_name(runtime->phase), (unsigned long)runtime->received_bytes,
            (unsigned long)runtime->total_bytes);
    if (!runtime->update_seen)
        append("unavailable");
    else
        appendf("%lu", (unsigned long)runtime->progress_age_ms);
    append(" target=");
    append_version(runtime->target_version);
    appendf(" attempt=%lu\r\n", (unsigned long)runtime->update_attempt);
}

static void append_observation(char board, const diagnostic_peer_observation_t *observation) {
    appendf("board=%c observation boot=%s progress=%s update=%s\r\n", board,
            peer_boot_name(observation->boot), progress_name(observation->progress),
            execution_name(observation->update));
}

static void append_peer(void) {
    const peer_status_snapshot_t *peer = &console.peer_result.snapshot;
    char board_id[17];
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < 8; ++i) {
        board_id[2*i] = hex[peer->board_id[i] >> 4];
        board_id[2*i+1] = hex[peer->board_id[i] & 15];
    }
    board_id[16] = 0;
    appendf("\r\nboard=%c\r\nboard_id=%s\r\nbuild=%u.%u\r\n"
            "image_crc_at_boot=%08lx\r\nboot_session=%08lx%08lx\r\n"
            "uptime_ms=%llu\r\n",
            peer->role == 0 ? 'A' : 'B', board_id,
            (unsigned)peer->major, (unsigned)peer->minor,
            (unsigned long)peer->image_crc_at_boot,
            (unsigned long)(peer->boot_session >> 32),
            (unsigned long)(uint32_t)peer->boot_session,
            (unsigned long long)peer->uptime_ms);
    char board = peer->role == 0 ? 'A' : 'B';
    if (peer->protocol == 2)
        append_runtime(board, &peer->runtime);
    else
        appendf("board=%c runtime=unavailable protocol=%u\r\n", board, (unsigned)peer->protocol);
    append_observation(board, &console.peer_result.observation);
}

static bool history_count(const char *line, unsigned *count) {
    if (line[7] == '\0') {
        *count = 16;
        return true;
    }
    /* Do not accept signs, trailing words or wraparound from strtoul(). */
    const char *p = line + 8;
    unsigned value = 0;
    if (!*p)
        return false;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10 + (unsigned)(*p - '0');
        if (value > HISTORY_CAPACITY)
            return false;
    }
    *count = value;
    return value != 0;
}

static char output_name(unsigned output) {
    return output == 0 ? 'A' : output == 1 ? 'B' : '?';
}

static void begin_history(unsigned count, uint64_t now_us) {
    peer_history_snapshot_t *local = &console.history_local;
    local->role = console.board == 'A' ? 0 : 1;
    local->boot_session = console.boot_session;
    local->window = diagnostic_history_window_at(count, &local->sampled_at_us);
    local->gap_mask = 0;
    console.history_count = count;
    console.history_captured = console.history_local_next = console.history_peer_next = 0;
    console.history_active = true;
    console.history_metadata_sent = false;
    console.history_outcome = NULL;
    console.query_started_us = now_us;
    if (++console.query_token == 0)
        ++console.query_token;
    if (!diagnostic_peer_history_request(console.query_token, count, now_us))
        console.history_outcome = "busy";
    appendf("BEGIN history\r\nscope=both\r\nrequested_per_board=%u\r\n"
            "timing=approximate_snapshot_alignment\r\ncapacity_per_board=%u\r\n",
            count, HISTORY_CAPACITY);
}

static void poll_history_peer(void) {
    if (console.history_peer)
        return; /* The borrowed result cannot be polled until released. */
    const peer_history_result_t *result = diagnostic_peer_history_poll();
    if (!result)
        return;
    if (!console.history_active || console.history_outcome || result->token != console.query_token) {
        diagnostic_peer_history_release();
        return;
    }
    if (result->outcome == PEER_HISTORY_OK) {
        const peer_history_snapshot_t *peer = &result->snapshot;
        if (peer->role == console.history_local.role || peer->role > 1
            || peer->window.count > console.history_count
            || peer->window.end_seq < peer->window.first_seq
            || peer->window.end_seq - peer->window.first_seq != peer->window.count
            || result->first_response_us < result->requested_at_us) {
            console.history_outcome = "invalid";
            diagnostic_peer_history_release();
        } else {
            console.history_peer = result;
            console.history_outcome = "ok";
        }
    } else {
        console.history_outcome = result->outcome == PEER_HISTORY_TIMEOUT
                                ? "timeout_or_unsupported" : "invalid";
        diagnostic_peer_history_release();
    }
}

static void capture_history_tick(uint64_t now_us) {
    if (!console.history_active)
        return;
    peer_history_snapshot_t *local = &console.history_local;
    if (console.history_captured < local->window.count) {
        unsigned index = console.history_captured++;
        if (!diagnostic_history_read(local->window.first_seq + index, &local->events[index]))
            local->gap_mask |= UINT64_C(1) << index;
    }
    if (!console.history_outcome && now_us - console.query_started_us >= CONSOLE_HISTORY_TIMEOUT_US)
        console.history_outcome = "timeout_or_unsupported";
}

static void append_history_metadata(const peer_history_snapshot_t *snapshot) {
    appendf("board=%c\r\nboot_session=%08lx%08lx\r\nsampled_uptime_ms=%llu\r\n"
            "returned=%u\r\noverwritten=%llu\r\n",
            output_name(snapshot->role), (unsigned long)(snapshot->boot_session >> 32),
            (unsigned long)(uint32_t)snapshot->boot_session,
            (unsigned long long)(snapshot->sampled_at_us / 1000), snapshot->window.count,
            (unsigned long long)snapshot->window.overwritten);
}

static uint64_t history_age(const peer_history_snapshot_t *snapshot, unsigned index) {
    return snapshot->sampled_at_us - snapshot->events[index].time_us;
}

static void append_history_row(const peer_history_snapshot_t *snapshot, unsigned index) {
    uint64_t seq = snapshot->window.first_seq + index;
    if (snapshot->gap_mask & (UINT64_C(1) << index)) {
        appendf("GAP board=%c seq=%llu\r\n", output_name(snapshot->role),
                (unsigned long long)seq);
        return;
    }
    const history_event_t event = snapshot->events[index];
    appendf("board=%c seq=%llu uptime_ms=%llu age_ms=%llu event=", output_name(snapshot->role),
            (unsigned long long)event.seq,
            (unsigned long long)(event.time_us / 1000),
            (unsigned long long)(history_age(snapshot, index) / 1000));
    switch (event.type) {
    case HISTORY_BOOT:
        appendf("boot build=%u.%u output=%c", (unsigned)(event.value >> 16),
                (unsigned)(event.value & 0xffff), output_name(event.a));
        break;
    case HISTORY_OUTPUT_LOCAL:
    case HISTORY_OUTPUT_PEER:
        appendf("%s old=%c new=%c", event.type == HISTORY_OUTPUT_LOCAL
                ? "output_local" : "output_peer", output_name(event.a),
                output_name(event.b));
        break;
    case HISTORY_USB_MOUNT:
        append("usb_mount");
        break;
    case HISTORY_USB_UNMOUNT:
        append("usb_unmount");
        break;
    case HISTORY_HID_MOUNT:
    case HISTORY_HID_UNMOUNT:
        appendf("%s device=%u instance=%u protocol=%u keyboard=%u mouse=%u",
                event.type == HISTORY_HID_MOUNT ? "hid_mount" : "hid_unmount",
                (unsigned)event.a, (unsigned)event.b, (unsigned)(event.value & 0xff),
                (unsigned)!!(event.value & 0x100), (unsigned)!!(event.value & 0x200));
        break;
    case HISTORY_DESCRIPTOR_REJECTED:
        appendf("descriptor_rejected device=%u instance=%u",
                (unsigned)event.a, (unsigned)event.b);
        break;
    case HISTORY_PACKET_CHECKSUM_ERROR:
    case HISTORY_UART_DROPPED:
        appendf("%s packet_type=%lu", event.type == HISTORY_PACKET_CHECKSUM_ERROR
                ? "packet_checksum_error" : "uart_dropped", (unsigned long)event.value);
        break;
    case HISTORY_UPDATE_BEGIN:
        appendf("update_begin source=%s target=", update_source_name(event.a));
        append_version(event.value);
        break;
    case HISTORY_UPDATE_PROGRESS:
        appendf("update_progress source=%s percent=%u received=%lu", update_source_name(event.a),
                (unsigned)event.b, (unsigned long)event.value);
        break;
    case HISTORY_UPDATE_PHASE:
        appendf("update_phase phase=%s source=%s target=", update_phase_name(event.a),
                update_source_name(event.b));
        append_version(event.value);
        break;
    case HISTORY_PEER_OBSERVED:
        appendf("peer_observed peer=%c boot=%s build=", output_name(event.a), peer_boot_name(event.b));
        append_version(event.value);
        break;
    case HISTORY_PEER_PROGRESS:
        appendf("peer_progress peer=%c progress=%s build=", output_name(event.a), progress_name(event.b));
        append_version(event.value);
        break;
    default:
        appendf("unknown type=%u a=%u b=%u value=%lu", (unsigned)event.type,
                (unsigned)event.a, (unsigned)event.b, (unsigned long)event.value);
        break;
    }
    append("\r\n");
}

static void emit_history_tick(void) {
    if (console.history_captured != console.history_local.window.count || !console.history_outcome)
        return;
    const peer_history_snapshot_t *local = &console.history_local;
    const peer_history_snapshot_t *peer = console.history_peer ? &console.history_peer->snapshot : NULL;
    if (!console.history_metadata_sent) {
        append_history_metadata(local);
        if (peer) {
            append("\r\n");
            append_history_metadata(peer);
        }
        appendf("peer=%s\r\n", console.history_outcome);
        if (peer)
            appendf("peer_capture_bound_us=%llu\r\n",
                    (unsigned long long)(console.history_peer->first_response_us
                                       - console.history_peer->requested_at_us));
        append("\r\n");
        console.history_metadata_sent = true;
        return;
    }
    unsigned li = console.history_local_next, pi = console.history_peer_next;
    bool have_local = li < local->window.count;
    bool have_peer = peer && pi < peer->window.count;
    if (!have_local && !have_peer) {
        console.history_active = false;
        release_history_peer();
        append("END history\r\n");
        append(prompt);
        return;
    }
    /* GAP has no time to invent. Emit it when it reaches its board's head;
     * otherwise merge event ages oldest first, preserving both sequence orders.
     * A wins equal-age ties regardless of which board owns this terminal. */
    bool choose_local = !have_peer || (have_local && (local->gap_mask & (UINT64_C(1) << li)));
    if (have_local && have_peer && !choose_local && !(peer->gap_mask & (UINT64_C(1) << pi))) {
        uint64_t la = history_age(local, li), pa = history_age(peer, pi);
        choose_local = la > pa || (la == pa && local->role == 0);
    }
    if (choose_local) {
        append_history_row(local, console.history_local_next++);
    } else {
        append_history_row(peer, console.history_peer_next++);
    }
}

static void command(uint64_t now_us) {
    console.line[console.line_used] = '\0';
    if (console.line_error == LINE_TOO_LONG) {
        append("ERROR line too long (maximum 63 characters)\r\n");
    } else if (console.line_error == LINE_INVALID) {
        append("ERROR command must contain printable ASCII characters\r\n");
    } else if (strcmp(console.line, "help") == 0) {
        append("BEGIN help\r\n"
               "DeskHop console - all commands are read-only.\r\n"
               "\r\n"
               "  help             Show help.\r\n"
               "  status           Show build, boot, core checkpoints and update state.\r\n"
               "  history [count]  Show both boards' recent events (default 16; 1..64 each).\r\n"
               "\r\n"
               "Status queries both boards by default and prints this board first.\r\n"
               "A missing peer has a bounded timeout; older firmware may lack runtime data.\r\n"
               "Core counts mark diagnostic task checkpoints.\r\n"
               "Peer observations update only when status is queried.\r\n"
               "Confirmation is historical and version-only; progress compares the latest queries.\r\n"
               "History merges snapshots by approximate event age; each board keeps its own order.\r\n"
               "History and observations live in RAM until reboot.\r\n"
               "GAP means unavailable during capture or through an older peer protocol.\r\n"
               "The image CRC is boot metadata, not an integrity check.\r\n"
               "Enter submits; Backspace edits; Ctrl-C cancels a line.\r\n"
               "END help\r\n");
    } else if (strcmp(console.line, "status") == 0) {
        /* Identity is captured before USB/core 1 start. In particular, do not
         * read _running_fw, which the peer updater changes BEFORE reboot. */
        appendf("BEGIN status\r\n"
                             "board=%c\r\nboard_id=%s\r\nbuild=%s\r\n"
                             "image_crc_at_boot=%08lx\r\n"
                             "boot_session=%08lx%08lx\r\nuptime_ms=%llu\r\n"
                             "\r\n",
                             console.board, console.board_id, build,
                             (unsigned long)console.image_crc_at_boot,
                             (unsigned long)(console.boot_session >> 32),
                             (unsigned long)(uint32_t)console.boot_session,
                             (unsigned long long)(now_us / 1000));
        const diagnostic_runtime_snapshot_t runtime = diagnostic_runtime_snapshot();
        append_runtime(console.board, &runtime);
        if (++console.query_token == 0)
            ++console.query_token;
        console.query_started_us = now_us;
        console.peer_ready = false;
        console.peer_waiting = diagnostic_peer_request(console.query_token, now_us);
        if (!console.peer_waiting)
            finish_status("busy");
    } else if (strcmp(console.line, "history") == 0
               || strncmp(console.line, "history ", 8) == 0) {
        unsigned count;
        if (history_count(console.line, &count))
            begin_history(count, now_us);
        else
            append("ERROR usage: history [count] (decimal 1..64)\r\n");
    } else if (console.line_used != 0) {
        append("ERROR unknown command; type help\r\n");
    }
    console.line_used = 0;
    console.line_error = LINE_OK;
    if (!console.peer_waiting && !console.history_active)
        append(prompt);
}

static void transmit(void) {
    unsigned remaining = console.tx_used - console.tx_sent;
    unsigned available = tud_cdc_write_available();
    unsigned count = remaining < available ? remaining : available;
    if (count > CONSOLE_TX_BUDGET)
        count = CONSOLE_TX_BUDGET;
    if (count)
        console.tx_sent += tud_cdc_write(console.tx + console.tx_sent, count);
    /* Flush only asks TinyUSB to submit what fits; it never waits for a host. */
    tud_cdc_write_flush();
    if (console.tx_sent == console.tx_used)
        console.tx_sent = console.tx_used = 0;
}

void console_task(uint64_t now_us) {
    if (!tud_cdc_connected()) {
        if (console.connected)
            console_disconnect();
        poll_history_peer();
        return;
    }
    if (!console.connected) {
        console.connected = true;
        append("\r\nDeskHop diagnostic console. Type help.\r\n");
        append(prompt);
    }

    /* The result queue crosses cores. Consume at most one per tick, including
       late replies from a disconnected terminal; only this query can finish. */
    peer_status_result_t result;
    if (diagnostic_peer_poll(&result) && console.peer_waiting && !console.peer_ready
        && result.token == console.query_token) {
        console.peer_result = result;
        console.peer_ready = true;
    }

    poll_history_peer();
    /* Capture continues even while USB output is stalled. */
    capture_history_tick(now_us);

    transmit();
    if (console.tx_used)
        return;

    if (console.history_active) {
        emit_history_tick();
        return;
    }

    if (console.peer_waiting) {
        if (console.peer_ready) {
            if (console.peer_result.outcome == PEER_STATUS_OK) {
                append_peer();
                finish_status("ok");
            } else {
                finish_status(console.peer_result.outcome == PEER_STATUS_TIMEOUT
                              ? "timeout_or_unsupported" : "invalid");
            }
        } else if (now_us - console.query_started_us >= CONSOLE_PEER_TIMEOUT_US) {
            /* Remain bounded even if core 1 cannot deliver a result. */
            finish_status("timeout_or_unsupported");
        } else {
            return;
        }
        append(prompt);
        return;
    }

    /* Stop after one completed command or a fixed number of input bytes.
     * Leaving the remaining bytes in TinyUSB lets bulk OUT apply backpressure. */
    for (unsigned i = 0; i < CONSOLE_RX_BUDGET; ++i) {
        int value = tud_cdc_read_char();
        if (value < 0)
            break;
        char c = (char)value;
        if (console.previous_cr && c == '\n') {
            console.previous_cr = false;
            continue;
        }
        console.previous_cr = c == '\r';
        if (c == '\r' || c == '\n') {
            append("\r\n");
            command(now_us);
            break;
        }
        if (c == 3) { /* Ctrl-C */
            console.line_used = 0;
            console.line_error = LINE_OK;
            append("^C\r\n");
            append(prompt);
            break;
        }
        if (console.line_error != LINE_OK)
            continue;
        if (c == '\b' || c == 127) {
            if (console.line_used) {
                --console.line_used;
                append("\b \b");
            }
        } else if (value < 32 || value > 126) {
            console.line_error = LINE_INVALID;
        } else if (console.line_used == sizeof(console.line) - 1) {
            console.line_error = LINE_TOO_LONG;
        } else {
            console.line[console.line_used++] = c;
            console.tx[console.tx_used++] = c;
        }
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == 0 && !dtr)
        console_disconnect();
}
#endif
