/* Deterministic bus seams around real, unmodified vendored production bodies. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb_definitions.h"

#define __no_inline_not_in_flash_func(name) name
#define __tusb_irq_path_func(name) name
#define __force_inline
#define IRQ_RX_COMP_MASK 1u
#define IRQ_TX_COMP_MASK 1u
#define PIO_USB_INTS_ENDPOINT_COMPLETE_BITS (1u << 5)
#define PIO_USB_INTS_ENDPOINT_ERROR_BITS (1u << 6)
#define PIO_USB_INTS_ENDPOINT_STALLED_BITS (1u << 7)

typedef struct { uint32_t irq; } fake_pio_t;
typedef struct {
    fake_pio_t *pio_usb_rx;
    fake_pio_t *pio_usb_tx;
    unsigned sm_rx;
    uint8_t usb_rx_buffer[128];
} pio_port_t;

static endpoint_t pio_usb_ep_pool[PIO_USB_EP_POOL_CNT];
static root_port_t pio_usb_root_port[PIO_USB_ROOT_PORT_CNT];
#define PIO_USB_ROOT_PORT(index) (&pio_usb_root_port[(index)])
static fake_pio_t rx, tx;
static pio_port_t port;
static uint8_t payload[128];
static uint8_t response_pid, response_sync;
static int response_len;
static unsigned sent_tokens, sent_packets, rx_stops;
static uint8_t last_token, last_packet[68];
static uint16_t last_packet_len;

/* Physical PIO, CRC calculation, and wire timing are outside this test seam. */
static uint16_t calc_usb_crc16(const uint8_t *data, uint16_t len) {
    (void)data;
    (void)len;
    return 0x1234;
}
static void pio_usb_bus_prepare_receive(pio_port_t *pp) { (void)pp; }
static void pio_usb_bus_start_receive(pio_port_t *pp) { (void)pp; }
static void pio_usb_bus_send_token(pio_port_t *pp, uint8_t token, uint8_t addr, uint8_t ep) {
    (void)pp;
    (void)addr;
    (void)ep;
    sent_tokens++;
    last_token = token;
}
static void pio_usb_bus_wait_handshake(pio_port_t *pp) {
    pp->usb_rx_buffer[0] = response_sync;
    pp->usb_rx_buffer[1] = response_pid;
}
static int pio_usb_bus_receive_packet_and_handshake(pio_port_t *pp, uint8_t handshake) {
    assert(handshake == USB_PID_ACK);
    pio_usb_bus_wait_handshake(pp);
    if (response_len > 0)
        memcpy(pp->usb_rx_buffer + 2, payload, (size_t)response_len);
    return response_len;
}
static void pio_usb_bus_usb_transfer(pio_port_t *pp, uint8_t *data, uint16_t len) {
    (void)pp;
    assert(len <= sizeof(last_packet));
    sent_packets++;
    last_packet_len = len;
    memcpy(last_packet, data, len);
}
static void pio_sm_set_enabled(fake_pio_t *pio, unsigned sm, bool enabled) {
    (void)sm;
    assert(pio == &rx && !enabled);
    rx_stops++;
}
void pio_usb_ll_transfer_complete(endpoint_t *ep, uint32_t flag);
#include "pio_production.h"

static endpoint_t *begin(bool out, bool setup, uint8_t *buffer, uint16_t length) {
    memset(pio_usb_ep_pool, 0, sizeof(pio_usb_ep_pool));
    memset(pio_usb_root_port, 0, sizeof(pio_usb_root_port));
    memset(&port, 0, sizeof(port));
    rx.irq = IRQ_RX_COMP_MASK;
    tx.irq = IRQ_TX_COMP_MASK;
    port.pio_usb_rx = &rx;
    port.pio_usb_tx = &tx;
    response_sync = USB_SYNC;
    response_pid = 0;
    response_len = -1;
    sent_tokens = sent_packets = rx_stops = 0;
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 3 + 1);
    endpoint_t *ep = pio_usb_ep_pool;
    ep->is_tx = out;
    ep->size = 8;
    ep->data_id = setup ? USB_PID_SETUP : 0;
    ep->failed_count = 99;
    ep->actual_len = 9;
    ep->transfer_aborted = ep->transfer_started = true;
    assert(pio_usb_ll_transfer_start(ep, buffer, length));
    assert(ep->has_transfer && !ep->transfer_aborted && !ep->transfer_started);
    assert(ep->actual_len == 0 && ep->failed_count == 0);
    return ep;
}

typedef int (*transaction_fn)(pio_port_t *, endpoint_t *);

static void test_retry_exhaustion(void) {
    transaction_fn functions[] = {usb_in_transaction, usb_out_transaction, usb_setup_transaction};
    uint8_t buffer[32] = {7, 8, 9};
    for (unsigned kind = 0; kind < 3; kind++) {
        endpoint_t *ep = begin(kind != 0, kind == 2, buffer, kind == 2 ? 8 : 16);
        uint8_t original_packet[68];
        memcpy(original_packet, ep->buffer, sizeof(original_packet));
        for (unsigned attempt = 1; attempt <= 3; attempt++) {
            assert(functions[kind](&port, ep) == -1);
            assert(ep->failed_count == attempt && ep->actual_len == 0);
            assert(ep->app_buf == buffer);
            assert(ep->has_transfer == (attempt < 3));
            assert(pio_usb_root_port[0].ep_error == (attempt == 3 ? 1u : 0u));
            assert(pio_usb_root_port[0].ep_complete == 0);
            if (kind == 2) assert(ep->data_id == USB_PID_SETUP);
            if (kind != 0) {
                assert(sent_packets == attempt && last_packet_len == 12);
                assert(memcmp(last_packet, original_packet, 12) == 0);
            }
            assert(sent_tokens == attempt);
        }
        assert(last_token == (kind == 0 ? USB_PID_IN : kind == 1 ? USB_PID_OUT : USB_PID_SETUP));
        assert(rx_stops == (kind == 2 ? 0u : 3u));
        assert(pio_usb_ll_transfer_start(ep, buffer, 8));
        assert(ep->failed_count == 0);
    }
    endpoint_t *ep = begin(false, false, buffer, sizeof(buffer));
    rx.irq = 0; // no completed receive, distinct timeout return value
    assert(usb_in_transaction(&port, ep) == -2);
    assert(ep->has_transfer && ep->failed_count == 1);
    ep->failed_count = 2;
    assert(!pio_usb_ll_transfer_start(ep, buffer + 1, 1));
    assert(ep->failed_count == 2 && ep->app_buf == buffer && ep->total_len == sizeof(buffer));
}

static void test_sound_response_resets_run(void) {
    uint8_t buffer[32] = {0};
    transaction_fn functions[] = {usb_in_transaction, usb_out_transaction};
    for (unsigned kind = 0; kind < 2; kind++) {
        endpoint_t *ep = begin(kind != 0, false, buffer, 16);
        assert(functions[kind](&port, ep) == -1);
        assert(functions[kind](&port, ep) == -1);
        response_pid = USB_PID_NAK;
        assert(functions[kind](&port, ep) == 0);
        assert(ep->has_transfer && ep->failed_count == 0 && ep->actual_len == 0);
        response_pid = 0;
        assert(functions[kind](&port, ep) == -1);
        assert(functions[kind](&port, ep) == -1);
        response_pid = kind == 0 ? USB_PID_DATA0 : USB_PID_ACK;
        response_len = 8;
        assert(functions[kind](&port, ep) == 0);
        assert(ep->has_transfer && ep->failed_count == 0 && ep->actual_len == 8);
        assert(ep->data_id == 1 && ep->app_buf == buffer + 8);
        if (kind == 0) assert(memcmp(buffer, payload, 8) == 0);
        else assert(ep->buffer[1] == USB_PID_DATA1);
        response_pid = kind == 0 ? USB_PID_DATA1 : USB_PID_ACK;
        assert(functions[kind](&port, ep) == 0);
        assert(!ep->has_transfer && ep->actual_len == 16 && ep->failed_count == 0);
        assert(pio_usb_root_port[0].ep_complete == 1 && pio_usb_root_port[0].ep_error == 0);
        assert(ep->new_data_flag == (kind == 0));
    }
    endpoint_t *ep = begin(false, false, buffer, sizeof(buffer));
    ep->failed_count = 2;
    response_pid = USB_PID_DATA1; // duplicate/wrong toggle: ACK, but do not copy it twice
    response_len = 8;
    assert(usb_in_transaction(&port, ep) == 0);
    assert(ep->has_transfer && ep->failed_count == 0 && ep->actual_len == 0 && ep->data_id == 0);
    response_pid = USB_PID_DATA0;
    response_len = 3; // short packet ends the transfer
    assert(usb_in_transaction(&port, ep) == 0);
    assert(!ep->has_transfer && ep->actual_len == 3);
}

static void test_setup_recovery_and_stall(void) {
    uint8_t buffer[16] = {1, 2, 3};
    endpoint_t *ep = begin(true, true, buffer, 8);
    assert(usb_setup_transaction(&port, ep) == -1);
    assert(usb_setup_transaction(&port, ep) == -1);
    response_pid = USB_PID_ACK;
    assert(usb_setup_transaction(&port, ep) == 0);
    assert(!ep->has_transfer && ep->actual_len == 8 && ep->failed_count == 0 && ep->data_id == 0);
    assert(pio_usb_root_port[0].ep_complete == 1 && pio_usb_root_port[0].ep_error == 0);
    assert(sent_tokens == 3 && last_token == USB_PID_SETUP && last_packet[1] == USB_PID_DATA0);
    ep = begin(true, true, buffer, 8);
    response_pid = USB_PID_ACK;
    response_sync = 0; // an ACK with invalid sync is still a failed SETUP
    assert(usb_setup_transaction(&port, ep) == -1);
    assert(ep->has_transfer && ep->failed_count == 1 && ep->actual_len == 0 && ep->data_id == USB_PID_SETUP);
    transaction_fn functions[] = {usb_in_transaction, usb_out_transaction};
    for (unsigned kind = 0; kind < 2; kind++) {
        ep = begin(kind != 0, false, buffer, 8);
        ep->failed_count = 1;
        response_pid = USB_PID_STALL;
        assert(functions[kind](&port, ep) == 0);
        assert(!ep->has_transfer && ep->failed_count == 0 && ep->actual_len == 0);
        assert(pio_usb_root_port[0].ep_stalled == 1 && pio_usb_root_port[0].ep_error == 0);
    }
}

static void test_endpoint_isolation(void) {
    uint8_t buffer[16] = {0};
    endpoint_t *first = begin(false, false, buffer, 8);
    endpoint_t *second = first + 1;
    second->size = 8;
    assert(pio_usb_ll_transfer_start(second, buffer + 8, 8));
    for (unsigned i = 0; i < 2; i++) assert(usb_in_transaction(&port, first) == -1);
    assert(usb_in_transaction(&port, second) == -1);
    assert(first->failed_count == 2 && second->failed_count == 1);
    assert(usb_in_transaction(&port, first) == -1);
    assert(!first->has_transfer && second->has_transfer && second->failed_count == 1);
    assert(pio_usb_root_port[0].ep_error == 1); // only the failed endpoint's bit
}

/* RP2040 register seam; endpoint layout and both buffer helpers are real. */
typedef volatile uint32_t io_rw_32;
#define CFG_TUH_ENABLED 0
#define CFG_TUD_RP2040_RX_WIPE 1
#define USB_BUF_CTRL_FULL 0x8000u
#define USB_BUF_CTRL_LAST 0x4000u
#define USB_BUF_CTRL_DATA1_PID 0x2000u
#define USB_BUF_CTRL_DATA0_PID 0u
#define USB_BUF_CTRL_AVAIL 0x400u
#define USB_BUF_CTRL_LEN_MASK 0x3ffu
#define tu_min16(a, b) ((a) < (b) ? (a) : (b))
#define pico_trace(...) ((void)0)
#define _hw_endpoint_buffer_control_get_value32(ep) (*(ep)->buffer_control)
#include "dpram_production.h"

#define USB_MAX_ENDPOINTS 16
#define tu_memclr(buffer, length) memset(buffer, 0, length)
typedef struct {
    struct { uint32_t in, out; } ep_ctrl[USB_MAX_ENDPOINTS - 1];
    uint8_t epx_data[3840];
} fake_dpram_t;
static fake_dpram_t fake_usb_dpram;
static fake_dpram_t *usb_dpram = &fake_usb_dpram;
static hw_endpoint_t hw_endpoints[USB_MAX_ENDPOINTS][2];
static uint8_t *next_buffer_ptr;
#include "dpram_reset_production.h"

static void test_reset_erases_abandoned_rx(void) {
    memset(&fake_usb_dpram, 0xa5, sizeof(fake_usb_dpram));
    memset(hw_endpoints, 0xa5, sizeof(hw_endpoints));
    next_buffer_ptr = fake_usb_dpram.epx_data + 128;
    reset_non_control_endpoints();
    for (unsigned i = 0; i < USB_MAX_ENDPOINTS - 1; ++i)
        assert(fake_usb_dpram.ep_ctrl[i].in == 0 && fake_usb_dpram.ep_ctrl[i].out == 0);
    for (unsigned i = 0; i < sizeof(fake_usb_dpram.epx_data); ++i)
        assert(fake_usb_dpram.epx_data[i] == 0);
    for (unsigned i = 0; i < sizeof(hw_endpoints); ++i)
        assert(((uint8_t *)hw_endpoints)[i] == (i < 2 * sizeof(hw_endpoint_t) ? 0xa5 : 0));
    assert(next_buffer_ptr == fake_usb_dpram.epx_data);
}

static void test_copy_alignment(void) {
    uint8_t source[160], destination[160];
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 13);
    for (unsigned src_offset = 0; src_offset < 8; src_offset++) {
        for (unsigned dst_offset = 0; dst_offset < 8; dst_offset++) {
            for (unsigned length = 0; length <= 129; length++) {
                memset(destination, 0xa5, sizeof(destination));
                unaligned_memcpy(destination + dst_offset, source + src_offset, length);
                assert(memcmp(destination + dst_offset, source + src_offset, length) == 0);
                for (unsigned i = 0; i < dst_offset; i++) assert(destination[i] == 0xa5);
                for (unsigned i = dst_offset + length; i < sizeof(destination); i++)
                    assert(destination[i] == 0xa5);
            }
        }
    }
}

static void test_endpoint_buffers(void) {
    uint8_t user[140], dpram[140];
    uint32_t control;
    for (unsigned offset = 0; offset < 4; offset++) {
        for (uint8_t buffer_id = 0; buffer_id < 2; buffer_id++) {
            for (uint16_t length = 0; length <= 64; length++) {
                for (unsigned i = 0; i < sizeof(user); i++) user[i] = (uint8_t)i;
                memset(dpram, 0xa5, sizeof(dpram));
                hw_endpoint_t ep = {.user_buf = user + offset, .hw_data_buf = dpram + offset,
                                    .remaining_len = length, .wMaxPacketSize = 64};
                control = prepare_ep_buffer(&ep, buffer_id) >> (buffer_id * 16);
                assert((control & USB_BUF_CTRL_LEN_MASK) == length);
                assert(control & USB_BUF_CTRL_FULL);
                assert(control & USB_BUF_CTRL_LAST);
                assert(control & USB_BUF_CTRL_AVAIL);
                assert(ep.user_buf == user + offset + length && ep.remaining_len == 0 && ep.next_pid == 1);
                assert(memcmp(dpram + offset + buffer_id * 64, user + offset, length) == 0);
                assert(dpram[offset + buffer_id * 64 + length] == 0xa5);
                memset(user, 0, sizeof(user));
                ep.rx = true;
                ep.user_buf = user + offset;
                ep.remaining_len = 100;
                control = (length | USB_BUF_CTRL_FULL) << (buffer_id * 16);
                ep.buffer_control = &control;
                assert(sync_ep_buffer(&ep, buffer_id) == length);
                assert(ep.xferred_len == length && ep.user_buf == user + offset + length);
                assert(ep.remaining_len == (length < 64 ? 0 : 100));
                for (uint16_t i = 0; i < length; ++i) {
                    assert(user[offset + i] == (uint8_t)(offset + i));
                    assert(dpram[offset + buffer_id * 64 + i] == 0);
                }
                /* Clear only the consumed bank/range, preserving its neighbor. */
                assert(dpram[offset + buffer_id * 64 + length] == 0xa5);
                if (buffer_id) assert(dpram[offset] == 0xa5);
                assert(user[offset + length] == 0);
            }
        }
    }
}

int main(void) {
    test_retry_exhaustion();
    test_sound_response_resets_run();
    test_setup_recovery_and_stall();
    test_endpoint_isolation();
    test_copy_alignment();
    test_endpoint_buffers();
    test_reset_erases_abandoned_rx();
    puts("upstream USB: transaction retry, reset, isolation, STALL/NAK, SETUP, and DPRAM tests passed");
    return 0;
}
