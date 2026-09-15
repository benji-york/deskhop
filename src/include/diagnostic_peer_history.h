/* Bounded cross-core bridge for read-only peer RAM history. */
#pragma once
#include "peer_history.h"
#include "peer_status.h"

/* Startup/shutdown only, with both cores stopped. */
void diagnostic_peer_history_init(const peer_status_snapshot_t *identity);
void diagnostic_peer_history_shutdown(void);
/* Core 0. poll borrows one immutable result until release. No large snapshot
 * crosses a queue lock; only a pointer/token descriptor is published. */
bool diagnostic_peer_history_request(uint32_t token, unsigned count, uint64_t requested_at_us);
const peer_history_result_t *diagnostic_peer_history_poll(void);
void diagnostic_peer_history_release(void);
/* Core 1. Serving the other board remains independent of a borrowed result. */
void diagnostic_peer_history_task(uint64_t now_us);
void diagnostic_peer_history_receive(bool response, const uint8_t data[8], uint64_t now_us);
