/* Clipboard history contains only these fixed labels. No text, length, CRC,
 * nonce, session ID, HID key report or clipboard-derived value is recorded. */
#pragma once

#define CLIPBOARD_DIAGNOSTIC_PHASES(X) \
    X(TRIGGER, "trigger") X(ADMITTED, "admitted") X(REJECTED, "rejected") \
    X(PULL, "pull") X(OFFER, "offer") X(GRANT, "grant") X(RELEASE, "release") \
    X(HELPER_OPEN, "helper_open") X(HELPER_CLOSE, "helper_close") \
    X(HELPER_REQUEST, "helper_request") X(HELPER_RESULT, "helper_result") \
    X(PAYLOAD_READY, "payload_ready") X(TYPING, "typing") X(DONE, "done") \
    X(CANCELLED, "cancelled")

#define CLIPBOARD_DIAGNOSTIC_REASONS(X) \
    X(NONE, "none") X(UNINITIALIZED, "uninitialized") \
    X(HOST_DISCONNECTED, "host_disconnected") X(SUSPENDED, "suspended") \
    X(CONFIG_MODE, "config_mode") X(MAINTENANCE, "maintenance") \
    X(UPDATE_ACTIVE, "update_active") X(FOCUS_INVALID, "focus_invalid") \
    X(LED_UNKNOWN, "led_unknown") X(CAPS_ON, "caps_on") X(BUSY, "busy") \
    X(HELPER_MISSING, "helper_missing") X(INPUT_HELD, "input_held") \
    X(COUNTER_LIMIT, "counter_limit") X(NON_BARE, "non_bare") \
    X(INCOMPLETE_REPORT, "incomplete_report") X(FOCUS_CHANGED, "focus_changed") \
    X(USB_CHANGED, "usb_changed") X(HELPER_EXPIRED, "helper_expired") \
    X(PEER_EXPIRED, "peer_expired") X(DEADLINE, "deadline") \
    X(PHYSICAL_INPUT, "physical_input") X(HELPER_CLOSED, "helper_closed") \
    X(PEER_RESTARTED, "peer_restarted") X(PROTOCOL, "protocol") \
    X(BINDING, "binding") X(STALE, "stale") X(EMPTY, "empty") \
    X(NON_TEXT, "non_text") X(OVERSIZE, "oversize") \
    X(UNSUPPORTED, "unsupported") X(UNAVAILABLE, "unavailable") \
    X(PEER_CANCELLED, "peer_cancelled") X(PAYLOAD_INVALID, "payload_invalid") \
    X(HOST_RESET, "host_reset")

#define CLIP_DIAG_ENUM(name, text) CLIP_DIAG_##name,
typedef enum { CLIPBOARD_DIAGNOSTIC_PHASES(CLIP_DIAG_ENUM) } clipboard_diagnostic_phase_t;
#undef CLIP_DIAG_ENUM
#define CLIP_REASON_ENUM(name, text) CLIP_REASON_##name,
typedef enum { CLIPBOARD_DIAGNOSTIC_REASONS(CLIP_REASON_ENUM) } clipboard_diagnostic_reason_t;
#undef CLIP_REASON_ENUM
