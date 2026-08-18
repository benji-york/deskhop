/* Host-independent firmware transfer state machine. */

#include "fw_update.h"

bool fw_update_peer_compatible(uint16_t protocol_marker) {
    return protocol_marker == FW_UPDATE_PROTOCOL_MARKER;
}

fw_update_action_t fw_update_next_action(fw_update_source_t source,
                                         bool word_complete,
                                         bool image_dirty,
                                         uint32_t now_us,
                                         uint32_t requested_at_us,
                                         uint32_t progressed_at_us,
                                         uint32_t peer_seen_at_us) {
    if (source != FW_UPDATE_SOURCE_PULL)
        return FW_UPDATE_WAIT;

    if ((uint32_t)(now_us - progressed_at_us) >= FW_UPDATE_STALL_TIMEOUT_US) {
        if (!image_dirty)
            return FW_UPDATE_ABANDON;

        if ((uint32_t)(now_us - peer_seen_at_us) < FW_UPDATE_PEER_TIMEOUT_US)
            return FW_UPDATE_RESTART;

        return FW_UPDATE_PAUSE;
    }

    if (word_complete
        || (uint32_t)(now_us - requested_at_us) >= FW_UPDATE_RESPONSE_TIMEOUT_US)
        return FW_UPDATE_REQUEST;

    return FW_UPDATE_WAIT;
}

bool fw_update_response_expected(fw_update_source_t source,
                                 bool request_pending,
                                 uint32_t expected_address,
                                 uint32_t response_address) {
    return source == FW_UPDATE_SOURCE_PULL
        && request_pending
        && response_address == expected_address;
}

bool fw_update_metadata_matches(uint32_t magic,
                                uint16_t version,
                                uint32_t embedded_checksum,
                                uint32_t calculated_checksum,
                                uint16_t expected_version,
                                bool check_version) {
    const uint32_t firmware_metadata_magic = 0xf00d;

    return magic == firmware_metadata_magic
        && embedded_checksum == calculated_checksum
        && (!check_version || version == expected_version);
}

bool fw_update_mark_block(uint32_t *bitmap,
                          uint16_t *received_count,
                          uint32_t block_number) {
    uint32_t bitmap_index = block_number / 32;
    uint32_t bitmap_mask = 1u << (block_number % 32);

    if ((bitmap[bitmap_index] & bitmap_mask) != 0)
        return false;

    bitmap[bitmap_index] |= bitmap_mask;
    if (received_count != NULL)
        (*received_count)++;
    return true;
}
