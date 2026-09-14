/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console.h"
#include "tusb.h"

#if DH_CONSOLE && CFG_TUD_CDC
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CONSOLE_RX_BUDGET 32u
#define CONSOLE_TX_BUDGET 64u
#define CONSOLE_LINE_SIZE 64u
#define CONSOLE_TX_SIZE 1024u

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
    enum { LINE_OK, LINE_TOO_LONG, LINE_INVALID } line_error;
    char line[CONSOLE_LINE_SIZE], tx[CONSOLE_TX_SIZE];
    unsigned line_used, tx_used, tx_sent;
} console;

void console_disconnect(void) {
    console.connected = false;
    console.previous_cr = false;
    console.line_error = LINE_OK;
    console.line_used = console.tx_used = console.tx_sent = 0;
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
               "  help      Show this help.\r\n"
               "  status    Show this board's identity, build, boot session and uptime.\r\n"
               "\r\n"
               "First slice: local status only. Peer queries, history and firmware\r\n"
               "verification will follow in later releases.\r\n"
               "The image CRC is metadata captured at boot, not an integrity check.\r\n"
               "Enter submits; Backspace edits; Ctrl-C cancels a line.\r\n"
               "END help\r\n");
    } else if (strcmp(console.line, "status") == 0) {
        /* Identity is captured before USB/core 1 start. In particular, do not
         * read _running_fw, which the peer updater changes BEFORE reboot. */
        int count = snprintf(console.tx + console.tx_used,
                             sizeof(console.tx) - console.tx_used,
                             "BEGIN status\r\n"
                             "board=%c\r\nboard_id=%s\r\nbuild=%s\r\n"
                             "image_crc_at_boot=%08lx\r\n"
                             "boot_session=%08lx%08lx\r\nuptime_ms=%llu\r\n"
                             "peer=not_implemented\r\nverification=not_implemented\r\n"
                             "END status\r\n",
                             console.board, console.board_id, build,
                             (unsigned long)console.image_crc_at_boot,
                             (unsigned long)(console.boot_session >> 32),
                             (unsigned long)(uint32_t)console.boot_session,
                             (unsigned long long)(now_us / 1000));
        if (count > 0) {
            unsigned available = sizeof(console.tx) - console.tx_used;
            console.tx_used += (unsigned)count < available ? (unsigned)count : available - 1;
        }
    } else if (console.line_used != 0) {
        append("ERROR unknown command; type help\r\n");
    }
    console.line_used = 0;
    console.line_error = LINE_OK;
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

    transmit();
    if (console.tx_used)
        return;

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
