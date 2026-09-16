/* Exercise the real heartbeat task at the boot-grace boundary with ASan/UBSan.
 * These are queue/payload oracles, independent of the paired wire simulator. */
#include "main.h"
#include <assert.h>

void sim_init(uint8_t, void (*)(int, int, int, const void *, int));
void sim_destroy(void);
void sim_set_time(uint64_t);

static void observe(int kind, int a, int b, const void *data, int len) {
    /* This task may enqueue packets, never sleep/yield, write flash or reset. */
    assert(!"unexpected external effect from heartbeat task");
}

static void check_heartbeat(bool advertise) {
    uint64_t before = time_us_64();
    fw_upgrade_state_t fw_before = global_state.fw;
    heartbeat_output_task(&global_state);
    assert(time_us_64() == before);
    assert(memcmp(&fw_before, &global_state.fw, sizeof(fw_before)) == 0);
    if (global_state.fw.source == FW_UPDATE_SOURCE_DROP) {
        assert(queue_is_empty(&global_state.uart_tx_queue));
        return;
    }

    const uint8_t types[] = {HEARTBEAT_MSG, ACTIVITY_MSG, MOUSE_BUTTONS_SYNC_MSG,
        MODIFIER_STATE_MSG, ZOOM_ASSIST_MSG, OUTPUT_SELECT_SYNC_MSG};
    for (unsigned i = advertise ? 0 : 1; i < sizeof(types); ++i) {
        uart_packet_t packet;
        assert(queue_try_remove(&global_state.uart_tx_queue, &packet));
        assert(packet.type == types[i]);
        switch (packet.type) {
        case HEARTBEAT_MSG:
            assert(packet.data16[0] == 211);
            assert(packet.data16[1] == FW_UPDATE_PROTOCOL_MARKER);
            assert(packet.data32[1] == UINT32_C(0x12345678));
            break;
        case ACTIVITY_MSG:
            assert(packet.data32[0] == 0); /* Fresh local activity. */
            assert(packet.data32[1] == UINT32_MAX); /* Never seen. */
            break;
        case MOUSE_BUTTONS_SYNC_MSG:
            assert(packet.data[0] == 2 && packet.data[1] == 0);
            break;
        case MODIFIER_STATE_MSG:
            assert(packet.data[0] == KEYBOARD_MODIFIER_LEFTGUI);
            break;
        case ZOOM_ASSIST_MSG: {
            zoom_assist_sync_t sync;
            memcpy(&sync, packet.data, sizeof(sync));
            assert(sync.output == global_state.board_role);
            assert(sync.active && !sync.exit_pending && sync.debt == 7);
            break;
        }
        case OUTPUT_SELECT_SYNC_MSG: {
            selection_state_t selection;
            assert(selection_decode(packet.data, &selection));
            assert(selection.counter == 42 && selection.output == OUTPUT_B);
            assert(selection.origin == global_state.board_role);
            break;
        }
        }
    }
    assert(queue_is_empty(&global_state.uart_tx_queue));
}

int main(int argc, char **argv) {
    assert(argc == 2 && (argv[1][0] == '0' || argv[1][0] == '1'));
    uint8_t role = (uint8_t)(argv[1][0] - '0');
    sim_init(role, observe);
    assert(!global_state.keyboard_connected && !global_state.mouse_connected);
    global_state._running_fw.version = 211;
    global_state._running_fw.checksum = UINT32_C(0x12345678);
    global_state.local_mouse_buttons = 2;
    global_state.local_modifiers = KEYBOARD_MODIFIER_LEFTGUI;
    global_state.zoom_assist[role].active = true;
    global_state.zoom_assist[role].debt = 7;
    global_state.selection = (selection_state_t){.counter = 42, .origin = role,
        .output = OUTPUT_B, .joined = true};
    global_state.direct_activity_valid = 1;

    /* Literal expectations catch accidental changes to the promised one second.
     * The last case ensures the grace does not restart on 32-bit clock wrap. */
    const uint64_t times[] = {0, 999999, 1000000, 1000001, UINT64_C(4294967296) + 1};
    for (unsigned t = 0; t < sizeof(times) / sizeof(times[0]); ++t) {
        sim_set_time(times[t]);
        global_state.direct_activity[0] = times[t];
        bool advertise = t >= 2;
        for (unsigned source = FW_UPDATE_SOURCE_NONE; source <= FW_UPDATE_SOURCE_DROP; ++source) {
            global_state.fw.source = source;
            global_state.fw.upgrade_in_progress = source != FW_UPDATE_SOURCE_NONE;
            check_heartbeat(advertise);
        }

        global_state.fw.source = FW_UPDATE_SOURCE_NONE;
        global_state.fw.upgrade_in_progress = false;
        global_state.config_mode_active = true;
        global_state.config_mode_timer = UINT64_MAX;
        global_state.blinks_left = 0;
        check_heartbeat(advertise);
        assert(global_state.blinks_left == 5);
        global_state.blinks_left = 0;
        global_state.maintenance_reserved = true;
        check_heartbeat(advertise);
        assert(global_state.blinks_left == 0);
        global_state.maintenance_reserved = false;
        global_state.config_mode_active = false;

        /* Full admission must not block, overwrite old packets, or suppress a
         * later advertisement once normal queue service becomes available. */
        uart_packet_t sentinel = {.type = ACTIVITY_MSG, .data32 = {123, 456}};
        for (unsigned i = 0; i < UART_QUEUE_LENGTH; ++i)
            assert(queue_try_add(&global_state.uart_tx_queue, &sentinel));
        heartbeat_output_task(&global_state);
        assert(time_us_64() == times[t]);
        for (unsigned i = 0; i < UART_QUEUE_LENGTH; ++i) {
            uart_packet_t packet;
            assert(queue_try_remove(&global_state.uart_tx_queue, &packet));
            assert(memcmp(&packet, &sentinel, sizeof(packet)) == 0);
        }
        assert(queue_is_empty(&global_state.uart_tx_queue));
        check_heartbeat(advertise);
    }
    sim_destroy();
    printf("Firmware advertisement role %c: boundary, state sync, no peripherals, pull/drop, config, maintenance and full queue passed\n", 'A' + role);
    return 0;
}
