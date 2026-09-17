/* Exercise actual CDC/FIFO bodies with deterministic endpoint ownership. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "class/cdc/cdc_device.c"

bool tud_mounted(void) { return true; }
bool tud_suspended(void) { return false; }
void tud_cdc_tx_complete_cb(uint8_t itf) { (void)itf; }

static bool endpoint_busy;
static unsigned wanted_calls, rx_calls, rearms;
static bool callback_reads;
static uint8_t callback_byte;

static bool all_zero(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) if (data[i]) return false;
    return true;
}

bool usbd_edpt_busy(uint8_t rhport, uint8_t address) {
    (void)rhport; (void)address;
    return endpoint_busy;
}
bool usbd_edpt_claim(uint8_t rhport, uint8_t address) {
    (void)rhport; (void)address;
    return !endpoint_busy;
}
bool usbd_edpt_release(uint8_t rhport, uint8_t address) {
    (void)rhport; (void)address;
    return true;
}
bool usbd_edpt_xfer(uint8_t rhport, uint8_t address, uint8_t *buffer, uint16_t length) {
    (void)rhport;
    assert(address == 5 && buffer == _cdcd_itf[0].epout_buf && length == 64);
    assert(all_zero(buffer, length));
    endpoint_busy = true;
    rearms++;
    return true;
}
void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted) {
    assert(itf == 0 && wanted == '!');
    assert(all_zero(_cdcd_itf[0].epout_buf, sizeof(_cdcd_itf[0].epout_buf)));
    wanted_calls++;
    if (callback_reads) assert(tud_cdc_n_read(itf, &callback_byte, 1) == 1);
}
void tud_cdc_rx_cb(uint8_t itf) {
    assert(itf == 0);
    assert(all_zero(_cdcd_itf[0].epout_buf, sizeof(_cdcd_itf[0].epout_buf)));
    rx_calls++;
}

static cdcd_interface_t *reset_fixture(void) {
    cdcd_init();
    cdcd_interface_t *cdc = &_cdcd_itf[0];
    cdc->ep_out = 5; cdc->ep_in = 0x85;
    endpoint_busy = true; /* initial receive request owns epout_buf */
    wanted_calls = rx_calls = rearms = 0;
    callback_reads = false;
    return cdc;
}

static void test_each_read_length(void) {
    for (unsigned amount = 0; amount <= 70; ++amount) {
        cdcd_interface_t *cdc = reset_fixture();
        uint8_t fixture[64], result[72];
        for (unsigned i = 0; i < sizeof(fixture); ++i) fixture[i] = (uint8_t)(i + 1);
        memset(result, 0xa5, sizeof(result));
        assert(tu_fifo_write_n(&cdc->rx_ff, fixture, 64) == 64);
        unsigned count = amount < 64 ? amount : 64;
        assert(tud_cdc_n_read(0, result, amount) == count);
        assert(memcmp(result, fixture, count) == 0);
        assert(result[count] == 0xa5);
        assert(all_zero(cdc->rx_ff_buf, count));
        assert(memcmp(cdc->rx_ff_buf + count, fixture + count, 64 - count) == 0);
        assert(tud_cdc_n_available(0) == 64 - count);
    }
}

static void test_fifo_wrap_preserves_pending_bytes(void) {
    cdcd_interface_t *cdc = reset_fixture();
    uint8_t fixture[64], output[64];
    for (unsigned i = 0; i < 64; ++i) fixture[i] = (uint8_t)(i + 1);
    assert(tu_fifo_write_n(&cdc->rx_ff, fixture, 64) == 64);
    assert(tud_cdc_n_read(0, output, 17) == 17);
    assert(tu_fifo_write_n(&cdc->rx_ff, fixture, 17) == 17);
    assert(tud_cdc_n_read(0, output, 64) == 64);
    assert(memcmp(output, fixture + 17, 47) == 0);
    assert(memcmp(output + 47, fixture, 17) == 0);
    assert(all_zero(cdc->rx_ff_buf, sizeof(cdc->rx_ff_buf)));
    assert(tud_cdc_n_available(0) == 0);
}

static void test_callback_storage_erased_before_rearm(void) {
    cdcd_interface_t *cdc = reset_fixture();
    cdc->wanted_char = '!';
    callback_reads = true;
    memcpy(cdc->epout_buf, "!a!", 3);
    endpoint_busy = false; /* completion is software-owned now */
    assert(cdcd_xfer_cb(0, 5, XFER_RESULT_SUCCESS, 3));
    assert(wanted_calls == 2 && rx_calls == 1 && callback_byte == 'a');
    assert(rearms == 0 && !endpoint_busy);
    assert(all_zero(cdc->epout_buf, sizeof(cdc->epout_buf)));
    uint8_t remaining;
    assert(tud_cdc_n_read(0, &remaining, 1) == 1 && remaining == '!');
    assert(rearms == 1 && endpoint_busy);
    assert(all_zero(cdc->rx_ff_buf, sizeof(cdc->rx_ff_buf)));
}

static void test_flush_respects_active_dcd_ownership(void) {
    cdcd_interface_t *cdc = reset_fixture();
    memset(cdc->rx_ff_buf, 0xa5, sizeof(cdc->rx_ff_buf));
    memset(cdc->epout_buf, 0x5a, sizeof(cdc->epout_buf));
    tud_cdc_n_read_flush(0);
    assert(all_zero(cdc->rx_ff_buf, sizeof(cdc->rx_ff_buf)));
    for (unsigned i = 0; i < sizeof(cdc->epout_buf); ++i) assert(cdc->epout_buf[i] == 0x5a);
    endpoint_busy = false;
    assert(cdcd_xfer_cb(0, 5, XFER_RESULT_SUCCESS, 64));
    assert(all_zero(cdc->epout_buf, sizeof(cdc->epout_buf)));
    tud_cdc_n_read_flush(0);
    assert(all_zero(cdc->rx_ff_buf, sizeof(cdc->rx_ff_buf)));
    memset(cdc->epout_buf, 0xa5, sizeof(cdc->epout_buf));
    endpoint_busy = false;
    tud_cdc_n_read_flush(0);
    assert(all_zero(cdc->epout_buf, sizeof(cdc->epout_buf)));
}

static void test_reset_clears_retained_storage(void) {
    cdcd_interface_t *cdc = reset_fixture();
    memset(cdc->rx_ff_buf, 0xa5, sizeof(cdc->rx_ff_buf));
    memset(cdc->epout_buf, 0xa5, sizeof(cdc->epout_buf));
    cdcd_reset(0); /* bus-reset path stopped DCD first */
    assert(all_zero(cdc->rx_ff_buf, sizeof(cdc->rx_ff_buf)));
    assert(all_zero(cdc->epout_buf, sizeof(cdc->epout_buf)));
    assert(!cdc->ep_out && !tud_cdc_n_available(0));
    memset(cdc->rx_ff_buf, 0xa5, sizeof(cdc->rx_ff_buf));
    memset(cdc->epout_buf, 0xa5, sizeof(cdc->epout_buf));
    assert(cdcd_deinit());
    assert(all_zero(cdc->rx_ff_buf, sizeof(cdc->rx_ff_buf)));
    assert(all_zero(cdc->epout_buf, sizeof(cdc->epout_buf)));
}

int main(void) {
    test_each_read_length();
    test_fifo_wrap_preserves_pending_bytes();
    test_callback_storage_erased_before_rearm();
    test_flush_respects_active_dcd_ownership();
    test_reset_clears_retained_storage();
    puts("CDC privacy: bounded reads, wrap, ownership, callbacks, flush and reset passed");
}
