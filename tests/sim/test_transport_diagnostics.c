/* Register-only observation with production reader; no actual device opened. */
#include "main.h"
#include "diagnostic_transport.h"
#include <assert.h>
#include <stdio.h>

void sim_init(uint8_t, void (*)(int, int, int, const void *, int));
void sim_destroy(void);

int main(void) {
    sim_init(1, NULL);
    const unsigned channels[] = {global_state.dma_rx_channel,
        global_state.dma_control_channel, global_state.dma_tx_channel};
    sim_uart_hw_t *uart = uart_get_hw(SERIAL_UART);
    uart->rsr = 8; /* Overrun: reading diagnostics must not clear it. */
    uart->dmacr = 3;
    dma_hw->abort = 1u << channels[0];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned ch = channels[i];
        dma_channel_hw_addr(ch)->ctrl_trig = 0x61000001u + i;
        dma_channel_hw_addr(ch)->transfer_count = 123u + i;
        dma_debug_hw->ch[ch].ctrdeq = 61u + i;
        dma_debug_hw->ch[ch].tcr = 1024u + i;
    }
    /* Can still observe RX while the independent firmware lock is held.
     * Native locks assert against recursive acquisition. */
    firmware_update_lock();
    for (unsigned phase = 0; phase <= DIAGNOSTIC_RX_RESTART; ++phase) {
        diagnostic_rx_cleanup_phase = phase;
        const diagnostic_transport_snapshot_t first = diagnostic_transport_snapshot();
        const diagnostic_transport_snapshot_t second = diagnostic_transport_snapshot();
        assert(!memcmp(&first, &second, sizeof(first)));
        assert(first.phase_before == phase && first.phase_after == phase);
        assert(first.errors == 8 && uart->rsr == 8 && first.dma_enabled == 3);
        for (unsigned i = 0; i < 3; ++i) {
            assert(first.dma[i].control == 0x61000001u + i);
            assert(first.dma[i].remaining == 123u + i);
            assert(first.dma[i].dreq == 61u + i && first.dma[i].reload == 1024u + i);
            assert(first.dma[i].abort_pending == (i == 0));
            assert(dma_channel_hw_addr(channels[i])->transfer_count == 123u + i);
        }
        assert(dma_hw->abort == (1u << channels[0]));
    }
    firmware_update_unlock();
    sim_destroy();
    puts("Local transport diagnostics passed: independent lock, ordered channels, read-only registers, cleanup stages");
    return 0;
}
