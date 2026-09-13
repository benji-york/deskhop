#include <assert.h>
#include <stdio.h>
#include "main.h"
#include "hid_report.h"

/* These are production objects, so parser-state bounds and the receiver map are
 * checked without copying either the parser implementation or its structures. */
extern parser_state_t parser_state;

device_t global_state;
static unsigned activity_count, consumer_count, system_count, keyboard_count;
static unsigned mouse_count, receive_count;
static uint8_t last_consumer[CONSUMER_CONTROL_LENGTH], last_system;
static hid_keyboard_report_t last_keyboard;
static uint8_t host_protocol;

static void reset(void) {
    memset(&global_state, 0, sizeof(global_state));
    global_state.tud_connected = true;
    global_state.active_output = OUTPUT_A;
    global_state.board_role = OUTPUT_A;
    activity_count = consumer_count = system_count = keyboard_count = 0;
    mouse_count = receive_count = 0;
    host_protocol = HID_ITF_PROTOCOL_NONE;
    memset(last_consumer, 0, sizeof(last_consumer));
    memset(&last_keyboard, 0, sizeof(last_keyboard));
}

void record_local_activity(device_t *state, uint8_t output) {
    assert(state == &global_state);
    assert(output == OUTPUT_A);
    activity_count++;
}
void queue_cc_packet(uint8_t *report, device_t *state) {
    (void)state;
    memcpy(last_consumer, report, sizeof(last_consumer));
    consumer_count++;
}
void queue_system_packet(uint8_t *report, device_t *state) {
    (void)state;
    last_system = report[0];
    system_count++;
}
void process_mouse_report(uint8_t *report, int length, uint8_t itf, hid_interface_t *iface) {
    (void)report; (void)length; (void)itf; (void)iface;
    mouse_count++;
}
bool queue_try_add(queue_t *queue, const void *report) {
    assert(queue == &global_state.kbd_queue);
    memcpy(&last_keyboard, report, sizeof(last_keyboard));
    keyboard_count++;
    return true;
}
uint8_t tuh_hid_interface_protocol(uint8_t address, uint8_t instance) {
    (void)address; (void)instance;
    return host_protocol;
}
bool tuh_hid_receive_report(uint8_t address, uint8_t instance) {
    assert(address >= 1 && address <= MAX_DEVICES);
    assert(instance < MAX_INTERFACES);
    receive_count++;
    return true;
}

/* Hardware-only dependencies of the actual keyboard.c and usb.c units. */
void publish_local_modifiers(device_t *state) { (void)state; }
void restore_leds(device_t *state) { (void)state; }
void blink_led(device_t *state) { (void)state; }
void send_value(uint8_t value, uint8_t type) { (void)value; (void)type; }
void queue_packet(uint8_t *data, uint8_t type, uint8_t length) {
    (void)data; (void)type; (void)length;
    assert(!"test unexpectedly routed input to the remote output");
}
bool validate_packet(uart_packet_t *packet) { (void)packet; return false; }
void process_packet(uart_packet_t *packet, device_t *state) { (void)packet; (void)state; }
uint64_t time_us_64(void) { return 1000000; }
void tight_loop_contents(void) { assert(!"unexpected blocking queue"); }
bool queue_try_peek(queue_t *queue, void *data) { (void)queue; (void)data; return false; }
bool queue_try_remove(queue_t *queue, void *data) { (void)queue; (void)data; return false; }
bool tud_suspended(void) { return false; }
bool tud_remote_wakeup(void) { return true; }
bool tud_hid_n_ready(uint8_t instance) { (void)instance; return true; }
bool tud_hid_keyboard_report(uint8_t id, uint8_t modifier, const uint8_t *keys) {
    (void)id; (void)modifier; (void)keys; return true;
}
uint8_t tuh_hid_get_protocol(uint8_t address, uint8_t instance) {
    (void)address; (void)instance; return HID_PROTOCOL_REPORT;
}
bool tuh_hid_set_protocol(uint8_t address, uint8_t instance, uint8_t protocol) {
    (void)address; (void)instance; (void)protocol; return true;
}
#define DEFINE_HANDLER(name) \
    void name(device_t *state, hid_keyboard_report_t *report) { (void)state; (void)report; }
HID_TEST_HOTKEY_HANDLERS(DEFINE_HANDLER)
#undef DEFINE_HANDLER

static void test_carry_last_usage(void) {
    hid_interface_t iface = {0};
    const uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, /* Desktop Mouse application */
        0x09, 0x30, 0x09, 0x31,             /* X, Y */
        0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
        0x95, 0x01, 0x81, 0x02,             /* No local usage: retain Y */
        0x81, 0x02,                         /* No local usage again */
        0xc0
    };
    parse_report_descriptor(&iface, descriptor, sizeof(descriptor));
    assert(iface.mouse.move_x.offset == 0);
    assert(iface.mouse.move_y.offset == 24);
    assert(parser_state.p_usage == parser_state.usages + 2);
    assert(*parser_state.p_usage == HID_USAGE_DESKTOP_Y);
    assert(parser_state.usage_count == 0);
}

static void test_empty_and_oversized_usage_lists(void) {
    hid_interface_t iface = {0};
    const uint8_t no_usage[] = {
        0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
        0x75, 0x08, 0x95, 0x03, 0x81, 0x02, 0x81, 0x02, 0xc0
    };
    parse_report_descriptor(&iface, no_usage, sizeof(no_usage));
    assert(parser_state.p_usage == parser_state.usages);
    assert(parser_state.report_offsets[0].offset_in_bits == 48);

    uint8_t descriptor[1024];
    size_t length = 0;
    const uint8_t prefix[] = {0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01};
    memcpy(descriptor, prefix, sizeof(prefix)); length += sizeof(prefix);
    for (unsigned i = 0; i < HID_MAX_USAGES + 40; i++) {
        descriptor[length++] = 0x09;
        descriptor[length++] = (uint8_t)(i + 1);
    }
    const uint8_t suffix[] = {
        0x75, 0x08, 0x96, 0x00, 0x04, /* 1024 eight-bit elements */
        0x81, 0x02, 0x81, 0x02, 0xc0
    };
    memcpy(descriptor + length, suffix, sizeof(suffix)); length += sizeof(suffix);
    parse_report_descriptor(&iface, descriptor, (int)length);
    assert(parser_state.p_usage >= parser_state.usages);
    assert(parser_state.p_usage < parser_state.usages + HID_MAX_USAGES);
    assert(parser_state.report_offsets[0].offset_in_bits == 16384);
    assert(parser_state.collection.start == parser_state.collection.end);
    assert(parser_state.usage_count == 0);
}

/* Build real descriptors for each supported receiver. ID zero means an interface
 * with no Report ID item; the first payload byte must then remain payload. */
static void parse_receiver(hid_interface_t *iface, uint8_t id, receiver_id_t receiver) {
    uint8_t descriptor[80];
    size_t n = 0;
    uint8_t page = receiver == REPORT_RECEIVER_CONSUMER ? 0x0c : 0x01;
    uint8_t usage = receiver == REPORT_RECEIVER_MOUSE ? 0x02
                    : receiver == REPORT_RECEIVER_KEYBOARD ? 0x06
                    : receiver == REPORT_RECEIVER_CONSUMER ? 0x01 : 0x80;
    const uint8_t prefix[] = {0x05, page, 0x09, usage, 0xa1, 0x01};
    memcpy(descriptor, prefix, sizeof(prefix)); n += sizeof(prefix);
    if (id) { descriptor[n++] = 0x85; descriptor[n++] = id; }
    if (receiver == REPORT_RECEIVER_KEYBOARD) {
        const uint8_t keys[] = {
            0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
            0x19, 0x00, 0x29, 0xff, 0x95, 0x06, 0x81, 0x00
        };
        memcpy(descriptor + n, keys, sizeof(keys)); n += sizeof(keys);
    } else {
        const uint8_t value[] = {
            0x09, receiver == REPORT_RECEIVER_MOUSE ? 0x30 : 0x81,
            0x75, 0x08, 0x95, 0x01, 0x81, 0x00
        };
        memcpy(descriptor + n, value, sizeof(value)); n += sizeof(value);
    }
    descriptor[n++] = 0xc0;
    iface->protocol = HID_PROTOCOL_REPORT;
    parse_report_descriptor(iface, descriptor, (int)n);
}

static void test_all_report_id_receivers(void) {
    const uint8_t ids[] = {0, 23, 24, 255};
    const process_report_f expected[] = {
        NULL, process_mouse_report, process_keyboard_report,
        process_consumer_report, process_system_report
    };
    for (unsigned i = 0; i < sizeof(ids); i++) {
        for (receiver_id_t kind = REPORT_RECEIVER_MOUSE; kind <= REPORT_RECEIVER_SYSTEM; kind++) {
            reset();
            uint8_t id = ids[i];
            hid_interface_t *iface = &global_state.iface[0][0];
            parse_receiver(iface, id, kind);
            assert(iface->uses_report_id == (id != 0));
            assert(iface->report_handler[id] == kind);
            assert(report_receivers[iface->report_handler[id]] == expected[kind]);
            assert(iface->report_handler[254] == REPORT_RECEIVER_NONE);

            uint8_t report[9] = {0};
            unsigned offset = id != 0;
            if (id) report[0] = id;
            report[offset] = kind == REPORT_RECEIVER_KEYBOARD ? 0x02 : 0x81;
            if (kind == REPORT_RECEIVER_KEYBOARD) report[offset + 2] = HID_KEY_A;
            uint16_t length = (uint16_t)(offset + (kind == REPORT_RECEIVER_KEYBOARD ? 8 : 1));
            tuh_hid_report_received_cb(1, 0, report, length);
            assert(receive_count == 1);
            assert(mouse_count == (kind == REPORT_RECEIVER_MOUSE));
            assert(keyboard_count == (kind == REPORT_RECEIVER_KEYBOARD));
            assert(consumer_count == (kind == REPORT_RECEIVER_CONSUMER));
            assert(system_count == (kind == REPORT_RECEIVER_SYSTEM));
            assert(activity_count == (kind != REPORT_RECEIVER_MOUSE));
            if (kind == REPORT_RECEIVER_KEYBOARD) {
                assert(last_keyboard.modifier == 0x02);
                assert(last_keyboard.keycode[0] == HID_KEY_A);
            }
            if (kind == REPORT_RECEIVER_CONSUMER) assert(last_consumer[0] == 0x81);
            if (kind == REPORT_RECEIVER_SYSTEM) assert(last_system == 0x81);
        }
    }
}

static void test_unsupported_and_empty_dispatch(void) {
    reset();
    hid_interface_t *iface = &global_state.iface[0][0];
    parse_receiver(iface, 255, REPORT_RECEIVER_CONSUMER);
    uint8_t unknown[] = {254, 0x01};
    tuh_hid_report_received_cb(1, 0, unknown, sizeof(unknown));
    assert(receive_count == 1 && consumer_count == 0 && activity_count == 0);
    tuh_hid_report_received_cb(1, 0, NULL, 0);
    assert(receive_count == 2 && consumer_count == 0 && activity_count == 0);
    uint8_t id_only[] = {255};
    tuh_hid_report_received_cb(1, 0, id_only, sizeof(id_only));
    assert(receive_count == 3 && consumer_count == 0 && activity_count == 0);
    tuh_hid_report_received_cb(0, 0, unknown, sizeof(unknown));
    tuh_hid_report_received_cb(MAX_DEVICES + 1, 0, unknown, sizeof(unknown));
    tuh_hid_report_received_cb(1, MAX_INTERFACES, unknown, sizeof(unknown));
    assert(receive_count == 3);
}

static void test_distinct_report_capacity(void) {
    hid_interface_t iface = {0};
    uint8_t descriptor[(MAX_REPORTS_PER_IFACE + 1) * 16];
    size_t n = 0;
    const uint8_t prefix[] = {0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01};
    memcpy(descriptor, prefix, sizeof(prefix)); n += sizeof(prefix);
    for (unsigned id = 1; id <= MAX_REPORTS_PER_IFACE + 1; id++) {
        const uint8_t item[] = {0x85, (uint8_t)id, 0x09, 0x81, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00};
        memcpy(descriptor + n, item, sizeof(item)); n += sizeof(item);
    }
    descriptor[n++] = 0xc0;
    parse_report_descriptor(&iface, descriptor, (int)n);
    assert(parser_state.num_report_offsets == MAX_REPORTS_PER_IFACE);
    for (unsigned id = 1; id <= MAX_REPORTS_PER_IFACE; id++)
        assert(iface.report_handler[id] == REPORT_RECEIVER_CONSUMER);
    assert(iface.report_handler[MAX_REPORTS_PER_IFACE + 1] == REPORT_RECEIVER_NONE);
}

static void test_consumer_payloads_and_activity(void) {
    for (unsigned with_id = 0; with_id <= 1; with_id++) {
        reset();
        hid_interface_t iface = {0};
        iface.uses_report_id = with_id;
        iface.consumer.is_variable = true;
        iface.keyboards[0].cc_array[0] = 0x00e9;
        iface.keyboards[0].cc_array[15] = 0x0238;
        process_consumer_report(NULL, 0, 0, &iface);
        assert(consumer_count == 0 && activity_count == 0);
        uint8_t raw[] = {0xff, 0, 0, 0, 0, 0};
        if (with_id) {
            process_consumer_report(raw, 1, 0, &iface);
            assert(consumer_count == 0 && activity_count == 0);
        }
        raw[with_id] = 1;
        process_consumer_report(raw, (int)with_id + 1, 0, &iface);
        assert(last_consumer[0] == 0xe9 && last_consumer[1] == 0);
        assert(consumer_count == 1 && activity_count == 1);
        raw[with_id + 1] = 0x80;
        process_consumer_report(raw, (int)with_id + 2, 0, &iface);
        assert(last_consumer[0] == 0x38 && last_consumer[1] == 0x02);
        assert(consumer_count == 2 && activity_count == 2);
        raw[with_id] = raw[with_id + 1] = 0;
        process_consumer_report(raw, (int)with_id + 2, 0, &iface);
        assert(last_consumer[0] == 0 && last_consumer[1] == 0);
        assert(consumer_count == 3 && activity_count == 3);

        iface.consumer.is_variable = false;
        for (unsigned i = 0; i < 5; i++) raw[with_id + i] = (uint8_t)(i + 1);
        process_consumer_report(raw, (int)with_id + 5, 0, &iface);
        assert(memcmp(last_consumer, (uint8_t[]){1, 2, 3, 4}, 4) == 0);
        assert(consumer_count == 4 && activity_count == 4);
        process_consumer_report(raw, (int)with_id + 1, 0, &iface);
        assert(memcmp(last_consumer, (uint8_t[]){1, 0, 0, 0}, 4) == 0);
        assert(consumer_count == 5 && activity_count == 5);
    }
}

static void set_payload_bit(uint8_t *report, unsigned bit) {
    report[1 + bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void test_system_payloads_and_activity(void) {
    for (unsigned with_id = 0; with_id <= 1; with_id++) {
        reset();
        hid_interface_t iface = {0};
        iface.uses_report_id = with_id;
        process_system_report(NULL, 0, 0, &iface);
        assert(system_count == 0 && activity_count == 0);
        uint8_t raw[] = {0xff, 0x82};
        if (with_id) {
            process_system_report(raw, 1, 0, &iface);
            assert(system_count == 0 && activity_count == 0);
        }
        raw[with_id] = 0x82;
        process_system_report(raw, (int)with_id + 1, 0, &iface);
        assert(system_count == 1 && activity_count == 1 && last_system == 0x82);
    }
}

static void test_split_nkro(void) {
    hid_interface_t iface = {0};
    const uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 24,
        0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x19, 0x04, 0x29, 0x13, 0x95, 16, 0x81, 0x02,
        0x95, 3, 0x81, 0x01, /* Padding: next block starts at bit 27 */
        0x19, 0x20, 0x29, 0x27, 0x95, 8, 0x81, 0x02,
        0x95, 5, 0x81, 0x01,
        0x19, 0x30, 0x29, 0x3f, 0x95, 16, 0x81, 0x02,
        0x19, 0x40, 0x29, 0x47, 0x95, 8, 0x81, 0x02,
        0xc0
    };
    iface.protocol = HID_PROTOCOL_REPORT;
    parse_report_descriptor(&iface, descriptor, sizeof(descriptor));
    keyboard_t *kb = &iface.keyboards[0];
    assert(kb->nkro_count == 4 && kb->nkro_bits == 48 && kb->is_nkro);
    assert(kb->modifier.size == 8);
    assert(kb->nkro[1].offset == 27 && kb->nkro[1].size == 8);
    assert(kb->nkro[3].offset == 56 && kb->nkro[3].size == 8);
    uint8_t raw[9] = {24, 0x20};
    set_payload_bit(raw, 10);
    set_payload_bit(raw, 34);
    set_payload_bit(raw, 55);
    set_payload_bit(raw, 61);
    hid_keyboard_report_t report;
    assert(extract_kbd_data(raw, sizeof(raw), 0, &iface, &report) == 4);
    assert(report.modifier == 0x20);
    assert(memcmp(report.keycode, (uint8_t[]){0x06, 0x27, 0x3f, 0x45, 0, 0}, 6) == 0);

    /* Exact-sized allocation lets ASan catch a read into missing later blocks. */
    uint8_t *short_report = malloc(4);
    assert(short_report);
    memcpy(short_report, raw, 4);
    assert(extract_kbd_data(short_report, 4, 0, &iface, &report) == 1);
    assert(report.keycode[0] == 0x06 && report.keycode[1] == 0);
    free(short_report);

    memset(raw + 1, 0xff, sizeof(raw) - 1);
    assert(extract_kbd_data(raw, sizeof(raw), 0, &iface, &report) == KEYS_IN_USB_REPORT);
    assert(memcmp(report.keycode, (uint8_t[]){4, 5, 6, 7, 8, 9}, 6) == 0);
}

static void test_nkro_aggregate_threshold(void) {
    hid_interface_t iface = {0};
    report_val_t val = {
        .usage_page = HID_USAGE_PAGE_KEYBOARD,
        .global_usage = HID_USAGE_DESKTOP_KEYBOARD,
        .data_type = VARIABLE, .item_type = DATA,
        .usage_min = 4, .usage_max = 19, .size = 16,
    };
    extract_data(&iface, &val);
    assert(iface.keyboards[0].nkro_count == 1 && !iface.keyboards[0].is_nkro);
    val.offset = 16; val.offset_idx = 2;
    val.usage_min = 20; val.usage_max = 35;
    extract_data(&iface, &val);
    assert(iface.keyboards[0].nkro_bits == 32 && !iface.keyboards[0].is_nkro);
    val.offset = 32; val.offset_idx = 4;
    val.usage_min = 36; val.usage_max = 43; val.size = 8;
    extract_data(&iface, &val);
    assert(iface.keyboards[0].nkro_bits == 40 && iface.keyboards[0].is_nkro);
    assert(iface.keyboards[0].nkro[2].size == 8);
    /* A fifth block remains deliberately unsupported; it cannot overflow storage. */
    extract_data(&iface, &val);
    extract_data(&iface, &val);
    assert(iface.keyboards[0].nkro_count == MAX_NKRO_BLOCKS);
    assert(iface.keyboards[0].nkro_bits == 48);
}

int main(void) {
    test_carry_last_usage();
    test_empty_and_oversized_usage_lists();
    test_all_report_id_receivers();
    test_unsupported_and_empty_dispatch();
    test_distinct_report_capacity();
    test_consumer_payloads_and_activity();
    test_system_payloads_and_activity();
    test_split_nkro();
    test_nkro_aggregate_threshold();
    puts("HID regression tests passed (parser, receiver dispatch, consumer controls, split NKRO)");
    return 0;
}
