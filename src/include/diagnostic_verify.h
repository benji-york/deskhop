/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "verification.h"
#include "peer_status.h"

/* Startup/shutdown only with both cores stopped. */
void diagnostic_verify_init(const peer_status_snapshot_t *identity);
void diagnostic_verify_shutdown(void);
/* Core0: request starts fresh local and peer scans, returns at most one result
 * per poll. Match both token and remote flag. No cached verification in status. */
bool diagnostic_verify_request(uint32_t token, uint64_t requested_at_us);
bool diagnostic_verify_poll(verify_result_t *);
/* Nonblocking local result freshness check at presentation. May downgrade an
 * old local result to CHANGED/UPDATE_ACTIVE/BUSY. Never changes peer evidence. */
void diagnostic_verify_recheck_local(verify_result_t *);
/* Core1, once per existing 1kHz diagnostic task even without a local console. */
void diagnostic_verify_task(uint64_t now_us);
void diagnostic_verify_receive(bool response, const uint8_t data[8], uint64_t now_us);
