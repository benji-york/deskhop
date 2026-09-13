The HID regression suite compiles the production `hid_parser.c`, `hid_report.c`,
`keyboard.c`, `usb.c`, and `reboot_hotkey.c` directly. `main.h` replaces the
hardware dependency umbrella with native declarations and a minimal device state.
The production HID structures, packet constants, and TinyUSB HID definitions are
used unchanged. Queue, mouse, clock, and hardware callbacks are instrumented in
the test; descriptor parsing, keyboard extraction, consumer conversion/activity,
and USB receiver dispatch execute production code.

Run from the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-sign-compare \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Itests/hid_stubs -Isrc/include -Ipico-sdk/lib/tinyusb/src \
  tests/test_hid_regressions.c src/hid_parser.c src/hid_report.c \
  src/keyboard.c src/usb.c src/reboot_hotkey.c \
  -o /tmp/deskhop-hid-regressions-test
/tmp/deskhop-hid-regressions-test
```

The two warning exceptions accommodate existing production callbacks and loops.
Sanitizer failures terminate the suite. Coverage includes:

- Last-declared usage carry, repeated main inputs with no local usages, usage
  array exhaustion, and 1024-element report counts.
- Parser assignment and actual callback dispatch for all four receivers at IDs
  0 (no ID byte), 23, 24, and 255; unknown IDs; the 24-distinct-report parser
  limit; zero-length reports; and invalid device/interface indices.
- Consumer variable/array reports with and without IDs, ID-only/empty payload
  rejection, short zero-filled conversion, output-size clamping, release reports,
  and real-activity recording only for accepted payloads.
- System-control empty/ID-only rejection and accepted-payload activity with and
  without report IDs.
- Four split NKRO ranges, an unaligned 8-bit block, padding, exact 32-bit versus
  greater-than-32-bit aggregate detection, six-key truncation, and an exact-sized
  short input allocation checked by ASan.

This does not exercise physical USB timing, hardware queues, UART transport,
mouse motion decoding, or boot/ordinary-array malformed-payload extraction.
The retained limits of four NKRO blocks and 24 distinct reports per interface
are asserted explicitly.
