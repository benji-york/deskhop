/* A virtual USB device controller driving the real checked-in TinyUSB stack. */
#include "main.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
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
static char cdc_bytes[16384];
static size_t cdc_count;
static uint64_t cdc_submitted_bytes, console_now_us;
static bool no_ep0_out_payload;
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
void tud_cdc_rx_cb(uint8_t itf) {}
void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted) {}
void tud_cdc_tx_complete_cb(uint8_t itf) {}
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
    console_task(console_now_us);
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

static void console_drain(void) {
    for (unsigned tick = 0; tick < 128; ++tick) {
        console_tick();
        if (endpoint(0x86)->pending) complete(0x86, NULL, 0);
    }
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
    CHECK(strstr(cdc_bytes, "build=0.95") != NULL);
    CHECK(strstr(cdc_bytes, "image_crc_at_boot=89abcdef") != NULL);
    CHECK(strstr(cdc_bytes, "boot_session=1122334455667788") != NULL);
    CHECK(strstr(cdc_bytes, "peer=not_implemented") != NULL);
    CHECK(strstr(cdc_bytes, "verification=not_implemented") != NULL);
    CHECK(strstr(cdc_bytes, "deskhop> ") != NULL);
}

static void cdc_command_stream(void) {
    cdc_capture_clear();
    cdc_send("he");
    CHECK(strstr(cdc_bytes, "status") == NULL);
    cdc_send("lp\r\n");
    CHECK(occurrences("deskhop> ") == 1);
    CHECK(strstr(cdc_bytes, "help") && strstr(cdc_bytes, "status"));
    CHECK(strstr(cdc_bytes, "local") != NULL);

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
    scenario = "CDC backpressure and HID progress";
    cdc_backpressure_keeps_hid_working();
    scenario = "CDC close with pending IN and FIFO";
    cdc_close_discards_unsent_response();
    scenario = "CDC fast bus reset";
    cdc_bus_reset_discards_partial();
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
    puts("TinyUSB virtual-DCD tests passed (normal/config CDC enumeration and real console streams, per-tick budgets, HID progress under CDC backpressure, control stages, HID LED, MSC SCSI, reconnect)");
    return 0;
}
