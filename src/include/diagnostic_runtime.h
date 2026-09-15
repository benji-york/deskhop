#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DIAGNOSTIC_UPDATE_IDLE = 0,
    DIAGNOSTIC_UPDATE_RECEIVING,
    DIAGNOSTIC_UPDATE_PAUSED,
    DIAGNOSTIC_UPDATE_VALIDATING,
    DIAGNOSTIC_UPDATE_REBOOT_PENDING,
    DIAGNOSTIC_UPDATE_FAILED,
    DIAGNOSTIC_UPDATE_ABANDONED,
} diagnostic_update_phase_t;
typedef enum {
    DIAGNOSTIC_SOURCE_NONE = 0, DIAGNOSTIC_SOURCE_PEER, DIAGNOSTIC_SOURCE_USB,
} diagnostic_update_source_t;

/* core_valid bits 0/1 distinguish unpublished checkpoints from counter zero.
 * Counts wrap modulo 2^32 and represent 1 kHz diagnostic task checkpoints,
 * not individual instructions or proof that every task completed. */
typedef struct {
    uint32_t core_ticks[2], core_age_ms[2];
    uint32_t received_bytes, total_bytes, progress_age_ms, update_attempt;
    uint16_t target_version;
    uint8_t core_valid, phase, source;
    bool update_seen;
} diagnostic_runtime_snapshot_t;

typedef enum { DIAGNOSTIC_PEER_UNOBSERVED, DIAGNOSTIC_PEER_FIRST_SEEN,
               DIAGNOSTIC_PEER_SAME_BOOT, DIAGNOSTIC_PEER_NEW_BOOT,
               DIAGNOSTIC_PEER_IDENTITY_CHANGED } diagnostic_peer_boot_t;
typedef enum { DIAGNOSTIC_PROGRESS_UNAVAILABLE, DIAGNOSTIC_PROGRESS_BASELINE,
               DIAGNOSTIC_PROGRESS_ADVANCING, DIAGNOSTIC_PROGRESS_NOT_ADVANCING } diagnostic_progress_t;
typedef enum { DIAGNOSTIC_EXECUTION_NOT_OBSERVED, DIAGNOSTIC_EXECUTION_PENDING_REBOOT,
               DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS, DIAGNOSTIC_EXECUTION_CONFIRMED,
               DIAGNOSTIC_EXECUTION_UNEXPECTED_BOOT } diagnostic_execution_t;
typedef struct {
    diagnostic_peer_boot_t boot;
    diagnostic_progress_t progress;
    diagnostic_execution_t update;
} diagnostic_peer_observation_t;

/* Startup only, before USB callbacks and core 1. */
void diagnostic_runtime_init(void);
/* One checkpoint per existing diagnostic task, including console-disabled builds. */
void diagnostic_runtime_checkpoint(unsigned core);
/* Reads only the short runtime lock; never acquires the firmware/flash lock. */
diagnostic_runtime_snapshot_t diagnostic_runtime_snapshot(void);
/* Update hooks are serialized by the existing firmware lock. They publish only
 * small scalar state and sparse history records. Lock order is strictly
 * firmware -> runtime (released) -> history; neither reader takes firmware. */
void diagnostic_update_begin(diagnostic_update_source_t source, uint16_t target_version);
void diagnostic_update_progress(uint32_t received_bytes);
void diagnostic_update_phase(diagnostic_update_phase_t phase);
