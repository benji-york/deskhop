/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console.h"
#include "diagnostic_history.h"
#include "diagnostic_peer.h"
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
    bool history_active;
    uint64_t history_next, history_end;
    uint32_t query_token;
    uint64_t query_started_us;
    peer_status_result_t peer_result;
    enum { LINE_OK, LINE_TOO_LONG, LINE_INVALID } line_error;
    char line[CONSOLE_LINE_SIZE], tx[CONSOLE_TX_SIZE];
    unsigned line_used, tx_used, tx_sent;
} console;

void console_disconnect(void) {
    console.connected = false;
    console.previous_cr = false;
    console.line_error = LINE_OK;
    console.line_used = console.tx_used = console.tx_sent = 0;
    console.peer_waiting = console.peer_ready = false;
    console.history_active = false;
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

static void begin_history(unsigned count) {
    const history_window_t window = diagnostic_history_window(count);
    console.history_next = window.first_seq;
    console.history_end = window.end_seq;
    console.history_active = true;
    appendf("BEGIN history\r\nboard=%c\r\nboot_session=%08lx%08lx\r\n"
            "scope=local\r\npeer=not_implemented\r\ncapacity=%u\r\n"
            "returned=%u\r\noverwritten=%llu\r\n",
            console.board, (unsigned long)(console.boot_session >> 32),
            (unsigned long)(uint32_t)console.boot_session, HISTORY_CAPACITY,
            window.count, (unsigned long long)window.overwritten);
}

static void append_history_row(void) {
    /* Copy only one compact event while its slot is protected by the bridge.
     * Formatting and USB backpressure never hold the history lock. The fixed
     * end sequence keeps a stalled reader from chasing new producer events. */
    history_event_t event;
    uint64_t seq = console.history_next++;
    if (!diagnostic_history_read(seq, &event)) {
        appendf("GAP board=%c seq=%llu\r\n", console.board,
                (unsigned long long)seq);
        return;
    }
    appendf("board=%c seq=%llu uptime_ms=%llu event=", console.board,
            (unsigned long long)event.seq,
            (unsigned long long)(event.time_us / 1000));
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
    default:
        appendf("unknown type=%u", (unsigned)event.type);
        break;
    }
    append("\r\n");
}

static void command(uint64_t now_us) {
    console.line[console.line_used] = '\0';
    if (console.line_error == LINE_TOO_LONG) {
        append("ERROR line too long (maximum 63 characters)\r\n");
    } else if (console.line_error == LINE_INVALID) {
        append("ERROR command must contain printable ASCII characters\r\n");
    } else if (strcmp(console.line, "help") == 0) {
        append("BEGIN help\r\n"
               "DeskHop diagnostic console - all commands are read-only.\r\n"
               "\r\n"
               "  help             Show this help.\r\n"
               "  status           Show both boards' identity, build, boot session and uptime.\r\n"
               "  history [count]  Show this board's recent events (default 16; 1..64).\r\n"
               "\r\n"
               "Status queries both boards by default and prints this board first.\r\n"
               "A missing or older peer is reported after a bounded timeout.\r\n"
               "History is local in this release; peer history will follow.\r\n"
               "Firmware verification will follow in a later release.\r\n"
               "The image CRC is metadata captured at boot, not an integrity check.\r\n"
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
            begin_history(count);
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

    transmit();
    if (console.tx_used)
        return;

    if (console.history_active) {
        if (console.history_next < console.history_end) {
            append_history_row();
        } else {
            console.history_active = false;
            append("END history\r\n");
            append(prompt);
        }
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
