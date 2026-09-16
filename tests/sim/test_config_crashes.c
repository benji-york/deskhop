/* Durable bug #4 witnesses: production API ingestion followed by actual mouse
 * switching. Also compiled against the exact frozen predecessor by the runner. */
#include "main.h"
#include <assert.h>
#include <stdio.h>
void sim_init(uint8_t, void (*)(int, int, int, const void *, int));
void sim_destroy(void);
void do_screen_switch(device_t *, int);
static void observe(int kind, int a, int b, const void *data, int length) { }
static void set32(uint8_t field, uint32_t value) {
    uart_packet_t packet = {.type = SET_VAL_MSG, .data = {field}};
    memcpy(&packet.data[1], &value, sizeof(value));
    handle_api_msgs(&packet, &global_state);
}
int main(int argc, char **argv) {
    sim_init(0, observe);
    global_state.pointer_y = 16384;
    if (argc > 1 && strcmp(argv[1], "identity") == 0)
        set32(10, 3);
    else {
        set32(14, 16384);
        set32(15, 16384);
    }
    do_screen_switch(&global_state, LEFT);
    assert(global_state.config.output[0].number == 0);
    assert(global_state.config.output[0].border.top < global_state.config.output[0].border.bottom);
    assert(global_state.pointer_y >= 0 && global_state.pointer_y <= MAX_SCREEN_COORD);
    sim_destroy();
    puts("configuration API crash witness safely rejected");
}
