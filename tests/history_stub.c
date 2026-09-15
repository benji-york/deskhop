/* Explicit diagnostic boundary for isolated HID/USB-host tests. These tests
 * exercise production input behavior, not history capture. The simulator and
 * console tests compile the real store and test recorded events separately. */
#include "diagnostic_history.h"

void diagnostic_history_record(history_type_t type, uint8_t a, uint8_t b, uint32_t value) {
    (void)type;
    (void)a;
    (void)b;
    (void)value;
}

/* Isolated HID/host-stack tests do not link the paired keyboard transport or
 * multicore HAL. Their USB queue/source behavior remains production C. */
void keyboard_sync_publish(void) {}
void keyboard_sync_reset(void) {}
void firmware_update_lock(void) {}
void firmware_update_unlock(void) {}
