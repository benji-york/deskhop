# Keyboard reliability draft (bug #2)

2026-09-15; based on `af100bcaba8013633d694bd6de8583c2970f6e4d`
(v0.101 testing branch). This is the first draft in the ordered bug-fix stack.
Firmware version remains 0.101 and configuration format remains 10 deliberately:
this is **not a deployable release**, was not flashed, and does not claim hardware
acceptance. Both boards must eventually receive the same new protocol with a
coordinated version bump. Main `d42c930` is only a historical differential oracle.

## Problem and behavior

Ordinary keyboard all-up could be discarded by either the 128-report USB queue
or the 256-packet UART queue. Wire loss could leave the same permanent hold.
Modifier heartbeats changed the mirror but never repaired the host report.

The USB FIFO now has a durable latest-state tail. Reports retain order until
capacity is exhausted; subsequent reports coalesce into that tail until space
opens. A successful USB submission frees a slot for the tail. Failed submissions
retain the FIFO head. Focus changes discard old queued downs and retain an
all-up. USB reset/unplug discards historical output; reconnection obtains current
local source state and a fresh peer snapshot. No keyboard queue operation waits
for an endpoint or requests reboot. The separate mouse critical-queue timeout
and other unsafe reboot handling remain for the later reboot task.

A new core-1 task provides source-owned peer state, independent of the USB FIFO:

- Keep up to 128 physical source transitions plus a coalesced latest tail.
- Combine local physical keyboards only. Never echo remote or synthetic state.
- Send changed state promptly using the latest receiver challenge; periodically
  answer fresh receiver challenges with the current source snapshot.
- Accept complete, CRC-checked envelopes with the current challenge, exact focus
  token, source boot session and newer serial. Reject partial, reordered, stale
  or mixed envelopes. Adjacent identical fragment duplicates are harmless.
- Update USB only when peer state changes. Unchanged snapshots cause no HID
  retransmission. Source-owned `ACTIVITY_MSG` supplies physical input age;
  snapshots, including rehydration after lease expiry, do not invent activity.
- After 500 ms without an accepted snapshot, remove only the peer contribution.
  A local keyboard's overlapping modifier/key remains held. Fresh state restores
  actual remote holds after recovery. A new receiver epoch discards historical
  source FIFO transitions before replying, preserving its current snapshot.

Consumed hotkeys are removed from their source slot before publication, so
another keyboard or a later snapshot cannot leak the chord. Ordinary QMK typing,
F24 switching, multiple physical sources, held modifiers and both host directions
are covered. Existing collapsing of multiple report collections/interfaces into
keyboard source slots and the six-key output limit remain unchanged.

The lock-both-hosts shortcut is explicitly synthetic. Legacy ID1 accepts only
its supported lock chords and a matching pending all-up; it does not populate
physical state. The receiver owns a 20 ms release deadline, so a lost synthetic
all-up is repaired by the durable USB queue. Release restores the active host's
current physical aggregate, or empty on the inactive host. The synthetic down
itself remains best effort; lost down may skip a lock, and a delayed duplicate
can repeat the idempotent lock action. It cannot leave its synthetic keys held.

## Delivery contract and limits

All bounds require recurring core tasks, eventual correct UART delivery in both
directions, and an accepting host endpoint. Recovery requires recurring windows
in which a request and its complete five-fragment response arrive within the
**100 ms challenge lifetime**, including both queues and task latency. A correct
but permanently slower round trip cannot reconcile and remains lease-released.
An endpoint failure never causes a
new keyboard-triggered reset. Under permanent host disconnection no report can
be delivered; a bus reset discards old transitions. Under permanent link loss,
a responsive receiving host gets peer all-up after the **500 ms lease plus its
existing USB FIFO drain**. If that endpoint is itself stalled, all-up remains
durable until it accepts reports.

Receiver challenges recur every 100 ms and expire after 100 ms. A full request
queue retries each 1 ms task pass. Source events send at most one five-packet
envelope per 1 ms pass and only when at least five queue slots are available.
There is no polling loop for queue space. Updater/diagnostic traffic keeps using
the same FIFO; eventual keyboard progress requires recurring opportunities for
five free slots, not permanent saturation by other producers.

A useful conservative quiescent-recovery bound is:

`2R + 2U + 130E + 129H + task jitter`

Here `R=100 ms`, `U` bounds UART queue residence/delivery, `E` bounds admission
and transmission opportunities for a five-packet source envelope, and `H`
bounds host report acceptance. The source and host FIFO sizes make these finite
under the stated service assumptions, including the sub-100-ms request/response
window. This bound starts after correct focus agreement and healthy service
resume. Selection recovery has its own recurring
1 Hz reconciliation bound. The paired regressions assert host-visible recovery
within 1.2 seconds with the modeled 250 us loop quantum, 1 ms HID endpoint,
transient full queues and finite loss/corruption. This is not a physical timing
measurement or a promise under arbitrary recurring loss/starvation.

Intermediate transitions can be coalesced at overload, lost on the wire, or
rejected once their challenge expires. This is authoritative state recovery,
not exactly-once delivery of every keystroke. The lease intentionally releases
remote holds during a long outage and may re-press still-held physical keys on
return. Under normal service, FIFO transition order is retained.

Fresh 64-bit boot sessions and rotating receiver nonces reject prior receiver
boot traffic. Observing a new source boot immediately retires its old challenge;
source serials use half-range uint32 ordering. An already transmitted old-source
report cannot be identified as pre-reboot until the source restart is observed
or its challenge expires (at most 100 ms). Arbitrarily many resets, nonce
collisions, counter exhaustion and a malicious peer are not proven. No persisted
session/config field or migration is added.

## Wire format and checksum successor

The outer UART frame stays 12 bytes with its existing payload XOR. New packet
IDs are isolated to keyboard state:

| ID | Meaning |
| --- | --- |
| 42 | Receiver request: little-endian 64-bit challenge |
| 43–47 | Five consecutive 8-byte fragments of one state envelope |
| 48 | Receiver reset request: challenge plus instruction to discard historical source FIFO |

Reset requests repeat until a current snapshot confirms the epoch. A repeated
identical reset request does not repeatedly flush the source. Other UART packet
types can interleave between fragments. A new fragment zero starts assembly;
out-of-order fragments abandon it, and the next snapshot repairs loss.

The 40-byte envelope has these little-endian fields:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Receiver challenge |
| 8 | 8 | Source boot session |
| 16 | 4 | Selection counter |
| 20 | 4 | Source envelope serial |
| 24 | 8 | HID modifier/reserved/six-key source report; reserved must be zero |
| 32 | 1 | Selection origin |
| 33 | 1 | Selected output |
| 34 | 1 | Marker `0x4b` |
| 35 | 1 | Envelope version `1` |
| 36 | 4 | IEEE CRC32 of bytes 0–35 |

The CRC protects the complete state/context against fragment corruption and
mixing; it does **not** fix the outer UART packet-type checksum defect. The next
task must protect all packet types, especially RESET versus ordinary REQUEST:
a type substitution could skip a FIFO reset before an otherwise valid response.
Corruption that redirects a fragment to an unrelated existing handler remains
outside this draft's guarantee. Preserve the narrow envelope's integrity checks
while changing the outer transport.

Old firmware ignores IDs42–48 and cannot provide these guarantees. Ordinary
ID1 forwarding is intentionally retired; its remaining synthetic use cannot
act as a legacy physical-state fallback. Mixed-version normal keyboard routing
is unsupported. Do not roll out this unversioned draft or let auto-update infer
compatibility from its unchanged 0.101 version. The successor release must plan
both-board propagation and a deliberate firmware version bump; no config bump
is needed for this RAM-only change.

## Core ownership and tests

`keyboard_sync_init` executes before `multicore_launch_core1`. Source rings,
assembly and protocol state are then core-1-owned. The core-0 startup announcement
only invalidates USB output through the locked generation boundary. USB callbacks
increment that same generation. Cached peer state carries its accepted generation,
so even a local report before the next sync tick cannot reintroduce old peer keys.
The short firmware RAM lock serializes USB FIFO/tail operations and nonblocking
TinyUSB submission with focus/reset invalidation; it covers no wait or flash work.
The standalone HID/USB-stack boundaries stub the peer bridge explicitly; paired
checks run its real C, SDK queues and UART parser.

Regressions live in `tests/sim/test_keyboard_reliability.py`; existing critical
queue and task-map expectations now describe durable release rather than reboot.
A source-order startup check explicitly covers the launch boundary that the
paired scheduler does not execute. Historical comparisons retain 1 ms limits
for non-keyboard effects and allow 3 ms for the new five-frame keyboard path
(observed 1.5 ms). The reboot-chord scenario explicitly asserts the newly
immediate source releases; it does not claim unchanged keyboard trace semantics. No instruction-level, PIO, electrical, physical USB or
macOS/Karabiner acceptance is claimed.

## Completed validation

- Final `python3 tests/run.py fast`: **38/38 steps passed**, including all 24
  keyboard scenarios and startup/task-ownership assertions.
- `python3 tests/run.py deep`: **46/46 steps passed**: 20,000 generated HID cases,
  16 storage seeds, 32 paired input seeds, four existing 24-order explorations,
  21 storage plus 15 paired production mutations, and historical comparisons.
  This preceded the final synthetic-remount timer correction; the complete fast
  tier, all keyboard scenarios/seeds, its 24-order race exploration and ARM build
  were rerun afterward.
- Final keyboard suite: **24 scenarios × 3 seeds = 72 passes**. The final remount
  race passed **24/24 core orders**. Earlier targeted local saturation, remote
  host-reset backlog and wire CRC/order cases passed **72 additional orders**.
- Captured actual `af100bc` library: **15 host-visible regression failures** and
  six passing controls. The two envelope-specific cases and new-task remount
  schedule are not runnable on that baseline. The latter race separately failed
  on both hosts in the prior draft library before its correction.
- Final ARM configure/build: **2/2 steps passed** with the requested ARM GCC14
  PATH. RAM use: **219,060 / 262,144 bytes**. No flash/push/merge performed.
- Python compilation and `git diff --check` passed.

Results are in `build/tests/results-{fast,deep,arm}.json`; targeted evidence is
in `build/tests/reliability-final/` and `build/tests/reliability-remount/`.
The final targeted library SHA256 is
`2022706ef2ed2003ccd33e2f0799d97b99612524bd80566b3c51852f753fc087`.
The unflashed `build/arm-validation/deskhop.uf2` SHA256 is
`dec71b1b99ccba4da1cb8dae89b54f783a9995c50bd1d6547937a391de88dcf2`.

Useful focused reruns:

```sh
python3 tests/sim/build.py
python3 tests/sim/run.py --scenario keyboard_local_queue_release --interleavings
python3 tests/sim/run.py --scenario keyboard_snapshot_wire_order_crc_and_context --interleavings
python3 tests/sim/run.py --scenario keyboard_synthetic_remount_before_sync --interleavings
```


## Changed files

```text
BENJI_DESKHOP_RUNBOOK.md
CMakeLists.txt
docs/testing/README.md
docs/testing/coverage.md
docs/testing/keyboard-reliability-draft.md
src/handlers.c
src/include/keyboard.h
src/include/keyboard_sync.h
src/include/protocol.h
src/include/structs.h
src/keyboard.c
src/keyboard_sync.c
src/main.c
src/setup.c
src/uart.c
src/usb.c
tests/hid_stubs/main.h
tests/history_stub.c
tests/sim/build.py
tests/sim/differential.py
tests/sim/mutations.py
tests/sim/node.c
tests/sim/run.py
tests/sim/test_harness.py
tests/sim/test_keyboard_reliability.py
tests/sim/test_transport.py
tests/usb_host/main.h
tests/usb_stack/test_usb_stack.c
```
