/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "verification.h"

static verify_assessment_t assessment(verify_verdict_t verdict, verify_reason_t reason) {
    return (verify_assessment_t){verdict, reason};
}

verify_assessment_t verification_assess(const verify_result_t *result,
                                         uint16_t expected_version, uint32_t expected_slot_crc32) {
    if (result->transport != VERIFY_TRANSPORT_OK) {
        verify_reason_t reason = result->transport == VERIFY_TRANSPORT_TIMEOUT ? VERIFY_REASON_TIMEOUT
                                 : result->transport == VERIFY_TRANSPORT_BUSY ? VERIFY_REASON_BUSY
                                                                            : VERIFY_REASON_INVALID;
        return assessment(VERIFY_UNVERIFIED, reason);
    }
    const verify_snapshot_t *s = &result->snapshot;
    if (s->outcome != VERIFY_SCAN_COMPLETE) {
        verify_reason_t reason = s->outcome == VERIFY_SCAN_UPDATE_ACTIVE ? VERIFY_REASON_UPDATE_ACTIVE
                               : s->outcome == VERIFY_SCAN_CHANGED ? VERIFY_REASON_CHANGED
                               : s->outcome == VERIFY_SCAN_BUSY ? VERIFY_REASON_BUSY
                               : s->outcome == VERIFY_SCAN_TIMEOUT ? VERIFY_REASON_TIMEOUT
                                                                  : VERIFY_REASON_INVALID;
        return assessment(VERIFY_UNVERIFIED, reason);
    }
    if (s->role > 1 || s->minor >= 1000 || s->bytes_read != VERIFY_IMAGE_BYTES
        || s->completed_us <= s->started_us || s->completed_us - s->started_us > VERIFY_TIMEOUT_US)
        return assessment(VERIFY_UNVERIFIED, VERIFY_REASON_INVALID);
    if (s->generation_start != s->generation_end
        || s->start.update_attempt != s->end.update_attempt
        || s->start.phase != s->end.phase || s->start.source != s->end.source)
        return assessment(VERIFY_UNVERIFIED, VERIFY_REASON_CHANGED);
    uint32_t version = (uint32_t)s->major * 1000 + s->minor + 100;
    if (version > UINT16_MAX || s->metadata_magic != UINT32_C(0xf00d)
        || s->metadata_reserved || s->metadata_version != version
        || s->metadata_crc32 != s->image_crc_at_boot)
        return assessment(VERIFY_FAIL, VERIFY_REASON_METADATA);
    if (version != expected_version)
        return assessment(VERIFY_FAIL, VERIFY_REASON_BUILD);
    if (s->slot_crc32 != expected_slot_crc32)
        return assessment(VERIFY_FAIL, VERIFY_REASON_CRC);
    if (s->start.core_valid != 3 || s->end.core_valid != 3)
        return assessment(VERIFY_UNVERIFIED, VERIFY_REASON_CORE_UNAVAILABLE);
    for (unsigned core = 0; core < 2; ++core) {
        uint32_t delta = s->end.core_ticks[core] - s->start.core_ticks[core];
        if (!delta || delta >= UINT32_C(0x80000000)
            || s->end.core_age_ms[core] > VERIFY_CORE_MAX_AGE_MS)
            return assessment(VERIFY_UNVERIFIED, VERIFY_REASON_CORE_PROGRESS);
    }
    return assessment(VERIFY_PASS, VERIFY_REASON_MATCH);
}
