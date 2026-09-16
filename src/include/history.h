#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HISTORY_CAPACITY 64u

typedef enum {
    HISTORY_BOOT = 1,
    HISTORY_OUTPUT_LOCAL,
    HISTORY_OUTPUT_PEER,
    HISTORY_USB_MOUNT,
    HISTORY_USB_UNMOUNT,
    HISTORY_HID_MOUNT,
    HISTORY_HID_UNMOUNT,
    HISTORY_DESCRIPTOR_REJECTED,
    HISTORY_PACKET_CHECKSUM_ERROR,
    HISTORY_UART_DROPPED,
    HISTORY_UPDATE_BEGIN,
    HISTORY_UPDATE_PROGRESS,
    HISTORY_UPDATE_PHASE,
    HISTORY_PEER_OBSERVED,
    HISTORY_PEER_PROGRESS,
    HISTORY_TRANSFER_SOURCE,
    HISTORY_TRANSFER_TIMING,
    HISTORY_TRANSFER_COUNT,
} history_type_t;

/* Source observations, not receiver commit or delivery acknowledgements.
 * Existing event IDs and the fixed history wire record stay unchanged. */
typedef enum {
    TRANSFER_CAPS_QUEUED = 1, TRANSFER_BATCH_BEGIN, TRANSFER_WORDS_BEGIN,
    TRANSFER_RETRY, TRANSFER_PROGRESS, TRANSFER_BATCH_END, TRANSFER_WORDS_END,
} history_transfer_phase_t;
typedef enum {
    TRANSFER_MODE_NONE, TRANSFER_MODE_PAGES, TRANSFER_MODE_WORDS, TRANSFER_MODE_MIXED,
} history_transfer_mode_t;
typedef enum {
    TRANSFER_ELAPSED_US = 1, TRANSFER_PAGE_SERVICE_US, TRANSFER_PAGE_GAP_US,
    TRANSFER_PAGE_MAX_US,
} history_transfer_timing_t;
typedef enum {
    TRANSFER_PAGE_REQUESTS = 1, TRANSFER_WORD_REQUESTS, TRANSFER_PAGE_RETRIES,
} history_transfer_count_t;

typedef struct {
    uint64_t seq;
    uint64_t time_us;
    uint32_t value;
    uint8_t type;
    uint8_t a;
    uint8_t b;
    uint8_t reserved;
} history_event_t;

/* Fixed storage, with all access serialized by the caller. Sequence zero is
 * never used. UINT64_MAX is reserved as an exclusive end; once reached,
 * record() refuses further writes, preserving the retained history. */
typedef struct {
    history_event_t records[HISTORY_CAPACITY];
    uint64_t next_seq;
    uint64_t overwritten;
    unsigned count;
} history_store_t;

typedef struct {
    uint64_t first_seq;
    uint64_t end_seq;
    uint64_t oldest_seq;
    uint64_t overwritten;
    unsigned count;
} history_window_t;

void history_store_init(history_store_t *);
void history_store_record(history_store_t *, uint64_t time_us, history_type_t,
                          uint8_t a, uint8_t b, uint32_t value);
/* Select at most limit of the latest retained events, oldest first. Limits
 * above capacity are clamped; zero selects none. All returned values describe
 * this instant, including the fixed exclusive end. New events cannot extend
 * the window, but can overwrite its records before a reader reaches them. */
history_window_t history_store_window(const history_store_t *, unsigned limit);
/* Copy a retained event, or return false without changing out when the event
 * is absent. A copied event remains independent of subsequent store writes. */
bool history_store_read(const history_store_t *, uint64_t seq, history_event_t *out);
