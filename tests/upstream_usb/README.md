# Upstream USB regressions

Run `python3 tests/upstream_usb/run.py` (also part of the fast suite), or add
`--mutations` to verify six deliberately broken production-body variants are
rejected. No device, serial port, or libusb connection is opened.

## Provenance

The integration preserves the vendor changes verbatim from DeskHop upstream:

- [801d388 / #369](https://github.com/hrvach/deskhop/commit/801d388b7bca0157d4c5e4610e2d9ca9e069c8ed):
  use volatile byte accesses for both directions of USB DPRAM buffer copies.
  This avoids compiler-widened, potentially unaligned memory accesses.
- [9782d5b / #370](https://github.com/hrvach/deskhop/commit/9782d5b05f55a7f7545748ba92940617e2c84039):
  retry failed PIO USB transactions until the third consecutive failure before
  terminating the transfer; reset the count on a sound response or new transfer.
  Failed SETUP transactions restore the SETUP marker and do not claim eight
  transferred bytes. Upstream identifies original Pico-PIO-USB backports as
  `9c8df30`, `92ea116`, and `ace22e0`.

This is three attempts total, **not** three additional attempts after the first.
NAK and duplicate IN data-toggle responses are not transaction failures; STALL
still ends the transfer immediately.

## Execution seam and coverage

The Python runner extracts named C definitions **verbatim from the current
vendor sources**, using balanced braces and original line directives, then
compiles them under AddressSanitizer and UndefinedBehaviorSanitizer. Production
files are never rewritten. The test uses the real `endpoint_t` header and real
`hw_endpoint_t` definition, along with the actual:

- transfer start, continue, completion, transaction-length and TX preparation;
- IN, OUT, and SETUP transaction functions;
- DPRAM byte-copy, prepare-buffer and synchronize-buffer functions.

Scripted responses replace the physical bus receive/transmit functions and PIO
registers. CRC calculation is a deterministic stub, not a CRC implementation
test. Assertions cover failure thresholds, timeout distinction, resetting a
failure streak, short packets, successful multiple-packet transfers, duplicate
IN data, NAK, STALL, exact SETUP retry payload/toggle, per-endpoint isolation,
preserving a busy transfer, and restarting a completed transfer.

DPRAM tests vary both pointer offsets through 0–7 and lengths through 0–129,
check sentinels, and exercise buffer 0/1 copy paths and short/full endpoint
packets. If `ARM_CC`, an `arm-none-eabi-gcc` on PATH, or the project's Homebrew
ARM compiler is available, the actual copy helper is also compiled at `-O3`
for Cortex-M0+. The emitted load/store instructions must all be byte-sized;
this checks the compiler-width property ordinary host memory cannot reproduce.
The runner explicitly prints a skip if no ARM compiler is available.

The optional mutation run reverts the failure threshold, transfer-start count
reset, IN success reset, SETUP retry marker, SETUP error byte count, and DPRAM
buffer selection one at a time in temporary generated definitions. Every variant
must compile and then fail a C assertion; compile errors are not counted as
successful mutation detection.

## Limits

This is a focused host test, not a complete USB controller emulator. It does
not execute PIO instructions, model electrical/clock timing, reproduce RP2350
DPRAM faults, exercise real hardware enumeration, or prove endpoint scheduling
and cross-core behavior. Retry dispatch uses the unchanged production scheduler;
the test checks the SETUP marker it consumes, not the whole frame scheduler.
The full ARM firmware build remains necessary, followed by real-device checks
of keyboard and trackball enumeration, warm reboots, and both-host input paths
before this integration is treated as hardware-accepted.
