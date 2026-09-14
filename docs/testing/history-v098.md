# v0.98 local RAM-history deployment

Pico A was flashed and rebooted on 2026-09-14 at 21:28 UTC. Both boards now
report executing v0.98, and the physical serial/history checks passed. Benji confirmed "looks good" after the requested post-flash typing,
trackball/buttons, and Layer 3 S switch-and-back/history check. The previously accepted v0.97
release needed no trackball replug; its local checkpoint is `f2463d1`.

## Deployment and physical observations

Layer 3 A entered disk-free PICOBOOT. The 21:27:34 UTC Mac check found only
the vendor interface (class 255), no mass-storage interface, and five media
clients, all active and not busy. Official picotool selected A by physical
flash UID `E6654854574C3E30`. After loading, an independent readback matched
all 262,144 firmware bytes to the frozen binary, and all 4,096 saved-settings
bytes were unchanged. Normal reboot was requested at 21:28:57 UTC. At
21:29:16 UTC, no RP2 boot object remained and the same five media clients were
active and not busy. These checks do not establish that the earlier Mac panic
cannot recur.

The bounded read-only checker passed at 21:29 UTC on
`/dev/cu.usbmodem21203`. All seven status responses returned `peer=ok`, these
stable distinct identities/sessions, and increasing uptimes:

| Board | Physical flash UID | Executing build | Boot session | Sampled uptime range |
| --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `0.98` | `4d718a3937cb36d7` | 31,765–35,140 ms |
| B | `E6654854577F2330` | `0.98` | `19877e4a63cdaeae` | 11,086–14,461 ms |

Both reported boot CRC metadata `4e15626f`. B's result confirms its executing
identity/build and boot metadata; B's flash was not independently read back.
The CRC field does not establish flash integrity, and status continues to
report `verification=not_implemented`.

Four history responses (`history`, `history 16`, `history 64`, and `history 16`
after close/reopen) returned the same six records, with `scope=local`, board A's
boot session, `capacity=64`, and `overwritten=0`:

| Board | Sequence | Uptime | Event | Details |
| --- | --- | --- | --- | --- |
| A | 1 | 17 ms | `boot` | Build 0.98, initial output A |
| A | 2 | 268 ms | `usb_mount` | PC-facing USB enumerated |
| A | 3 | 543 ms | `hid_mount` | Device 1, instance 0, protocol 1; keyboard |
| A | 4 | 548 ms | `hid_mount` | Device 1, instance 1, protocol 0; neither keyboard nor mouse classification |
| A | 5 | 555 ms | `hid_mount` | Device 1, instance 2, protocol 0; keyboard and mouse |
| A | 6 | 841 ms | `output_peer` | Accepted peer selection changed A to B |

These are A's locally recorded events, including its observation of a peer
selection. No history was retrieved from B. The six records show boot and
enumeration capture. Benji subsequently confirmed the requested switch/input/
history check; those later rows were not captured again by the assistant.

Fragmented help/status, two queued status commands, invalid `history 0`, and
discarding partial commands on terminal close/reopen passed. Brief application
read pauses completed with intact status/history responses; OS/USB buffering
may have continued, so this does not demonstrate forced physical USB
backpressure. The retained history and both boot sessions survived reconnect.

The standard macOS `/usr/bin/screen` terminal at 115200 also displayed `help`,
`history 16` with the same six records, and a successful two-board `status`.
That status retained both sessions, with A at 75,787 ms and B at 55,109 ms.
The terminal closed normally. A final history check still contained the same
six records before the user check. Benji later confirmed "looks good" after
the switch/input/history instructions. Replugging was not separately reported.

Evidence is in `build/flashing/pico-a-v098-result.json`,
`console-v098-smoke-20260914T212929.521734Z.json` and its `.txt` transcript,
plus `pico-a-preflash-v098-health.json` and
`pico-a-postreboot-v098-health.json` in the same directory.

## Scope and concurrency

`history [count]` reads the connected board in this slice. The default is 16
records; the accepted range is 1–64. Help and the response identify that local
scope, and each event or gap row includes the originating board. Peer history
will follow in the next interactive slice and will collect both boards by
default into one interleaved list.

Records contain a 64-bit sequence, 64-bit boot-relative time in microseconds,
event type, and compact numeric arguments. Each record is 24 bytes; the fixed
array occupies 1,536 bytes. No keystroke contents, mouse movements, report
payloads, or text are stored. History clears on board reset; closing the
terminal leaves it intact. The response identifies the boot session.

Both application cores record sparse events through a dedicated Pico SDK
critical section. Recording samples the clock and writes one record and its
counters. A reader copies bounds or one record per lock acquisition. Formatting,
USB output, and other application lock acquisition happen outside this lock.
There is brief spin-lock contention; this is not a lock-free producer or a
hardware latency guarantee. No terminal wait or whole-ring copy occurs while
holding the history lock, and no new scheduled task or cross-core queue is added.

The command fixes its requested sequence window when it starts. Later events
cannot extend the response. If new events overwrite a requested record before
the console copies it, the response emits an explicit gap. The overwrite
counter in the header describes losses before the command started; records
excluded by a smaller requested count are not losses. Console output retains
the 32-byte RX / 64-byte TX work limits and produces at most one event row per
tick. USB backpressure may delay completion without retaining the history lock.

## Initial events

| Event | Meaning |
| --- | --- |
| Boot | Executing build and initial selected output, captured before USB starts |
| Local output change | A local action changed the selected output |
| Peer output change | An accepted peer selection changed the selected output |
| USB mount/unmount | This board's connection to its Mac enumerated/disconnected |
| HID mount/unmount | A peripheral HID interface enumerated/disconnected, with device address, interface and keyboard/mouse classification |
| Descriptor rejected | Descriptor parsing rejected a peripheral interface |
| Packet checksum error | Shared USB/UART packet dispatch rejected a checksum |
| UART dropped | A fire-and-forget UART packet was discarded because its queue was full |

Output events describe actual changes, so unchanged synchronization heartbeats
do not fill the ring. HID mount records describe enumeration, including an
interface that enforced-port policy subsequently ignores; they do not promise
that input reports are accepted. Retryable UART queue refusal is not recorded
as a dropped packet. Persistent packet faults can quickly evict older events in
this initial slice; the overwrite count makes that loss visible. Error
aggregation and additional troubleshooting events can follow observed needs.

## Interactive acceptance

The flash/readback, unchanged settings, both executing builds, count rejection,
and retained history across terminal close/reopen passed. Benji confirmed
"looks good" after the requested typing, trackball movement/buttons, Layer 3 S
switch-and-back, and history check. This is user acceptance, not an additional
assistant capture of the resulting rows. Replugging was not separately reported.

## Software validation

The complete deep tier passed all 36 recorded steps, including all fast
components, 20,000 generated HID cases, 16 storage seeds, 46 paired scenarios,
32 generated paired sequences, and 24 fixed core-priority orders each for
backpressure and peer-status scenarios. All 11 storage and 15 paired source
mutations were caught. The nine baseline contracts passed: four exact effect
traces and five per-host state comparisons, with at most 500 microseconds
observed drift (1 ms permitted). This does not establish hardware timing or
universal equivalence.

Pure-store ASan/UBSan tests compare the ring against an independent shifting
list over 2,567 writes and all count limits. They cover every field, wraparound,
copied-record independence, fixed-window loss during continued writes, reset,
and 64-bit sequence exhaustion. The exclusive end saturates at `UINT64_MAX`;
sequences never wrap to zero or alias old records.

The simulator's native boundary runs the actual store, capture bridge, and
production hooks with SDK lock doubles and CDC disabled. It checks the hotkey's
pre-mutated output state, no-op/duplicate selection suppression, both USB sides,
ignored-port enumeration, descriptor rejection, invalid device indices,
checksum errors, and retryable queue refusal versus actual discarded packets.
Review caught that USB configuration and UART packets share checksum dispatch;
the event is therefore named `packet_checksum_error`. A malformed USB GET
regression verifies the event without falsely blaming the UART, changing
configuration, or queuing a reply; a valid control reaches the same path.

The real TinyUSB device stack runs the production console and ring with a
native capture bridge. It checks empty/default/1/64 histories, 80-event wrap,
invalid counts, all ten event formats, A/B labels, full-width numbers, paused
readers overwritten by producers, explicit gaps, queued status/history in both
orders, DTR close/reopen, and bus-reset cancellation. Per-tick instrumentation
checks at most one event read, 32 RX bytes, and 64 TX bytes. Keyboard reports
and LED control traffic continue while CDC is stalled. The isolated HID and
USB-host tests replace recording with an explicit no-op boundary; those tests
do not claim history capture coverage.

ARM configure/build passed, including the final checksum-label correction.
RAM is 180,308 / 262,144 bytes (68.78%), 3,644 bytes above v0.97; SCRATCH_X
remains 2 KiB. Compiler frames are 160 bytes for the console task and at most
56 bytes for an individual history store/bridge function. These are compiler
estimates, not a measured hardware stack high-water mark. The console retains
its 1 KiB TX buffer and adds only a flag and two sequence cursors.

All six native coverage layers built, ran, and exported their reports. The HID
coverage build includes the new explicit capture stub without counting it as
production source. Paired and policy reports now include the relevant peer and
history modules. Each layer has an independent denominator; their percentages
are not whole-firmware coverage and cannot be added.

Logs and results: `build/tests/deep-v098.log`, `results-deep-v098.json`,
`arm-v098.log`, `arm-v098-final.log`, `coverage-v098.log`, and
`coverage-v098/html/index.html`. `git diff --check` passed.

The read-only hardware checker `build/flashing/check_console_v098.py` requires
an explicit serial port and expected CRC. It
reuses the prior bounded transport and checks both statuses, local history
framing/fields, count rejection, and retained history across terminal reopen.
Synthetic parser checks and the physical v0.98 run above passed. Benji
subsequently accepted the interactive input/switch/history check.

## Frozen artifact

- UF2: `build/flashing/deskhop-v0.98-history.uf2`
- UF2 SHA-256: `2214e8a1e206f3e6b7e14c7b69f0b4560842cbd1f8c6bab7ee8d6644b917bf55`
- Binary SHA-256: `59fc940cc6465d193f7de156716ff47d85d44bfbfbb46e0bad605d15c688513d`
- Metadata: magic `0xf00d`, encoded version `198`, CRC `4e15626f`.

An independent check recalculated the CRC over the first 258,048 bytes and
matched all 1,024 unique RP2040-family UF2 blocks to the binary. The 262,144-byte
image occupies `[0x10000000, 0x10040000)` and excludes saved settings at
`[0x101ff000, 0x10200000)`. The source/artifact hash manifest is
`build/flashing/v098-candidate.json`; `hardware_flashed` is true and its
deployment result links to `build/flashing/pico-a-v098-result.json`.
