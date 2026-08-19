/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 *
 * Pure helpers for synchronizing real input activity and deciding when the
 * keep-awake screensaver should stop. Kept independent of Pico SDK APIs so the
 * edge cases can be covered by native sanitizer tests.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ACTIVITY_AGE_UNKNOWN_SEC UINT32_MAX
#define ACTIVITY_AGE_MAX_SEC     (UINT32_MAX - 1U)
#define ACTIVITY_OUTPUT_COUNT    2

uint32_t activity_age_seconds(uint64_t now_us, uint64_t activity_us, bool valid);
bool activity_merge_age_seconds(uint64_t now_us, uint32_t age_sec, uint64_t *activity_us);
uint64_t activity_latest_timestamp(const uint64_t direct_activity[ACTIVITY_OUTPUT_COUNT],
                                   uint8_t direct_valid,
                                   const uint64_t peer_activity[ACTIVITY_OUTPUT_COUNT],
                                   uint8_t peer_valid);
bool screensaver_system_idle_timed_out(uint64_t now_us,
                                       uint64_t latest_activity_us,
                                       uint32_t timeout_sec);
