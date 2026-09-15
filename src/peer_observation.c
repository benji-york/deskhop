/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "peer_observation.h"
#include <string.h>

void peer_observation_init(peer_observation_t *state) {
    memset(state, 0, sizeof(*state));
}

static bool active_update(const diagnostic_runtime_snapshot_t *runtime) {
    return runtime->update_seen && runtime->target_version >= 100
           && runtime->phase >= DIAGNOSTIC_UPDATE_RECEIVING
           && runtime->phase <= DIAGNOSTIC_UPDATE_REBOOT_PENDING;
}

static bool target_matches(uint16_t target, const peer_status_snapshot_t *peer) {
    return (uint32_t)peer->major * 1000u + peer->minor + 100u == target;
}

diagnostic_peer_observation_t peer_observation_accept(peer_observation_t *state,
                                                       const peer_status_snapshot_t *peer) {
    diagnostic_peer_observation_t result = {0};
    bool same_id = state->seen && !memcmp(state->previous.board_id, peer->board_id, 8);
    bool same_boot = same_id && state->previous.boot_session == peer->boot_session;
    result.boot = !state->seen ? DIAGNOSTIC_PEER_FIRST_SEEN
                  : !same_id ? DIAGNOSTIC_PEER_IDENTITY_CHANGED
                  : !same_boot ? DIAGNOSTIC_PEER_NEW_BOOT : DIAGNOSTIC_PEER_SAME_BOOT;
    if (peer->protocol >= 2 && peer->runtime.core_valid == 3) {
        result.progress = DIAGNOSTIC_PROGRESS_BASELINE;
        if (same_boot && state->previous.protocol >= 2
            && state->previous.runtime.core_valid == 3) {
            result.progress = DIAGNOSTIC_PROGRESS_NOT_ADVANCING;
            uint32_t delta0 = peer->runtime.core_ticks[0] - state->previous.runtime.core_ticks[0];
            uint32_t delta1 = peer->runtime.core_ticks[1] - state->previous.runtime.core_ticks[1];
            if (peer->uptime_ms > state->previous.uptime_ms
                && delta0 && delta0 < UINT32_C(0x80000000)
                && delta1 && delta1 < UINT32_C(0x80000000))
                result.progress = DIAGNOSTIC_PROGRESS_ADVANCING;
        }
    }

    /* A known target observed before reset is necessary to associate a later
     * boot with that update. First contact with an already-updated board
     * cannot retrospectively prove an update or reset happened. */
    if (peer->protocol >= 2 && active_update(&peer->runtime)
        && (!state->expected || !same_id || peer->boot_session != state->previous_session
            || peer->runtime.update_attempt != state->previous_attempt)) {
        state->expected = true;
        state->reboot_seen = state->confirmed = false;
        memcpy(state->expected_id, peer->board_id, 8);
        state->previous_session = peer->boot_session;
        state->previous_attempt = peer->runtime.update_attempt;
        state->target_version = peer->runtime.target_version;
    }
    if (state->expected) {
        if (memcmp(state->expected_id, peer->board_id, 8)) {
            result.update = DIAGNOSTIC_EXECUTION_UNEXPECTED_BOOT;
            state->expected = false;
        } else if (peer->boot_session == state->previous_session) {
            if (peer->protocol >= 2 && (peer->runtime.phase == DIAGNOSTIC_UPDATE_FAILED
                                       || peer->runtime.phase == DIAGNOSTIC_UPDATE_ABANDONED)) {
                state->expected = false;
            } else {
                result.update = DIAGNOSTIC_EXECUTION_PENDING_REBOOT;
            }
        } else if ((state->reboot_seen && !same_boot)
                   || !target_matches(state->target_version, peer)) {
            result.update = DIAGNOSTIC_EXECUTION_UNEXPECTED_BOOT;
            state->expected = false;
        } else {
            if (!same_boot)
                state->confirmed = false;
            state->reboot_seen = true;
            if (same_boot && result.progress == DIAGNOSTIC_PROGRESS_ADVANCING)
                state->confirmed = true;
            result.update = state->confirmed ? DIAGNOSTIC_EXECUTION_CONFIRMED
                                             : DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS;
        }
    }
    state->seen = true;
    state->previous = *peer;
    return result;
}
