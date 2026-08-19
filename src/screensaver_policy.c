/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 */

#include "screensaver_policy.h"

#define US_PER_SECOND 1000000ULL

uint32_t activity_age_seconds(uint64_t now_us, uint64_t activity_us, bool valid) {
    if (!valid)
        return ACTIVITY_AGE_UNKNOWN_SEC;

    if (activity_us >= now_us)
        return 0;

    uint64_t age_sec = (now_us - activity_us) / US_PER_SECOND;
    return age_sec > ACTIVITY_AGE_MAX_SEC ? ACTIVITY_AGE_MAX_SEC : (uint32_t)age_sec;
}

bool activity_merge_age_seconds(uint64_t now_us, uint32_t age_sec, uint64_t *activity_us) {
    if (age_sec == ACTIVITY_AGE_UNKNOWN_SEC)
        return false;

    uint64_t age_us = (uint64_t)age_sec * US_PER_SECOND;

    /* The peer can have been running longer than this Pico. Such an event is
       already older than our boot-time grace period and cannot be represented
       on our local monotonic clock, so retaining the prior value is safest. */
    if (age_us > now_us)
        return false;

    uint64_t candidate = now_us - age_us;
    if (candidate > *activity_us)
        *activity_us = candidate;

    return true;
}

uint64_t activity_latest_timestamp(const uint64_t direct_activity[ACTIVITY_OUTPUT_COUNT],
                                   uint8_t direct_valid,
                                   const uint64_t peer_activity[ACTIVITY_OUTPUT_COUNT],
                                   uint8_t peer_valid) {
    uint64_t latest = 0;

    for (uint8_t output = 0; output < ACTIVITY_OUTPUT_COUNT; output++) {
        uint8_t mask = 1u << output;

        if ((direct_valid & mask) && direct_activity[output] > latest)
            latest = direct_activity[output];

        if ((peer_valid & mask) && peer_activity[output] > latest)
            latest = peer_activity[output];
    }

    return latest;
}

bool screensaver_system_idle_timed_out(uint64_t now_us,
                                       uint64_t latest_activity_us,
                                       uint32_t timeout_sec) {
    if (timeout_sec == 0 || latest_activity_us > now_us)
        return false;

    return now_us - latest_activity_us >= (uint64_t)timeout_sec * US_PER_SECOND;
}
