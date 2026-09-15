#pragma once
#include "peer_status.h"

/* Core 1 only. Observations are derived from successful explicit status
 * queries, never heartbeats or mutable updater metadata. No persistence. */
typedef struct {
    bool seen;
    peer_status_snapshot_t previous;
    bool expected, reboot_seen, confirmed;
    uint8_t expected_id[8];
    uint64_t previous_session;
    uint32_t previous_attempt;
    uint16_t target_version;
} peer_observation_t;

void peer_observation_init(peer_observation_t *);
diagnostic_peer_observation_t peer_observation_accept(peer_observation_t *,
                                                       const peer_status_snapshot_t *);
