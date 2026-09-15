# Diagnostic console: incremental hardware validation

The agreed approach is six small firmware releases, with a hardware check after
each. Keep both RP2040 cores and their existing responsibilities. Diagnostics
use fixed memory and bounded work; input paths never format text or wait for a
terminal. Commands remain read-only.

## Release sequence

Before the peer-status slice, v0.96 added a small maintenance prerequisite after
the Mac storage-service panic: routine A/B bootloader shortcuts enter PICOBOOT
without a USB disk. It adds no console commands. Pico A's deployment and its
physical disk-free enumeration check passed; the record is in
[maintenance-v096.md](testing/maintenance-v096.md).

1. **Serial identity (v0.95, deployed):** normal-mode USB CDC, `help`, and
   `status` with local identity, executing version, boot metadata CRC, random
   boot-session ID, and uptime. Check terminal discovery, reconnect, slow readers,
   and keyboard/mouse operation.
2. **Peer status (v0.97, deployed and input checked):** extend `status`
   to query both boards by default through request-correlated UART replies.
   Print local information immediately; report
   unavailable/unsupported peers explicitly. Check actual A/B identities and
   fresh uptimes from one terminal.
3. **Local history (v0.98 deployed and input checked):** a small fixed RAM ring, initially 64 compact records for
   boot, output selection, USB attach/detach, and errors. Add `history [count]`.
   Check recognizable interactions, wraparound, and slow readers.
4. **Peer history (v0.99 deployed and input checked):** `history` collects both boards by default in bounded chunks.
   Interleave records in one list; every row includes the originating board and
   its sequence number. Align times to a common query reference, preserve each
   board's sequence order, and disclose approximate cross-board timing. Report
   overwrites/gaps; the requested count is per board. Exact clock synchronization
   and optional board filters are deferred.
5. **Update/boot observations (v0.100 deployed and input checked):** record receiving, validation, reboot pending,
   and new executing-build observations separately. Require new boot sessions
   after an observed update, then increasing uptime and progress on both cores.
6. **Image assurance:** `verify <expected-build>` checks both boards by default,
   with fresh observations and boot-time flash checksums compared against the
   expected artifact. Report checksum coverage; invalidate cached verification
   when flash changes. Results distinguish PASS, FAIL, and UNVERIFIED with reasons.

Additional events and counters follow troubleshooting needs discovered during
these sessions. Histories are volatile across reboot. No peer/history/verification
commands are advertised before their implementation exists.

## Update observations slice (v0.100 deployed and input checked)

`status` now includes both cores' diagnostic checkpoints and updater phase,
source, byte progress, target, and attempt. Fresh peer queries distinguish first
contact from a new boot and compare both core counters. RAM history records
sparse update milestones and peer observations. The [deployment record](testing/update-observations-v100.md)
describes the exact evidence required for target-version confirmation, legacy
protocol support, limits, and the physical flash/readback/serial results. Both boards now report
v0.100 with advancing core checkpoints; A's history captured B's old v0.99 boot,
new v0.100 boot, and later progress. Benji confirmed "Everything works" after the input and switch-and-back check
on both Macs. Observations are
query-driven and volatile; the first v0.99-to-v0.100 rollout cannot prove the
whole update retrospectively. Image integrity remains the next slice.

## Peer history slice (v0.99 deployed and input checked)

`history [count]` now collects both boards into one list.
Each row names its originating board and sequence; the requested count is per
board. Fixed snapshots merge by approximate age while preserving each board's
event order. A missing peer still permits local history. Capturing continues
under serial backpressure, and peer replies remain available while the local
terminal is paused. See the [deployment record](testing/peer-history-v099.md)
for the format, clock limitations, protocol, work bounds, and validation.

Pico A was flashed through disk-free PICOBOOT and reboot requested at
2026-09-15 00:09:48 UTC (September 14 locally). All 262,144 firmware bytes
matched an independent readback, and all 4,096 saved-configuration bytes were
unchanged. Only the vendor bootloader interface appeared; Mac checks found five
active, nonbusy media clients before and after, with no RP2 boot object after
reboot. Seven status snapshots confirmed both boards executing `0.99`, boot
CRC metadata `ce70e3d6`, stable distinct sessions, and increasing uptimes.

Four successful combined-history responses preserved the same five A records
and three B records across terminal reopen, with no gaps or overwrites. These
were boot, PC USB mount, and peripheral HID enumeration events; B's later boot
placed its events after A's in these particular responses. This establishes
live peer history retrieval, without independently verifying B's flash.
Benji confirmed "Everything works" after the requested typing, trackball/buttons,
and Layer 3 S switch-and-back check on both Macs. Replugging was not separately
reported. A subsequent `history 16` capture returned 13 A and 11 B records,
including eight output changes per board, with both directions and both local
and peer events on each. These physical rows interleaved without gaps or
overwrites. Their approximate ages do not establish cross-board causality or
identify the particular input action responsible for every transition.

## Local history slice (introduced and input checked in v0.98)

In v0.98, `history [count]` reads the connected board's recent RAM events, with a default
of 16 and a maximum of 64. Every event and gap row names its board. The command
header identifies the local scope and boot session; peer retrieval is the next
slice. A fixed sequence window and per-record reads let producers continue
while the terminal drains. Overwritten records are reported as gaps. The
[deployment record](testing/history-v098.md) describes event meanings, short
critical sections, limits, and interactive acceptance.

Pico A was flashed through disk-free PICOBOOT at 21:28 UTC on 2026-09-14.
All 262,144 firmware bytes matched an independent readback and all 4,096 saved
settings bytes were unchanged. Mac checks found no mass-storage interface
during maintenance and no retained RP2 object after reboot; five media clients
were active and not busy before and after. All seven serial status checks
confirmed both boards executing `0.98` with boot CRC metadata `4e15626f`,
stable distinct sessions, and increasing uptime. This confirms B's executing
identity, without an independent B flash readback or flash-integrity check.

Four local history reads returned the same six A-tagged records across terminal
reopen: boot, PC-facing USB mount, three peripheral HID interfaces, and an
accepted peer output change from A to B. The standard macOS `screen` terminal
also passed help, history, and two-board status. Benji confirmed "looks good"
after the requested input/switch/history check. The assistant did not capture
those later history rows independently. Peer history
retrieval is not implemented in this slice.

## Peer status slice (introduced in v0.97)

`status` now queries both boards by default. It opens one response frame and
prints the connected board immediately. A successful peer reply adds a second
`board=` block with that board's own identity, executing build, boot session,
boot metadata CRC, and uptime. For example:

```text
deskhop> status
BEGIN status
board=A
board_id=<A's physical flash UID>
build=0.97
image_crc_at_boot=<A's boot metadata>
boot_session=<A's session>
uptime_ms=<A's sampled uptime>

board=B
board_id=<B's physical flash UID>
build=0.97
image_crc_at_boot=<B's boot metadata>
boot_session=<B's session>
uptime_ms=<B's sampled uptime>
peer=ok
verification=not_implemented
END status
deskhop>
```

The placeholders illustrate the format introduced in v0.97. Its 2026-09-14
deployment confirmed both boards executing that version, as recorded below;
the current v0.99 observation is recorded above. With a terminal
on B, B prints first. Builds may differ during propagation. The peer snapshot
is taken when it accepts the request; the local snapshot is taken when the
command is processed. Their uptimes have independent boot origins and are not
timestamps on a shared clock.

An absent, old, or silent peer produces `peer=timeout_or_unsupported` without a
fabricated board block. A malformed matching reply produces `peer=invalid`;
an occupied request queue produces `peer=busy`. The frame still ends normally.
Each status uses a new token; closing/reopening the terminal discards that
terminal's partial output, and late replies cannot complete a later query.
No extra `both` command variant or board filter is needed.

Core 0 formats the console and communicates with core 1 through one-entry SDK
request/result queues. Core 1 owns all protocol state and services the new
task at 1 kHz. It emits at most one diagnostic UART packet per millisecond,
using a nonblocking enqueue. Each peer request has a 500 ms lifetime, including
time waiting in the cross-core request queue. A 600 ms console fallback avoids
waiting forever for a result; USB backpressure can still delay when text is
actually delivered. Commands queued by the terminal wait until the current
response finishes. There are no automatic retries after a request is queued.

The server accepts at most one new request per 200 ms and captures a fixed
snapshot for the entire reply. The client waits 200 ms after completion before
starting a queued query, so fast consecutive status commands remain valid.
The server transfer also expires after 500 ms if its TX queue cannot progress.
The peer service remains available when the local USB console is disabled.

UART types 36/37 carry a request token and 13 three-byte response chunks. The
39-byte snapshot encodes fields explicitly in little endian, with a protocol
version, board role, CRC32 over the first 34 bytes, and zero padding. Chunk
indices, duplicate consistency, role, token, padding, and CRC are checked before
accepting a result. This CRC protects diagnostic transport; it is not the flash
integrity check still planned for the later `verify` slice.

Pico A was flashed at 21:03 UTC on 2026-09-14 through the verified v0.96
PICOBOOT-only entry. All 262,144 firmware bytes matched an independent readback;
all 4,096 saved-configuration bytes were unchanged. The Mac's post-reboot check
found no retained RP2 object or inactive/busy media client. The first status
returned A immediately with an explicit peer timeout; a later query returned B.
Six subsequent status snapshots all returned `peer=ok`, both executing build
0.97, stable distinct sessions, increasing uptimes, and boot metadata CRC
`9a2b3827`. A's UID is `E6654854574C3E30`; B's is `E6654854577F2330`.

The physical serial check also passed fragmented and queued commands, a brief
application read pause, and terminal close/reopen. These observations confirm
B's executing identity and boot metadata, without an independent B flash
readback or flash-integrity check. Benji confirmed the ordinary input/switching
check on both Macs: "Working great. No replug needed." The terminal open/closed
phases were not separately reported. That accepted release preceded the v0.98
local-history deployment above. See the [v0.97 deployment record](testing/peer-status-v097.md) for
the sessions, evidence, and remaining checks.

## Console use and first-slice interpretation

The default firmware enables `DH_CONSOLE`, independently of `DH_DEBUG`. The
normal USB device retains its VID/PID, serial number, and HID report interfaces,
and adds a CDC ACM serial function. Configuration mode also includes the console.
The device descriptor advertises the composite IAD class for the CDC function.
macOS enumeration passed in the deployed releases. The v0.97 interactive
input and switching check passed with no replug needed.

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

The first, local-only v0.95 release produced the following format; v0.97 uses
the two-board status shown above.

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
