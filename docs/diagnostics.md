# Diagnostic console: incremental hardware validation

The agreed approach is six small firmware releases, with a hardware check after
each. Keep both RP2040 cores and their existing responsibilities. Diagnostics
use fixed memory and bounded work; input paths never format text or wait for a
terminal. Commands remain read-only.

## Release sequence

1. **Serial identity (v0.95 candidate):** normal-mode USB CDC, `help`, and
   `status` with local identity, executing version, boot metadata CRC, random
   boot-session ID, and uptime. Check terminal discovery, reconnect, slow readers,
   and keyboard/mouse operation.
2. **Peer status:** extend `status` to query both boards by default through
   request-correlated UART replies. Print local information immediately; report
   unavailable/unsupported peers explicitly. Check actual A/B identities and
   fresh uptimes from one terminal.
3. **Local history:** a small fixed RAM ring, initially 64 compact records for
   boot, output selection, USB attach/detach, and errors. Add `history [count]`.
   Check recognizable interactions, wraparound, and slow readers.
4. **Peer history:** `history` collects both boards by default in bounded chunks.
   Interleave records in one list; every row includes the originating board and
   its sequence number. Align times to a common query reference, preserve each
   board's sequence order, and disclose approximate cross-board timing. Report
   overwrites/gaps; the requested count is per board. Exact clock synchronization
   and optional board filters are deferred.
5. **Update/boot observations:** record receiving, validation, reboot pending,
   and new executing-build observations separately. Require new boot sessions
   after an observed update, then increasing uptime and progress on both cores.
6. **Image assurance:** `verify <expected-build>` checks both boards by default,
   with fresh observations and boot-time flash checksums compared against the
   expected artifact. Report checksum coverage; invalidate cached verification
   when flash changes. Results distinguish PASS, FAIL, and UNVERIFIED with reasons.

Additional events and counters follow troubleshooting needs discovered during
these sessions. Histories are volatile across reboot. No peer/history/verification
commands are advertised before their implementation exists.

## First slice: use and interpretation

The default firmware enables `DH_CONSOLE`, independently of `DH_DEBUG`. The
normal USB device retains its VID/PID, serial number, and HID report interfaces,
and adds a CDC ACM serial function. Configuration mode also includes the console.
The device descriptor advertises the composite IAD class for the CDC function.
macOS enumeration and Karabiner behavior still require a hardware check.

On macOS, find the new `/dev/cu.usbmodem*` device and open it with the existing
terminal program (replace the example path with the discovered one):

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodemEXAMPLE 115200
```

Use a terminal that asserts DTR, with local echo disabled (the firmware echoes).
The CDC line rate does not change the board-to-board UART baud rate.
`Ctrl-A`, then `K`, then `Y` exits `screen`. Closing the terminal does not reset
the Pico. USB suspend/resume or closing/reopening starts a fresh console session.

```text
deskhop> help
deskhop> status
BEGIN status
board=A
board_id=<physical flash UID>
build=0.95
image_crc_at_boot=<eight hex digits>
boot_session=<sixteen hex digits>
uptime_ms=<milliseconds since boot>
peer=not_implemented
verification=not_implemented
END status
deskhop>
```

The version is compiled into the executing code; UID, session, and image CRC are
captured before USB and core 1 start. The CRC is **metadata copied at boot**, not
a newly calculated checksum or evidence that either Pico has verified flash.
It is deliberately separate from the updater's version cache, which may already
describe newly written flash while the old firmware still executes in RAM.
The random 64-bit boot session is an operational identifier, not an attestation.

Commands accept CR, LF, or CRLF; Backspace/DEL edits and Ctrl-C cancels the line.
The maximum command length is 63 printable ASCII characters. Invalid/overlong
lines are discarded through a line ending, then reported; suffixes cannot become
commands. A close/unmount discards partial input and output still held by the
console/FIFO. Bytes already submitted to USB may still arrive at the host.

One core 0 task runs after input/UART tasks, at most once per millisecond. Each
invocation reads at most 32 bytes, completes at most one command, and writes at
most 64 bytes to TinyUSB. A fixed 1 KiB response buffer applies backpressure when
the host stops reading. There is no USB wait loop or recursive `tud_task()` call.
Debug printf output is suppressed while the console owns CDC. The legacy debug
bootloader command requires `DH_DEBUG=ON`, `DH_CONSOLE=OFF`, and
`DH_DEBUG_CDC_FLASH=ON`; it is absent from the production console.

## Hardware acceptance for the first slice

After native regressions and an ARM build pass, flash Pico A with the higher
version and let peer propagation settle. Record the artifact hashes, direct
firmware/config readback, and observed device identity using the existing runbook
procedure. Then:

1. Find the serial device and run `help` and `status`.
2. Query repeatedly: boot session stays fixed and uptime increases.
3. Close/reopen: partial commands disappear and boot identity remains unchanged.
4. Type and move the mouse on both outputs while requesting status; pause reading
   and check that keyboard, buttons, pointer motion, and switching still work.
5. Once updates have settled, perform the existing coordinated reboot gesture;
   reconnect and confirm a new session with low uptime.

The first slice can inspect B through B's own host serial port; querying B from
A's terminal starts in slice 2. An A-only status or readback never verifies B.

## Automated coverage

The real TinyUSB device harness exercises the CDC descriptors and class/control
requests along with the production parser, serial buffers, and HID endpoints.
The paired simulator retains its modeled USB boundary with CDC disabled: the
new console task is inert there, representing operation without a terminal.
It extracts task grouping and names from production tables so the additional
core 0 task does not shift the meaning of core 1 regression steps or older
baseline comparisons. Actual host drivers, USB bus timing, and perceived input
smoothness remain hardware checks.
