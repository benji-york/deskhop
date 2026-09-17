/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "main.h"
#include "diagnostic_transport.h"

diagnostic_transport_snapshot_t diagnostic_transport_snapshot(void) {
    diagnostic_transport_snapshot_t result;
    result.phase_before = diagnostic_rx_cleanup_phase;
    result.flags = uart_get_hw(SERIAL_UART)->fr;
    result.errors = uart_get_hw(SERIAL_UART)->rsr;
    result.dma_enabled = uart_get_hw(SERIAL_UART)->dmacr;
    const uint32_t channels[] = {global_state.dma_rx_channel,
        global_state.dma_control_channel, global_state.dma_tx_channel};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned channel = channels[i];
        result.dma[i].control = dma_channel_hw_addr(channel)->ctrl_trig;
        result.dma[i].remaining = dma_channel_hw_addr(channel)->transfer_count;
        result.dma[i].dreq = dma_debug_hw->ch[channel].ctrdeq;
        result.dma[i].reload = dma_debug_hw->ch[channel].tcr;
        result.dma[i].abort_pending = (dma_hw->abort >> channel) & 1u;
    }
    result.phase_after = diagnostic_rx_cleanup_phase;
    return result;
}
