/* Read-only, local USB diagnostics. No payload, FIFO data, or memory addresses. */
#pragma once
#include <stdint.h>

enum {
    DIAGNOSTIC_RX_IDLE, DIAGNOSTIC_RX_STOP_CONTROL, DIAGNOSTIC_RX_STOP_DATA,
    DIAGNOSTIC_RX_CLEAR, DIAGNOSTIC_RX_RESTART,
};

typedef struct {
    uint32_t control, remaining, dreq, reload, abort_pending;
} diagnostic_dma_snapshot_t;
typedef struct {
    uint32_t flags, errors, dma_enabled;
    uint32_t phase_before, phase_after;
    diagnostic_dma_snapshot_t dma[3]; /* RX, reload, TX */
} diagnostic_transport_snapshot_t;

/* Core 1 is the sole writer; core 0 reads one naturally aligned word.
 * Stages are observations, not locks or recovery actions. */
extern volatile uint32_t diagnostic_rx_cleanup_phase;
/* Sequential register reads, deliberately not an atomic snapshot. Never
 * stops DMA, clears an error, reads UARTDR, takes a lock or waits for a peer. */
diagnostic_transport_snapshot_t diagnostic_transport_snapshot(void);
