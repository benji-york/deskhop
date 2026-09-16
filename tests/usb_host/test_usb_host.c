#include "main.h"
#include "host/hcd.h"
#include "host/usbh_pvt.h"
#include "fixtures.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "USB host failure %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

device_t global_state;
/* Host-class tests are single-threaded; real config exclusion has its own
 * production storage-boundary schedule tests. */
void config_snapshot(const device_t *state, config_t *snapshot) { *snapshot = state->config; }
static uint32_t virtual_ms;
static bool connected, trackball_only, multi_keyboard;
static unsigned controls, mounts, unmounts, resets, mouse_reports, keyboard_reports, modifier_publishes, activity_records, idle_stalls;
static uint8_t configured_address, peripheral_led;
static hid_keyboard_report_t last_keyboard;
static tusb_control_request_t setup;
static uint8_t setup_address;
static uint8_t configuration[59];
static unsigned configuration_size;
static const uint8_t multi_keyboard_descriptor[] = {
    0x05,1,0x09,6,0xa1,1,0x85,7,
    0x05,7,0x19,0xe0,0x29,0xe7,0x75,1,0x95,8,0x81,2,
    0x75,8,0x95,1,0x81,1,0x19,0,0x29,0x65,0x95,6,0x81,0,0xc0,
    0x05,1,0x09,6,0xa1,1,0x85,17,
    0x05,7,0x19,0xe0,0x29,0xe7,0x75,1,0x95,8,0x81,2,
    0x19,4,0x29,123,0x75,1,0x95,120,0x81,2,0xc0
};
static struct { bool pending; uint8_t address; uint8_t *buffer; unsigned length; } incoming[16];
static const tusb_desc_device_t device_descriptor = {
    .bLength = 18, .bDescriptorType = TUSB_DESC_DEVICE, .bcdUSB = 0x0200,
    .bMaxPacketSize0 = 64, .idVendor = 0xfeed, .idProduct = 0x1234,
    .bcdDevice = 0x0100, .bNumConfigurations = 1,
};

bool hcd_init(uint8_t port) { CHECK(port == 0); return true; }
void hcd_int_handler(uint8_t port, bool in_isr) {}
void hcd_int_enable(uint8_t port) {}
void hcd_int_disable(uint8_t port) {}
uint32_t hcd_frame_number(uint8_t port) { return virtual_ms; }
void osal_task_delay(uint32_t ms) { virtual_ms += ms; }
bool hcd_port_connect_status(uint8_t port) { return connected; }
void hcd_port_reset(uint8_t port) { ++resets; }
void hcd_port_reset_end(uint8_t port) {}
tusb_speed_t hcd_port_speed_get(uint8_t port) { return TUSB_SPEED_FULL; }
void hcd_device_close(uint8_t port, uint8_t address) {
    for (unsigned ep = 0; ep < 16; ++ep)
        if (incoming[ep].address == address) incoming[ep].pending = false;
}
bool hcd_edpt_open(uint8_t port, uint8_t address, const tusb_desc_endpoint_t *ep) { return true; }
bool hcd_edpt_abort_xfer(uint8_t port, uint8_t address, uint8_t ep) {
    bool pending = incoming[ep & 15].pending; incoming[ep & 15].pending = false; return pending;
}
bool hcd_edpt_clear_stall(uint8_t port, uint8_t address, uint8_t ep) { return true; }
bool hcd_setup_send(uint8_t port, uint8_t address, const uint8_t bytes[8]) {
    memcpy(&setup, bytes, 8); setup_address = address; ++controls;
    if (setup.bRequest == TUSB_REQ_SET_ADDRESS && setup.bmRequestType == 0) configured_address = (uint8_t)setup.wValue;
    hcd_event_xfer_complete(address, 0, 8, XFER_RESULT_SUCCESS, true);
    return true;
}
bool hcd_edpt_xfer(uint8_t port, uint8_t address, uint8_t ep, uint8_t *buffer, uint16_t length) {
    if ((ep & 15) != 0) {
        CHECK(!incoming[ep & 15].pending);
        incoming[ep & 15].pending = true;
        incoming[ep & 15].address = address;
        incoming[ep & 15].buffer = buffer;
        incoming[ep & 15].length = length;
        return true;
    }
    CHECK(address == setup_address);
    unsigned actual = length;
    if (length && (ep & 0x80)) {
        const uint8_t *source = NULL; unsigned available = 0;
        CHECK(setup.bRequest == TUSB_REQ_GET_DESCRIPTOR);
        switch (setup.wValue >> 8) {
        case TUSB_DESC_DEVICE: source = (const uint8_t *)&device_descriptor; available = sizeof(device_descriptor); break;
        case TUSB_DESC_CONFIGURATION: source = configuration; available = configuration_size; break;
        case HID_DESC_TYPE_REPORT:
            if (trackball_only || setup.wIndex == 1) {
                source = fixture_mouse; available = sizeof(fixture_mouse);
            } else if (multi_keyboard) {
                source = multi_keyboard_descriptor; available = sizeof(multi_keyboard_descriptor);
            } else {
                source = fixture_keyboard; available = sizeof(fixture_keyboard);
            }
            break;
        default: CHECK(false);
        }
        actual = length < available ? length : available;
        memcpy(buffer, source, actual);
    } else if (length) {
        CHECK(setup.bRequest == HID_REQ_CONTROL_SET_REPORT);
        CHECK(length == 1); peripheral_led = buffer[0];
    }
    /* HID permits a peripheral to stall SET_IDLE; the real class driver
       must still finish enumeration and start interrupt polling. */
    xfer_result_t result = XFER_RESULT_SUCCESS;
    if (!length && setup.bRequest == HID_REQ_CONTROL_SET_IDLE && setup.bmRequestType == 0x21) {
        ++idle_stalls; result = XFER_RESULT_STALLED;
    }
    hcd_event_xfer_complete(address, ep, actual, result, true);
    return true;
}

const usbh_class_driver_t *usbh_app_driver_get_cb(uint8_t *count) { *count = 0; return NULL; }
void tuh_mount_cb(uint8_t address) { ++mounts; }
void tuh_umount_cb(uint8_t address) { ++unmounts; }
void tuh_hid_report_sent_cb(uint8_t address, uint8_t instance, const uint8_t *report, uint16_t size) {}
void tuh_hid_get_report_complete_cb(uint8_t address, uint8_t instance, uint8_t report, uint8_t type, uint16_t size) {}
void tuh_hid_set_report_complete_cb(uint8_t address, uint8_t instance, uint8_t report, uint8_t type, uint16_t size) {}
void blink_led(device_t *state) {}
void restore_leds(device_t *state) {}
void send_value(uint8_t value, enum packet_type_e type) { CHECK(type == FLASH_LED_MSG || type == KBD_SET_REPORT_MSG); }
void publish_local_modifiers(device_t *state) { ++modifier_publishes; }
void record_local_activity(device_t *state, uint8_t output) { ++activity_records; }
void queue_cc_packet(uint8_t *bytes, device_t *state) { CHECK(false); }
void queue_system_packet(uint8_t *bytes, device_t *state) { CHECK(false); }
void queue_packet(const uint8_t *bytes, enum packet_type_e type, int length) { CHECK(false); }
uint64_t time_us_64(void) { return (uint64_t)virtual_ms * 1000; }
void tight_loop_contents(void) { ++virtual_ms; }
bool queue_try_add(queue_t *queue, const void *item) {
    CHECK(queue == &global_state.kbd_queue);
    memcpy(&last_keyboard, item, sizeof(last_keyboard)); ++keyboard_reports; return true;
}
bool queue_try_remove(queue_t *queue, void *item) { return false; }
bool queue_try_peek(queue_t *queue, void *item) { return false; }
void mouse_interface_removed(hid_interface_t *iface, device_t *state) { iface->mouse_buttons = 0; }
void process_mouse_report(uint8_t *bytes, int length, uint8_t index, hid_interface_t *iface) {
    CHECK(length == 5 && iface->mouse.is_found);
    CHECK(get_report_value(bytes, length, &iface->mouse.move_x) == -7);
    CHECK(get_report_value(bytes, length, &iface->mouse.move_y) == 9);
    ++mouse_reports;
}
#define EMPTY_HOTKEY(name) void name(device_t *state, hid_keyboard_report_t *report) {}
EMPTY_HOTKEY(output_toggle_hotkey_handler)
EMPTY_HOTKEY(mouse_zoom_hotkey_handler)
EMPTY_HOTKEY(switchlock_hotkey_handler)
EMPTY_HOTKEY(screenlock_hotkey_handler)
EMPTY_HOTKEY(toggle_gaming_mode_handler)
EMPTY_HOTKEY(clear_zoom_assist_hotkey_handler)
EMPTY_HOTKEY(enable_screensaver_pong_hotkey_handler)
EMPTY_HOTKEY(enable_screensaver_jitter_hotkey_handler)
EMPTY_HOTKEY(disable_screensaver_hotkey_handler)
EMPTY_HOTKEY(wipe_config_hotkey_handler)
EMPTY_HOTKEY(screen_border_hotkey_handler)
EMPTY_HOTKEY(config_enable_hotkey_handler)
EMPTY_HOTKEY(fw_upgrade_hotkey_handler_A)
EMPTY_HOTKEY(fw_upgrade_hotkey_handler_B)
EMPTY_HOTKEY(reboot_hotkey_handler)

static void pump(void) { tuh_task_ext(0, false); }
static void make_configuration(bool trackball) {
    trackball_only = trackball;
    configuration_size = trackball ? 34 : 59;
    uint8_t header[] = {9,2,(uint8_t)configuration_size,0,(uint8_t)(trackball ? 1 : 2),1,0,0x80,50};
    memcpy(configuration, header, sizeof(header));
    for (unsigned instance = 0; instance < (trackball ? 1u : 2u); ++instance) {
        bool mouse = trackball || instance == 1;
        unsigned report_len = mouse ? sizeof(fixture_mouse)
            : multi_keyboard ? sizeof(multi_keyboard_descriptor) : sizeof(fixture_keyboard);
        uint8_t descriptors[] = {
            9,4,(uint8_t)instance,0,1,3,1,(uint8_t)(mouse ? 2 : 1),0,
            9,0x21,0x11,1,0,1,0x22,(uint8_t)report_len,0,
            7,5,(uint8_t)(0x81 + instance),3,(uint8_t)(mouse ? 5 : multi_keyboard ? 32 : 8),0,1,
        };
        memcpy(configuration + 9 + instance * 25, descriptors, sizeof(descriptors));
    }
}
static void attach(bool trackball) {
    make_configuration(trackball);
    connected = true;
    hcd_event_device_attach(0, true); pump();
    CHECK(configured_address == 1 && tuh_mounted(1));
    CHECK(tuh_hid_instance_count(1) == (trackball ? 1 : 2));
    CHECK(global_state.mouse_connected);
    CHECK(global_state.iface[0][trackball ? 0 : 1].protocol == HID_PROTOCOL_REPORT);
    CHECK(global_state.keyboard_connected == !trackball);
    CHECK(incoming[1].pending);
    if (!trackball) CHECK(incoming[2].pending);
}
static void deliver(unsigned ep, const uint8_t *bytes, unsigned size, xfer_result_t result) {
    CHECK(incoming[ep].pending && size <= incoming[ep].length);
    if (size) memcpy(incoming[ep].buffer, bytes, size);
    incoming[ep].pending = false;
    hcd_event_xfer_complete(1, (uint8_t)(0x80 | ep), size, result, true); pump();
    CHECK(incoming[ep].pending); /* Production received callback rearms it. */
}
static void detach(void) {
    connected = false;
    hcd_event_device_remove(0, true); pump();
    CHECK(!tuh_mounted(1) && !global_state.keyboard_connected && !global_state.mouse_connected);
    CHECK(!incoming[1].pending && !incoming[2].pending);
}
int main(void) {
    global_state.board_role = OUTPUT_A; global_state.active_output = OUTPUT_A; global_state.tud_connected = true;
    CHECK(tusb_init());
    attach(false);
    CHECK(controls >= 8 && resets >= 1 && virtual_ms >= 50 && idle_stalls == 2);
    CHECK(global_state.iface[0][0].num_keyboards == 1);
    uint8_t keyboard[] = {KEYBOARD_MODIFIER_LEFTCTRL,0,HID_KEY_A,0,0,0,0,0};
    unsigned before = keyboard_reports;
    deliver(1, keyboard, sizeof(keyboard), XFER_RESULT_SUCCESS);
    CHECK(keyboard_reports == before + 1 && last_keyboard.modifier == KEYBOARD_MODIFIER_LEFTCTRL && last_keyboard.keycode[0] == HID_KEY_A);
    uint8_t mouse[] = {2, (uint8_t)-7, 9, 0, 0};
    deliver(2, mouse, sizeof(mouse), XFER_RESULT_SUCCESS);
    CHECK(mouse_reports == 1);
    unsigned activity_before = activity_records;
    deliver(1, NULL, 0, XFER_RESULT_SUCCESS);
    deliver(1, NULL, 0, XFER_RESULT_FAILED);
    deliver(1, keyboard, 2, XFER_RESULT_SUCCESS);
    CHECK(keyboard_reports == before + 1 && activity_records == activity_before);
    uint8_t leds = 5;
    CHECK(tuh_hid_set_report(1, 0, 0, HID_REPORT_TYPE_OUTPUT, &leds, 1)); pump();
    CHECK(peripheral_led == 5);
    detach();
    CHECK(last_keyboard.modifier == 0 && last_keyboard.keycode[0] == 0);
    CHECK(modifier_publishes > 0);
    global_state.board_role = OUTPUT_B; global_state.active_output = OUTPUT_B;
    attach(true);
    deliver(1, mouse, sizeof(mouse), XFER_RESULT_SUCCESS);
    CHECK(mouse_reports == 2);
    detach();
    CHECK(mounts == 2 && unmounts == 2);
    global_state.board_role = OUTPUT_A; global_state.active_output = OUTPUT_A;
    multi_keyboard = true;
    /* Production setup selects report protocol unless force-boot is enabled. */
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
    attach(false);
    CHECK(global_state.iface[0][0].protocol == HID_PROTOCOL_REPORT);
    CHECK(global_state.iface[0][0].num_keyboards == 2);
    before = keyboard_reports;
    const uint8_t six_keys[] = {7,2,0,HID_KEY_A,HID_KEY_B,HID_KEY_C,0,0,0};
    deliver(1, six_keys, sizeof(six_keys), XFER_RESULT_SUCCESS);
    CHECK(keyboard_reports == before + 1 && last_keyboard.modifier == 2);
    CHECK(memcmp(last_keyboard.keycode, (uint8_t[]){4,5,6,0,0,0}, 6) == 0);
    uint8_t nkro_keys[17] = {17,4,1,2};
    deliver(1, nkro_keys, sizeof(nkro_keys), XFER_RESULT_SUCCESS);
    CHECK(keyboard_reports == before + 2 && last_keyboard.modifier == 4);
    CHECK(memcmp(last_keyboard.keycode, (uint8_t[]){4,13,0,0,0,0}, 6) == 0);
    unsigned active_before = activity_records;
    nkro_keys[0] = 254;
    deliver(1, nkro_keys, sizeof(nkro_keys), XFER_RESULT_SUCCESS);
    CHECK(keyboard_reports == before + 2 && activity_records == active_before);
    deliver(1, six_keys, sizeof(six_keys), XFER_RESULT_SUCCESS);
    CHECK(last_keyboard.keycode[0] == HID_KEY_A);
    detach();
    CHECK(last_keyboard.modifier == 0 && last_keyboard.keycode[0] == 0);
    CHECK(mounts == 3 && unmounts == 3);
    puts("TinyUSB virtual-HCD tests passed (composite keyboard/mouse, mixed 6KRO/NKRO collections, trackball, polling/rearm, LED OUT, detach all-up)");
    return 0;
}
