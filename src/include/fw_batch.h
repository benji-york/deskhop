/* Host-independent, opt-in firmware page transfer over protected UART v1. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_BATCH_PROTOCOL 1u
#define FW_BATCH_PAGE_SIZE 256u
#define FW_BATCH_IMAGE_SIZE 262144u
#define FW_BATCH_WORDS (FW_BATCH_PAGE_SIZE / 4u)
#define FW_BATCH_TAG_MASK UINT32_C(0xffffffc0)

/* Tags name requests, not addresses. Keep this allocator outside the state that
 * is reset when an image transfer restarts. Never reuse a tag in one boot;
 * exhaustion requires the existing word protocol, not counter wraparound. The
 * boot-session seed separates normal reboots; page and image CRC checks remain
 * mandatory because a finite on-wire tag cannot guarantee cross-boot uniqueness. */
typedef struct {
    uint32_t next;
    bool exhausted;
} fw_batch_token_t;

void fw_batch_token_init(fw_batch_token_t *, uint64_t boot_session);
bool fw_batch_token_next(fw_batch_token_t *, uint32_t *tag);

/* All wire payloads are exactly eight bytes, little endian. CAPS request:
 * tag32, expected source version16, protocol16. Response: echoed tag32, source
 * metadata CRC32. Callers must also check the source version before responding. */
bool fw_batch_caps_encode_request(uint32_t tag, uint16_t version, uint8_t out[8]);
bool fw_batch_caps_decode_request(const uint8_t in[8], uint32_t *tag, uint16_t *version);
bool fw_batch_caps_encode_response(uint32_t tag, uint32_t checksum, uint8_t out[8]);
bool fw_batch_caps_response_matches(const uint8_t in[8], uint32_t tag, uint32_t checksum);

/* Page request: tag32, aligned byte address32, within the fixed image slot. */
bool fw_batch_request_encode(uint32_t tag, uint32_t address, uint8_t out[8]);
bool fw_batch_request_decode(const uint8_t in[8], uint32_t *tag, uint32_t *address);

typedef enum { FW_BATCH_DATA, FW_BATCH_END } fw_batch_kind_t;

/* Data: (tag | word index)32, four original image bytes. End: tag32, page CRC32.
 * The outer UART CRC protects the type and all eight bytes as usual. */
bool fw_batch_data_encode(uint32_t tag, unsigned word, const uint8_t data[4], uint8_t out[8]);
bool fw_batch_end_encode(uint32_t tag, uint32_t checksum, uint8_t out[8]);
uint32_t fw_batch_page_crc(const uint8_t page[FW_BATCH_PAGE_SIZE]);

typedef struct {
    uint32_t tag;
    uint32_t address;
    uint32_t checksum;
    uint64_t received;
    uint8_t *page;
    bool active;
    bool have_end;
    bool invalid;
} fw_batch_rx_t;

typedef enum {
    FW_BATCH_RX_IGNORED = 0,
    FW_BATCH_RX_PROGRESS,
    FW_BATCH_RX_COMPLETE,
    FW_BATCH_RX_INVALID,
} fw_batch_rx_result_t;

/* The caller owns page storage and starts a collector only after its request is
 * admitted for TX. A rejected begin changes nothing. END may precede DATA;
 * duplicate words never advance progress, and conflicting duplicates poison
 * this request. COMPLETE/INVALID disarm the collector. Only COMPLETE permits a
 * flash write. The caller applies bounded timeouts/retries using a fresh tag. */
bool fw_batch_rx_begin(fw_batch_rx_t *, uint32_t tag, uint32_t address,
                       uint8_t page[FW_BATCH_PAGE_SIZE]);
fw_batch_rx_result_t fw_batch_rx_receive(fw_batch_rx_t *, fw_batch_kind_t,
                                        const uint8_t in[8]);
void fw_batch_rx_cancel(fw_batch_rx_t *);

typedef struct {
    uint32_t tag;
    uint32_t address;
    uint32_t checksum;
    const uint8_t *page;
    uint8_t cursor;
    bool active;
} fw_batch_tx_t;

/* The source page must stay immutable until completion/cancel. peek is pure;
 * sent advances once, only after actual admission to the wire/DMA. This never
 * queues a page-sized burst and gives the caller ordinary-traffic priority. */
bool fw_batch_tx_begin(fw_batch_tx_t *, uint32_t tag, uint32_t address,
                       const uint8_t page[FW_BATCH_PAGE_SIZE]);
bool fw_batch_tx_peek(const fw_batch_tx_t *, fw_batch_kind_t *, uint8_t out[8]);
bool fw_batch_tx_sent(fw_batch_tx_t *);
void fw_batch_tx_cancel(fw_batch_tx_t *);
typedef bool (*fw_batch_emit_t)(void *, fw_batch_kind_t, const uint8_t payload[8]);
bool fw_batch_tx_pump(fw_batch_tx_t *, fw_batch_emit_t, void *context);
