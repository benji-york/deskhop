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
  src/keyboard.c src/usb.c src/reboot_hotkey.c tests/history_stub.c \
  -o /tmp/deskhop-hid-regressions-test
/tmp/deskhop-hid-regressions-test
```

The history recorder is an explicit no-op boundary in this isolated input
suite. Actual history capture is covered by the simulator's native boundary
suite; the console tests use the production ring with a native bridge.

The two warning exceptions accommodate existing production callbacks and loops.
Sanitizer failures terminate the suite. Coverage includes:

- Last-declared usage carry, repeated main inputs with no local usages, usage
  array exhaustion, and 1024-element report counts.
- Parser assignment and actual callback dispatch for all four receivers at IDs
  every ID 0 through 255 (0 means no ID byte); unknown IDs; the 24-distinct-report parser
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
mouse motion decoding. The new properties below exercise truncated boot/array
keyboard input; real mouse decoding is exercised by the paired/native-boundary
and host-stack layers.
The retained limits of four NKRO blocks and 24 distinct reports per interface
are asserted explicitly.


Generated input and scalar properties run as part of `python3 tests/run.py fast`
(2,000 descriptors) or `deep` (20,000). The runner retains
`build/tests/hid_properties` and `build/tests/last-hid-input.hex`:

```sh
build/tests/hid_properties --seed 4737348 --iterations 20000
build/tests/hid_properties --seed 4737348 --case 125 --artifact /tmp/hid-case.hex
build/tests/hid_properties --replay /tmp/hid-case.hex
build/tests/hid_properties --scalar
```

Seed + case reproduces the generated descriptor and report sequence. Hex replay
replays a descriptor with a fixed report-generation seed; it is not guaranteed
to recreate a different seed's report bytes. The harness writes the current
input before exercising it, so a sanitizer termination preserves that descriptor.
These are deterministic property workloads, not coverage-guided libFuzzer/AFL.
There is no claim to exhaust every possible descriptor.

The independent scalar oracle gathers bits one at a time and converts signed
values arithmetically. It checks offsets, exact allocation lengths and widths
0..40, including valid 32-bit fields. Generated descriptors mix changed valid
fixtures, truncation and arbitrary bytes, with bounded parser-state invariants.
Both report-only and keyboard-protocol interfaces reach real USB dispatch;
malformed descriptor rejection must survive the keyboard fallback path, while
explicitly negotiated boot protocol remains usable.

Production fixes discovered here bound short/long item reads, report field
reads and parser work, reject invalid report IDs/collection boundaries, and
prevent short keyboard reports from reading beyond their transfer. Descriptors
whose expanded input-element work exceeds 4,096 or whose bit offsets exceed
65,535 are rejected, with no partial report map used afterward. The four-NKRO-
block and 24-report-offset compatibility limits remain. Truncated NKRO behavior
still follows the retained partial-bitmap policy; these tests do not assert that
an incomplete NKRO transfer can never release an omitted held key.
