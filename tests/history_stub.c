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
