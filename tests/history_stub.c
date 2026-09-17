/* Explicit diagnostic boundary for isolated HID/USB-host tests. These tests
 * exercise production input behavior, not history capture. The simulator and
 * console tests compile the real store and test recorded events separately. */
#include "main.h"
#include "diagnostic_history.h"
#include "config_confirm.h"
#include "clipboard.h"
#include <assert.h>

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
unsigned hid_boundary_lock_entries;
unsigned hid_boundary_lock_depth;
void firmware_update_lock(void) {
    assert(hid_boundary_lock_depth == 0);
    ++hid_boundary_lock_entries;
    hid_boundary_lock_depth = 1;
}
void firmware_update_unlock(void) {
    assert(hid_boundary_lock_depth == 1);
    hid_boundary_lock_depth = 0;
}

/* Clipboard transactions have separate real-runtime unit/paired tests. These
 * parser/host-stack suites retain their existing HID translation boundary. */
bool clipboard_keyboard_raw(hid_interface_t *iface, const hid_keyboard_report_t *report,
                            device_t *state) {
    (void)iface; (void)report; (void)state;
    return false;
}
void clipboard_keyboard_incomplete(hid_interface_t *iface, const hid_keyboard_report_t *report,
                                   device_t *state) { (void)iface; (void)report; (void)state; }
void clipboard_physical_disconnect(hid_interface_t *iface, device_t *state) {
    (void)iface; (void)state;
}
void clipboard_physical_unknown(hid_interface_t *iface, device_t *state) {
    (void)iface; (void)state;
}
void clipboard_host_reset(device_t *state) { (void)state; }
void clipboard_usb_session_reset(device_t *state) { (void)state; }
void clipboard_host_led_report(device_t *state) { (void)state; }
void clipboard_usb_task(device_t *state) { (void)state; }
void clipboard_remote_input(device_t *state) { (void)state; }
void clipboard_cancel(void) {}
bool config_confirm_usb_request(const uart_packet_t *packet, device_t *state) {
    (void)packet;
    (void)state;
    return false;
}
void config_confirm_usb_disconnect(void) {}
