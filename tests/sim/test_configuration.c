/* Configuration contracts exercised through production WebHID/API/UART
 * dispatch and mouse consumers. Bounds below are independent policy oracles. */
#include "main.h"
#include "config_packet.h"
#include <assert.h>
#include <stdio.h>
void sim_init(uint8_t, void (*)(int, int, int, const void *, int));
void sim_destroy(void);
void sim_host(int, int);
void sim_set_time(uint64_t);
int16_t scale_y_coordinate(int, int, device_t *);
void do_screen_switch(device_t *, int);
static unsigned checks;
static void observe(int kind, int a, int b, const void *data, int length) { }
static void drain(void) {
    uart_packet_t uart;
    hid_generic_pkt_t hid;
    mouse_report_t mouse;
    while (queue_try_remove(&global_state.uart_tx_queue, &uart)) { }
    while (queue_try_remove(&global_state.hid_queue_out, &hid)) { }
    while (queue_try_remove(&global_state.mouse_queue, &mouse)) { }
}
static void fresh(void) {
    drain();
    global_state.config = default_config;
    global_state.active_output = 0;
    global_state.board_role = 0;
    global_state.config_mode_active = true;
    global_state.pointer_y = 16384;
}
/* Do not derive the expected destination from the production API field map:
 * an accidentally remapped field must fail independently of that same map. */
static size_t expected_field(uint8_t field, size_t *width) {
#define EXPECT(ID, MEMBER) case ID: *width = sizeof(((config_t *)0)->MEMBER); return offsetof(config_t, MEMBER)
    switch (field) {
    EXPECT(11, output[0].screen_count); EXPECT(12, output[0].speed_x);
    EXPECT(13, output[0].speed_y); EXPECT(14, output[0].border.top);
    EXPECT(15, output[0].border.bottom); EXPECT(16, output[0].os);
    EXPECT(17, output[0].pos); EXPECT(18, output[0].mouse_park_pos);
    EXPECT(19, output[0].screensaver.mode); EXPECT(20, output[0].screensaver.only_if_inactive);
    EXPECT(21, output[0].screensaver.idle_time_us); EXPECT(22, output[0].screensaver.max_time_us);
    EXPECT(41, output[1].screen_count); EXPECT(42, output[1].speed_x);
    EXPECT(43, output[1].speed_y); EXPECT(44, output[1].border.top);
    EXPECT(45, output[1].border.bottom); EXPECT(46, output[1].os);
    EXPECT(47, output[1].pos); EXPECT(48, output[1].mouse_park_pos);
    EXPECT(49, output[1].screensaver.mode); EXPECT(50, output[1].screensaver.only_if_inactive);
    EXPECT(51, output[1].screensaver.idle_time_us); EXPECT(52, output[1].screensaver.max_time_us);
    EXPECT(71, force_mouse_boot_mode); EXPECT(72, force_kbd_boot_protocol);
    EXPECT(73, kbd_led_as_indicator); EXPECT(74, hotkey_toggle);
    EXPECT(75, enable_acceleration); EXPECT(76, enforce_ports);
    EXPECT(77, jump_threshold); EXPECT(83, screensaver_system_timeout_sec);
    default: *width = 0; return 0;
    }
#undef EXPECT
}
static void dispatch(uart_packet_t packet, bool proxy) {
    uint8_t raw[CONFIG_PACKET_LENGTH];
    if (proxy) {
        uart_packet_t wrapped = {.type = PROXY_PACKET_MSG, .data = {packet.type}};
        memcpy(wrapped.data + 1, packet.data, 7);
        packet = wrapped;
    }
    write_config_packet(raw, &packet);
    tud_hid_set_report_cb(ITF_NUM_HID_VENDOR, REPORT_ID_VENDOR,
                         HID_REPORT_TYPE_OUTPUT, raw, sizeof(raw));
    if (proxy) {
        uart_packet_t forwarded;
        while (queue_try_remove(&global_state.uart_tx_queue, &forwarded)) {
            /* Execute the actual recipient dispatcher, with the checksum a
             * UART encoder would attach. Integrity faults have their own suite. */
            forwarded.checksum = calc_packet_checksum(&forwarded);
            process_packet(&forwarded, &global_state);
        }
    }
}
static void set(uint8_t field, uint64_t value, bool proxy) {
    uart_packet_t packet = {.type = SET_VAL_MSG, .data = {field}};
    for (unsigned i = 0; i < 7; ++i)
        packet.data[i + 1] = (uint8_t)(value >> (i * 8));
    dispatch(packet, proxy);
}
static void probe(uint8_t field, uint64_t value, bool accepted, bool proxy) {
    fresh();
    config_t expected = global_state.config;
    if (accepted) {
        size_t width, offset = expected_field(field, &width);
        assert(width && offset + width <= sizeof(expected));
        memcpy((uint8_t *)&expected + offset, &value, width);
    }
    set(field, value, proxy);
    if (memcmp(&global_state.config, &expected, sizeof(expected))) {
        fprintf(stderr, "configuration field=%u value=%llu accepted=%u proxy=%u\n",
                field, (unsigned long long)value, accepted, proxy);
        assert(false);
    }
    ++checks;
}
static void boundaries(void) {
    for (unsigned proxy = 0; proxy < 2; ++proxy) {
        for (unsigned output = 0; output < 2; ++output) {
            unsigned base = output ? 40 : 10;
            probe(base + 1, 1, true, proxy);
            probe(base + 1, INT32_MAX, true, proxy);
            probe(base + 1, 0, false, proxy);
            probe(base + 1, (uint32_t)INT32_MAX + 1u, false, proxy);
            probe(base + 1, UINT32_MAX, false, proxy);
            for (unsigned axis = 2; axis <= 3; ++axis) {
                probe(base + axis, 1, true, proxy);
                probe(base + axis, 128, true, proxy);
                probe(base + axis, 0, false, proxy);
                probe(base + axis, 129, false, proxy);
                probe(base + axis, UINT32_MAX, false, proxy);
                probe(base + axis, INT32_MAX, false, proxy);
            }
            probe(base + 4, 0, true, proxy);
            probe(base + 4, 32766, true, proxy);
            probe(base + 4, 32767, false, proxy);
            probe(base + 4, UINT32_MAX, false, proxy);
            probe(base + 5, 1, true, proxy);
            probe(base + 5, 32767, true, proxy);
            probe(base + 5, 0, false, proxy);
            probe(base + 5, 32768, false, proxy);
            probe(base + 5, UINT32_MAX, false, proxy);
            for (unsigned value = 0; value <= 255; ++value) {
                probe(base + 6, value, (value >= 1 && value <= 4) || value == 255, proxy);
                probe(base + 7, value, value == 1 || value == 2, proxy);
                probe(base + 8, value, value == 0 || value == 1 || value == 3, proxy);
                probe(base + 9, value, value <= 2, proxy);
                probe(base + 10, value, value <= 1, proxy);
            }
            for (unsigned field = base + 11; field <= base + 12; ++field) {
                const uint64_t values[] = {0, UINT32_MAX, UINT64_C(4294967296),
                    UINT64_C(7200000000), UINT64_C(281474976710655)};
                for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
                    probe(field, values[i], true, proxy);
                if (!proxy) {
                    probe(field, UINT64_C(281474976710656), false, false);
                    probe(field, UINT64_C(0x00ffffffffffffff), false, false);
                }
            }
        }
        const uint8_t booleans[] = {71, 72, 73, 75, 76};
        for (unsigned i = 0; i < sizeof(booleans); ++i)
            for (unsigned value = 0; value <= 255; ++value)
                probe(booleans[i], value, value <= 1, proxy);
        probe(74, 0, true, proxy); probe(74, 255, true, proxy);
        probe(77, 0, true, proxy); probe(77, UINT16_MAX, true, proxy);
        probe(77, UINT16_MAX + 1u, false, proxy);
        probe(83, 0, true, proxy); probe(83, UINT32_MAX, true, proxy);
        probe(83, UINT64_C(4294967296), false, proxy);
    }
}
static void readonly_unknown_padding(void) {
    for (unsigned proxy = 0; proxy < 2; ++proxy) {
        for (unsigned field = 0; field <= 255; ++field) {
            size_t storage_width, offset = expected_field(field, &storage_width);
            if (!storage_width)
                probe(field, 3, false, proxy);
            else {
                /* All unused payload bytes must be zero, including the old
                 * seventh timeout byte on the direct configuration path. */
                unsigned width = storage_width == 8 ? 6 : storage_width;
                for (unsigned byte = width; byte < (proxy ? 6u : 7u); ++byte) {
                    fresh();
                    config_t saved = global_state.config;
                    uart_packet_t packet = {.type = SET_VAL_MSG, .data = {field}};
                    memcpy(packet.data + 1, (uint8_t *)&global_state.config + offset, width);
                    packet.data[byte + 1] = 1;
                    dispatch(packet, proxy);
                    assert(memcmp(&global_state.config, &saved, sizeof(saved)) == 0);
                    ++checks;
                }
            }
        }
        const unsigned identities[] = {10, 40, 70};
        for (unsigned i = 0; i < 3; ++i) {
            assert(get_field_map_entry(identities[i])->readonly);
            probe(identities[i], 3, false, proxy);
        }
    }
}
static void cross_field_edits_and_consumers(void) {
    for (unsigned proxy = 0; proxy < 2; ++proxy) {
        fresh();
        set(14, 10000, proxy); set(15, 20000, proxy);
        set(14, 25000, proxy); /* Invalid intermediate pair: reject intact. */
        assert(global_state.config.output[0].border.top == 10000);
        set(15, 30000, proxy); set(14, 25000, proxy);
        assert(global_state.config.output[0].border.top == 25000);
        assert(global_state.config.output[0].border.bottom == 30000);
        set(14, 100, proxy); set(15, 1000, proxy); /* Move pair downward. */
        assert(global_state.config.output[0].border.top == 100);
        assert(global_state.config.output[0].border.bottom == 1000);
        set(11, 100, proxy);
        global_state.config.output[0].screen_index = 100;
        set(11, 2, proxy);
        assert(global_state.config.output[0].screen_index == 2);
        set(11, 0, proxy);
        assert(global_state.config.output[0].screen_index == 2);
        const uint64_t times[] = {UINT64_C(7200000000), UINT64_C(281474976710655), 1, 0};
        for (unsigned t = 0; t < 4; ++t) {
            set(21, times[t], proxy);
            assert(global_state.config.output[0].screensaver.idle_time_us == times[t]);
            drain();
            dispatch((uart_packet_t){.type = GET_VAL_MSG, .data = {21}}, proxy);
            hid_generic_pkt_t reply;
            uart_packet_t decoded;
            assert(queue_try_remove(&global_state.hid_queue_out, &reply));
            assert(read_config_packet(reply.data, reply.len, &decoded));
            uint64_t value = 0;
            memcpy(&value, decoded.data + 1, 7);
            assert(value == times[t]);
        }
    }
    fresh();
    /* A legacy partial-width writer could leave nonzero upper storage bytes.
     * A new accepted timeout SET must replace the complete uint64 field. */
    global_state.config.output[0].screensaver.idle_time_us = UINT64_C(0xffff000000000000);
    set(21, UINT64_C(7200000000), false);
    assert(global_state.config.output[0].screensaver.idle_time_us == UINT64_C(7200000000));
    fresh();
    static device_t separate;
    memset(&separate, 0, sizeof(separate));
    separate.config = default_config;
    uart_packet_t independent = {.type = SET_VAL_MSG, .data = {12, 47}};
    handle_api_msgs(&independent, &separate);
    assert(separate.config.output[0].speed_x == 47);
    assert(memcmp(&global_state.config, &default_config, sizeof(config_t)) == 0);
    fresh();
    global_state.config.output[0].screen_count = INT32_MAX;
    global_state.config.output[0].screen_index = INT32_MAX - 1;
    global_state.config.output[0].os = OTHER;
    do_screen_switch(&global_state, RIGHT);
    assert(global_state.config.output[0].screen_index == INT32_MAX);
    do_screen_switch(&global_state, RIGHT);
    assert(global_state.config.output[0].screen_index == INT32_MAX);
    fresh();
    /* Invalid flash/RAM fixtures must also be harmless at the consumer edge. */
    global_state.config.output[0].border = (border_size_t){16384, 16384};
    global_state.pointer_y = 16384;
    int16_t y = scale_y_coordinate(0, 1, &global_state);
    assert(y >= 0 && y <= 32767);
    y = scale_y_coordinate(3, -2, &global_state);
    assert(y >= 0 && y <= 32767);
    fresh();
    global_state.config.output[0].number = 3;
    do_screen_switch(&global_state, LEFT);
    assert(global_state.pointer_y >= 0 && global_state.pointer_y <= 32767);
}
static void synchronization_and_calibration(void) {
    fresh();
    global_state.active_output = 1; /* Receive borders for remote active output. */
    const border_size_t borders[] = {{100, 30000}, {16384, 16384}, {-1, 32000}, {0, 32768}, {30000, 100}};
    for (unsigned i = 0; i < sizeof(borders) / sizeof(borders[0]); ++i) {
        border_size_t before = global_state.config.output[1].border;
        uart_packet_t packet = {.type = SYNC_BORDERS_MSG};
        memcpy(packet.data, &borders[i], sizeof(borders[i]));
        handle_sync_borders_msg(&packet, &global_state);
        assert(memcmp(&global_state.config.output[1].border, i == 0 ? &borders[i] : &before,
                      sizeof(before)) == 0);
        drain();
    }
    fresh();
    global_state.config.output[0].border = (border_size_t){17000, 30000};
    global_state.pointer_y = 16384; /* Would put bottom below the saved top. */
    screen_border_hotkey_handler(&global_state, NULL);
    assert(global_state.config.output[0].border.top == 17000);
    assert(global_state.config.output[0].border.bottom == 30000);
    drain();
    global_state.pointer_y = 31000;
    screen_border_hotkey_handler(&global_state, NULL);
    assert(global_state.config.output[0].border.bottom == 31000);
    drain();
    for (unsigned value = 0; value <= 255; ++value) {
        uint8_t before = global_state.config.output[0].screensaver.mode;
        uart_packet_t packet = {.type = SCREENSAVER_MSG, .data = {value}};
        handle_screensaver_msg(&packet, &global_state);
        assert(global_state.config.output[0].screensaver.mode == (value <= 2 ? value : before));
    }
}
static void preserved_timer_consumer(void) {
    fresh();
    sim_host(1, 0);
    global_state.last_activity[0] = 0;
    global_state.config.screensaver_system_timeout_sec = 0;
    screensaver_t *saver = &global_state.config.output[0].screensaver;
    saver->mode = JITTER;
    saver->only_if_inactive = 0;
    saver->idle_time_us = UINT64_C(20000000);
    saver->max_time_us = UINT64_MAX;
    sim_set_time(UINT64_C(30000000));
    screensaver_task(&global_state);
    mouse_report_t report;
    /* idle + max wraps to 19,999,999 in the predecessor. A preserved large
       maximum means a long run, not an already-expired screensaver. */
    assert(queue_try_remove(&global_state.mouse_queue, &report));
    assert(report.mode == RELATIVE && report.y != 0);
    assert(!queue_try_remove(&global_state.mouse_queue, &report));
    saver->idle_time_us = UINT64_C(30000000);
    saver->max_time_us = UINT64_C(20000000);
    sim_set_time(UINT64_C(50000000));
    screensaver_task(&global_state); /* Exact maximum is still permitted. */
    assert(queue_try_remove(&global_state.mouse_queue, &report));
    sim_set_time(UINT64_C(60000001));
    screensaver_task(&global_state);
    assert(!queue_try_remove(&global_state.mouse_queue, &report));
    saver->idle_time_us = UINT64_C(0x8000000000000000);
    saver->max_time_us = UINT64_C(0x8000000000000000);
    sim_set_time(UINT64_MAX - 100);
    screensaver_task(&global_state);
    assert(queue_try_remove(&global_state.mouse_queue, &report));
    saver->idle_time_us = UINT64_MAX;
    screensaver_task(&global_state); /* Below an intact 64-bit idle threshold. */
    assert(!queue_try_remove(&global_state.mouse_queue, &report));
}
int main(void) {
    sim_init(0, observe);
    boundaries();
    readonly_unknown_padding();
    cross_field_edits_and_consumers();
    synchronization_and_calibration();
    preserved_timer_consumer();
    sim_destroy();
    printf("configuration: %u direct/proxy field and padding cases, round trips, cross-field edits, calibration/sync and safe mouse consumers passed\n", checks);
}
