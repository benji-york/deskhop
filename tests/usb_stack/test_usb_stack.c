/* A virtual USB device controller driving the real checked-in TinyUSB stack. */
#include "main.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "peer_status.h"
#include "diagnostic_history.h"
#include "diagnostic_peer_history.h"
#include "diagnostic_runtime.h"
#include "diagnostic_verify.h"
#include "maintenance.h"
#include "config_confirm.h"
#include <stdio.h>
#ifdef DH_DEBUG
#error The CDC regression must exercise a production console without DH_DEBUG
#endif
_Static_assert(CFG_TUD_CDC == 1, "production console must enumerate CDC");
static const char *scenario = "initialization";
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "USB stack failure [%s] %s:%d: %s\n", scenario, __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct { bool pending, stalled, opened; uint8_t *buffer; uint16_t length, max_packet; } endpoint_t;
static endpoint_t endpoints[2][16];
static uint8_t address;
static unsigned led_restores, led_peer_messages, remote_wakeups, suspend_callbacks, resume_callbacks;
static uint8_t last_led;
static unsigned msc_reads;
static uint32_t last_msc_lba;
static uint8_t host_bytes[2048];
static size_t host_count;
static char cdc_bytes[32768];
static size_t cdc_count;
static uint64_t cdc_submitted_bytes, console_now_us;
/* Console/USB contract double. Real maintenance admission, UART transactions,
 * updater guards and ROM entry are covered by the paired production simulator. */
static maintenance_start_t maintenance_start = MAINTENANCE_STARTED;
static bool maintenance_auto = true, maintenance_pending, maintenance_ready, maintenance_config;
static uint8_t maintenance_role, maintenance_target;
static uint32_t maintenance_token;
/* The real configuration engine is covered by the paired firmware simulator;
 * this controller harness tests USB endpoint admission and console behavior. */
bool config_confirm_usb_request(const uart_packet_t *packet, device_t *state) {
    (void)packet;
    (void)state;
    return false;
}
void config_confirm_usb_disconnect(void) {}
static unsigned maintenance_requests, maintenance_config_requests, maintenance_completions, maintenance_cancels, maintenance_polls;
static maintenance_result_t maintenance_result;
static void maintenance_publish(uint32_t token, uint8_t target, maintenance_outcome_t outcome) {
    CHECK(!maintenance_ready);
    maintenance_result = (maintenance_result_t){.token = token, .target = target, .outcome = outcome};
    maintenance_ready = true;
}
static maintenance_start_t maintenance_request_common(uint8_t target, uint32_t token) {
    ++maintenance_requests;
    CHECK(target < 2 && token != 0);
    if (maintenance_start != MAINTENANCE_STARTED)
        return maintenance_start;
    CHECK(!maintenance_pending);
    maintenance_pending = true;
    maintenance_target = target;
    maintenance_token = token;
    if (maintenance_auto)
        maintenance_publish(token, target, target == maintenance_role
                            ? MAINTENANCE_LOCAL_READY : MAINTENANCE_REMOTE_ACCEPTED);
    return MAINTENANCE_STARTED;
}
maintenance_start_t maintenance_request(uint8_t target, uint32_t token, uint64_t now_us) {
    maintenance_config = false;
    return maintenance_request_common(target, token);
}
maintenance_start_t maintenance_request_config(uint32_t token, uint64_t now_us) {
    ++maintenance_config_requests;
    maintenance_config = true;
    return maintenance_request_common(maintenance_role, token);
}
bool maintenance_poll(maintenance_result_t *result) {
    ++maintenance_polls;
    if (!maintenance_ready)
        return false;
    *result = maintenance_result;
    maintenance_ready = false;
    if (result->token == maintenance_token && result->target == maintenance_target
        && result->outcome != MAINTENANCE_LOCAL_READY)
        maintenance_pending = false;
    return true;
}
void maintenance_console_reply_complete(uint32_t token, uint64_t now_us) {
    CHECK(maintenance_pending && maintenance_target == maintenance_role && token == maintenance_token);
    CHECK(strstr(cdc_bytes, maintenance_config ? "END config\r\n" : "END bootloader\r\n") != NULL);
    ++maintenance_completions;
    maintenance_pending = false;
}
void maintenance_cancel(uint32_t token, uint64_t now_us) {
    ++maintenance_cancels;
    CHECK(token == maintenance_token);
    maintenance_pending = false;
    maintenance_ready = false;
}
static bool no_ep0_out_payload;
static const diagnostic_runtime_snapshot_t default_runtime = {
    .core_ticks = {123, 456}, .core_age_ms = {0, 1}, .core_valid = 3,
    .phase = DIAGNOSTIC_UPDATE_IDLE, .source = DIAGNOSTIC_SOURCE_NONE,
    .total_bytes = 262144, .progress_age_ms = UINT32_MAX,
};
static diagnostic_runtime_snapshot_t local_runtime;
static unsigned runtime_reads;
diagnostic_runtime_snapshot_t diagnostic_runtime_snapshot(void) {
    ++runtime_reads;
    return local_runtime;
}
/* The production assessment policy consumes fixed scalar bridge fixtures. */
static verify_result_t verify_queue[4], verify_fixture[2];
static unsigned verify_queued, verify_head, verify_polls, verify_requests, verify_rechecks;
static bool verify_accept = true, verify_auto = true;
static uint32_t verify_token;
static uint64_t verify_requested_at;
static verify_scan_outcome_t verify_recheck_outcome = VERIFY_SCAN_COMPLETE;
static bool verify_recheck_busy;
static unsigned verify_busy_remaining;
static void verify_publish(unsigned index, uint32_t token) {
    CHECK(verify_queued < 4);
    verify_result_t result = verify_fixture[index];
    result.token = token;
    result.remote = index != 0;
    verify_queue[(verify_head + verify_queued++) % 4] = result;
}
bool diagnostic_verify_request(uint32_t token, uint64_t requested_at_us) {
    ++verify_requests;
    verify_token = token;
    verify_requested_at = requested_at_us;
    if (!verify_accept)
        return false;
    if (verify_auto) {
        verify_publish(0, token);
        verify_publish(1, token);
    }
    return true;
}
bool diagnostic_verify_poll(verify_result_t *result) {
    ++verify_polls;
    if (!verify_queued)
        return false;
    *result = verify_queue[verify_head];
    verify_head = (verify_head + 1) % 4;
    --verify_queued;
    return true;
}
bool diagnostic_verify_recheck_local(verify_result_t *result) {
    CHECK(!result->remote);
    ++verify_rechecks;
    if (result->transport != VERIFY_TRANSPORT_OK || result->snapshot.outcome != VERIFY_SCAN_COMPLETE)
        return true;
    if (verify_recheck_busy || verify_busy_remaining) {
        if (verify_busy_remaining) --verify_busy_remaining;
        return false;
    }
    if (verify_recheck_outcome != VERIFY_SCAN_COMPLETE)
        result->snapshot.outcome = verify_recheck_outcome;
    return true;
}
static void verify_fixtures(void) {
    CHECK(!verify_queued);
    verify_auto = verify_accept = true;
    verify_recheck_outcome = VERIFY_SCAN_COMPLETE;
    verify_recheck_busy = false;
    verify_busy_remaining = 0;
    for (unsigned i = 0; i < 2; ++i) {
        verify_fixture[i] = (verify_result_t){.remote = i != 0, .transport = VERIFY_TRANSPORT_OK,
            .snapshot = {.role = i, .major = 0, .minor = 101,
                .board_id = {1,2,3,4,5,6,7,8}, .boot_session = UINT64_C(0x1122334455667788) + i,
                .image_crc_at_boot = UINT32_C(0x89abcdef), .started_us = 1000000, .completed_us = 2000000,
                .generation_start = 7, .generation_end = 7, .bytes_read = VERIFY_IMAGE_BYTES,
                .slot_crc32 = UINT32_C(0x12345678), .metadata_magic = 0xf00d, .metadata_version = 201,
                .metadata_crc32 = UINT32_C(0x89abcdef), .outcome = VERIFY_SCAN_COMPLETE,
                .start = {.core_valid = 3, .core_ticks = {100, 200}},
                .end = {.core_valid = 3, .core_ticks = {1100, 1200}, .core_age_ms = {0, 1}},
            }};
    }
}
/* Exercise the production ring; only the Pico lock/time adapter is replaced. */
static history_store_t history_store;
static unsigned history_reads;
void diagnostic_history_init(void) { history_store_init(&history_store); }
void diagnostic_history_record(history_type_t type, uint8_t a, uint8_t b, uint32_t value) {
    history_store_record(&history_store, console_now_us, type, a, b, value);
}
history_window_t diagnostic_history_window_at(unsigned limit, uint64_t *sampled_at_us) {
    *sampled_at_us = console_now_us;
    return history_store_window(&history_store, limit);
}
history_window_t diagnostic_history_window(unsigned limit) {
    return history_store_window(&history_store, limit);
}
bool diagnostic_history_read(uint64_t seq, history_event_t *event) {
    ++history_reads;
    return history_store_read(&history_store, seq, event);
}
/* A fixed borrowed result models the production bridge's ownership boundary. */
static bool history_peer_manual, history_peer_accept = true;
static bool history_peer_pending, history_peer_ready, history_peer_borrowed;
static unsigned history_peer_polls, history_peer_releases, history_peer_requests, history_peer_limit;
static uint32_t history_peer_token;
static uint64_t history_peer_requested_at;
static peer_history_result_t history_peer_result;
bool diagnostic_peer_history_request(uint32_t token, unsigned count, uint64_t requested_at_us) {
    ++history_peer_requests;
    CHECK(!history_peer_borrowed && !history_peer_ready);
    if (!history_peer_accept || history_peer_pending)
        return false;
    CHECK(token != 0 && count > 0 && count <= 64);
    history_peer_token = token;
    history_peer_limit = count;
    history_peer_requested_at = requested_at_us;
    history_peer_pending = true;
    return true;
}
static void history_peer_publish(uint32_t token, peer_history_outcome_t outcome) {
    CHECK(!history_peer_ready && !history_peer_borrowed);
    history_peer_result.token = token;
    history_peer_result.outcome = outcome;
    history_peer_result.requested_at_us = history_peer_requested_at;
    history_peer_result.first_response_us = history_peer_requested_at + 5000;
    history_peer_ready = true;
}
const peer_history_result_t *diagnostic_peer_history_poll(void) {
    CHECK(!history_peer_borrowed);
    ++history_peer_polls;
    if (!history_peer_manual && history_peer_pending && !history_peer_ready)
        history_peer_publish(history_peer_token, PEER_HISTORY_TIMEOUT);
    if (!history_peer_ready)
        return NULL;
    history_peer_ready = false;
    history_peer_borrowed = true;
    return &history_peer_result;
}
void diagnostic_peer_history_release(void) {
    CHECK(history_peer_borrowed);
    history_peer_borrowed = false;
    if (history_peer_result.token == history_peer_token)
        history_peer_pending = false;
    ++history_peer_releases;
}
/* The real CDC/console stack crosses the same nonblocking request/result
 * boundary as core 0. UART and core 1 are tested by the paired simulator. */
static bool peer_manual, peer_accept = true, peer_pending, peer_result_ready;
static uint32_t peer_token;
static uint64_t peer_requested_at;
static unsigned peer_requests;
static peer_status_result_t peer_result;
static peer_status_snapshot_t peer_snapshot = {
    .protocol = 2,
    .runtime = {.core_ticks = {900, 901}, .core_age_ms = {2, 3}, .core_valid = 3,
                .phase = DIAGNOSTIC_UPDATE_RECEIVING, .source = DIAGNOSTIC_SOURCE_PEER,
                .received_bytes = 65536, .total_bytes = 262144, .progress_age_ms = 4,
                .target_version = 200, .update_attempt = 1, .update_seen = true},
    .role = 1, .major = 0, .minor = 96,
    .board_id = {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10},
    .boot_session = UINT64_C(0x8877665544332211),
    .uptime_ms = 7654321, .image_crc_at_boot = UINT32_C(0x12345678),
};
static diagnostic_peer_observation_t peer_observation = {
    .boot = DIAGNOSTIC_PEER_FIRST_SEEN, .progress = DIAGNOSTIC_PROGRESS_BASELINE,
    .update = DIAGNOSTIC_EXECUTION_NOT_OBSERVED,
};
bool diagnostic_peer_request(uint32_t token, uint64_t requested_at_us) {
    ++peer_requests;
    if (!peer_accept || peer_pending) return false;
    CHECK(token != 0);
    peer_token = token; peer_requested_at = requested_at_us; peer_pending = true;
    return true;
}
static void peer_publish(uint32_t token, peer_status_outcome_t outcome) {
    CHECK(!peer_result_ready);
    peer_result = (peer_status_result_t){.token = token, .outcome = outcome,
                                       .snapshot = peer_snapshot, .observation = peer_observation};
    peer_result_ready = true;
}
bool diagnostic_peer_poll(peer_status_result_t *result) {
    if (!peer_manual && peer_pending && !peer_result_ready)
        peer_publish(peer_token, PEER_STATUS_OK);
    if (!peer_result_ready) return false;
    *result = peer_result;
    peer_result_ready = false;
    if (result->token == peer_token) peer_pending = false;
    return true;
}
device_t global_state;
/* This USB fixture is single-threaded. Production snapshot locking and copy
 * publication interleavings are exercised by the storage-boundary suite. */
void config_snapshot(const device_t *state, config_t *snapshot) {
    *snapshot = state->config;
}
static endpoint_t *endpoint(uint8_t ep) { return &endpoints[(ep >> 7) & 1][ep & 15]; }
void dcd_init(uint8_t rhport) { CHECK(rhport == 0); }
bool dcd_deinit(uint8_t rhport) { return true; }
void dcd_int_handler(uint8_t rhport) {}
void dcd_int_enable(uint8_t rhport) {}
void dcd_int_disable(uint8_t rhport) {}
void dcd_connect(uint8_t rhport) {}
void dcd_disconnect(uint8_t rhport) {}
void dcd_sof_enable(uint8_t rhport, bool enabled) {}
void dcd_remote_wakeup(uint8_t rhport) { ++remote_wakeups; }
void dcd_set_address(uint8_t rhport, uint8_t dev_addr) { address = dev_addr; CHECK(dcd_edpt_xfer(rhport, 0x80, NULL, 0)); }
bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *descriptor) {
    endpoint_t *ep = endpoint(descriptor->bEndpointAddress);
    CHECK(!ep->opened);
    ep->opened = true;
    ep->max_packet = tu_edpt_packet_size(descriptor);
    return true;
}
void dcd_edpt_close_all(uint8_t rhport) {
    for (unsigned direction = 0; direction < 2; ++direction)
        for (unsigned ep = 1; ep < 16; ++ep) memset(&endpoints[direction][ep], 0, sizeof(endpoint_t));
}
void dcd_edpt_close(uint8_t rhport, uint8_t ep_addr) { memset(endpoint(ep_addr), 0, sizeof(endpoint_t)); }
bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t length) {
    /* Deconfiguration resets CDC endpoint addresses to zero. A subsequent
     * console FIFO flush must not accidentally arm CDC's RX buffer as EP0. */
    CHECK(!(no_ep0_out_payload && ep_addr == 0 && length != 0));
    endpoint_t *ep = endpoint(ep_addr);
    CHECK(!ep->pending);
    CHECK(!ep->stalled);
    ep->pending = true; ep->buffer = buffer; ep->length = length;
    if (ep_addr == 0x86) cdc_submitted_bytes += length;
    return true;
}
void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) { endpoint_t *ep = endpoint(ep_addr); ep->stalled = true; ep->pending = false; }
void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) { endpoint(ep_addr)->stalled = false; }
void pico_get_unique_board_id_string(char *buffer, unsigned length) { snprintf(buffer, length, "0123456789abcdef"); }
void restore_leds(device_t *state) { ++led_restores; }
void send_value(uint8_t value, enum packet_type_e type) { CHECK(type == KBD_SET_REPORT_MSG); last_led = value; ++led_peer_messages; }
bool validate_packet(uart_packet_t *packet) { return false; }
uint32_t calc_packet_checksum(const uart_packet_t *packet) { return 0; }
void process_packet(uart_packet_t *packet, device_t *state) { CHECK(false); }
/* This stack-only fixture rejects config packets above. Bootloader dispatch and
 * updater-guard behavior are exercised by the paired production-code simulator. */
void firmware_update_lock(void) { CHECK(false); }
void firmware_update_unlock(void) { CHECK(false); }
void tud_suspend_cb(bool remote_wakeup_en) { ++suspend_callbacks; }
void tud_resume_cb(void) { ++resume_callbacks; }

/* MSC backing-store callbacks are deliberately outside this stack prototype;
 * actual flash/UF2 callbacks are covered independently by tests/storage. */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4]) {
    memcpy(vendor, "DeskHop ", 8); memcpy(product, "Virtual disk    ", 16); memcpy(revision, "1.0 ", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) { return true; }
void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) { *count = 4096; *size = 512; }
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t size) { ++msc_reads; last_msc_lba = lba; memset(buffer, 0x5a, size); return (int32_t)size; }
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) { return (int32_t)size; }
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *buffer, uint16_t size) { return -1; }

/* ELF permits absent weak callbacks; Mach-O requires concrete definitions.
 * These provide the stack's absent-callback defaults for unimplemented hooks. */
const uint8_t *tud_descriptor_bos_cb(void) { return NULL; }
const uint8_t *tud_descriptor_device_qualifier_cb(void) { return NULL; }
const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index) { return NULL; }
const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count) { *count = 0; return NULL; }
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request) { return false; }
void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t length) {}
void tud_hid_report_fail_cb(uint8_t instance, uint8_t endpoint_address, uint16_t length) {}
void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {}
bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle) { return true; }
uint8_t tud_msc_get_maxlun_cb(void) { return 1; }
bool tud_msc_is_writable_cb(uint8_t lun) { return true; }
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit, uint8_t control) { return true; }
int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t size) { return size; }
void tud_msc_read10_complete_cb(uint8_t lun) {}
void tud_msc_write10_complete_cb(uint8_t lun) {}
void tud_msc_scsi_complete_cb(uint8_t lun, const uint8_t command[16]) {}
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t condition, bool start, bool eject) { return true; }
void tud_cdc_rx_cb(uint8_t itf) {}
void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted) {}
void tud_cdc_line_coding_cb(uint8_t itf, const cdc_line_coding_t *coding) {}
void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration) {}

static void pump(void) { tud_task_ext(0, false); }
static void complete(uint8_t ep_addr, const uint8_t *out, unsigned out_length) {
    endpoint_t *ep = endpoint(ep_addr);
    CHECK(ep->pending);
    unsigned length = ep->length;
    if (ep_addr == 0x86) {
        CHECK(cdc_count + length < sizeof(cdc_bytes));
        if (length) memcpy(cdc_bytes + cdc_count, ep->buffer, length);
        cdc_count += length;
        cdc_bytes[cdc_count] = '\0';
    } else if (ep_addr & 0x80) {
        CHECK(host_count + length <= sizeof(host_bytes));
        if (length) memcpy(host_bytes + host_count, ep->buffer, length);
        host_count += length;
    } else {
        if (length > out_length)
            fprintf(stderr, "OUT endpoint 0x%02x requested %u bytes, host supplied %u\n", ep_addr, length, out_length);
        CHECK(length <= out_length);
        if (length) memcpy(ep->buffer, out, length);
    }
    ep->pending = false;
    dcd_event_xfer_complete(0, ep_addr, length, XFER_RESULT_SUCCESS, true);
    pump();
}
/* A bulk endpoint is armed for its maximum packet size, but command fragments
 * normally arrive as short USB packets. Report the actual completed length. */
static void complete_short_out(uint8_t ep_addr, const void *bytes, unsigned length) {
    endpoint_t *ep = endpoint(ep_addr);
    CHECK(!(ep_addr & 0x80) && ep->pending && length <= ep->length);
    if (length) memcpy(ep->buffer, bytes, length);
    ep->pending = false;
    dcd_event_xfer_complete(0, ep_addr, length, XFER_RESULT_SUCCESS, true);
    pump();
}
static bool control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                    uint16_t length, const uint8_t *out) {
    tusb_control_request_t setup = {.bmRequestType = request_type, .bRequest = request,
                                   .wValue = value, .wIndex = index, .wLength = length};
    endpoint(0)->stalled = endpoint(0x80)->stalled = false;
    endpoint(0)->pending = endpoint(0x80)->pending = false;
    host_count = 0;
    dcd_event_setup_received(0, (uint8_t *)&setup, true);
    pump();
    unsigned out_offset = 0;
    for (unsigned guard = 0; guard < 100; ++guard) {
        if (endpoint(0)->stalled || endpoint(0x80)->stalled) return false;
        if (endpoint(0x80)->pending) complete(0x80, NULL, 0);
        else if (endpoint(0)->pending) {
            unsigned bytes = endpoint(0)->length;
            complete(0, out ? out + out_offset : NULL, length - out_offset);
            out_offset += bytes;
        } else return true;
    }
    CHECK(false); return false;
}
static uint16_t little16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void reset_bus(void) {
    memset(endpoints, 0, sizeof(endpoints)); address = 0;
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, true); pump();
    CHECK(!tud_mounted());
}
static void enumerate(bool config_mode) {
    global_state.config_mode_active = config_mode;
    reset_bus();
    CHECK(control(0x80, TUSB_REQ_GET_DESCRIPTOR, TUSB_DESC_DEVICE << 8, 0, 8, NULL));
    CHECK(host_count == 8 && host_bytes[0] == 18 && host_bytes[7] == 64);
    CHECK(control(0x80, TUSB_REQ_GET_DESCRIPTOR, TUSB_DESC_DEVICE << 8, 0, 18, NULL));
    CHECK(host_count == 18);
    CHECK(little16(host_bytes + 8) == (config_mode ? 0x2e8a : 0x1209));
    CHECK(little16(host_bytes + 10) == (config_mode ? 0x107c : 0xc000));
    CHECK(control(0, TUSB_REQ_SET_ADDRESS, 7, 0, 0, NULL)); CHECK(address == 7);
    CHECK(control(0x80, TUSB_REQ_GET_DESCRIPTOR, TUSB_DESC_CONFIGURATION << 8, 0, 9, NULL));
    unsigned total = little16(host_bytes + 2);
    CHECK(host_count == 9 && host_bytes[4] == (config_mode ? 6 : 4));
    CHECK(control(0x80, TUSB_REQ_GET_DESCRIPTOR, TUSB_DESC_CONFIGURATION << 8, 0, total, NULL));
    CHECK(host_count == total && host_bytes[1] == TUSB_DESC_CONFIGURATION);
    /* Independent descriptor walking validates interface/endpoint structure. */
    unsigned interfaces = 0, ep_count = 0, cdc_control = 0, cdc_data = 0;
    unsigned iad_count = 0, union_count = 0;
    uint8_t cdc_interface = config_mode ? 4 : 2;
    for (unsigned at = 0; at < total;) {
        CHECK(host_bytes[at] >= 2 && at + host_bytes[at] <= total);
        if (host_bytes[at + 1] == TUSB_DESC_INTERFACE) {
            ++interfaces;
            if (host_bytes[at + 5] == 2) {
                ++cdc_control;
                CHECK(host_bytes[at + 2] == cdc_interface && host_bytes[at + 6] == 2);
            } else if (host_bytes[at + 5] == 10) {
                ++cdc_data;
                CHECK(host_bytes[at + 2] == cdc_interface + 1);
            }
        }
        if (host_bytes[at + 1] == TUSB_DESC_ENDPOINT) ++ep_count;
        if (host_bytes[at + 1] == 0x0b) {
            ++iad_count;
            CHECK(host_bytes[at + 2] == cdc_interface && host_bytes[at + 3] == 2);
            CHECK(host_bytes[at + 4] == 2);
        }
        if (host_bytes[at + 1] == 0x24 && host_bytes[at + 2] == 6) {
            ++union_count;
            CHECK(host_bytes[at + 3] == cdc_interface && host_bytes[at + 4] == cdc_interface + 1);
        }
        at += host_bytes[at];
    }
    CHECK(interfaces == (config_mode ? 6u : 4u));
    CHECK(ep_count == (config_mode ? 8u : 5u));
    CHECK(cdc_control == 1 && cdc_data == 1 && iad_count == 1 && union_count == 1);
    CHECK(control(0, TUSB_REQ_SET_CONFIGURATION, 1, 0, 0, NULL));
    CHECK(tud_mounted() && global_state.tud_connected);
    CHECK(endpoint(0x81)->opened && endpoint(0x82)->opened);
    CHECK(endpoint(0x81)->max_packet == 32);
    CHECK(endpoint(0x83)->opened == config_mode);
    CHECK(endpoint(0x84)->opened == config_mode && endpoint(0x04)->opened == config_mode);
    CHECK(endpoint(0x85)->opened && endpoint(0x85)->max_packet == 8);
    CHECK(endpoint(0x06)->opened && endpoint(0x06)->max_packet == 64);
    CHECK(endpoint(0x86)->opened && endpoint(0x86)->max_packet == 64);
    CHECK(endpoint(0x06)->pending && endpoint(0x06)->length == 64);
    for (unsigned instance = 0; instance < (config_mode ? 3u : 2u); ++instance) {
        CHECK(control(0x81, TUSB_REQ_GET_DESCRIPTOR, HID_DESC_TYPE_REPORT << 8, instance, 1024, NULL));
        CHECK(host_count > 20 && host_count < 512);
        CHECK(memcmp(host_bytes, tud_hid_descriptor_report_cb(instance), host_count) == 0);
    }
    CHECK(control(0x80, TUSB_REQ_GET_DESCRIPTOR, (TUSB_DESC_STRING << 8) | 2, 0x0409, 255, NULL));
    CHECK(host_count == 30 && host_bytes[2] == 'D' && host_bytes[4] == 'e');
    CHECK(!control(0x80, TUSB_REQ_GET_DESCRIPTOR, (TUSB_DESC_STRING << 8) | 250, 0x0409, 255, NULL));
    CHECK(!control(0x81, TUSB_REQ_GET_DESCRIPTOR, HID_DESC_TYPE_REPORT << 8, 12, 100, NULL));
}
static void hid_data_and_leds(void) {
    uint8_t leds = 5;
    global_state.keyboard_connected = true;
    CHECK(control(0x21, HID_REQ_CONTROL_SET_REPORT, (HID_REPORT_TYPE_OUTPUT << 8) | REPORT_ID_KEYBOARD, 0, 1, &leds));
    CHECK(global_state.keyboard_leds_desired[OUTPUT_A] == 5);
    CHECK(led_restores == 1 && led_peer_messages == 1 && last_led == 5);
    uint8_t wrong[] = {6, 7};
    CHECK(control(0x21, HID_REQ_CONTROL_SET_REPORT, (HID_REPORT_TYPE_OUTPUT << 8) | REPORT_ID_KEYBOARD, 0, 2, wrong));
    CHECK(led_peer_messages == 1 && global_state.keyboard_leds_desired[0] == 5);
    /* This TinyUSB revision prepends a nonzero report ID even when the
       application returns zero bytes. Capture the real stack behavior. */
    CHECK(control(0xa1, HID_REQ_CONTROL_GET_REPORT, (HID_REPORT_TYPE_INPUT << 8) | REPORT_ID_KEYBOARD, 0, 8, NULL));
    CHECK(host_count == 1 && host_bytes[0] == REPORT_ID_KEYBOARD);
    CHECK(!control(0xa1, HID_REQ_CONTROL_GET_REPORT, HID_REPORT_TYPE_INPUT << 8, 0, 8, NULL));
    uint8_t oversized[33] = {0};
    CHECK(!control(0x21, HID_REQ_CONTROL_SET_REPORT, (HID_REPORT_TYPE_OUTPUT << 8) | REPORT_ID_KEYBOARD, 0, sizeof(oversized), oversized));
    CHECK(led_peer_messages == 1);
    CHECK(control(0x21, HID_REQ_CONTROL_SET_IDLE, 4u << 8, 0, 0, NULL));
    CHECK(control(0xa1, HID_REQ_CONTROL_GET_IDLE, 0, 0, 1, NULL));
    CHECK(host_count == 1 && host_bytes[0] == 4);
    CHECK(control(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_BOOT, 0, 0, NULL));
    CHECK(control(0xa1, HID_REQ_CONTROL_GET_PROTOCOL, 0, 0, 1, NULL));
    CHECK(host_count == 1 && host_bytes[0] == HID_PROTOCOL_BOOT);
    CHECK(control(0x21, HID_REQ_CONTROL_SET_PROTOCOL, HID_PROTOCOL_REPORT, 0, 0, NULL));
    uint8_t keys[6] = {HID_KEY_A};
    CHECK(tud_hid_n_ready(0));
    CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, KEYBOARD_MODIFIER_LEFTCTRL, keys));
    CHECK(!tud_hid_n_ready(0));
    CHECK(!tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
    host_count = 0;
    complete(0x81, NULL, 0);
    CHECK(host_count == 9 && host_bytes[0] == REPORT_ID_KEYBOARD && host_bytes[1] == KEYBOARD_MODIFIER_LEFTCTRL && host_bytes[3] == HID_KEY_A);
    CHECK(tud_hid_n_ready(0));
    CHECK(tud_mouse_report(RELATIVE, 2, -100, 45, -1, 0));
    host_count = 0; complete(0x82, NULL, 0);
    CHECK(host_count == 9 && host_bytes[0] == REPORT_ID_RELMOUSE && host_bytes[1] == 2);
    CHECK((int16_t)little16(host_bytes + 2) == -100 && little16(host_bytes + 4) == 45);
}
static void suspend_and_unplug(void) {
    CHECK(control(0, TUSB_REQ_SET_FEATURE, TUSB_REQ_FEATURE_REMOTE_WAKEUP, 0, 0, NULL));
    dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, true); pump();
    CHECK(tud_suspended() && !tud_ready() && suspend_callbacks == 1);
    CHECK(tud_remote_wakeup() && remote_wakeups == 1);
    dcd_event_bus_signal(0, DCD_EVENT_RESUME, true); pump();
    CHECK(!tud_suspended() && tud_ready() && resume_callbacks == 1);
    no_ep0_out_payload = true;
    dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, true); pump();
    no_ep0_out_payload = false;
    CHECK(!tud_mounted() && !global_state.tud_connected);
}

static void console_tick(void) {
    bool connected = tud_cdc_connected();
    uint32_t rx_before = tud_cdc_available();
    int64_t space_before = tud_cdc_write_available();
    uint64_t submitted_before = cdc_submitted_bytes;
    unsigned reads_before = history_reads;
    unsigned history_polls_before = history_peer_polls;
    unsigned verify_polls_before = verify_polls;
    unsigned verify_rechecks_before = verify_rechecks;
    unsigned maintenance_polls_before = maintenance_polls;
    console_task(console_now_us);
    CHECK(history_reads - reads_before <= 1);
    CHECK(history_peer_polls - history_polls_before <= 1);
    CHECK(verify_polls - verify_polls_before <= 1);
    CHECK(verify_rechecks - verify_rechecks_before <= 1);
    CHECK(maintenance_polls - maintenance_polls_before <= 1);
    if (connected) {
        uint32_t rx_after = tud_cdc_available();
        CHECK(rx_after <= rx_before && rx_before - rx_after <= 32);
        /* Count both bytes retained in the real CDC FIFO and bytes moved into
         * a real DCD transfer by an automatic flush during this task call. */
        int64_t produced = (int64_t)(cdc_submitted_bytes - submitted_before)
                         + space_before - (int64_t)tud_cdc_write_available();
        CHECK(produced >= 0 && produced <= 64);
    }
}

static void console_pump(unsigned ticks) {
    for (unsigned tick = 0; tick < ticks; ++tick) {
        console_tick();
        if (endpoint(0x86)->pending) complete(0x86, NULL, 0);
    }
}

static void console_drain(void) {
    console_pump(1024);
    CHECK(!endpoint(0x86)->pending && tud_cdc_available() == 0);
}

static void cdc_capture_clear(void) {
    cdc_count = 0;
    cdc_bytes[0] = '\0';
}

static void cdc_send_bytes(const void *source, size_t length) {
    const uint8_t *bytes = source;
    while (length) {
        if (!endpoint(0x06)->pending) console_drain();
        unsigned chunk = length > 64 ? 64 : (unsigned)length;
        complete_short_out(0x06, bytes, chunk);
        console_drain();
        bytes += chunk;
        length -= chunk;
    }
}

static void cdc_send(const char *text) { cdc_send_bytes(text, strlen(text)); }

static unsigned occurrences(const char *needle) {
    unsigned count = 0;
    const char *p = cdc_bytes;
    while ((p = strstr(p, needle)) != NULL) { ++count; p += strlen(needle); }
    return count;
}

static void cdc_controls(bool config_mode) {
    uint8_t instance = config_mode ? 4 : 2;
    CHECK(!tud_cdc_connected());
    /* Host-created 115200 baud, one stop bit, no parity, eight data bits. */
    uint8_t coding[] = {0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08};
    CHECK(control(0x21, 0x20, 0, instance, sizeof(coding), coding));
    CHECK(control(0xa1, 0x21, 0, instance, sizeof(coding), NULL));
    CHECK(host_count == sizeof(coding) && memcmp(host_bytes, coding, sizeof(coding)) == 0);
    CHECK(control(0x21, 0x22, 3, instance, 0, NULL));
    CHECK(tud_cdc_connected() && tud_cdc_get_line_state() == 3);
    cdc_capture_clear();
    console_drain();
    CHECK(strstr(cdc_bytes, "deskhop> ") != NULL);
}

static void cdc_status_fields(void) {
    CHECK(occurrences("BEGIN status") == 1 && occurrences("END status") == 1);
    CHECK(strstr(cdc_bytes, "board=A") != NULL);
    CHECK(strstr(cdc_bytes, "board_id=0123456789abcdef") != NULL);
    CHECK(strstr(cdc_bytes, "build=0.101") != NULL);
    CHECK(strstr(cdc_bytes, "image_crc_at_boot=89abcdef") != NULL);
    CHECK(strstr(cdc_bytes, "board=A core=0 checkpoints=123 age_ms=0\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=A core=1 checkpoints=456 age_ms=1\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=A update seen=0 source=none phase=idle received=0 total=262144 "
                            "progress_age_ms=unavailable target=unknown attempt=0\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=B core=0 checkpoints=900 age_ms=2\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=B core=1 checkpoints=901 age_ms=3\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=B update seen=1 source=peer phase=receiving received=65536 total=262144 "
                            "progress_age_ms=4 target=0.100 attempt=1\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=B observation boot=first_seen progress=baseline update=not_observed\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "boot_session=1122334455667788") != NULL);
    CHECK(strstr(cdc_bytes, "board=B\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board_id=FEDCBA9876543210") != NULL);
    CHECK(strstr(cdc_bytes, "build=0.96") != NULL);
    CHECK(strstr(cdc_bytes, "image_crc_at_boot=12345678") != NULL);
    CHECK(strstr(cdc_bytes, "boot_session=8877665544332211") != NULL);
    CHECK(strstr(cdc_bytes, "uptime_ms=7654321") != NULL);
    CHECK(strstr(cdc_bytes, "peer=ok") != NULL);
    CHECK(strstr(cdc_bytes, "verification=available") != NULL);
    CHECK(strstr(cdc_bytes, "deskhop> ") != NULL);
}

static void cdc_command_stream(void) {
    cdc_capture_clear();
    cdc_send("he");
    CHECK(strstr(cdc_bytes, "status") == NULL);
    cdc_send("lp\r\n");
    CHECK(occurrences("deskhop> ") == 1);
    CHECK(strstr(cdc_bytes, "help") && strstr(cdc_bytes, "status"));
    CHECK(strstr(cdc_bytes, "Diagnostics are read-only") != NULL);
    CHECK(strstr(cdc_bytes, "all commands are read-only") == NULL);
    CHECK(strstr(cdc_bytes, "bootloader A|B") != NULL);
    CHECK(strstr(cdc_bytes, "DISRUPTIVE") != NULL);
    CHECK(strstr(cdc_bytes, "both boards") != NULL);
    CHECK(strstr(cdc_bytes, "history [count]") != NULL);
    CHECK(strstr(cdc_bytes, "event ages align approximately") != NULL);
    CHECK(strstr(cdc_bytes, "Core counts mark diagnostic task checkpoints") != NULL);
    CHECK(strstr(cdc_bytes, "Peer confirmation is historical/version-only; progress compares status queries") != NULL);
    CHECK(strstr(cdc_bytes, "progress compares status queries") != NULL);
    CHECK(strstr(cdc_bytes, "History/observations are in RAM until reboot") != NULL);
    CHECK(strstr(cdc_bytes, "GAP means unavailable capture data or older peer support") != NULL);
    CHECK(strstr(cdc_bytes, "Verify CRC: full 256KiB firmware including metadata; configuration excluded") != NULL);
    CHECK(strstr(cdc_bytes, "PASS describes a fresh scan; rerun verify after changes") != NULL);
    CHECK(occurrences("END help") == 1); /* The complete help fits the fixed TX buffer. */

    cdc_capture_clear();
    console_now_us = UINT64_C(1234567000);
    cdc_send("sta");
    CHECK(occurrences("BEGIN status") == 0);
    cdc_send("tus\n");
    cdc_status_fields();
    CHECK(strstr(cdc_bytes, "uptime_ms=1234567") != NULL);
    /* The peer updater changes this RAM metadata before reboot. Console
     * identity must continue describing the code that actually booted. */
    global_state._running_fw.version = 999;
    global_state._running_fw.checksum = UINT32_C(0x76543210);
    cdc_capture_clear();
    console_now_us += 2000000;
    cdc_send("status\r");
    cdc_status_fields();
    CHECK(strstr(cdc_bytes, "uptime_ms=1236567") != NULL);

    /* Coalesced commands stay in the real CDC RX FIFO while each response
     * drains. CRLF cannot execute the preceding command twice. */
    cdc_capture_clear();
    cdc_send("help\nstatus\r\n");
    CHECK(occurrences("BEGIN status") == 1 && occurrences("END status") == 1);
    CHECK(occurrences("deskhop> ") == 2);

    cdc_capture_clear();
    cdc_send("status both\nflash\nreboot\n");
    CHECK(occurrences("ERROR") == 3 && occurrences("BEGIN status") == 0);
    CHECK(!global_state.reboot_requested && !global_state.config_mode_active);

    cdc_capture_clear();
    cdc_send("statux\bs\n");
    cdc_status_fields();
    cdc_capture_clear();
    cdc_send("helpX\x7f\n");
    CHECK(strstr(cdc_bytes, "status") != NULL && occurrences("ERROR") == 0);
    cdc_capture_clear();
    cdc_send("stat\x03help\n");
    CHECK(occurrences("BEGIN status") == 0 && occurrences("ERROR") == 0);
    CHECK(strstr(cdc_bytes, "status") != NULL);

    /* The suffix of an invalid/overlong command is never interpreted as a new
     * command. A following complete line must recover without a reset. */
    char long_line[86];
    memset(long_line, 'x', 80);
    memcpy(long_line + 80, "help\n", 6);
    cdc_capture_clear();
    cdc_send(long_line);
    CHECK(occurrences("ERROR") == 1 && strstr(cdc_bytes, "status") == NULL);
    cdc_send("help\n");
    CHECK(strstr(cdc_bytes, "status") != NULL);
    const uint8_t invalid_line[] = {'s', 't', 0x80, 'h', 'e', 'l', 'p', '\n'};
    cdc_capture_clear();
    cdc_send_bytes(invalid_line, sizeof(invalid_line));
    CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN status") == 0);
    cdc_send("status\n");
    cdc_status_fields();
    const uint8_t nul_suffix[] = {'s', 't', 'a', 't', 'u', 's', 0, 'x', '\n'};
    cdc_capture_clear();
    cdc_send_bytes(nul_suffix, sizeof(nul_suffix));
    CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN status") == 0);
    cdc_send("status\n");
    cdc_status_fields();
}

static void cdc_runtime_rows_and_legacy(void) {
    const peer_status_snapshot_t saved_peer = peer_snapshot;
    const diagnostic_peer_observation_t saved_observation = peer_observation;
    unsigned reads_before = runtime_reads;
    local_runtime.core_valid = 1;
    local_runtime.core_ticks[0] = 0; /* Valid zero can be a wrapped checkpoint counter. */
    local_runtime.core_ticks[1] = UINT32_MAX;
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(runtime_reads == reads_before + 1);
    CHECK(strstr(cdc_bytes, "board=A core=0 checkpoints=0 age_ms=0\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=A core=1 checkpoints=unavailable age_ms=unavailable\r\n") != NULL);

    local_runtime = default_runtime;
    local_runtime.update_seen = true;
    local_runtime.source = DIAGNOSTIC_SOURCE_USB;
    local_runtime.phase = DIAGNOSTIC_UPDATE_REBOOT_PENDING;
    local_runtime.received_bytes = 262144;
    local_runtime.progress_age_ms = 17;
    local_runtime.target_version = 200;
    local_runtime.update_attempt = 2;
    peer_observation = (diagnostic_peer_observation_t){DIAGNOSTIC_PEER_NEW_BOOT,
        DIAGNOSTIC_PROGRESS_NOT_ADVANCING, DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS};
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(strstr(cdc_bytes, "board=A update seen=1 source=usb phase=reboot_pending received=262144 total=262144 "
                            "progress_age_ms=17 target=0.100 attempt=2\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=B observation boot=new_boot progress=not_advancing update=awaiting_progress\r\n") != NULL);
    peer_observation.progress = DIAGNOSTIC_PROGRESS_ADVANCING;
    peer_observation.update = DIAGNOSTIC_EXECUTION_CONFIRMED;
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(strstr(cdc_bytes, "board=B observation boot=new_boot progress=advancing update=confirmed\r\n") != NULL);

    peer_snapshot.protocol = 1;
    peer_observation.progress = DIAGNOSTIC_PROGRESS_UNAVAILABLE;
    peer_observation.update = DIAGNOSTIC_EXECUTION_NOT_OBSERVED;
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(strstr(cdc_bytes, "board=B runtime=unavailable protocol=1\r\n") != NULL);
    CHECK(occurrences("board=B core=") == 0 && occurrences("board=B update ") == 0);
    CHECK(strstr(cdc_bytes, "board=B observation boot=new_boot progress=unavailable update=not_observed\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "peer=ok\r\n") != NULL && occurrences("END status") == 1);

    /* Maximum scalar widths and long enum names fit each fixed response chunk. */
    local_runtime = (diagnostic_runtime_snapshot_t){
        .core_ticks = {UINT32_MAX, UINT32_MAX}, .core_age_ms = {UINT32_MAX, UINT32_MAX}, .core_valid = 3,
        .received_bytes = UINT32_MAX, .total_bytes = UINT32_MAX, .progress_age_ms = UINT32_MAX,
        .update_attempt = UINT32_MAX, .target_version = UINT16_MAX, .update_seen = true,
        .source = DIAGNOSTIC_SOURCE_PEER, .phase = DIAGNOSTIC_UPDATE_REBOOT_PENDING,
    };
    peer_snapshot.protocol = 2;
    peer_snapshot.runtime = local_runtime;
    peer_observation = (diagnostic_peer_observation_t){DIAGNOSTIC_PEER_IDENTITY_CHANGED,
        DIAGNOSTIC_PROGRESS_NOT_ADVANCING, DIAGNOSTIC_EXECUTION_AWAITING_PROGRESS};
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(occurrences("checkpoints=4294967295 age_ms=4294967295\r\n") == 4);
    CHECK(occurrences("progress_age_ms=4294967295 target=65.435 attempt=4294967295\r\n") == 2);
    CHECK(strstr(cdc_bytes, "board=B observation boot=identity_changed progress=not_advancing update=awaiting_progress\r\n") != NULL);
    CHECK(occurrences("END status") == 1 && occurrences("deskhop> ") == 1);
    local_runtime = default_runtime;
    peer_snapshot = saved_peer;
    peer_observation = saved_observation;

    /* Snapshot exactly once at command start even while USB output is paused. */
    local_runtime.core_ticks[0] = 111;
    cdc_capture_clear();
    reads_before = runtime_reads;
    complete_short_out(0x06, "status\n", 7);
    for (unsigned i = 0; i < 32; ++i) console_tick();
    CHECK(runtime_reads == reads_before + 1 && endpoint(0x86)->pending);
    local_runtime.core_ticks[0] = 999;
    console_drain();
    CHECK(strstr(cdc_bytes, "board=A core=0 checkpoints=111 age_ms=0\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "checkpoints=999") == NULL);
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(strstr(cdc_bytes, "board=A core=0 checkpoints=999 age_ms=0\r\n") != NULL);
    CHECK(runtime_reads == reads_before + 2);
    local_runtime = default_runtime;

    /* Editing echo can consume three TX bytes per RX byte. Keep enough room
     * for a complete help response even after27 backspaces in the final tick. */
    char erase_line[33];
    memset(erase_line, '\b', 27);
    memcpy(erase_line + 27, "help\n", 6);
    cdc_send("xxxxxxxxxxxxxxxxxxxxxxxxxxx");
    cdc_capture_clear();
    cdc_send(erase_line);
    CHECK(occurrences("BEGIN help") == 1 && occurrences("END help") == 1);
    CHECK(occurrences("deskhop> ") == 1 && occurrences("ERROR") == 0);
}

static void verify_frame(const char *overall) {
    CHECK(occurrences("BEGIN verify") == 1 && occurrences("END verify") == 1);
    CHECK(strstr(cdc_bytes, "scope=both\r\nclaim=scan_snapshot\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "coverage_start=0x10000000\r\ncoverage_bytes=262144\r\n") != NULL);
    char expected[64];
    snprintf(expected, sizeof(expected), "\r\nresult=%s reason=", overall);
    CHECK(strstr(cdc_bytes, expected) != NULL);
    CHECK(strstr(cdc_bytes, "END verify\r\ndeskhop> ") != NULL);
    CHECK(occurrences("board=A result=") == 1 && occurrences("board=B result=") == 1);
    CHECK(strstr(cdc_bytes, "board=A result=") < strstr(cdc_bytes, "board=B result="));
}

static void cdc_verify_parser_and_results(void) {
    verify_fixtures();
    cdc_capture_clear();
    unsigned rechecks_before = verify_rechecks;
    cdc_send("ver");
    CHECK(occurrences("BEGIN verify") == 0);
    cdc_send("ify 0.101 12345678\r\n");
    verify_frame("PASS");
    CHECK(strstr(cdc_bytes, "expected_build=0.101\r\nexpected_crc32=12345678\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=A result=PASS reason=match\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=B result=PASS reason=match\r\n") != NULL);
    CHECK(occurrences("bytes=262144 crc32=12345678\r\n") == 2);
    CHECK(strstr(cdc_bytes, "board=A core=1 start=200 end=1200 age_ms=1 valid=1\r\n") != NULL);
    CHECK(verify_rechecks == rechecks_before + 2 && occurrences("deskhop> ") == 1);

    const char *invalid[] = {
        "verify\n", "verify 0.101\n", "verify 0.101 1234567\n", "verify 0.101 123456789\n",
        "verify 0.101 0x12345678\n", "verify 0.101 1234567g\n", "verify +0.101 12345678\n",
        "verify -0.101 12345678\n", "verify 0.-1 12345678\n", "verify 0.1000 12345678\n",
        "verify 65.436 12345678\n", "verify 66.0 12345678\n", "verify 0. 12345678\n",
        "verify .101 12345678\n", "verify 4294967296.1 12345678\n", "verify 0.4294967296 12345678\n",
        "verify 0.101 12345678 extra\n", "verify  0.101 12345678\n", "verify 0.101  12345678\n",
        "verify 0.101 12345678 \n", "verify 0.101.1 12345678\n",
    };
    unsigned requests_before = verify_requests;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cdc_capture_clear();
        cdc_send(invalid[i]);
        CHECK(occurrences("ERROR usage: verify") == 1 && occurrences("BEGIN verify") == 0);
        CHECK(occurrences("deskhop> ") == 1);
    }
    CHECK(verify_requests == requests_before);
    cdc_capture_clear();
    cdc_send("verify 00.0101 ABCDEF12\n");
    verify_frame("FAIL");
    CHECK(strstr(cdc_bytes, "expected_build=0.101\r\nexpected_crc32=abcdef12\r\n") != NULL);
    cdc_capture_clear();
    cdc_send("verify 65.435 00000000\n");
    verify_frame("FAIL");
    CHECK(strstr(cdc_bytes, "expected_build=65.435") != NULL);

    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned fault = 0; fault < 12; ++fault) {
            verify_fixtures();
            verify_result_t *result = &verify_fixture[side];
            const char *reason = "";
            const char *verdict = "UNVERIFIED";
            switch (fault) {
            case 0: result->snapshot.minor = 102; result->snapshot.metadata_version = 202;
                    reason = "build_mismatch"; verdict = "FAIL"; break;
            case 1: result->snapshot.slot_crc32++; reason = "crc_mismatch"; verdict = "FAIL"; break;
            case 2: result->snapshot.metadata_magic = 0; reason = "metadata_invalid"; verdict = "FAIL"; break;
            case 3: result->snapshot.outcome = VERIFY_SCAN_UPDATE_ACTIVE; reason = "update_active"; break;
            case 4: result->snapshot.generation_end++; reason = "image_changed"; break;
            case 5: result->transport = VERIFY_TRANSPORT_BUSY; reason = "busy"; break;
            case 6: result->transport = VERIFY_TRANSPORT_TIMEOUT; reason = "timeout_or_unsupported"; break;
            case 7: result->transport = VERIFY_TRANSPORT_INVALID; reason = "invalid_response"; break;
            case 8: result->snapshot.end.core_valid = 1; reason = "core_unavailable"; break;
            case 9: result->snapshot.end.core_ticks[1] = result->snapshot.start.core_ticks[1];
                    reason = "core_progress_unverified"; break;
            case 10: result->snapshot.role ^= 1; reason = "invalid_response"; break;
            case 11: result->snapshot.bytes_read = 128; reason = "invalid_response"; break;
            }
            cdc_capture_clear();
            cdc_send("verify 0.101 12345678\n");
            verify_frame(verdict);
            char expected[128];
            snprintf(expected, sizeof(expected), "board=%c result=%s reason=%s\r\n", side ? 'B' : 'A', verdict, reason);
            CHECK(strstr(cdc_bytes, expected) != NULL);
            if (fault == 11) CHECK(strstr(cdc_bytes, "bytes=128 crc32=unavailable") != NULL);
        }
    }
    /* Reading the final byte does not imply the final guard succeeded or that
     * the scan assigned its CRC. Never render that unassigned zero as evidence. */
    const verify_scan_outcome_t unfinished[] = {VERIFY_SCAN_CHANGED, VERIFY_SCAN_TIMEOUT};
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned fault = 0; fault < sizeof(unfinished) / sizeof(unfinished[0]); ++fault) {
            verify_fixtures();
            verify_fixture[side].snapshot.outcome = unfinished[fault];
            verify_fixture[side].snapshot.bytes_read = VERIFY_IMAGE_BYTES;
            verify_fixture[side].snapshot.slot_crc32 = 0;
            cdc_capture_clear();
            cdc_send("verify 0.101 12345678\n");
            verify_frame("UNVERIFIED");
            CHECK(occurrences("bytes=262144 crc32=unavailable\r\n") == 1);
            CHECK(occurrences("bytes=262144 crc32=12345678\r\n") == 1);
            CHECK(strstr(cdc_bytes, "bytes=262144 crc32=00000000") == NULL);
        }
    }
    verify_fixtures();
    verify_fixture[0].transport = VERIFY_TRANSPORT_TIMEOUT;
    verify_fixture[1].snapshot.slot_crc32++;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    verify_frame("FAIL"); // A definite mismatch dominates the other board's missing evidence.
    verify_fixtures();
    verify_accept = false;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    verify_frame("UNVERIFIED");
    CHECK(occurrences("result=UNVERIFIED reason=busy") == 2);
    verify_fixtures();
    for (unsigned i = 0; i < 2; ++i) {
        verify_snapshot_t *snapshot = &verify_fixture[i].snapshot;
        snapshot->major = 65; snapshot->minor = 435;
        memset(snapshot->board_id, 0xff, sizeof(snapshot->board_id));
        snapshot->boot_session = UINT64_MAX;
        snapshot->started_us = UINT64_MAX - 1000000;
        snapshot->completed_us = UINT64_MAX;
        snapshot->generation_start = snapshot->generation_end = UINT64_MAX;
        snapshot->slot_crc32 = snapshot->image_crc_at_boot = snapshot->metadata_crc32 = UINT32_MAX;
        snapshot->metadata_magic = UINT32_MAX;
        snapshot->metadata_version = snapshot->metadata_reserved = UINT16_MAX;
        snapshot->start.core_ticks[0] = snapshot->start.core_ticks[1] = UINT32_MAX;
        snapshot->end.core_ticks[0] = snapshot->end.core_ticks[1] = UINT32_MAX;
        snapshot->end.core_age_ms[0] = snapshot->end.core_age_ms[1] = UINT32_MAX;
    }
    cdc_capture_clear();
    cdc_send("verify 65.435 FFFFFFFF\n");
    verify_frame("FAIL");
    CHECK(occurrences("metadata_magic=ffffffff metadata_version=65535 metadata_reserved=65535 metadata_crc32=ffffffff\r\n") == 2);
    CHECK(occurrences("generation_start=18446744073709551615 generation_end=18446744073709551615\r\n") == 2);
    CHECK(occurrences("start=4294967295 end=4294967295 age_ms=4294967295 valid=1\r\n") == 4);
    CHECK(occurrences("END verify") == 1 && occurrences("deskhop> ") == 1);
    verify_fixtures();
    console_init(OUTPUT_B, "fedcba9876543210", UINT64_C(0x8877665544332211), UINT32_C(0x12345678));
    console_drain();
    verify_fixture[0].snapshot.role = 1;
    verify_fixture[1].snapshot.role = 0;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    CHECK(strstr(cdc_bytes, "board=B result=PASS reason=match") != NULL);
    CHECK(strstr(cdc_bytes, "board=A result=PASS reason=match") != NULL);
    CHECK(strstr(cdc_bytes, "board=B result=") < strstr(cdc_bytes, "board=A result="));
    CHECK(strstr(cdc_bytes, "\r\nresult=PASS reason=both_match\r\nEND verify\r\ndeskhop> ") != NULL);
    console_init(OUTPUT_A, "0123456789abcdef", UINT64_C(0x1122334455667788), UINT32_C(0x89abcdef));
    console_drain();
    verify_fixtures();
}

static void cdc_verify_busy_deferrals(void) {
    /* Independently stall the local row and the final verdict, after any PASS
     * rows have drained. One lock miss must yield without caching a verdict. */
    for (unsigned footer = 0; footer < 2; ++footer) {
        for (unsigned outcome = 0; outcome < 4; ++outcome) {
            verify_fixtures();
            verify_auto = false;
            cdc_capture_clear();
            cdc_send("verify 0.101 12345678\n");
            if (footer) {
                verify_publish(0, verify_token);
                console_drain();
                CHECK(strstr(cdc_bytes, "board=A result=PASS") != NULL);
            }
            verify_recheck_busy = true;
            if (!footer) verify_publish(0, verify_token);
            verify_publish(1, verify_token);
            unsigned rechecks_before = verify_rechecks;
            console_pump(64);
            CHECK(verify_rechecks > rechecks_before);
            CHECK(occurrences("END verify") == 0);
            CHECK(occurrences("board=A result=") == footer);
            CHECK(occurrences("board=B result=") == footer);

            /* HID still moves through the real USB stack while CDC is waiting
             * on the guard. No flash lock or busy retry loop is held here. */
            uint8_t keys[6] = {HID_KEY_F};
            CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
            host_count = 0;
            complete(0x81, NULL, 0);
            CHECK(host_count == 9 && host_bytes[3] == HID_KEY_F);

            if (outcome == 3) {
                console_now_us = verify_requested_at + 3500000 - 1;
                console_pump(8);
                CHECK(occurrences("END verify") == 0);
                console_now_us++;
                rechecks_before = verify_rechecks;
                console_drain();
                CHECK(verify_rechecks == rechecks_before); // no try after expiry
                verify_frame("UNVERIFIED");
                CHECK(strstr(cdc_bytes, "\r\nresult=UNVERIFIED reason=expired") != NULL);
            } else {
                verify_recheck_busy = false;
                verify_busy_remaining = 3; // multiple transient collisions
                verify_recheck_outcome = outcome == 1 ? VERIFY_SCAN_CHANGED
                                        : outcome == 2 ? VERIFY_SCAN_UPDATE_ACTIVE
                                                       : VERIFY_SCAN_COMPLETE;
                console_drain();
                CHECK(verify_busy_remaining == 0);
                verify_frame(outcome == 0 ? "PASS" : "UNVERIFIED");
                if (outcome && !footer)
                    CHECK(strstr(cdc_bytes, outcome == 1
                        ? "board=A result=UNVERIFIED reason=image_changed"
                        : "board=A result=UNVERIFIED reason=update_active") != NULL);
            }
            CHECK(occurrences("END verify") == 1);
        }
    }

    /* Disconnect while a completed local scan awaits freshness. A new console
     * query must reject the old token and wait for its own two results. */
    verify_fixtures();
    verify_auto = false;
    verify_recheck_busy = true;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    verify_publish(0, verify_token);
    console_pump(32);
    CHECK(occurrences("board=A result=") == 0 && occurrences("END verify") == 0);
    uint32_t abandoned = verify_token;
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    console_drain();
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    verify_recheck_busy = false;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    CHECK(verify_token != abandoned);
    verify_publish(0, abandoned);
    verify_publish(1, abandoned);
    console_pump(16);
    CHECK(occurrences("board=A result=") == 0);
    verify_publish(0, verify_token);
    verify_publish(1, verify_token);
    console_drain();
    verify_frame("PASS");
    verify_fixtures();
}

static void cdc_verify_backpressure_freshness_and_reconnect(void) {
    /* A local change after its PASS row must invalidate the final verdict.
     * The already-sent rows retain their scan evidence, including the peer's. */
    const verify_scan_outcome_t late_guard[] = {
        VERIFY_SCAN_CHANGED, VERIFY_SCAN_UPDATE_ACTIVE,
    };
    for (unsigned fault = 0; fault < sizeof(late_guard) / sizeof(late_guard[0]); ++fault) {
        verify_fixtures();
        verify_auto = false;
        cdc_capture_clear();
        unsigned rechecks_before = verify_rechecks;
        cdc_send("verify 0.101 12345678\n");
        verify_publish(0, verify_token);
        console_drain();
        CHECK(strstr(cdc_bytes, "board=A result=PASS reason=match\r\n") != NULL);
        CHECK(occurrences("board=B result=") == 0 && occurrences("END verify") == 0);
        CHECK(verify_rechecks == rechecks_before + 1);
        verify_recheck_outcome = late_guard[fault];
        verify_publish(1, verify_token);
        console_drain();
        verify_frame("UNVERIFIED");
        CHECK(verify_rechecks == rechecks_before + 2);
        CHECK(strstr(cdc_bytes, "board=A result=PASS reason=match\r\n") != NULL);
        CHECK(strstr(cdc_bytes, "board=B result=PASS reason=match\r\n") != NULL);
        CHECK(occurrences("bytes=262144 crc32=12345678\r\n") == 2);
        CHECK(strstr(cdc_bytes, "\r\nresult=UNVERIFIED reason=incomplete\r\nEND verify") != NULL);
    }

    verify_fixtures();
    verify_auto = false;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    CHECK(occurrences("END verify") == 0 && occurrences("board=A result=") == 0);
    verify_publish(1, verify_token); // The peer can finish first; output remains local-first.
    console_pump(8);
    CHECK(occurrences("board=B result=") == 0);
    verify_publish(0, verify_token);
    verify_recheck_outcome = VERIFY_SCAN_CHANGED;
    console_drain();
    verify_frame("UNVERIFIED");
    CHECK(strstr(cdc_bytes, "board=A result=UNVERIFIED reason=image_changed") != NULL);
    CHECK(strstr(cdc_bytes, "board=B result=PASS reason=match") != NULL);

    verify_fixtures();
    verify_auto = false;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    verify_publish(0, verify_token);
    console_pump(2);
    verify_fixture[0].snapshot.slot_crc32++;
    verify_publish(0, verify_token); // A duplicate must not replace accepted local evidence.
    verify_publish(1, verify_token);
    console_drain();
    verify_frame("PASS");
    CHECK(occurrences("bytes=262144 crc32=12345678\r\n") == 2);

    verify_fixtures();
    verify_auto = false;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    verify_publish(0, verify_token);
    console_drain();
    CHECK(strstr(cdc_bytes, "board=A result=PASS reason=match") != NULL);
    CHECK(occurrences("END verify") == 0);
    console_now_us = verify_requested_at + 3500000;
    console_drain();
    verify_frame("UNVERIFIED");
    CHECK(strstr(cdc_bytes, "board=B result=UNVERIFIED reason=expired") != NULL);
    CHECK(strstr(cdc_bytes, "\r\nresult=UNVERIFIED reason=expired") != NULL);

    verify_fixtures();
    cdc_capture_clear();
    complete_short_out(0x06, "verify 0.101 12345678\nstatus\n", 29);
    for (unsigned tick = 0; tick < 64; ++tick) console_tick();
    CHECK(!verify_queued && endpoint(0x86)->pending && tud_cdc_available() == 7);
    uint8_t keys[6] = {HID_KEY_E};
    CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
    host_count = 0;
    complete(0x81, NULL, 0);
    CHECK(host_count == 9 && host_bytes[3] == HID_KEY_E);
    console_now_us = verify_requested_at + 3500000;
    console_drain();
    verify_frame("UNVERIFIED");
    CHECK(occurrences("result=UNVERIFIED reason=expired") == 3);
    CHECK(occurrences("result=PASS") == 0);
    CHECK(strstr(cdc_bytes, "END verify") < strstr(cdc_bytes, "BEGIN status"));
    cdc_status_fields();

    verify_fixtures();
    verify_auto = false;
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    uint32_t abandoned = verify_token;
    console_now_us = verify_requested_at + 3500000;
    console_drain();
    verify_frame("UNVERIFIED");
    CHECK(occurrences("result=UNVERIFIED reason=expired") == 3);
    verify_publish(0, abandoned);
    verify_publish(1, abandoned);
    cdc_capture_clear();
    console_drain();
    CHECK(cdc_count == 0 && !verify_queued);

    cdc_send("verify 0.101 12345678\n");
    CHECK(verify_token != abandoned);
    verify_publish(0, abandoned);
    verify_publish(1, abandoned);
    console_pump(8);
    CHECK(occurrences("board=A result=") == 0);
    verify_publish(1, verify_token);
    verify_publish(0, verify_token);
    console_drain();
    verify_frame("PASS");
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    abandoned = verify_token;
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    verify_publish(0, abandoned);
    verify_publish(1, abandoned);
    unsigned polls_before = verify_polls;
    console_tick();
    CHECK(verify_queued == 1 && verify_polls == polls_before + 1);
    console_tick();
    CHECK(!verify_queued && verify_polls == polls_before + 2);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("END verify") == 0 && occurrences("board=A result=") == 0);
    verify_fixtures();
    cdc_capture_clear();
    complete_short_out(0x06, "verify 0.101 12345678\n", 22);
    for (unsigned tick = 0; tick < 32; ++tick) console_tick();
    CHECK(endpoint(0x86)->pending && !verify_queued);
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    complete(0x86, NULL, 0);
    if (endpoint(0x86)->pending) {
        CHECK(endpoint(0x86)->length == 0);
        complete(0x86, NULL, 0);
    }
    console_drain();
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("END verify") == 0 && occurrences("board=A result=") == 0);
    cdc_capture_clear();
    cdc_send("verify 0.101 12345678\n");
    verify_frame("PASS");
}

static void history_fill(unsigned count) {
    diagnostic_history_init();
    for (unsigned i = 0; i < count; ++i) {
        console_now_us = (uint64_t)(i + 1) * 1000;
        diagnostic_history_record(HISTORY_OUTPUT_LOCAL, i & 1, (i + 1) & 1, 0);
    }
}

static void history_frame(unsigned returned, uint64_t overwritten) {
    char expected[96];
    CHECK(occurrences("BEGIN history") == 1 && occurrences("END history") == 1);
    CHECK(strstr(cdc_bytes, "scope=both\r\nrequested_per_board=") != NULL);
    CHECK(strstr(cdc_bytes, "timing=approximate_snapshot_alignment\r\ncapacity_per_board=64\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "board=A\r\nboot_session=1122334455667788\r\nsampled_uptime_ms=") != NULL);
    snprintf(expected, sizeof(expected), "returned=%u\r\noverwritten=%llu\r\n",
             returned, (unsigned long long)overwritten);
    CHECK(strstr(cdc_bytes, expected) != NULL);
    CHECK(strstr(cdc_bytes, "END history\r\ndeskhop> ") != NULL);
    CHECK(occurrences("ERROR") == 0);
}

static void history_rows(uint64_t first, uint64_t end, bool allow_gaps) {
    const char *p = strstr(cdc_bytes, "BEGIN history");
    CHECK(p != NULL);
    p = strstr(p, "\r\noverwritten=");
    CHECK(p != NULL);
    p = strstr(p + 2, "\r\n\r\n");
    CHECK(p != NULL);
    p += 4;
    for (uint64_t expected = first; expected < end; ++expected) {
        unsigned long long seq, uptime, age;
        int width = 0;
        if (strncmp(p, "GAP ", 4) == 0) {
            CHECK(allow_gaps);
            CHECK(sscanf(p, "GAP board=A seq=%llu%n", &seq, &width) == 1);
            CHECK(p[width] == '\r' && p[width + 1] == '\n');
        } else {
            CHECK(sscanf(p, "board=A seq=%llu uptime_ms=%llu age_ms=%llu event=%n", &seq, &uptime, &age, &width) == 3);
            CHECK(width > 0 && uptime == seq); /* Our fixture time is seq milliseconds. */
            CHECK(strncmp(p + width, "output_local old=", 17) == 0);
        }
        CHECK(seq == expected);
        p = strstr(p, "\r\n");
        CHECK(p != NULL);
        p += 2;
    }
    CHECK(strncmp(p, "END history\r\n", 13) == 0);
}

static void cdc_history_count_and_wrap(void) {
    history_fill(0);
    cdc_capture_clear();
    cdc_send("history\n");
    history_frame(0, 0);
    CHECK(occurrences(" seq=") == 0 && occurrences("deskhop> ") == 1);

    history_fill(80);
    cdc_capture_clear();
    cdc_send("his");
    CHECK(occurrences("BEGIN history") == 0);
    cdc_send("tory\r\n");
    history_frame(16, 16);
    history_rows(65, 81, false);
    CHECK(occurrences("deskhop> ") == 1);

    cdc_capture_clear();
    cdc_send("history 1\n");
    history_frame(1, 16);
    history_rows(80, 81, false);

    cdc_capture_clear();
    cdc_send("history 64\n");
    history_frame(64, 16);
    history_rows(17, 81, false);
    CHECK(occurrences("event=") == 64);

    static const char *invalid[] = {
        "history 0\n", "history -1\n", "history +1\n", "history 65\n",
        "history \n", "history  1\n", "history 1 \n", "history 1 2\n",
        "history 1x\n", "history 0x10\n", "history 4294967297\n",
        "history 18446744073709551617\n", "history 1.5\n", "history both\n",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cdc_capture_clear();
        cdc_send(invalid[i]);
        CHECK(occurrences("ERROR usage: history [count] (decimal 1..64)") == 1);
        CHECK(occurrences("BEGIN history") == 0 && occurrences("deskhop> ") == 1);
    }
    cdc_capture_clear();
    cdc_send("history 0001\n");
    history_frame(1, 16);
    history_rows(80, 81, false);
    CHECK(!global_state.reboot_requested && !global_state.config_mode_active);
}

static void cdc_history_event_fields(void) {
    diagnostic_history_init();
    console_now_us = 1000000;
    diagnostic_history_record(HISTORY_BOOT, 1, 0, (2u << 16) | 98);
    diagnostic_history_record(HISTORY_OUTPUT_LOCAL, 0, 1, 0);
    diagnostic_history_record(HISTORY_OUTPUT_PEER, 1, 0, 0);
    diagnostic_history_record(HISTORY_USB_MOUNT, 0, 0, 0);
    diagnostic_history_record(HISTORY_USB_UNMOUNT, 0, 0, 0);
    diagnostic_history_record(HISTORY_HID_MOUNT, 17, 3, 2 | 0x300);
    diagnostic_history_record(HISTORY_HID_UNMOUNT, 17, 3, 1 | 0x100);
    diagnostic_history_record(HISTORY_DESCRIPTOR_REJECTED, 18, 4, 0);
    diagnostic_history_record(HISTORY_PACKET_CHECKSUM_ERROR, 0, 0, 36);
    diagnostic_history_record(HISTORY_UART_DROPPED, 0, 0, 37);
    cdc_capture_clear();
    cdc_send("history\n");
    history_frame(10, 0);
    static const char *expected[] = {
        "boot build=2.98 output=B", "output_local old=A new=B", "output_peer old=B new=A",
        "usb_mount", "usb_unmount", "hid_mount device=17 instance=3 protocol=2 keyboard=1 mouse=1",
        "hid_unmount device=17 instance=3 protocol=1 keyboard=1 mouse=0",
        "descriptor_rejected device=18 instance=4", "packet_checksum_error packet_type=36",
        "uart_dropped packet_type=37",
    };
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        char row[256];
        snprintf(row, sizeof(row), "board=A seq=%u uptime_ms=1000 age_ms=0 event=%s\r\n", i + 1, expected[i]);
        CHECK(strstr(cdc_bytes, row) != NULL);
    }
    CHECK(occurrences("board=A seq=") == 10 && occurrences("board=B seq=") == 0);

    /* Keep full-width formatting even though real boot uptime starts small. */
    console_now_us = UINT64_C(18446744073709551000);
    diagnostic_history_record(HISTORY_UART_DROPPED, 0, 0, UINT32_MAX);
    cdc_capture_clear();
    cdc_send("history 1\n");
    CHECK(strstr(cdc_bytes, "uptime_ms=18446744073709551 age_ms=0 event=uart_dropped packet_type=4294967295") != NULL);
    console_now_us = 2000000;
    diagnostic_history_init();
    diagnostic_history_record(HISTORY_USB_MOUNT, 0, 0, 0);

    /* A terminal attached to B must label every row B as well as the header. */
    console_init(OUTPUT_B, "fedcba9876543210", UINT64_C(0x8877665544332211), UINT32_C(0x12345678));
    cdc_capture_clear();
    cdc_send("history 1\n");
    CHECK(strstr(cdc_bytes, "board=B\r\nboot_session=8877665544332211\r\n") != NULL);
    CHECK(occurrences("board=B seq=") == 1 && occurrences("board=A seq=") == 0);
    CHECK(occurrences("END history") == 1);
    console_init(OUTPUT_A, "0123456789abcdef", UINT64_C(0x1122334455667788), UINT32_C(0x89abcdef));
    console_drain();
}

static void cdc_runtime_history_fields(void) {
    diagnostic_history_init();
    console_now_us = 1000000;
    diagnostic_history_record(HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_PEER, 0, 200);
    diagnostic_history_record(HISTORY_UPDATE_PROGRESS, DIAGNOSTIC_SOURCE_PEER, 50, 131072);
    diagnostic_history_record(HISTORY_UPDATE_PHASE, DIAGNOSTIC_UPDATE_REBOOT_PENDING, DIAGNOSTIC_SOURCE_PEER, 200);
    diagnostic_history_record(HISTORY_PEER_OBSERVED, 1, DIAGNOSTIC_PEER_NEW_BOOT, 200);
    diagnostic_history_record(HISTORY_PEER_PROGRESS, 1, DIAGNOSTIC_PROGRESS_ADVANCING, 200);
    diagnostic_history_record((history_type_t)250, 255, 254, UINT32_MAX);
    diagnostic_history_record(HISTORY_UPDATE_BEGIN, DIAGNOSTIC_SOURCE_USB, 0, 0);
    cdc_capture_clear();
    cdc_send("history\n");
    history_frame(7, 0);
    const char *expected[] = {
        "update_begin source=peer target=0.100", "update_progress source=peer percent=50 received=131072",
        "update_phase phase=reboot_pending source=peer target=0.100",
        "peer_observed peer=B boot=new_boot build=0.100", "peer_progress peer=B progress=advancing build=0.100",
        "unknown type=250 a=255 b=254 value=4294967295", "update_begin source=usb target=unknown",
    };
    for (unsigned i = 0; i < 7; ++i) {
        char row[256];
        snprintf(row, sizeof(row), "board=A seq=%u uptime_ms=1000 age_ms=0 event=%s\r\n", i + 1, expected[i]);
        CHECK(strstr(cdc_bytes, row) != NULL);
    }
    CHECK(occurrences("board=A seq=") == 7 && occurrences("END history") == 1);
}

static void cdc_transfer_history_fields(void) {
    diagnostic_history_init();
    console_now_us = 1000000;
    const uint8_t phases[] = {TRANSFER_CAPS_QUEUED, TRANSFER_BATCH_BEGIN, TRANSFER_RETRY,
        TRANSFER_WORDS_BEGIN, TRANSFER_PROGRESS, TRANSFER_BATCH_END, TRANSFER_WORDS_END};
    const uint8_t modes[] = {TRANSFER_MODE_NONE, TRANSFER_MODE_PAGES, TRANSFER_MODE_PAGES,
        TRANSFER_MODE_MIXED, TRANSFER_MODE_MIXED, TRANSFER_MODE_PAGES, TRANSFER_MODE_WORDS};
    const uint32_t values[] = {0xffffffc0, 0, 256, 256, 65536, 262144, 262144};
    for (unsigned i = 0; i < sizeof(phases); ++i)
        diagnostic_history_record(HISTORY_TRANSFER_SOURCE, phases[i], modes[i], values[i]);
    for (unsigned i = TRANSFER_ELAPSED_US; i <= TRANSFER_PAGE_MAX_US; ++i)
        diagnostic_history_record(HISTORY_TRANSFER_TIMING, i, TRANSFER_MODE_MIXED, UINT32_MAX);
    for (unsigned i = TRANSFER_PAGE_REQUESTS; i <= TRANSFER_PAGE_RETRIES; ++i)
        diagnostic_history_record(HISTORY_TRANSFER_COUNT, i, TRANSFER_MODE_MIXED, UINT32_MAX);
    /* New rows use the existing bounded/partial CDC formatter and ring. */
    cdc_capture_clear();
    cdc_send("history\n");
    history_frame(14, 0);
    const char *expected[] = {
        "transfer_source phase=caps_queued mode=none value=4294967232",
        "transfer_source phase=batch_begin mode=pages value=0",
        "transfer_source phase=retry mode=pages value=256",
        "transfer_source phase=words_begin mode=mixed value=256",
        "transfer_source phase=progress mode=mixed value=65536",
        "transfer_source phase=batch_end mode=pages value=262144",
        "transfer_source phase=words_end mode=words value=262144",
        "transfer_timing metric=elapsed_us mode=mixed value=4294967295",
        "transfer_timing metric=page_service_us mode=mixed value=4294967295",
        "transfer_timing metric=page_gap_us mode=mixed value=4294967295",
        "transfer_timing metric=page_max_us mode=mixed value=4294967295",
        "transfer_count metric=page_requests mode=mixed value=4294967295",
        "transfer_count metric=word_requests mode=mixed value=4294967295",
        "transfer_count metric=page_retries mode=mixed value=4294967295",
    };
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        char row[256];
        snprintf(row, sizeof(row), "board=A seq=%u uptime_ms=1000 age_ms=0 event=%s\r\n", i + 1, expected[i]);
        CHECK(strstr(cdc_bytes, row) != NULL);
    }
    CHECK(occurrences("board=A seq=") == 14 && occurrences("END history") == 1);
}

static void cdc_history_stalled_reader_and_hid(void) {
    history_fill(64);
    cdc_capture_clear();
    complete_short_out(0x06, "history 64\nstatus\n", 18);
    for (unsigned tick = 0; tick < 64; ++tick) console_tick();
    CHECK(endpoint(0x86)->pending && tud_cdc_available() == 7);
    uint8_t keys[6] = {HID_KEY_C};
    CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
    host_count = 0;
    complete(0x81, NULL, 0);
    CHECK(host_count == 9 && host_bytes[3] == HID_KEY_C);
    uint8_t leds = 4;
    unsigned before = led_peer_messages;
    CHECK(control(0x21, HID_REQ_CONTROL_SET_REPORT, (HID_REPORT_TYPE_OUTPUT << 8) | REPORT_ID_KEYBOARD, 0, 1, &leds));
    CHECK(led_peer_messages == before + 1 && last_led == 4);
    CHECK(endpoint(0x86)->pending);

    /* Producers lap every remaining requested event while the host is paused.
     * New events must not extend the frozen command window or silently replace
     * the requested sequence numbers. */
    for (unsigned i = 0; i < 80; ++i) {
        console_now_us = (uint64_t)(65 + i) * 1000;
        diagnostic_history_record(HISTORY_OUTPUT_LOCAL, 0, 1, 0);
    }
    console_drain();
    history_frame(64, 0); /* Counts describe the window at command start. */
    history_rows(1, 65, true);
    CHECK(occurrences("GAP board=A seq=") != 0);
    CHECK(occurrences("board=A seq=") == 64);
    CHECK(strstr(cdc_bytes, "END history") < strstr(cdc_bytes, "BEGIN status"));
    cdc_status_fields();
    CHECK(occurrences("deskhop> ") == 2);

    cdc_capture_clear();
    cdc_send("status\nhistory 1\n");
    cdc_status_fields();
    history_frame(1, 80);
    history_rows(144, 145, false);
    CHECK(strstr(cdc_bytes, "END status") < strstr(cdc_bytes, "BEGIN history"));
    CHECK(occurrences("deskhop> ") == 2);
}

static void cdc_history_close_discards_window(void) {
    history_fill(64);
    cdc_capture_clear();
    complete_short_out(0x06, "history 64\nstatus", 17);
    for (unsigned tick = 0; tick < 64; ++tick) console_tick();
    CHECK(endpoint(0x86)->pending && tud_cdc_available() == 6);
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(!tud_cdc_connected() && tud_cdc_available() == 0);
    /* The controller owns one already submitted IN transfer until completion. */
    complete(0x86, NULL, 0);
    if (endpoint(0x86)->pending) {
        CHECK(endpoint(0x86)->length == 0);
        complete(0x86, NULL, 0);
    }
    console_drain();
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("deskhop> ") == 1);
    CHECK(occurrences(" seq=") == 0 && occurrences("END history") == 0);
    cdc_capture_clear();
    cdc_send("history 1\n");
    history_frame(1, 0);
    history_rows(64, 65, false);
    CHECK(occurrences("BEGIN status") == 0);
}

static void history_peer_fixture(unsigned count) {
    CHECK(!history_peer_borrowed && !history_peer_ready && !history_peer_pending);
    memset(&history_peer_result, 0, sizeof(history_peer_result));
    peer_history_snapshot_t *peer = &history_peer_result.snapshot;
    peer->role = 1;
    peer->boot_session = UINT64_C(0x8877665544332211);
    peer->sampled_at_us = UINT64_C(90000000);
    peer->window = (history_window_t){.first_seq = 51, .end_seq = 51 + count,
                                     .oldest_seq = 31, .overwritten = 30, .count = count};
    for (unsigned i = 0; i < count; ++i)
        peer->events[i] = (history_event_t){.seq = 51 + i,
            .time_us = peer->sampled_at_us - (count - i) * 1000,
            .type = HISTORY_OUTPUT_PEER, .a = 0, .b = 1};
}

static void cdc_history_interleaving(void) {
    history_peer_manual = true;
    diagnostic_history_init();
    const uint64_t local_times[] = {100000, 400000, 600000, 900000};
    for (unsigned i = 0; i < 4; ++i)
        history_store_record(&history_store, local_times[i], HISTORY_OUTPUT_LOCAL, 0, 1, 0);
    console_now_us = 1000000;
    history_peer_fixture(4);
    const unsigned peer_ages[] = {950, 600, 500, 50};
    for (unsigned i = 0; i < 4; ++i)
        history_peer_result.snapshot.events[i].time_us = 90000000 - peer_ages[i] * 1000;
    cdc_capture_clear();
    cdc_send("history 4\n");
    CHECK(history_peer_pending && history_peer_limit == 4);
    CHECK(occurrences("event=") == 0 && occurrences("END history") == 0);
    unsigned releases_before = history_peer_releases;
    history_peer_publish(history_peer_token, PEER_HISTORY_OK);
    console_drain();
    history_frame(4, 0);
    CHECK(strstr(cdc_bytes, "board=B\r\nboot_session=8877665544332211\r\nsampled_uptime_ms=90000\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "peer=ok\r\npeer_capture_bound_us=5000\r\n") != NULL);
    CHECK(history_peer_releases == releases_before + 1 && !history_peer_borrowed);
    const char *expected[] = {
        "board=B seq=51 uptime_ms=89050 age_ms=950 event=",
        "board=A seq=1 uptime_ms=100 age_ms=900 event=",
        "board=A seq=2 uptime_ms=400 age_ms=600 event=",
        "board=B seq=52 uptime_ms=89400 age_ms=600 event=",
        "board=B seq=53 uptime_ms=89500 age_ms=500 event=",
        "board=A seq=3 uptime_ms=600 age_ms=400 event=",
        "board=A seq=4 uptime_ms=900 age_ms=100 event=",
        "board=B seq=54 uptime_ms=89950 age_ms=50 event=",
    };
    const char *previous = cdc_bytes;
    for (unsigned i = 0; i < 8; ++i) {
        const char *row = strstr(cdc_bytes, expected[i]);
        CHECK(row != NULL && row > previous);
        previous = row;
    }
    CHECK(occurrences("board=A seq=") == 4 && occurrences("board=B seq=") == 4);

    /* Equal-age ties choose board A even when the terminal runs on B. */
    console_init(OUTPUT_B, "fedcba9876543210", UINT64_C(0x8877665544332211), UINT32_C(0x12345678));
    console_drain();
    history_peer_fixture(1);
    history_peer_result.snapshot.role = 0;
    history_peer_result.snapshot.events[0].time_us = 89900000;
    cdc_capture_clear();
    cdc_send("history 1\n");
    history_peer_publish(history_peer_token, PEER_HISTORY_OK);
    console_drain();
    CHECK(strstr(cdc_bytes, "board=A seq=51 uptime_ms=89900 age_ms=100 event=") <
          strstr(cdc_bytes, "board=B seq=4 uptime_ms=900 age_ms=100 event="));
    CHECK(occurrences("board=A seq=") == 1 && occurrences("board=B seq=") == 1);
    console_init(OUTPUT_A, "0123456789abcdef", UINT64_C(0x1122334455667788), UINT32_C(0x89abcdef));
    console_drain();

    history_peer_fixture(4);
    history_peer_result.snapshot.gap_mask = UINT64_C(1) << 1;
    cdc_capture_clear();
    cdc_send("history 4\n");
    history_peer_publish(history_peer_token, PEER_HISTORY_OK);
    console_drain();
    CHECK(strstr(cdc_bytes, "GAP board=B seq=52\r\n") != NULL);
    CHECK(occurrences("board=B seq=") == 4 && occurrences("board=A seq=") == 4);
    CHECK(occurrences("event=") == 7 && occurrences("END history") == 1);
    history_peer_manual = false;
}

static void cdc_history_capture_ignores_usb_backpressure(void) {
    history_peer_manual = true;
    history_fill(64);
    history_peer_fixture(64);
    unsigned reads_before = history_reads;
    cdc_capture_clear();
    complete_short_out(0x06, "history 64\nstatus\n", 18);
    console_tick(); /* Consume command; both snapshots are now fixed windows. */
    CHECK(history_peer_pending && history_peer_limit == 64);
    history_peer_publish(history_peer_token, PEER_HISTORY_OK);
    for (unsigned tick = 0; tick < 80; ++tick) console_tick();
    CHECK(history_reads == reads_before + 64);
    CHECK(history_peer_borrowed && endpoint(0x86)->pending);
    CHECK(tud_cdc_available() == 7);
    /* Even after producers overwrite all original slots, captured local rows
     * remain immutable. The peer result stays borrowed through output stalls. */
    for (unsigned i = 0; i < 80; ++i) {
        console_now_us = (uint64_t)(65 + i) * 1000;
        diagnostic_history_record(HISTORY_OUTPUT_LOCAL, 0, 1, 0);
    }
    uint8_t keys[6] = {HID_KEY_D};
    CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
    host_count = 0;
    complete(0x81, NULL, 0);
    CHECK(host_count == 9 && host_bytes[3] == HID_KEY_D);
    console_drain();
    CHECK(!history_peer_borrowed && !history_peer_pending);
    CHECK(occurrences("GAP ") == 0 && occurrences("event=") == 128);
    CHECK(occurrences("board=A seq=") == 64 && occurrences("board=B seq=") == 64);
    CHECK(strstr(cdc_bytes, "board=A seq=1 uptime_ms=1 age_ms=63 event=") != NULL);
    CHECK(strstr(cdc_bytes, "board=A seq=64 uptime_ms=64 age_ms=0 event=") != NULL);
    CHECK(strstr(cdc_bytes, "END history") < strstr(cdc_bytes, "BEGIN status"));
    cdc_status_fields();
    history_peer_manual = false;
}

static void cdc_history_peer_failures_and_cancel(void) {
    history_peer_manual = true;
    history_fill(4);
    const peer_history_outcome_t outcomes[] = {PEER_HISTORY_TIMEOUT, PEER_HISTORY_INVALID};
    const char *texts[] = {"peer=timeout_or_unsupported", "peer=invalid"};
    for (unsigned i = 0; i < 2; ++i) {
        history_peer_fixture(1);
        cdc_capture_clear();
        cdc_send("history 4\n");
        CHECK(occurrences("event=") == 0);
        history_peer_publish(history_peer_token, outcomes[i]);
        console_drain();
        history_frame(4, 0);
        CHECK(strstr(cdc_bytes, texts[i]) != NULL);
        CHECK(occurrences("board=A seq=") == 4 && occurrences("board=B seq=") == 0);
        CHECK(!history_peer_borrowed);
    }
    /* Reject structurally impossible snapshots at the borrow boundary before
     * any peer array index or timestamp subtraction can use them. */
    for (unsigned fault = 0; fault < 5; ++fault) {
        history_peer_fixture(4);
        cdc_capture_clear();
        cdc_send("history 4\n");
        history_peer_publish(history_peer_token, PEER_HISTORY_OK);
        if (fault == 0) history_peer_result.snapshot.role = 0;
        if (fault == 1) history_peer_result.snapshot.role = 2;
        if (fault == 2) history_peer_result.snapshot.window.count = 65;
        if (fault == 3) history_peer_result.snapshot.window.end_seq++;
        if (fault == 4) history_peer_result.first_response_us = history_peer_requested_at - 1;
        console_drain();
        CHECK(strstr(cdc_bytes, "peer=invalid") != NULL);
        CHECK(occurrences("board=A seq=") == 4 && occurrences("board=B seq=") == 0);
        CHECK(!history_peer_borrowed);
    }
    history_peer_accept = false;
    cdc_capture_clear();
    cdc_send("history 4\n");
    CHECK(strstr(cdc_bytes, "peer=busy") != NULL && occurrences("event=") == 4);
    history_peer_accept = true;

    history_peer_fixture(1);
    cdc_capture_clear();
    cdc_send("history 4\n");
    uint32_t abandoned = history_peer_token;
    console_now_us = history_peer_requested_at + 3499999;
    console_pump(32);
    CHECK(occurrences("END history") == 0);
    console_now_us++;
    console_drain();
    CHECK(strstr(cdc_bytes, "peer=timeout_or_unsupported") != NULL && occurrences("event=") == 4);
    unsigned releases_before = history_peer_releases;
    history_peer_publish(abandoned, PEER_HISTORY_OK);
    cdc_capture_clear();
    console_drain();
    CHECK(cdc_count == 0 && history_peer_releases == releases_before + 1);

    history_peer_fixture(1);
    cdc_send("history 4\n");
    CHECK(history_peer_token != abandoned);
    history_peer_publish(abandoned, PEER_HISTORY_OK);
    console_pump(32);
    CHECK(occurrences("END history") == 0 && history_peer_pending && !history_peer_borrowed);
    history_peer_publish(history_peer_token, PEER_HISTORY_OK);
    console_drain();
    CHECK(strstr(cdc_bytes, "peer=ok") != NULL && occurrences("END history") == 1);
    CHECK(!history_peer_borrowed);

    /* Close while the core-0 console owns a borrowed successful peer result. */
    history_fill(64);
    history_peer_fixture(64);
    complete_short_out(0x06, "history 64\n", 11);
    console_tick();
    history_peer_publish(history_peer_token, PEER_HISTORY_OK);
    for (unsigned tick = 0; tick < 80; ++tick) console_tick();
    CHECK(history_peer_borrowed && endpoint(0x86)->pending);
    releases_before = history_peer_releases;
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(!history_peer_borrowed && history_peer_releases == releases_before + 1);
    complete(0x86, NULL, 0);
    if (endpoint(0x86)->pending) complete(0x86, NULL, 0);
    console_drain();
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("event=") == 0 && occurrences("END history") == 0);

    /* Close before a result arrives; the disconnected task still returns late
     * borrowed storage to core 1, so a missing terminal cannot strand it. */
    history_peer_fixture(1);
    cdc_send("history 1\n");
    abandoned = history_peer_token;
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    history_peer_publish(abandoned, PEER_HISTORY_OK);
    releases_before = history_peer_releases;
    console_tick();
    CHECK(!history_peer_borrowed && history_peer_releases == releases_before + 1);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("event=") == 0 && occurrences("END history") == 0);
    history_peer_manual = false;
    cdc_send("history 1\n");
    CHECK(history_peer_token != abandoned && occurrences("END history") == 1);
}

static void cdc_peer_wait_is_local_first(void) {
    peer_manual = true;
    cdc_capture_clear();
    unsigned requests_before = peer_requests;
    complete_short_out(0x06, "status\nhelp\n", 12);
    console_pump(32);
    CHECK(peer_requests == requests_before + 1 && peer_pending);
    CHECK(occurrences("BEGIN status") == 1);
    CHECK(strstr(cdc_bytes, "board=A\r\n") != NULL);
    CHECK(strstr(cdc_bytes, "boot_session=1122334455667788") != NULL);
    CHECK(occurrences("board=B") == 0 && occurrences("END status") == 0);
    CHECK(occurrences("deskhop> ") == 0 && occurrences("BEGIN help") == 0);
    CHECK(tud_cdc_available() == 5); /* One open frame owns the console. */
    size_t before = cdc_count;
    console_pump(32);
    CHECK(cdc_count == before && peer_requests == requests_before + 1);

    /* USB HID progresses while core 1 has not produced a peer reply. */
    uint8_t keys[6] = {HID_KEY_C};
    CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
    host_count = 0;
    complete(0x81, NULL, 0);
    CHECK(host_count == 9 && host_bytes[3] == HID_KEY_C);
    peer_publish(peer_token, PEER_STATUS_OK);
    console_drain();
    cdc_status_fields();
    CHECK(occurrences("BEGIN help") == 1 && occurrences("deskhop> ") == 2);
    CHECK(strstr(cdc_bytes, "board=A\r\n") < strstr(cdc_bytes, "board=B\r\n"));
    CHECK(strstr(cdc_bytes, "END status") < strstr(cdc_bytes, "BEGIN help"));
    peer_manual = false;
}

static void cdc_peer_failures_and_fallback(void) {
    peer_manual = true;
    const peer_status_outcome_t outcomes[] = {PEER_STATUS_TIMEOUT, PEER_STATUS_INVALID};
    const char *texts[] = {"peer=timeout_or_unsupported", "peer=invalid"};
    for (unsigned i = 0; i < 2; ++i) {
        cdc_capture_clear();
        cdc_send("status\n");
        CHECK(peer_pending && occurrences("END status") == 0);
        console_now_us = peer_requested_at + 500000;
        peer_publish(peer_token, outcomes[i]);
        console_drain();
        CHECK(strstr(cdc_bytes, texts[i]) != NULL);
        CHECK(occurrences("board=A\r\n") == 1 && occurrences("board=B") == 0);
        CHECK(occurrences("END status") == 1 && occurrences("deskhop> ") == 1);
    }
    peer_accept = false;
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(strstr(cdc_bytes, "peer=busy") != NULL);
    CHECK(occurrences("END status") == 1 && occurrences("deskhop> ") == 1);
    peer_accept = true;

    /* A paused core 1 cannot strand an open console frame indefinitely. */
    cdc_capture_clear();
    cdc_send("status\n");
    uint32_t abandoned = peer_token;
    console_now_us = peer_requested_at + 599999;
    console_pump(32);
    CHECK(occurrences("END status") == 0);
    console_now_us++;
    console_drain();
    CHECK(strstr(cdc_bytes, "peer=timeout_or_unsupported") != NULL);
    CHECK(occurrences("END status") == 1 && occurrences("deskhop> ") == 1);
    /* The late result is consumed without printing a second completion. */
    peer_publish(abandoned, PEER_STATUS_OK);
    cdc_capture_clear();
    console_drain();
    CHECK(cdc_count == 0 && !peer_pending && !peer_result_ready);
    peer_manual = false;
    cdc_send("status\n");
    cdc_status_fields();
    CHECK(peer_token != abandoned);
}

static void cdc_disconnect_discards_peer_result(void) {
    peer_manual = true;
    cdc_capture_clear();
    cdc_send("status\n");
    uint32_t abandoned = peer_token;
    CHECK(occurrences("BEGIN status") == 1 && occurrences("END status") == 0);
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    peer_publish(abandoned, PEER_STATUS_OK);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("board=B") == 0 && occurrences("END status") == 0);
    CHECK(occurrences("deskhop> ") == 1);
    CHECK(!peer_pending && !peer_result_ready);
    cdc_capture_clear();
    cdc_send("status\n");
    CHECK(peer_token != abandoned);
    /* A stale completion arriving during a new query is also ignored. */
    peer_publish(abandoned, PEER_STATUS_OK);
    console_pump(32);
    CHECK(peer_pending && occurrences("END status") == 0);
    peer_publish(peer_token, PEER_STATUS_OK);
    console_drain();
    cdc_status_fields();
    peer_manual = false;
}

static void cdc_backpressure_keeps_hid_working(void) {
    cdc_capture_clear();
    complete_short_out(0x06, "status\nhelp\n", 12);
    for (unsigned tick = 0; tick < 64; ++tick) console_tick();
    CHECK(endpoint(0x86)->pending);
    CHECK(tud_cdc_available() >= 5); /* Second command waits behind status. */
    uint8_t keys[6] = {HID_KEY_B};
    CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
    host_count = 0;
    complete(0x81, NULL, 0);
    CHECK(host_count == 9 && host_bytes[0] == REPORT_ID_KEYBOARD && host_bytes[3] == HID_KEY_B);
    uint8_t leds = 2;
    unsigned before = led_peer_messages;
    CHECK(control(0x21, HID_REQ_CONTROL_SET_REPORT, (HID_REPORT_TYPE_OUTPUT << 8) | REPORT_ID_KEYBOARD, 0, 1, &leds));
    CHECK(led_peer_messages == before + 1 && last_led == 2);
    CHECK(endpoint(0x86)->pending); /* HID did not complete the CDC transfer. */
    console_drain();
    cdc_status_fields();
    CHECK(occurrences("deskhop> ") == 2);
}

static void cdc_disconnect_discards_partial(void) {
    cdc_send("stat");
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(!tud_cdc_connected());
    console_tick();
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(strstr(cdc_bytes, "deskhop> ") != NULL);
    cdc_capture_clear();
    cdc_send("us\n");
    CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN status") == 0);
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields(); /* Transport reconnect does not change boot identity. */
    cdc_send("sta"); /* Unplug/re-enumeration must discard this too. */
}

static void cdc_close_discards_unsent_response(void) {
    cdc_capture_clear();
    complete_short_out(0x06, "status\nhelp", 11);
    for (unsigned tick = 0; tick < 16; ++tick) console_tick();
    CHECK(endpoint(0x86)->pending && endpoint(0x86)->length != 0);
    CHECK(tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE);
    CHECK(tud_cdc_available() == 4);
    uint8_t submitted[64];
    unsigned submitted_length = endpoint(0x86)->length;
    CHECK(submitted_length <= sizeof(submitted));
    memcpy(submitted, endpoint(0x86)->buffer, submitted_length);
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(!tud_cdc_connected());
    CHECK(tud_cdc_available() == 0 && tud_cdc_write_available() == CFG_TUD_CDC_TX_BUFSIZE);
    /* Bytes already submitted to the controller cannot be recalled by clearing
     * TinyUSB's FIFO. Only that existing transfer (and a possible ZLP) remains. */
    complete(0x86, NULL, 0);
    CHECK(cdc_count == submitted_length && memcmp(cdc_bytes, submitted, submitted_length) == 0);
    if (endpoint(0x86)->pending) {
        CHECK(endpoint(0x86)->length == 0);
        complete(0x86, NULL, 0);
    }
    console_drain();
    CHECK(cdc_count == submitted_length);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(strstr(cdc_bytes, "deskhop> ") != NULL && occurrences("END status") == 0);
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();
}

static void cdc_bus_reset_discards_partial(void) {
    cdc_send("sta");
    /* No console_task runs during the reset, enumeration or DTR change. The
     * real stack need not call unmount for this fast host-driven bus reset. */
    enumerate(false);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    cdc_capture_clear();
    cdc_send("tus\n");
    CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN status") == 0);
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();

    history_fill(64);
    cdc_capture_clear();
    complete_short_out(0x06, "history 64\nstatus", 17);
    for (unsigned tick = 0; tick < 64; ++tick) console_tick();
    CHECK(endpoint(0x86)->pending && tud_cdc_available() == 6);
    enumerate(false);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    cdc_capture_clear();
    console_drain();
    CHECK(occurrences("deskhop> ") == 1 && occurrences(" seq=") == 0);
    CHECK(occurrences("END history") == 0 && occurrences("BEGIN status") == 0);
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();
}

static void bootloader_console_reset(uint8_t role) {
    console_disconnect();
    maintenance_pending = maintenance_ready = false;
    maintenance_start = MAINTENANCE_STARTED;
    maintenance_auto = true;
    maintenance_role = role;
    console_init(role, "0123456789abcdef", UINT64_C(0x1122334455667788) + role,
                 UINT32_C(0x89abcdef));
    console_now_us += UINT64_C(5000000);
    console_drain();
    cdc_capture_clear();
}

static void cdc_bootloader_parser(void) {
    bootloader_console_reset(OUTPUT_A);
    unsigned before = maintenance_requests;
    const char *invalid[] = {
        "bootloader\n", "bootloader \n", "bootloader a\n", "bootloader b\n",
        "bootloader C\n", "bootloader both\n", "bootloader A B\n",
        "bootloader A \n", "bootloader  A\n", " bootloader A\n",
        "bootloader\tA\n", "bootloader A\t\n", "bootloader A extra\n",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cdc_capture_clear();
        cdc_send(invalid[i]);
        CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN bootloader") == 0);
    }
    char oversized[96];
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "bootloader A", 12);
    oversized[sizeof(oversized) - 1] = '\n';
    cdc_capture_clear();
    cdc_send_bytes(oversized, sizeof(oversized));
    CHECK(occurrences("ERROR line too long") == 1);
    const char nul[] = "bootloader A\0ignored\n";
    cdc_capture_clear();
    cdc_send_bytes(nul, sizeof(nul) - 1);
    CHECK(occurrences("ERROR command must contain printable ASCII") == 1);
    CHECK(maintenance_requests == before);
    cdc_capture_clear();
    cdc_send("help\n");
    CHECK(occurrences("END help") == 1 && occurrences("deskhop> ") == 1);
}

static void bootloader_send_held(uint8_t target) {
    const char *command = target == OUTPUT_A ? "bootloader A\n" : "bootloader B\n";
    complete_short_out(0x06, command, (unsigned)strlen(command));
    for (unsigned tick = 0; tick < 32; ++tick)
        console_tick();
    CHECK(endpoint(0x86)->pending);
}

static void cdc_bootloader_completion(void) {
    for (uint8_t role = OUTPUT_A; role <= OUTPUT_B; ++role) {
        bootloader_console_reset(role);
        global_state.active_output = role ^ 1; /* Focus is not a target selector. */
        unsigned before = maintenance_completions;
        bootloader_send_held(role);
        CHECK(maintenance_target == role && maintenance_completions == before);
        uint8_t keys[6] = {HID_KEY_J};
        CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
        host_count = 0;
        complete(0x81, NULL, 0);
        CHECK(host_count == 9 && host_bytes[3] == HID_KEY_J);
        CHECK(endpoint(0x86)->pending && maintenance_completions == before);
        /* The real CDC endpoint, not merely the software FIFO, holds the reply.
         * Drive everything except its deliberate final one-byte newline. */
        unsigned tick;
        bool saw_zlp = false;
        for (tick = 0; tick < 256; ++tick) {
            console_tick();
            CHECK(maintenance_completions == before);
            if (!endpoint(0x86)->pending)
                continue;
            if (endpoint(0x86)->length == 1 && endpoint(0x86)->buffer[0] == '\n')
                break;
            if (endpoint(0x86)->length == 0) {
                saw_zlp = true;
                /* Queue the fence behind an in-flight ZLP: its completion
                 * must not be mistaken for completion of the queued newline. */
                console_tick();
                CHECK(tud_cdc_write_available() == CFG_TUD_CDC_TX_BUFSIZE - 1);
                CHECK(maintenance_completions == before);
            }
            complete(0x86, NULL, 0);
        }
        CHECK(tick < 256);
        CHECK(saw_zlp); /* The 128-byte body exercises TinyUSB's real ZLP path. */
        CHECK(tud_cdc_write_available() == CFG_TUD_CDC_TX_BUFSIZE);
        CHECK(strstr(cdc_bytes, "END bootloader\r") != NULL);
        CHECK(strstr(cdc_bytes, "END bootloader\r\n") == NULL);
        for (unsigned i = 0; i < 32; ++i) console_tick();
        CHECK(maintenance_completions == before);
        complete(0x86, NULL, 0);
        CHECK(maintenance_completions == before); /* Callback cannot enter ROM. */
        console_tick();
        CHECK(maintenance_completions == before + 1);
        CHECK(!endpoint(0x86)->pending);
        CHECK(strstr(cdc_bytes, "result=accepted scope=local\r\n") != NULL);
        for (unsigned i = 0; i < 3; ++i) console_tick();
        CHECK(maintenance_completions == before + 1);
    }
    global_state.active_output = OUTPUT_A;
    bootloader_console_reset(OUTPUT_A);
}

static void cdc_bootloader_remote_and_rejections(void) {
    for (uint8_t role = OUTPUT_A; role <= OUTPUT_B; ++role) {
        bootloader_console_reset(role);
        uint8_t target = role ^ 1;
        unsigned before = maintenance_completions;
        cdc_send(target == OUTPUT_A ? "bootloader A\n" : "bootloader B\n");
        CHECK(maintenance_target == target && maintenance_completions == before);
        CHECK(strstr(cdc_bytes, "result=accepted reason=peer_admitted_not_boot_proof") != NULL);
        CHECK(occurrences("END bootloader") == 1 && occurrences("deskhop> ") == 1);
    }
    bootloader_console_reset(OUTPUT_A);
    const maintenance_start_t starts[] = {MAINTENANCE_BUSY, MAINTENANCE_UPDATE_ACTIVE,
                                         MAINTENANCE_REBOOT_PENDING, MAINTENANCE_BAD_ARGUMENT};
    const char *reasons[] = {"busy", "update_active", "reboot_pending", "bad_argument"};
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        maintenance_start = starts[i];
        cdc_capture_clear();
        cdc_send("bootloader A\n");
        char expected[80];
        snprintf(expected, sizeof(expected), "result=rejected reason=%s\r\n", reasons[i]);
        CHECK(strstr(cdc_bytes, expected) != NULL);
        CHECK(occurrences("END bootloader") == 1 && occurrences("deskhop> ") == 1);
    }
    maintenance_start = MAINTENANCE_STARTED;
    maintenance_auto = false;
    const maintenance_outcome_t outcomes[] = {MAINTENANCE_REMOTE_BUSY, MAINTENANCE_REMOTE_UPDATE_ACTIVE,
        MAINTENANCE_REMOTE_REBOOT_PENDING, MAINTENANCE_TIMEOUT_NOT_SENT,
        MAINTENANCE_TIMEOUT_UNCONFIRMED, MAINTENANCE_ABORTED};
    const char *rows[] = {"result=rejected reason=peer_busy", "result=rejected reason=peer_update_active",
        "result=rejected reason=peer_reboot_pending", "result=not_sent reason=queue_timeout",
        "result=unconfirmed reason=peer_timeout_or_unsupported", "result=cancelled reason=maintenance_aborted"};
    for (unsigned i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i) {
        cdc_capture_clear();
        cdc_send("bootloader B\n");
        CHECK(occurrences("BEGIN bootloader") == 0);
        maintenance_publish(maintenance_token, OUTPUT_B, outcomes[i]);
        console_drain();
        CHECK(strstr(cdc_bytes, rows[i]) != NULL);
        CHECK(occurrences("END bootloader") == 1 && occurrences("deskhop> ") == 1);
    }
    /* A stale token or wrong physical target cannot authorize the current command. */
    cdc_capture_clear();
    cdc_send("bootloader B\n");
    maintenance_publish(maintenance_token + 1, OUTPUT_B, MAINTENANCE_REMOTE_ACCEPTED);
    console_drain();
    maintenance_publish(maintenance_token, OUTPUT_A, MAINTENANCE_LOCAL_READY);
    console_drain();
    CHECK(occurrences("BEGIN bootloader") == 0);
    console_now_us += UINT64_C(4000000);
    console_drain();
    CHECK(strstr(cdc_bytes, "result=unconfirmed reason=console_timeout") != NULL);
    bootloader_console_reset(OUTPUT_A);
}

static void cdc_bootloader_cancel(void) {
    bootloader_console_reset(OUTPUT_A);
    unsigned completions = maintenance_completions;
    unsigned cancels = maintenance_cancels;
    bootloader_send_held(OUTPUT_A);
    console_now_us += UINT64_C(2000000);
    console_tick();
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    console_drain();
    CHECK(maintenance_completions == completions);
    CHECK(strstr(cdc_bytes, "result=cancelled reason=usb_reply_timeout") != NULL);

    bootloader_console_reset(OUTPUT_A);
    cancels = maintenance_cancels;
    bootloader_send_held(OUTPUT_A);
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    console_drain(); /* Late actual IN completion cannot resurrect authority. */
    CHECK(maintenance_completions == completions);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();

    bootloader_console_reset(OUTPUT_A);
    cancels = maintenance_cancels;
    bootloader_send_held(OUTPUT_A);
    enumerate(false); /* Bus reset without an intervening console task. */
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    CHECK(maintenance_completions == completions);
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();
}

static void cdc_config_parser(void) {
    bootloader_console_reset(OUTPUT_A);
    unsigned before = maintenance_requests;
    unsigned config_before = maintenance_config_requests;
    const char *invalid[] = {
        "config A\n", "config B\n", "config both\n", "config on\n",
        "config off\n", "config \n", "config  \n", "config extra\n",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        cdc_capture_clear();
        cdc_send(invalid[i]);
        CHECK(occurrences("ERROR usage: config (connected board; no arguments)") == 1);
        CHECK(occurrences("BEGIN config") == 0 && occurrences("deskhop> ") == 1);
    }
    const char *other_invalid[] = {" config\n", "CONFIG\n", "configuration\n", "config\t\n"};
    for (unsigned i = 0; i < sizeof(other_invalid) / sizeof(other_invalid[0]); ++i) {
        cdc_capture_clear();
        cdc_send(other_invalid[i]);
        CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN config") == 0);
    }
    char oversized[96];
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "config", 6);
    oversized[sizeof(oversized) - 1] = '\n';
    cdc_capture_clear();
    cdc_send_bytes(oversized, sizeof(oversized));
    CHECK(occurrences("ERROR line too long") == 1);
    const char nul[] = "config\0ignored\n";
    cdc_capture_clear();
    cdc_send_bytes(nul, sizeof(nul) - 1);
    CHECK(occurrences("ERROR command must contain printable ASCII") == 1);
    CHECK(maintenance_requests == before && maintenance_config_requests == config_before);
    cdc_capture_clear();
    cdc_send("help\n");
    CHECK(occurrences("END help") == 1 && occurrences("deskhop> ") == 1);
    CHECK(strstr(cdc_bytes, "config") != NULL && strstr(cdc_bytes, "bootloader A|B") != NULL);
    CHECK(cdc_count < 1280); /* Help remains complete in the existing fixed TX buffer. */
}

static void config_send_held(void) {
    /* A queued command must not run while the mode-changing reply is pending. */
    const char *commands = "config\nhelp\n";
    complete_short_out(0x06, commands, (unsigned)strlen(commands));
    for (unsigned tick = 0; tick < 32; ++tick)
        console_tick();
    CHECK(endpoint(0x86)->pending);
    CHECK(occurrences("BEGIN help") == 0);
}

static void config_drive_to_final_newline(unsigned completions) {
    unsigned tick;
    for (tick = 0; tick < 256; ++tick) {
        console_tick();
        CHECK(maintenance_completions == completions);
        if (!endpoint(0x86)->pending)
            continue;
        if (endpoint(0x86)->length == 1 && endpoint(0x86)->buffer[0] == '\n')
            break;
        complete(0x86, NULL, 0);
    }
    CHECK(tick < 256);
    CHECK(tud_cdc_write_available() == CFG_TUD_CDC_TX_BUFSIZE);
    CHECK(strstr(cdc_bytes, "END config\r") != NULL);
    CHECK(strstr(cdc_bytes, "END config\r\n") == NULL);
    CHECK(occurrences("BEGIN help") == 0);
}

static void cdc_config_completion(void) {
    for (uint8_t role = OUTPUT_A; role <= OUTPUT_B; ++role) {
        bootloader_console_reset(role);
        global_state.active_output = role ^ 1; /* Focus never selects another Pico. */
        unsigned before = maintenance_completions;
        unsigned config_before = maintenance_config_requests;
        unsigned total_before = maintenance_requests;
        config_send_held();
        CHECK(maintenance_config_requests == config_before + 1);
        CHECK(maintenance_requests == total_before + 1 && maintenance_config);
        CHECK(maintenance_target == role && maintenance_completions == before);
        uint8_t keys[6] = {HID_KEY_C};
        CHECK(tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keys));
        host_count = 0;
        complete(0x81, NULL, 0);
        CHECK(host_count == 9 && host_bytes[3] == HID_KEY_C);
        config_drive_to_final_newline(before);
        char row[80];
        snprintf(row, sizeof(row), "board=%c result=accepted scope=local\r\n", role == OUTPUT_A ? 'A' : 'B');
        CHECK(strstr(cdc_bytes, row) != NULL);
        CHECK(strstr(cdc_bytes, "action=enter_configuration_mode_after_reply\r\n") != NULL);
        for (unsigned tick = 0; tick < 32; ++tick) console_tick();
        CHECK(maintenance_completions == before && occurrences("BEGIN help") == 0);
        complete(0x86, NULL, 0);
        CHECK(maintenance_completions == before); /* USB callback cannot change modes. */
        console_tick();
        CHECK(maintenance_completions == before + 1);
        CHECK(strstr(cdc_bytes, "END config\r\n") != NULL);
        for (unsigned tick = 0; tick < 32; ++tick) console_tick();
        CHECK(maintenance_completions == before + 1);
        CHECK(occurrences("BEGIN help") == 0 && occurrences("deskhop> ") == 0);
    }
    global_state.active_output = OUTPUT_A;
    bootloader_console_reset(OUTPUT_A);
}

static void cdc_config_rejections_and_stale_results(void) {
    bootloader_console_reset(OUTPUT_A);
    unsigned completions = maintenance_completions;
    const maintenance_start_t starts[] = {MAINTENANCE_BUSY, MAINTENANCE_UPDATE_ACTIVE,
                                         MAINTENANCE_REBOOT_PENDING, MAINTENANCE_BAD_ARGUMENT};
    const char *reasons[] = {"busy", "update_active", "reboot_pending", "bad_argument"};
    for (unsigned i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        maintenance_start = starts[i];
        cdc_capture_clear();
        cdc_send("config\n");
        char row[96];
        snprintf(row, sizeof(row), "board=A result=rejected reason=%s\r\n", reasons[i]);
        CHECK(strstr(cdc_bytes, row) != NULL);
        CHECK(occurrences("END config") == 1 && occurrences("deskhop> ") == 1);
        CHECK(!maintenance_pending && maintenance_completions == completions);
    }
    maintenance_start = MAINTENANCE_STARTED;
    maintenance_auto = false;
    cdc_capture_clear();
    cdc_send("config\n");
    maintenance_publish(maintenance_token + 1, OUTPUT_A, MAINTENANCE_LOCAL_READY);
    console_drain();
    maintenance_publish(maintenance_token, OUTPUT_B, MAINTENANCE_LOCAL_READY);
    console_drain();
    CHECK(occurrences("BEGIN config") == 0 && maintenance_completions == completions);
    unsigned cancels = maintenance_cancels;
    console_now_us += UINT64_C(4000000);
    console_drain();
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    CHECK(strstr(cdc_bytes, "result=unconfirmed reason=console_timeout") != NULL);
    CHECK(occurrences("END config") == 1 && occurrences("deskhop> ") == 1);
    CHECK(maintenance_completions == completions);
    bootloader_console_reset(OUTPUT_A);
}

static void cdc_config_cancel(void) {
    bootloader_console_reset(OUTPUT_A);
    unsigned completions = maintenance_completions;
    unsigned cancels = maintenance_cancels;
    config_send_held();
    console_now_us += UINT64_C(2000000);
    console_tick();
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    console_drain();
    CHECK(maintenance_completions == completions);
    CHECK(strstr(cdc_bytes, "result=cancelled reason=usb_reply_timeout") != NULL);

    bootloader_console_reset(OUTPUT_A);
    cancels = maintenance_cancels;
    config_send_held();
    config_drive_to_final_newline(completions);
    CHECK(control(0x21, 0x22, 0, 2, 0, NULL));
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    console_drain(); /* Even a late final newline cannot resurrect the cancelled mode change. */
    CHECK(maintenance_completions == completions);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();

    bootloader_console_reset(OUTPUT_A);
    cancels = maintenance_cancels;
    config_send_held();
    enumerate(false);
    CHECK(maintenance_cancels == cancels + 1 && !maintenance_pending);
    CHECK(control(0x21, 0x22, 1, 2, 0, NULL));
    console_drain();
    CHECK(maintenance_completions == completions);
    cdc_capture_clear();
    cdc_send("status\n");
    cdc_status_fields();
}

static void cdc_config_already_active(void) {
    CHECK(global_state.config_mode_active);
    bootloader_console_reset(OUTPUT_A);
    maintenance_start = MAINTENANCE_ALREADY_CONFIG;
    unsigned completions = maintenance_completions;
    unsigned cancels = maintenance_cancels;
    unsigned requests = maintenance_config_requests;
    cdc_send("config\r\nconfig\nhelp\n");
    CHECK(occurrences("BEGIN config") == 2 && occurrences("END config") == 2);
    CHECK(occurrences("result=already_active") == 2);
    CHECK(occurrences("action=enter_configuration_mode_after_reply") == 0);
    CHECK(occurrences("BEGIN help") == 1 && occurrences("END help") == 1);
    CHECK(occurrences("deskhop> ") == 3);
    CHECK(maintenance_config_requests == requests + 2);
    CHECK(maintenance_completions == completions && maintenance_cancels == cancels);
    CHECK(!maintenance_pending);
    maintenance_start = MAINTENANCE_STARTED;
}

static void msc_bulk_command(uint8_t opcode, uint32_t bytes, unsigned expected_payload) {
    msc_cbw_t cbw = {.signature = MSC_CBW_SIGNATURE, .tag = 0x10203040,
                     .total_bytes = bytes, .dir = 0x80, .cmd_len = 10};
    cbw.command[0] = opcode;
    if (opcode == SCSI_CMD_INQUIRY) cbw.command[4] = 36;
    if (opcode == SCSI_CMD_READ_10) { cbw.command[5] = 7; cbw.command[8] = 2; }
    CHECK(endpoint(0x04)->pending && endpoint(0x04)->length == sizeof(cbw));
    host_count = 0;
    complete(0x04, (uint8_t *)&cbw, sizeof(cbw));
    for (unsigned packets = 0; endpoint(0x84)->pending && packets < 10; ++packets)
        complete(0x84, NULL, 0);
    CHECK(host_count == expected_payload + sizeof(msc_csw_t));
    msc_csw_t csw;
    memcpy(&csw, host_bytes + expected_payload, sizeof(csw));
    CHECK(csw.signature == MSC_CSW_SIGNATURE && csw.tag == cbw.tag);
    CHECK(csw.status == MSC_CSW_STATUS_PASSED && csw.data_residue == 0);
    CHECK(endpoint(0x04)->pending && endpoint(0x04)->length == sizeof(cbw));
}
static void msc_bulk(void) {
    msc_bulk_command(SCSI_CMD_INQUIRY, 36, 36);
    CHECK(memcmp(host_bytes + 8, "DeskHop ", 8) == 0);
    msc_bulk_command(SCSI_CMD_READ_CAPACITY_10, 8, 8);
    CHECK(host_bytes[2] == 15 && host_bytes[3] == 255); /* Last LBA = 4095, big endian. */
    CHECK(host_bytes[6] == 2 && host_bytes[7] == 0); /* Block size = 512. */
    msc_bulk_command(SCSI_CMD_READ_10, 1024, 1024);
    CHECK(msc_reads == 2 && last_msc_lba == 8);
    for (unsigned i = 0; i < 1024; ++i) CHECK(host_bytes[i] == 0x5a);
    msc_cbw_t bad = {.signature = 0};
    complete(0x04, (uint8_t *)&bad, sizeof(bad));
    CHECK(endpoint(0x04)->stalled && endpoint(0x84)->stalled);
}
int main(void) {
    local_runtime = default_runtime;
    diagnostic_history_init();
    console_init(OUTPUT_A, "0123456789abcdef", UINT64_C(0x1122334455667788), UINT32_C(0x89abcdef));
    console_now_us = 1000000;
    CHECK(tusb_init());
    scenario = "normal enumeration";
    enumerate(false);
    scenario = "HID controls and reports";
    hid_data_and_leds();
    scenario = "CDC controls";
    cdc_controls(false);
    scenario = "CDC command streams";
    cdc_command_stream();
    scenario = "CDC runtime rows, legacy peer and bounded response chunks";
    cdc_runtime_rows_and_legacy();
    scenario = "CDC verification grammar and policy results";
    cdc_verify_parser_and_results();
    scenario = "CDC verification freshness, backpressure and reconnect";
    cdc_verify_backpressure_freshness_and_reconnect();
    scenario = "CDC verification bounded BUSY deferrals";
    cdc_verify_busy_deferrals();
    scenario = "CDC history counts and ring wrap";
    cdc_history_count_and_wrap();
    scenario = "CDC history event fields";
    cdc_history_event_fields();
    scenario = "CDC runtime and unknown history event fields";
    cdc_runtime_history_fields();
    cdc_transfer_history_fields();
    scenario = "CDC history stalled reader and HID progress";
    cdc_history_stalled_reader_and_hid();
    scenario = "CDC history close discards frozen window";
    cdc_history_close_discards_window();
    scenario = "CDC both-board history age merge and gaps";
    cdc_history_interleaving();
    scenario = "CDC history capture during USB backpressure";
    cdc_history_capture_ignores_usb_backpressure();
    scenario = "CDC peer history failure, fallback and cancellation";
    cdc_history_peer_failures_and_cancel();
    scenario = "CDC asynchronous local-first peer status";
    cdc_peer_wait_is_local_first();
    scenario = "CDC peer failure and core-1 fallback";
    cdc_peer_failures_and_fallback();
    scenario = "CDC reconnect and stale peer results";
    cdc_disconnect_discards_peer_result();
    scenario = "CDC backpressure and HID progress";
    cdc_backpressure_keeps_hid_working();
    scenario = "CDC close with pending IN and FIFO";
    cdc_close_discards_unsent_response();
    scenario = "CDC fast bus reset";
    cdc_bus_reset_discards_partial();
    scenario = "CDC bootloader strict grammar and truthful help";
    cdc_bootloader_parser();
    scenario = "CDC bootloader physical targets and actual final IN completion";
    cdc_bootloader_completion();
    scenario = "CDC bootloader peer admission, rejection and unknown outcomes";
    cdc_bootloader_remote_and_rejections();
    scenario = "CDC bootloader timeout, DTR drop and reset cancellation";
    cdc_bootloader_cancel();
    scenario = "CDC config local-only strict grammar and complete help";
    cdc_config_parser();
    scenario = "CDC config both local roles, final IN completion and pipeline fence";
    cdc_config_completion();
    scenario = "CDC config admission rejections, stale outcomes and timeout";
    cdc_config_rejections_and_stale_results();
    scenario = "CDC config USB stall, DTR drop and reset cancellation";
    cdc_config_cancel();
    scenario = "CDC close and reopen";
    cdc_disconnect_discards_partial();
    scenario = "suspend and unplug";
    suspend_and_unplug();
    scenario = "config enumeration";
    enumerate(true);
    scenario = "config CDC controls and commands";
    cdc_controls(true);
    cdc_capture_clear();
    cdc_send("tus\n");
    CHECK(occurrences("ERROR") == 1 && occurrences("BEGIN status") == 0);
    cdc_capture_clear();
    cdc_send("help\n");
    CHECK(strstr(cdc_bytes, "status") != NULL);
    scenario = "CDC config already-active requests are idempotent";
    cdc_config_already_active();
    scenario = "MSC controls and transfers";
    CHECK(control(0xa1, MSC_REQ_GET_MAX_LUN, 0, ITF_NUM_MSC, 1, NULL));
    CHECK(host_count == 1 && host_bytes[0] == 0);
    CHECK(control(0x21, MSC_REQ_RESET, 0, ITF_NUM_MSC, 0, NULL));
    msc_bulk();
    scenario = "configuration zero";
    no_ep0_out_payload = true;
    CHECK(control(0, TUSB_REQ_SET_CONFIGURATION, 0, 0, 0, NULL));
    no_ep0_out_payload = false;
    CHECK(!tud_mounted() && !endpoint(0x81)->opened);
    puts("TinyUSB virtual-DCD tests passed (normal/config CDC enumeration and real console streams, bootloader target/grammar/actual-IN-completion/cancellation, local config grammar/idempotence/completion/cancellation, per-tick budgets, HID progress under CDC backpressure, control stages, HID LED, MSC SCSI, reconnect)");
    return 0;
}

/* Queue invalidation is covered by the paired production keyboard layer. */
void keyboard_host_reset(device_t *state) { (void)state; }
