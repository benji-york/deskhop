#include "peer_observation.h"
#include <assert.h>
#include <stdio.h>

static peer_status_snapshot_t sample(void) {
    return (peer_status_snapshot_t){.protocol=2, .role=1, .minor=100,
        .board_id={1,2,3,4,5,6,7,8}, .boot_session=7, .uptime_ms=1000,
        .runtime={.core_ticks={100,200}, .core_valid=3, .total_bytes=262144}};
}
static void advance(peer_status_snapshot_t *peer) {
    peer->uptime_ms += 300;
    peer->runtime.core_ticks[0] += 300;
    peer->runtime.core_ticks[1] += 300;
}
int main(void) {
    peer_observation_t state;
    peer_status_snapshot_t peer = sample();
    peer_observation_init(&state);
    diagnostic_peer_observation_t result = peer_observation_accept(&state, &peer);
    assert(result.boot == DIAGNOSTIC_PEER_FIRST_SEEN);
    assert(result.progress == DIAGNOSTIC_PROGRESS_BASELINE);
    assert(result.update == DIAGNOSTIC_EXECUTION_NOT_OBSERVED);
    advance(&peer);
    result = peer_observation_accept(&state, &peer);
    assert(result.boot == DIAGNOSTIC_PEER_SAME_BOOT);
    assert(result.progress == DIAGNOSTIC_PROGRESS_ADVANCING);
    result = peer_observation_accept(&state, &peer);
    assert(result.progress == DIAGNOSTIC_PROGRESS_NOT_ADVANCING);
    peer.uptime_ms += 300;
    peer.runtime.core_ticks[0] += 300;
    result = peer_observation_accept(&state, &peer);
    assert(result.progress == DIAGNOSTIC_PROGRESS_NOT_ADVANCING); // core1 stopped
    peer.runtime.core_valid = 1;
    assert(peer_observation_accept(&state, &peer).progress == DIAGNOSTIC_PROGRESS_UNAVAILABLE);
    peer.protocol = 1;
    peer.runtime.core_valid = 3; // untrusted legacy telemetry is never used
    assert(peer_observation_accept(&state, &peer).progress == DIAGNOSTIC_PROGRESS_UNAVAILABLE);
    peer.protocol = 2;
    assert(peer_observation_accept(&state, &peer).progress == DIAGNOSTIC_PROGRESS_BASELINE);

    peer.runtime.core_ticks[0] = peer.runtime.core_ticks[1] = UINT32_MAX - 5;
    peer_observation_accept(&state, &peer);
    advance(&peer);
    assert(peer_observation_accept(&state, &peer).progress == DIAGNOSTIC_PROGRESS_ADVANCING);
    peer.runtime.core_ticks[0]--;
    advance(&peer); // valid net increase
    assert(peer_observation_accept(&state, &peer).progress == DIAGNOSTIC_PROGRESS_ADVANCING);
    peer.runtime.core_ticks[0]--;
    peer.uptime_ms++;
    assert(peer_observation_accept(&state, &peer).progress == DIAGNOSTIC_PROGRESS_NOT_ADVANCING);

    peer.runtime.update_seen = true;
    peer.runtime.phase = DIAGNOSTIC_UPDATE_RECEIVING;
    peer.runtime.source = DIAGNOSTIC_SOURCE_PEER;
    peer.runtime.target_version = 201;
    peer.runtime.update_attempt = 1;
    assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_PENDING_REBOOT);
    peer.runtime.phase = DIAGNOSTIC_UPDATE_REBOOT_PENDING;
    advance(&peer);
    assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_PENDING_REBOOT);
    // Even advertising the intended version within the same session proves no reboot.
    peer.minor = 101;
    advance(&peer);
    assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_PENDING_REBOOT);
    peer.boot_session++;
    peer.uptime_ms = 10;
    peer.runtime = (diagnostic_runtime_snapshot_t){.core_ticks={2,3}, .core_valid=3};
    result = peer_observation_accept(&state, &peer);
    assert(result.boot == DIAGNOSTIC_PEER_NEW_BOOT);
    assert(result.progress == DIAGNOSTIC_PROGRESS_BASELINE);
    assert(result.update == DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS);
    peer_observation_accept(&state, &peer);
    assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS);
    advance(&peer);
    assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_CONFIRMED);
    advance(&peer);
    assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_CONFIRMED);

    // Confirmation records historical completion; it does not claim that
    // counters are advancing now or that the latest query contains telemetry.
    peer.uptime_ms += 300;
    result = peer_observation_accept(&state, &peer);
    assert(result.progress == DIAGNOSTIC_PROGRESS_NOT_ADVANCING);
    assert(result.update == DIAGNOSTIC_EXECUTION_CONFIRMED);
    peer.runtime.core_valid = 1;
    result = peer_observation_accept(&state, &peer);
    assert(result.progress == DIAGNOSTIC_PROGRESS_UNAVAILABLE);
    assert(result.update == DIAGNOSTIC_EXECUTION_CONFIRMED);
    peer.runtime.core_valid = 3;
    result = peer_observation_accept(&state, &peer);
    assert(result.progress == DIAGNOSTIC_PROGRESS_BASELINE);
    assert(result.update == DIAGNOSTIC_EXECUTION_CONFIRMED);

    // A later transfer may start and fail between explicit queries. Its failed
    // runtime target is not the older target whose boot was already confirmed.
    peer.runtime.update_seen = true;
    peer.runtime.source = DIAGNOSTIC_SOURCE_PEER;
    peer.runtime.phase = DIAGNOSTIC_UPDATE_FAILED;
    peer.runtime.target_version = 202;
    peer.runtime.update_attempt = 1;
    peer.uptime_ms += 300;
    result = peer_observation_accept(&state, &peer);
    assert(result.progress == DIAGNOSTIC_PROGRESS_NOT_ADVANCING);
    assert(result.update == DIAGNOSTIC_EXECUTION_CONFIRMED);
    assert(peer.minor == 101); // still executing the historical confirmed version

    for (unsigned failure = 0; failure < 5; ++failure) {
        peer_observation_init(&state);
        peer = sample();
        peer.runtime.update_seen = true;
        peer.runtime.phase = DIAGNOSTIC_UPDATE_RECEIVING;
        peer.runtime.target_version = 201;
        peer.runtime.update_attempt = 1;
        peer_observation_accept(&state, &peer);
        if (failure < 2) {
            peer.runtime.phase = failure ? DIAGNOSTIC_UPDATE_ABANDONED : DIAGNOSTIC_UPDATE_FAILED;
            assert(peer_observation_accept(&state, &peer).update == DIAGNOSTIC_EXECUTION_NOT_OBSERVED);
        } else {
            peer.runtime.phase = DIAGNOSTIC_UPDATE_IDLE;
            peer.boot_session++;
            if (failure == 3) peer.board_id[0]++;
            if (failure == 4) peer.minor = 101;
            result = peer_observation_accept(&state, &peer);
            if (failure == 4) {
                assert(result.update == DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS);
                peer.boot_session++; // another reset cannot confirm the first update
                result = peer_observation_accept(&state, &peer);
            }
            assert(result.update == DIAGNOSTIC_EXECUTION_UNEXPECTED_BOOT);
            if (failure == 3) assert(result.boot == DIAGNOSTIC_PEER_IDENTITY_CHANGED);
        }
    }
    peer_observation_init(&state);
    peer = sample();
    peer_observation_accept(&state, &peer);
    peer.boot_session++;
    advance(&peer);
    result = peer_observation_accept(&state, &peer);
    assert(result.boot == DIAGNOSTIC_PEER_NEW_BOOT);
    assert(result.update == DIAGNOSTIC_EXECUTION_NOT_OBSERVED);
    puts("peer observation tests passed");
}
