/* Host-independent, opt-in firmware page transfer over protected UART v1. */
#include "fw_batch.h"

#include <string.h>

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

static bool valid_tag(uint32_t tag) {
    return tag != 0 && (tag & ~FW_BATCH_TAG_MASK) == 0;
}

static bool valid_address(uint32_t address) {
    return address < FW_BATCH_IMAGE_SIZE && address % FW_BATCH_PAGE_SIZE == 0;
}

void fw_batch_token_init(fw_batch_token_t *state, uint64_t boot_session) {
    if (!state)
        return;
    uint32_t seed = (uint32_t)boot_session ^ (uint32_t)(boot_session >> 32);
    seed &= FW_BATCH_TAG_MASK;
    *state = (fw_batch_token_t){.next = seed ? seed : FW_BATCH_WORDS};
}

bool fw_batch_token_next(fw_batch_token_t *state, uint32_t *tag) {
    if (!state || !tag || state->exhausted || !valid_tag(state->next))
        return false;
    *tag = state->next;
    if (state->next == FW_BATCH_TAG_MASK)
        state->exhausted = true;
    else
        state->next += FW_BATCH_WORDS;
    return true;
}

bool fw_batch_caps_encode_request(uint32_t tag, uint16_t version, uint8_t out[8]) {
    if (!out || !valid_tag(tag) || !version)
        return false;
    write32(out, tag);
    write16(out + 4, version);
    write16(out + 6, FW_BATCH_PROTOCOL);
    return true;
}

bool fw_batch_caps_decode_request(const uint8_t in[8], uint32_t *tag, uint16_t *version) {
    if (!in || !tag || !version || !valid_tag(read32(in)) || !read16(in + 4)
        || read16(in + 6) != FW_BATCH_PROTOCOL)
        return false;
    *tag = read32(in);
    *version = read16(in + 4);
    return true;
}

bool fw_batch_caps_encode_response(uint32_t tag, uint32_t checksum, uint8_t out[8]) {
    if (!out || !valid_tag(tag))
        return false;
    write32(out, tag);
    write32(out + 4, checksum);
    return true;
}

bool fw_batch_caps_response_matches(const uint8_t in[8], uint32_t tag, uint32_t checksum) {
    return in && valid_tag(tag) && read32(in) == tag && read32(in + 4) == checksum;
}

bool fw_batch_request_encode(uint32_t tag, uint32_t address, uint8_t out[8]) {
    if (!out || !valid_tag(tag) || !valid_address(address))
        return false;
    write32(out, tag);
    write32(out + 4, address);
    return true;
}

bool fw_batch_request_decode(const uint8_t in[8], uint32_t *tag, uint32_t *address) {
    if (!in || !tag || !address || !valid_tag(read32(in)) || !valid_address(read32(in + 4)))
        return false;
    *tag = read32(in);
    *address = read32(in + 4);
    return true;
}

bool fw_batch_data_encode(uint32_t tag, unsigned word, const uint8_t data[4], uint8_t out[8]) {
    if (!out || !data || !valid_tag(tag) || word >= FW_BATCH_WORDS)
        return false;
    write32(out, tag | word);
    memcpy(out + 4, data, 4);
    return true;
}

bool fw_batch_end_encode(uint32_t tag, uint32_t checksum, uint8_t out[8]) {
    return fw_batch_caps_encode_response(tag, checksum, out);
}

uint32_t fw_batch_page_crc(const uint8_t page[FW_BATCH_PAGE_SIZE]) {
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < FW_BATCH_PAGE_SIZE; ++i) {
        crc ^= page[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool fw_batch_rx_begin(fw_batch_rx_t *state, uint32_t tag, uint32_t address,
                       uint8_t page[FW_BATCH_PAGE_SIZE]) {
    if (!state || !page || !valid_tag(tag) || !valid_address(address))
        return false;
    *state = (fw_batch_rx_t){.tag = tag, .address = address, .page = page, .active = true};
    return true;
}

static fw_batch_rx_result_t invalid_page(fw_batch_rx_t *state) {
    state->active = false;
    state->invalid = true;
    return FW_BATCH_RX_INVALID;
}

fw_batch_rx_result_t fw_batch_rx_receive(fw_batch_rx_t *state, fw_batch_kind_t kind,
                                        const uint8_t in[8]) {
    if (!state || !in || !state->active || !state->page)
        return FW_BATCH_RX_IGNORED;

    uint32_t id = read32(in);
    if (kind == FW_BATCH_DATA) {
        if ((id & FW_BATCH_TAG_MASK) != state->tag)
            return FW_BATCH_RX_IGNORED;
        unsigned word = id & ~FW_BATCH_TAG_MASK;
        uint64_t bit = UINT64_C(1) << word;
        if ((state->received & bit) != 0) {
            if (memcmp(state->page + 4 * word, in + 4, 4) != 0)
                return invalid_page(state);
            return FW_BATCH_RX_IGNORED;
        }
        memcpy(state->page + 4 * word, in + 4, 4);
        state->received |= bit;
    }
    else if (kind == FW_BATCH_END) {
        if (id != state->tag)
            return FW_BATCH_RX_IGNORED;
        uint32_t checksum = read32(in + 4);
        if (state->have_end) {
            if (state->checksum != checksum)
                return invalid_page(state);
            return FW_BATCH_RX_IGNORED;
        }
        state->have_end = true;
        state->checksum = checksum;
    }
    else {
        return FW_BATCH_RX_IGNORED;
    }

    if (state->received != UINT64_MAX || !state->have_end)
        return FW_BATCH_RX_PROGRESS;
    if (fw_batch_page_crc(state->page) != state->checksum)
        return invalid_page(state);
    state->active = false;
    return FW_BATCH_RX_COMPLETE;
}

void fw_batch_rx_cancel(fw_batch_rx_t *state) {
    if (state)
        *state = (fw_batch_rx_t){0};
}

bool fw_batch_tx_begin(fw_batch_tx_t *state, uint32_t tag, uint32_t address,
                       const uint8_t page[FW_BATCH_PAGE_SIZE]) {
    if (!state || !page || !valid_tag(tag) || !valid_address(address))
        return false;
    *state = (fw_batch_tx_t){
        .tag = tag, .address = address, .page = page,
        .checksum = fw_batch_page_crc(page), .active = true,
    };
    return true;
}

bool fw_batch_tx_peek(const fw_batch_tx_t *state, fw_batch_kind_t *kind, uint8_t out[8]) {
    if (!state || !kind || !out || !state->active || !state->page)
        return false;
    if (state->cursor < FW_BATCH_WORDS) {
        *kind = FW_BATCH_DATA;
        return fw_batch_data_encode(state->tag, state->cursor, state->page + 4 * state->cursor, out);
    }
    if (state->cursor == FW_BATCH_WORDS) {
        *kind = FW_BATCH_END;
        return fw_batch_end_encode(state->tag, state->checksum, out);
    }
    return false;
}

bool fw_batch_tx_sent(fw_batch_tx_t *state) {
    if (!state || !state->active || state->cursor > FW_BATCH_WORDS)
        return false;
    if (state->cursor == FW_BATCH_WORDS)
        state->active = false;
    else
        ++state->cursor;
    return true;
}

void fw_batch_tx_cancel(fw_batch_tx_t *state) {
    if (state)
        *state = (fw_batch_tx_t){0};
}

bool fw_batch_tx_pump(fw_batch_tx_t *state, fw_batch_emit_t emit, void *context) {
    fw_batch_kind_t kind;
    uint8_t payload[8];
    if (!emit || !fw_batch_tx_peek(state, &kind, payload) || !emit(context, kind, payload))
        return false;
    return fw_batch_tx_sent(state);
}
