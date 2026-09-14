/* A virtual USB device controller driving the real checked-in TinyUSB stack. */
#include "main.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "USB stack failure %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct { bool pending, stalled, opened; uint8_t *buffer; uint16_t length, max_packet; } endpoint_t;
static endpoint_t endpoints[2][16];
static uint8_t address;
static unsigned led_restores, led_peer_messages, remote_wakeups, suspend_callbacks, resume_callbacks;
static uint8_t last_led;
static unsigned msc_reads;
static uint32_t last_msc_lba;
static uint8_t host_bytes[2048];
static size_t host_count;
device_t global_state;
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
    endpoint_t *ep = endpoint(ep_addr);
    CHECK(!ep->pending);
    CHECK(!ep->stalled);
    ep->pending = true; ep->buffer = buffer; ep->length = length;
    return true;
}
void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) { endpoint_t *ep = endpoint(ep_addr); ep->stalled = true; ep->pending = false; }
void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) { endpoint(ep_addr)->stalled = false; }
void pico_get_unique_board_id_string(char *buffer, unsigned length) { snprintf(buffer, length, "0123456789abcdef"); }
void restore_leds(device_t *state) { ++led_restores; }
void send_value(uint8_t value, enum packet_type_e type) { CHECK(type == KBD_SET_REPORT_MSG); last_led = value; ++led_peer_messages; }
bool validate_packet(uart_packet_t *packet) { return false; }
void process_packet(uart_packet_t *packet, device_t *state) { CHECK(false); }
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

static void pump(void) { tud_task_ext(0, false); }
static void complete(uint8_t ep_addr, const uint8_t *out, unsigned out_length) {
    endpoint_t *ep = endpoint(ep_addr);
    CHECK(ep->pending);
    unsigned length = ep->length;
    if (ep_addr & 0x80) {
        CHECK(host_count + length <= sizeof(host_bytes));
        if (length) memcpy(host_bytes + host_count, ep->buffer, length);
        host_count += length;
    } else {
        CHECK(length <= out_length);
        if (length) memcpy(ep->buffer, out, length);
    }
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
    CHECK(host_count == 9 && host_bytes[4] == (config_mode ? 4 : 2));
    CHECK(control(0x80, TUSB_REQ_GET_DESCRIPTOR, TUSB_DESC_CONFIGURATION << 8, 0, total, NULL));
    CHECK(host_count == total && host_bytes[1] == TUSB_DESC_CONFIGURATION);
    /* Independent descriptor walking validates interface/endpoint structure. */
    unsigned interfaces = 0, ep_count = 0;
    for (unsigned at = 0; at < total;) {
        CHECK(host_bytes[at] >= 2 && at + host_bytes[at] <= total);
        if (host_bytes[at + 1] == TUSB_DESC_INTERFACE) ++interfaces;
        if (host_bytes[at + 1] == TUSB_DESC_ENDPOINT) ++ep_count;
        at += host_bytes[at];
    }
    CHECK(interfaces == (config_mode ? 4u : 2u));
    CHECK(ep_count == (config_mode ? 5u : 2u));
    CHECK(control(0, TUSB_REQ_SET_CONFIGURATION, 1, 0, 0, NULL));
    CHECK(tud_mounted() && global_state.tud_connected);
    CHECK(endpoint(0x81)->opened && endpoint(0x82)->opened);
    CHECK(endpoint(0x81)->max_packet == 32);
    CHECK(endpoint(0x83)->opened == config_mode);
    CHECK(endpoint(0x84)->opened == config_mode && endpoint(0x04)->opened == config_mode);
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
    dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, true); pump();
    CHECK(!tud_mounted() && !global_state.tud_connected);
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
    CHECK(tusb_init());
    enumerate(false);
    hid_data_and_leds();
    suspend_and_unplug();
    enumerate(true);
    CHECK(control(0xa1, MSC_REQ_GET_MAX_LUN, 0, ITF_NUM_MSC, 1, NULL));
    CHECK(host_count == 1 && host_bytes[0] == 0);
    CHECK(control(0x21, MSC_REQ_RESET, 0, ITF_NUM_MSC, 0, NULL));
    msc_bulk();
    CHECK(control(0, TUSB_REQ_SET_CONFIGURATION, 0, 0, 0, NULL));
    CHECK(!tud_mounted() && !endpoint(0x81)->opened);
    puts("TinyUSB virtual-DCD tests passed (normal/config enumeration, real control stages, HID IN/OUT LED, stalls, MSC class and bulk SCSI, suspend/resume, unplug)");
    return 0;
}
