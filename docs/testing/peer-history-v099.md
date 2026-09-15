# v0.99 peer-history deployment

v0.99 is deployed and both boards report executing it. `history [count]` now
retrieves both boards' records by default. The flash/readback and physical
serial/history checks passed. Benji confirmed "Everything works" after the
requested typing, trackball/buttons, and Layer 3 S switch-and-back check on
both Macs. The previous accepted release is v0.98,
locally checkpointed as `8560d5f`; see its [deployment record](history-v098.md).

## Deployment and physical observations

Pico A was flashed through disk-free PICOBOOT and normal reboot was requested
at **2026-09-15 00:09:48 UTC** (September 14 locally). The 00:08:29 UTC preflash
check found one bootloader with only vendor interface class 255, no mass-storage
interface, and five media clients, all active and not busy. Official picotool
selected A by UID `E6654854574C3E30`. Its preflash image matched the frozen
v0.98 binary. An independent postflash readback matched all 262,144 bytes of
the frozen v0.99 image; all 4,096 saved-configuration bytes remained unchanged.
At 00:10:02 UTC, no RP2 boot object remained and all five media clients were
active and not busy.

The bounded read-only checker passed at 00:10 UTC on
`/dev/cu.usbmodem21203`, receiving 8,577 bytes in 5.731 seconds. All seven
status snapshots returned `peer=ok` with stable distinct identities/sessions
and increasing uptimes:

| Board | Physical flash UID | Executing build | Boot session | Sampled uptime range |
| --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `0.99` | `bf0cb9dbdd5d6c39` | 34,948–39,868 ms |
| B | `E6654854577F2330` | `0.99` | `5b8f298bd669b78a` | 14,192–19,112 ms |

Both reported boot CRC metadata `ce70e3d6`. This establishes B's executing
identity/build and fresh boot metadata, not an independent readback or
integrity check of B's flash. `verification=not_implemented` remains explicit.

Four successful combined-history reads (`history`, `history 16`, `history 64`,
and `history 16` after reopening the terminal) returned these same five A
records and three B records. Both overwrite counters were zero; no gaps were
present. Each history boot session matched that board's status session, and
later snapshot uptimes increased while retained event contents stayed fixed.

| Board | Sequence | Boot-relative uptime | Event | Details |
| --- | --- | --- | --- | --- |
| A | 1 | 17 ms | `boot` | Build 0.99, initial output A |
| A | 2 | 295 ms | `usb_mount` | PC-facing USB enumerated |
| A | 3 | 543 ms | `hid_mount` | Device 1, instance 0, protocol 1; keyboard |
| A | 4 | 548 ms | `hid_mount` | Device 1, instance 1, protocol 0; neither keyboard nor mouse classification |
| A | 5 | 555 ms | `hid_mount` | Device 1, instance 2, protocol 0; keyboard and mouse |
| B | 1 | 35 ms | `boot` | Build 0.99, initial output A |
| B | 2 | 443 ms | `usb_mount` | PC-facing USB enumerated |
| B | 3 | 576 ms | `hid_mount` | Device 1, instance 0, protocol 2; trackball mouse interface |

B booted later, so these initial merged responses list A's events followed
by B's. After Benji confirmed "Everything works" for the input and switch check,
a separate read-only capture at **00:12:17 UTC** passed with the same builds,
CRC metadata, and boot sessions. Status uptimes reached 148,264 ms on A and
127,507 ms on B.

That `history 16` response contained 13 A records and 11 B records, including
16 output-change records: eight from each board, with both A-to-B and B-to-A
changes and both `output_local` and `output_peer` events on each. The physical
capture interleaved B and A rows, with no overwrites or gaps and a peer capture
bound of 24,320 microseconds. Ages remain approximate: an observer's row can
precede its origin's row by 1 ms after alignment. The merged display therefore
does not establish cross-board causality, and these sparse events cannot
attribute every transition to a particular key press or mouse-edge crossing.

Fragmented and queued status commands, exact invalid-count rejection, and
partial-command cleanup across close/reopen passed. Brief application read
pauses completed with intact responses, but OS/USB buffering may have continued;
this does not establish forced physical USB backpressure or input smoothness.
The checker closed the serial device without cleanup errors.

Evidence: `build/flashing/pico-a-v099-result.json`,
`console-v099-smoke-20260915T001024.062005Z.json` and its `.txt` transcript,
`console-v099-switch-check.json` and its `.txt` transcript,
`pico-a-preflash-v099-health.json`, and `pico-a-postreboot-v099-health.json` in
the same directory. The v0.99 changes have not been pushed.

## Console behavior

The default is 16 records per board; accepted counts are 1–64. A successful
response contains one interleaved list, with every event and gap labeled A or
B and its board-local sequence. The header gives each board's boot session,
snapshot uptime, selected record count, and overwrite counter. `returned`
counts selected slots, including any explicit gaps. No board filter or `both`
variant is required.

The following abbreviated example illustrates the format, not a hardware
capture. Here A booted earlier, so its uptime differs substantially from B's:

```text
deskhop> history 2
BEGIN history
scope=both
requested_per_board=2
timing=approximate_snapshot_alignment
capacity_per_board=64
board=A
boot_session=1111111111111111
sampled_uptime_ms=50000
returned=2
overwritten=0

board=B
boot_session=2222222222222222
sampled_uptime_ms=30000
returned=2
overwritten=0
peer=ok
peer_capture_bound_us=12000

board=A seq=7 uptime_ms=49000 age_ms=1000 event=output_local old=A new=B
board=B seq=4 uptime_ms=29001 age_ms=999 event=output_peer old=A new=B
board=A seq=8 uptime_ms=49900 age_ms=100 event=output_local old=B new=A
board=B seq=5 uptime_ms=29901 age_ms=99 event=output_peer old=B new=A
END history
deskhop>
```

Each board's event age is its snapshot uptime minus the event uptime. The
console merges by age, oldest first, preserving each board's sequence order.
Equal microsecond ages use A first, regardless of where the terminal connects.
Displayed milliseconds are truncated, so equal displayed ages need not be
exact ties. The two snapshot instants are treated as a common reference;
cross-board order is approximate and does not prove which board acted first.

`peer_capture_bound_us` is the local elapsed time from command start to the
first matching peer response chunk. The peer freezes its sequence window
between the request and that first response, so this is a conservative bound
on capture skew. It includes scheduling and capture work. It is not half the
round-trip time, which also includes serialization of the whole snapshot.
No attempt is made to synchronize the boards' clocks or infer causality.

Missing records produce `GAP board=A seq=...` (or B) without a fabricated
timestamp. A gap prints when it reaches the head of its board's stream. The
header overwrite counter describes the ring when its window was selected;
new overwrites during capture appear as gaps. Selecting fewer retained records
does not count as data loss. Captured records remain unchanged while printing,
even if the live ring subsequently wraps. New events do not extend a response.

An unavailable or older peer produces `peer=timeout_or_unsupported`; malformed
matching data produces `peer=invalid`; a full request queue produces `peer=busy`.
These outcomes still print the captured local history and close the frame.
They contain no fabricated peer metadata or rows. History remains volatile
across board reset, and terminal reconnect preserves the live ring.

## Work bounds and ownership

The existing core assignments and task tables stay intact. Core 0 owns console
capture and formatting. Core 1 serves peer requests and assembles responses.
The 64-record live ring still uses brief lock scopes around counters or one
24-byte record. Selecting a window also samples its clock under that lock.
There is no whole-ring copy, formatting, USB operation, or queue wait under
the history lock.

Core 0 copies at most one local record each console tick, before checking USB
output backpressure. Core 1 captures at most one server record per tick,
computes at most 64 CRC bytes per direction per tick, and validates at most
one received record per tick. The console emits at most one event/gap row per
tick; its existing 32-byte RX and 64-byte TX limits remain.

The peer module has independent server and client storage. A completed client
result is published through a one-entry queue containing only a pointer and
token. Core 0 borrows that immutable result until the final row or disconnect,
then returns its token through a release queue. It never copies the whole
snapshot through an SDK queue lock. Core 1 can continue serving the other
board while its own terminal holds a result. Startup allocates only the small
SDK queues; snapshot storage is static and commands allocate no memory.

Status and history share one diagnostic UART enqueue attempt per millisecond,
including failed attempts on a full queue. First service alternates between
the two protocols. The transfer also alternates request/response priority when
both directions need service. Normal input remains on its existing queue and
scheduler. These bounds prevent diagnostic flooding; native tests do not
establish physical interrupt latency or a hardware smoothness guarantee.

The protocol client and server each have a three-second deadline; the client
deadline starts at the original core-0 request, including queue wait. A
3.5-second console fallback bounds waiting if core 1 cannot report a result.
USB backpressure can delay delivery of text. Requests are not automatically
retried. After a completed local query, a 200 ms cooldown accommodates the
server's request limit. Closing a terminal discards its partial output; stale
results are released and cannot complete a later command.

## Wire contract

UART type 38 requests a snapshot. Its eight bytes contain a little-endian
32-bit query token, protocol version 1, requested count, and two zero reserved
bytes. Type 39 carries the token, a little-endian 16-bit chunk index, and two
snapshot bytes. These packet types remain excluded from direct and proxied
USB configuration commands.

For a requested count N, the wire snapshot is exactly `64 + 24*N + 4` bytes:
a 64-byte header, N event slots, and CRC32. The requested count fixes framing
before any response arrives. The header contains protocol, role, actual count,
record size, reserved zeros, boot session, snapshot uptime, first/exclusive-end/
oldest sequences, overwrite count, and a 64-bit gap mask. All multi-byte values
are explicitly little endian. Event slots retain the 24-byte history layout.
Unused and missing slots are zero. The maximum is 1,604 bytes / 802 chunks;
the default request is 452 bytes / 226 chunks.

The client checks the token, index bounds, duplicate consistency, opposite
board role, version, reserved values, CRC, ring/sequence invariants, gap bits,
event sequence/type/time, and zero unused slots. Reordered and identical
duplicate chunks are accepted. Missing data times out. Conflicting duplicates
or malformed matching data invalidate the query. Responses before the request
was queued and stale tokens are ignored. Tokens are not reused during ordinary
operation; deduplication remembers only the last accepted server token.

The CRC protects this diagnostic message. It does not verify firmware flash.
`status` still reports `verification=not_implemented`; update-phase events and
image verification remain later slices.

## Software validation

The final source passed all 39 recorded deep-tier steps, the ARM configure/build,
and all six native source-coverage layers on 2026-09-14. The deep run includes:

- Pure protocol tests under ASan/UBSan with an independent CRC/wire oracle,
  both roles and counts 1/16/64, reordered/duplicate chunks, 452 single-byte
  corruptions, repaired-CRC structural violations, every missing short-frame
  chunk, gaps, actual ring overwrite during capture, deadlines, and borrowed
  result preservation while serving another query.
- All 53 paired production scenarios, including seven peer-history scenarios:
  simultaneous bidirectional transfers with status/HID traffic, result borrowing,
  stale/in-flight abandoned queries, capture gaps, disconnected/full-queue
  deadlines and recovery, and malformed replies. Actual UART enqueue attempts,
  including refusals, obeyed the shared millisecond budget.
- Real TinyUSB console tests for 128 rows, unequal boot clocks, exact ties,
  gaps, frozen captures under CDC backpressure, peer failures, reconnect,
  borrowed-result release, and continued HID traffic. Per-tick RX/TX/capture
  budgets remained bounded. These tests mock peer results; the paired suite
  exercises the real cross-core bridge and transport separately.
- Native sanitizer boundaries, including checksum-valid direct/proxied USB
  rejection of the new packet types; 24 fixed simultaneous core-priority
  orders each for backpressure, peer status, and peer history; 32 generated
  input seeds; all 11 storage and 15 paired mutation checks; and nine baseline
  contracts (four exact effect traces, five per-host comparisons with at most
  500 microseconds of modeled timing drift).

Review found that changing request/response preference after a refused shared
UART slot could repeatedly deny requests while granting replies. Both status
and history now keep their preferred direction until a packet is accepted.
New tests with alternating external TX grants fail against the old behavior
and pass with the fix. The final deep and coverage runs were repeated after
this correction.

The ARM linker reports 195,300 bytes of the 262,144-byte main RAM region used
(74.50%), up 14,992 bytes from v0.98. This includes code copied to RAM as well
as data. Core 1 retains its 2 KiB scratch stack. Compiler-reported individual
frames are 192 bytes for `console_task`, 152 for `peer_history_task`, and at
most 80 for the new bridge. These are individual frames, not a measured stack
high-water mark or a physical timing result. The maximum console metadata
block is 305 bytes, within its existing 1 KiB output buffer.

Evidence: `build/tests/deep-v099.log`, `results-deep-v099.json`,
`arm-v099-final.log`, `coverage-v099.log`, and
`coverage-v099/html/index.html`. Coverage retains separate source denominators;
the current figures are in the [coverage inventory](coverage.md).

The frozen artifact is `build/flashing/deskhop-v0.99-peer-history.uf2`, SHA-256
`0aa74de576024944c7c8195be922bd5dcc48c7f9826b5c528884381f127db9d7`.
Its binary SHA-256 is
`7075e8134afa64bbf668ac93e6824de864931628b154780f85137cef3fa4c4bb`;
encoded version is 199 and boot CRC metadata is `ce70e3d6`. Offline validation
recomputed the image CRC and reconstructed the binary from all 1,024 UF2
blocks. The image contains exactly 262,144 firmware bytes, excludes saved
configuration, and was used for the deployment above.
`build/flashing/v099-candidate.json` records artifact/source hashes, validation
paths, and the deployment result; `hardware_flashed` is true.

The read-only hardware checker is
`build/flashing/check_console_v099.py`; its offline self-tests reject 14
malformed status frames and 30 malformed combined-history frames. It checks
seven status snapshots and four combined histories, including sessions,
retained records, ordering, rounding, and reconnect. Its port and expected
CRC are explicit; it is bounded to 30 seconds and 64 KiB. Its successful
physical run is recorded above. These ignored local build artifacts supplement
the tracked tests.

Benji's "Everything works" response confirms the requested typing, trackball
movement/buttons, and Layer 3 S switching to the other Mac and back. Replugging
was not separately reported. The subsequent history capture verified both
switch directions on both boards, within the attribution and clock limits above.
