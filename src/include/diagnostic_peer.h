/* Bounded cross-core bridge for read-only peer diagnostics. */
#pragma once
#include "peer_status.h"

/* Startup/shutdown only, with both cores stopped. Identity is copied once. */
void diagnostic_peer_init(const peer_status_snapshot_t *identity);
void diagnostic_peer_shutdown(void);
/* Core 0: one queued request/result, no waiting. Tokens survive console close. */
bool diagnostic_peer_request(uint32_t token, uint64_t requested_at_us);
bool diagnostic_peer_poll(peer_status_result_t *result);
/* Core 1 owns the protocol and immutable local identity. */
void diagnostic_peer_task(uint64_t now_us);
void diagnostic_peer_receive(bool response, const uint8_t data[8], uint64_t now_us);
