/* Ownership oracles for the production RX privacy eraser, under ASan/UBSan.
 * DMA register behavior is modeled by node.c; no hardware is opened. */
#include "main.h"
#include <assert.h>
#include <stdio.h>

void sim_init(uint8_t, void (*)(int, int, int, const void *, int));
void sim_destroy(void);
void sim_rx_byte(uint8_t);
void sim_set_time(uint64_t);
void sim_dma_rx_fixture(uint32_t, uint32_t, bool, int);
void sim_dma_rx_assert_resumed(uint32_t);
void sim_dma_rx_assert_pauses(unsigned);

static void expect_ring(uint32_t start, uint32_t length,
                        uint32_t unread_start, uint32_t writer, int flushed_index) {
    for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE; ++i) {
        bool target = ((i - start) & (DMA_RX_BUFFER_SIZE - 1)) < length;
        unsigned unread = (writer - unread_start) & (DMA_RX_BUFFER_SIZE - 1);
        bool owned_by_rx = ((i - unread_start) & (DMA_RX_BUFFER_SIZE - 1)) < unread;
        uint8_t expected = target && !owned_by_rx ? 0 : 0xa5;
        if ((int)i == flushed_index && (!target || owned_by_rx)) expected = 0x3c;
        assert(uart_rxbuf[i] == expected);
    }
}

static void check(uint32_t consumer, uint32_t remaining, uint32_t start,
                   uint32_t length, bool reload_inflight, int last_byte) {
    memset(uart_rxbuf, 0xa5, DMA_RX_BUFFER_SIZE);
    sim_dma_rx_fixture(consumer, remaining, reload_inflight, last_byte);
    unsigned before_writer = (DMA_RX_BUFFER_SIZE - remaining) & (DMA_RX_BUFFER_SIZE - 1);
    unsigned expected_remaining = remaining ? remaining : DMA_RX_BUFFER_SIZE;
    if (last_byte >= 0) --expected_remaining;
    if (expected_remaining == 0) expected_remaining = DMA_RX_BUFFER_SIZE;
    unsigned writer = (DMA_RX_BUFFER_SIZE - expected_remaining) & (DMA_RX_BUFFER_SIZE - 1);
    uart_rx_erase_consumed(&global_state, start, length);
    expect_ring(start, length, consumer, writer, last_byte >= 0 ? (int)before_writer : -1);
    sim_dma_rx_assert_resumed(expected_remaining);
    /* Restart must preserve the actual producer address; no consumed-slot
     * cleanup may rewind DMA or restart from the beginning of the buffer. */
    sim_rx_byte(0x7e);
    assert(uart_rxbuf[writer] == 0x7e);
}

static void parser_erase_and_partial_deadline(void) {
    memset(uart_rxbuf, 0, DMA_RX_BUFFER_SIZE);
    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    uart_packet_t packet = {.type = CLIPBOARD_MSG, .data = {0x09, 0, 0, 'z'}};
    uint8_t encoded[RAW_PACKET_LENGTH];
    write_raw_packet(encoded, &packet);
    for (unsigned i = 0; i < sizeof(encoded); ++i) sim_rx_byte(encoded[i]);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == RAW_PACKET_LENGTH);
    for (unsigned i = 0; i < RAW_PACKET_LENGTH; ++i) assert(uart_rxbuf[i] == 0);
    const uint8_t *stored = (const uint8_t *)&global_state.in_packet;
    for (unsigned i = 0; i < sizeof(global_state.in_packet); ++i) assert(stored[i] == 0);

    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    uint64_t start = time_us_64();
    for (unsigned i = 0; i < 12; ++i) sim_rx_byte(encoded[i]);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 0 && uart_rxbuf[0] == UART_FRAME_START);
    sim_set_time(start + 49999);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 0 && uart_rxbuf[0] == UART_FRAME_START);
    /* Later bytes cannot extend the first partial-frame's absolute deadline. */
    sim_rx_byte(encoded[12]);
    sim_set_time(start + 50000);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 13);
    for (unsigned i = 0; i < 13; ++i) assert(uart_rxbuf[i] == 0);
}

static void parser_bounds_and_resync(void) {
    uart_packet_t packet = {.type = CLIPBOARD_MSG, .data = {0x09, 0, 0, 'z'}};
    uint8_t encoded[RAW_PACKET_LENGTH];
    write_raw_packet(encoded, &packet);

    memset(uart_rxbuf, 0, DMA_RX_BUFFER_SIZE);
    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    for (unsigned i = 0; i < 31; ++i) sim_rx_byte(0x4a);
    for (unsigned n = 0; n < 2; ++n)
        for (unsigned i = 0; i < sizeof(encoded); ++i) sim_rx_byte(encoded[i]);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 63); /* largest possible single paused wipe */
    for (unsigned i = 0; i < 63; ++i) assert(uart_rxbuf[i] == 0);
    assert(!memcmp(uart_rxbuf + 63, encoded, sizeof(encoded)));
    sim_dma_rx_assert_pauses(1);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 95);
    for (unsigned i = 0; i < 95; ++i) assert(uart_rxbuf[i] == 0);
    sim_dma_rx_assert_pauses(2);

    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    for (unsigned i = 0; i < 32; ++i) sim_rx_byte(0x4a);
    for (unsigned i = 0; i < sizeof(encoded); ++i) sim_rx_byte(encoded[i]);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 32); /* one frame of junk is the work limit */
    assert(!memcmp(uart_rxbuf + 32, encoded, sizeof(encoded)));
    sim_dma_rx_assert_pauses(1);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 64);
    sim_dma_rx_assert_pauses(2);

    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    for (unsigned i = 0; i < 12; ++i) sim_rx_byte(encoded[i]);
    for (unsigned i = 0; i < sizeof(encoded); ++i) sim_rx_byte(encoded[i]);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 44); /* preserve the next delimiter after truncation */
    for (unsigned i = 0; i < 44; ++i) assert(uart_rxbuf[i] == 0);
    sim_dma_rx_assert_pauses(1);

    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    packet.type = FLASH_LED_MSG;
    write_raw_packet(encoded, &packet);
    for (unsigned i = 0; i < sizeof(encoded); ++i) sim_rx_byte(encoded[i]);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == RAW_PACKET_LENGTH);
    assert(!memcmp(uart_rxbuf, encoded, sizeof(encoded)));
    sim_dma_rx_assert_pauses(0); /* ordinary valid frames never pause DMA */
}

static void idle_ring_lap_cleanup(void) {
    sim_destroy();
    sim_init(0, NULL);
    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, -1);
    for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE; ++i) sim_rx_byte(0x4a);
    uint64_t start = time_us_64();
    packet_receiver_task(&global_state); /* an exact producer lap looks empty */
    sim_set_time(start + 49999);
    packet_receiver_task(&global_state);
    sim_dma_rx_assert_pauses(0);
    for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE; ++i) assert(uart_rxbuf[i] == 0x4a);
    sim_set_time(start + 50000);
    for (unsigned step = 0; step < DMA_RX_BUFFER_SIZE / 32; ++step) {
        packet_receiver_task(&global_state);
        sim_dma_rx_assert_pauses(step + 1);
        for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE; ++i)
            assert(uart_rxbuf[i] == (i < (step + 1) * 32 ? 0 : 0x4a));
    }
    sim_set_time(start + 100000);
    for (unsigned i = 0; i < 64; ++i) packet_receiver_task(&global_state);
    sim_dma_rx_assert_pauses(32); /* an already clean idle ring never pauses again */

    /* A second whole producer lap at the same modulo pointer must restart
     * cleanup, even though no producer pointer change can be observed. */
    for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE; ++i) sim_rx_byte(0x4b);
    start = time_us_64();
    packet_receiver_task(&global_state);
    sim_set_time(start + 49999);
    packet_receiver_task(&global_state);
    sim_dma_rx_assert_pauses(32);
    sim_set_time(start + 50000);
    for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE / 32; ++i)
        packet_receiver_task(&global_state);
    sim_dma_rx_assert_pauses(64);
    for (unsigned i = 0; i < DMA_RX_BUFFER_SIZE; ++i) assert(uart_rxbuf[i] == 0);

    sim_destroy();
    sim_init(0, NULL);
    memset(uart_rxbuf, 0x4a, DMA_RX_BUFFER_SIZE);
    sim_dma_rx_fixture(0, DMA_RX_BUFFER_SIZE, false, UART_FRAME_START);
    start = time_us_64();
    packet_receiver_task(&global_state);
    sim_set_time(start + 50000);
    packet_receiver_task(&global_state); /* final RX beat arrives after empty snapshot */
    assert(global_state.dma_ptr == 0 && uart_rxbuf[0] == UART_FRAME_START);
    for (unsigned i = 1; i < 32; ++i) assert(uart_rxbuf[i] == 0);
    for (unsigned i = 32; i < DMA_RX_BUFFER_SIZE; ++i) assert(uart_rxbuf[i] == 0x4a);
    sim_dma_rx_assert_resumed(DMA_RX_BUFFER_SIZE - 1);
    packet_receiver_task(&global_state);
    assert(global_state.dma_ptr == 0 && uart_rxbuf[0] == UART_FRAME_START);
    sim_dma_rx_assert_pauses(1); /* new unread input remains owned by the parser */
}

int main(void) {
    sim_init(0, NULL);
    assert(global_state.dma_rx_channel != global_state.dma_tx_channel);
    assert(global_state.dma_rx_channel != global_state.dma_control_channel);
    check(64, 800, 32, 32, false, -1);       /* ordinary consumed frame */
    check(8, 900, 1000, 32, false, -1);     /* consumed span crosses ring boundary */
    check(932, 108, 900, 32, false, -1);    /* producer wrapped into old consumed span */
    check(32, 1014, 0, 32, false, 0x3c);   /* final RX beat completes during abort */
    check(32, 1, 0, 32, false, 0x3c);      /* that beat exhausts RX at natural wrap */
    check(32, 0, 0, 32, true, -1);         /* already accepted control reload completes */
    check(32, 0, 0, 32, false, -1);        /* zero count before chain can reload */
    parser_erase_and_partial_deadline();
    parser_bounds_and_resync();
    idle_ring_lap_cleanup();
    sim_destroy();
    puts("Clipboard RX privacy DMA ownership tests passed (wrap, in-flight RX/control, zero count, resume position, packet erase, 50 ms partial deadline, bounded scan/resync, normal-frame no-pause, exact-lap idle scrub)");
    return 0;
}
