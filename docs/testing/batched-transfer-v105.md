# v0.105 negotiated firmware page transfers

## Status and scope

This is the implementation and validation record for **deployed v0.105**.
Both boards have passed fresh verification after installation; the final
section records the flash run and separate verification after a busy verdict.
It is not an input-device acceptance report or a physical new/new batch-speed
measurement: the first installation used the legacy receiver path. The
preceding Make/Python updater work was
committed and pushed as `2509929`; that host-side workflow remains the deployment
and verification entry point. Candidate hashes, final test evidence, and any
later physical acceptance belong in a separate evidence section when available.

The deployed firmware changes are committed as `0245392`; the subsequent host
CDC-lifetime/stock-picotool invocation changes and their tests are `07fa0b4`.
The frozen artifact still records the original pre-commit source snapshot;
these commits do not retroactively change its provenance or hardware evidence.

The change accelerates the Pico-to-Pico copying step. A receiver asks for one
256-byte flash page, and the source streams its words without another request
for each word. This does not replace the UART framing, bootloader, image
validation, host backup/readback, or both-board serial verification. It requires
no new software on either Mac and adds no persisted configuration setting.

Implementation is divided into `src/fw_batch.c` / `src/include/fw_batch.h`
(host-independent wire and page state), `src/firmware_batch.c` (firmware
integration), and the existing UART, updater, storage, and maintenance adapters.

## Compatibility and first installation

The protected UART-v1 frame remains exactly 32 wire bytes with an eight-byte
payload. The heartbeat marker remains `0xd485`, and the existing word-request
and word-response messages remain supported. No payload switches to an older,
unprotected framing format.

The receiver probes explicitly before requesting page zero. Only a response
with its current capability tag and the pinned source metadata checksum enables
page transfers. An older source ignores the probe; after the normal 100 ms
response deadline, the new receiver uses the existing four-byte word protocol.
An older receiver never probes and continues requesting words from a new source.

Consequently, **the first v0.105 installation still propagates to a v0.104 peer
at the old speed**: that peer is executing its old receiver until it finishes
upgrading. The faster path becomes available for a subsequent newer-image
upgrade after both boards have this implementation. Flashing an identical
already-running image does not force a peer transfer or benchmark the new path.

The mixed-version simulator builds the actual historical production C from
`2509929`, not an approximation of its negotiation behavior. Controlled fixture
versions exercise both old-source/new-receiver and new-source/old-receiver
directions without changing the normal higher-version upgrade policy.

## Wire protocol

All multibyte fields below are little endian. A tag is nonzero with its low six
bits clear; the remaining 26 bits identify an individual request. The UART
frame's existing checksum still protects the message type and entire payload.

| Message | ID | Payload bytes 0–3 | Payload bytes 4–7 |
| --- | --- | --- | --- |
| Capability request | 51 | Request tag | Expected source version, 16 bits; batch protocol `1`, 16 bits |
| Capability response | 52 | Echoed request tag | Source image CRC32 stored in boot metadata |
| Page request | 53 | New request tag | Page-aligned byte offset within the firmware slot |
| Page data | 54 | Request tag OR word index `0..63` | Four original image bytes |
| Page end | 55 | Exact request tag | CRC32 of all 256 bytes in this page |

There are 1,024 pages in the 262,144-byte slot. Page requests reject unaligned
offsets and offsets at or beyond the slot end; no page request can address saved
settings. A successful page requires 64 data frames and one end frame. The
source does not put all those frames into the ordinary transmit queue at once.

Before responding to a capability request, the source checks the requested
version against its running version. It retains that version and checksum as
the source context. Page service requires this context to remain valid and
accepts only a tag newer than the last accepted tag within that context.

## Correlation, retries, and integrity

Each receiver has one active page collector and a 64-bit received-word bitmap.
The collector accepts only its request tag. Words may arrive out of order, and
the end frame may arrive before data. An identical duplicate makes no progress;
a conflicting duplicate word or end checksum invalidates that request. Neither
64 words without an end frame nor an end frame with missing words can complete
a page. Page CRC validation must succeed before the running image checksum,
address, or flash-write ownership advances.

Each admitted page attempt receives a fresh tag. A queue-full refusal does not
count as an attempt or start a response deadline. An unanswered or invalid page
is retried at the existing 100 ms deadline, for at most three admitted attempts
at that page. Exhaustion falls back to words at the start of the same uncommitted
page; already committed pages and their accumulated checksum are retained.
Late page frames cannot populate the legacy receive path.

The tag allocator is seeded once from the boot session and kept outside state
that is cleared when a pull restarts. Tags never wrap or repeat within that
allocator's boot lifetime: exhaustion selects the legacy protocol. The finite
26-bit wire namespace cannot guarantee uniqueness across all reboots. Tags are
correlation, not authentication; page CRC, pinned image identity, and final
image verification remain required. CRC32 detects accidental corruption, not
maliciously constructed firmware.

The existing whole-transfer stall policy remains: retries are not progress;
after a prolonged stall, an untouched transfer can be abandoned, while a dirty
image is restarted from a live peer or paused for its return. Source changes
use the existing version/checksum-pinned restart rules. A page failure does not
trigger an automatic host reflash or a new unbounded recovery loop.

At the end, the existing accumulated image checksum must match the source's
advertised checksum, and the programmed flash image must independently pass
metadata/version/checksum validation. The metadata checksum covers the first
258,048 bytes; it is not the host updater's full-slot CRC. The host workflow
continues to verify all 262,144 bytes, including the metadata sector, and checks
saved settings separately. Page CRCs cover every byte of each page, including
pages in the metadata sector.

## Page ownership and flash timing

Core 1 receives packets and owns the firmware-upgrade task. A validated page
sets `fw.page_pending`; the flash helper clears that flag immediately after
programming it. This ownership is independent of `fw.byte_done`, which controls
read-request progress, and independent of whether transfer mode subsequently
falls back to legacy words. Queue pressure cannot manufacture a completed page
or cause a completed boundary to be programmed twice.

Batch mode programs the completed page **before admitting the next page
request**. This order is essential: a response occupies 2,080 wire bytes, while
the receive DMA ring holds only 1,024 bytes. Sending the next request before a
sector erase would let the peer fill and overrun that ring while the receiving
core cannot drain it. Explicit ownership permits a failed enqueue to be retried
without reprogramming the page. The legacy path retains its bounded one-word
prefetch and also uses `page_pending` for exactly-once commit ownership.

The collector never clears caller-owned page bytes when beginning a request.
Its bitmap, rather than pre-zeroed storage, proves that each word was supplied
by that attempt. The source snapshots one page into a separate fixed-size
buffer under the existing updater/flash locks; later configuration or receiver
buffer use cannot mutate an active source snapshot.

## Input priority and maintenance

Core 0 services a batch frame only when transmit DMA is idle and the ordinary
UART queue is empty. It uses a try-lock and yields if updater ownership is busy.
Ordinary input/control traffic wins over every subsequent batch frame; at most
the already-selected DMA frame can precede newly arriving input. There is no
64-frame queue flood and no promise of a fixed hardware input-latency bound.

Source service checks updater, dirty-image, reboot, and maintenance ownership
under the firmware lock. Accepted source work refreshes the maintenance lease,
including each emitted frame. An active source cursor also blocks serial
maintenance even if input starvation outlasts that lease; the deferred config
bootloader path likewise guards an active cursor. An independently admitted
host update or other ownership change cancels further source service. A frame
already selected for DMA cannot be recalled, but it remains tied to its old
request tag and does not bypass final image checks.

## Hardware-free validation

The validation layers exercise different boundaries rather than treating a
single simulator result as complete assurance:

- Pure C tests cover wire bounds/endianness, tag exhaustion, known CRC vectors,
  reordering, every missing-frame position, duplicate conflicts, every single-bit
  corruption of data/end contents, backpressure, and full-slot retry isolation.
- Storage tests run the production handlers, updater, and flash helpers against
  deterministic NOR/queue adapters. They check full images, settings preservation,
  final metadata/checksum failures, owner changes, partial-page fallback, and
  exactly-once commit with a full transmit queue.
- Runtime mutation tests deliberately reintroduce prefetch-before-commit, skipped
  page CRC, stale generation adoption, ignored duplicate conflicts, and synthetic
  completion on fallback. Each mutant must compile and then fail a runtime
  oracle; a compiler error is not counted as detection.
- Paired scenarios run production negotiation, UART encoding/parser, queues,
  updater, and flash validation in two isolated firmware instances. Fault cases
  include missing data/end frames, conflicting data, wrong page CRC, changed
  sources, absent capabilities, ownership changes, and input priority.
- A three-page background-scheduler scenario combines a dropped data frame,
  retry, and flash blackouts. Deep validation explores all 24 fixed simultaneous
  core-priority permutations for that scenario, and includes actual historical
  mixed-version transfers.

Useful hardware-free entry points, run from the repository root:

```sh
make test TEST_TIER=deep
python3 tests/storage/run.py --seeds 16
python3 tests/storage/mutations.py
python3 tests/sim/build.py
python3 tests/sim/run.py --scenario batch_full_image
python3 tests/sim/run.py --scenario batch_flash_blackout
python3 tests/sim/run.py --scenario batch_core_schedule --interleavings
python3 tests/sim/test_fw_batch.py --mixed-versions --baseline 2509929
```

## Timing results and limits of the model

These are **virtual simulator durations, not measured Pico upgrade times**:

| Modeled case | Virtual duration |
| --- | --- |
| New source and new receiver, nominal full-image scenario | 17.405 s |
| Actual v0.104/new-code mixed-version directions, legacy transfer | 49.2–49.38 s |
| New/new with flash-blackout stress assumptions | 21.286 s |

The nominal background scheduler advances core passes in 250 µs quanta. The
flash-blackout scenario assumes a 50 ms sector erase and 1 ms page program;
these values are stress inputs, not measurements of this board's flash chip.
UART byte delivery and peer execution remain active during modeled flash
blackouts, so the test can expose request/commit ordering that would overflow
the receive ring. Its successful transfer requires no extra page requests or
fallback caused by these blackouts.

Both local modeled cores pause during a flash operation: the native lock
adapter cannot suspend a reentrant blocking acquisition by the sibling core.
This is a conservative blackout assumption, not proof that real core 0 cannot
run. The harness records the durations, exactly replays a short transfer, and
uses a zero-duration negative control to detect an ignored timing setting.

The simulator executes production application C, but it is not an instruction-
accurate RP2040 emulator. Peripheral timing, scheduling/preemption boundaries,
flash, and USB endpoints are models. It does not reproduce macOS, Karabiner,
physical USB enumeration, electrical faults, arbitrary interrupt interleavings,
or real reboot execution. Fixed core-priority permutations are bounded
exploration, not a proof over every possible concurrent schedule. Native
sanitizers and an ARM build provide additional evidence, not hardware acceptance.

The model demonstrates fewer request round trips while preserving its tested
integrity and input-ordering properties. The deployment recorded below verifies
both-board upgrade/reboot via the legacy receiver path. Actual new/new batch
speed, trackball/buttons, keyboard shortcuts, and switching still require
physical measurement and user acceptance.

## Source-coverage evidence, 2026-09-15

The selected policies, storage and paired coverage layers all passed. Their
report is `build/tests/coverage-v105-batch/html/index.html`, with raw profiles,
source mappings and `llvm-summary.json` retained per layer. Selected new-module
measurements are:

| Layer / module | Executed lines | Covered branches |
| --- | --- | --- |
| Policies / `fw_batch.c` | 181 / 182 | 138 / 160 |
| Storage / `firmware_batch.c` | 163 / 171 | 86 / 114 |
| Paired / `firmware_batch.c` | 164 / 171 | 87 / 114 |

These denominators overlap; they are neither additive nor whole-firmware
coverage. The run used `tests/coverage.py --layer policies --layer storage
--layer paired --output build/tests/coverage-v105-batch`. An in-memory wrapper
only added the paired runner's `--artifact-dir` under that output directory
to isolate any failures from the concurrent deep suite. No source was changed
by the wrapper, and no failure artifacts were produced.

## Frozen candidate and final offline acceptance

`make release TEST_TIER=deep` passed on 2026-09-15: all 52 deep-tier steps,
including 99 host-updater contracts, 26 storage and 17 paired runtime mutation
checks, both full-image mixed-version directions, generated inputs, baseline
behavior traces, and the existing plus new core-order explorations. ARM
configuration and build then passed. RAM usage is 230,036 / 262,144 bytes
(87.75%); the existing flash-slot geometry is unchanged.

The candidate is frozen at
`build/releases/deskhop-v0.105-__6cn3kp/manifest.json`. It identifies the
**uncommitted working-tree snapshot** on `codex/batched-firmware-transfer`,
based on `25099293a2f689fe462f32fdabf4c4e6eaa7935e`; the base commit alone is
not the v0.105 source. Source fingerprints, an archive, validation evidence,
tool versions and CMake cache are retained with the artifacts.

- Full-slot CRC32: `e4843d6a`; metadata image CRC32: `6bffee14`.
- BIN SHA-256: `f4ac4870a80e0b7fc405cfe03bde67c4a117e75154e6710e12592f28b8443580`.
- UF2 SHA-256: `2b7efaa61906c5fabe6913fa058ad08fd1da7eac1f3a9338c8218550ac5aefff`.
- UART framing version 1; saved configuration format 10.

`make flash-plan TEST_TIER=deep` reused and revalidated that exact candidate.
Its profile selected physical A (`E6654854574C3E30`) on
`/dev/cu.usbmodem21203`; this was a plan, not fresh hardware observation.
Neither preparation nor preview opened USB, serial, or picotool. The subsequent
authorized attempt below stopped before writing; deployment and input acceptance
remain incomplete.

## Authorized deployment attempt: stopped before writing

On 2026-09-15 Benji authorized flashing while AFK. Run
`build/updater/runs/20260915T212614Z-_c8duiu_/` confirmed both physical identities,
executing v0.104 builds and progressing cores. One serial `bootloader A` command
received its complete accepted reply. A enumerated with only the PICOBOOT vendor
interface, and picotool independently confirmed UID `E6654854574C3E30`.

The initial `save -r 0x10000000 0x10040000 … --ser E6654854574C3E30` failed
after 10.175 seconds with RP2040 `unknown error` and exit 157. No backup file,
load command, firmware write or settings write followed. `write_started=false`;
the original run stopped at `backing_up`. The cause is unresolved, not a
demonstrated bad image or batching defect: neither board executed v0.105.

One separate restoration attempt checked unchanged Mac media, disk-free ROM and
A's exact identity, then requested a normal application reboot of its untouched
v0.104 image. That command timed out after 10 seconds, and A's serial port did
not return. Its journal is `restore-unchanged/result.json` under the failed run.
No further reboot or flash attempt was made. Manual power cycling and fresh
health checks are required before another upgrade attempt. This records serial
reply and ROM enumeration success, not successful ROM bulk transfer, restoration,
both-board verification, or user input acceptance.

### Recovery after manual power cycle

Benji completed the power cycle. Read-only run
`build/updater/runs/20260915T220007Z-qzl7nu4h/` then passed v0.104 verification
in 5.564 seconds: both exact identities and new boot sessions, advancing cores,
history checks, and fresh correct/wrong/correct full-slot scans with
PASS/FAIL/PASS on both boards. Every scan measured CRC `befb208b`, matching
the frozen accepted v0.104 image; boot metadata CRC remains `4f648cd9`.
Mac media checks passed. No flash or reboot was attempted during this check.
Physical input acceptance remains separate, and v0.105 is still not deployed.

### Explicitly authorized retry reproduced the failure

Benji then requested another flash attempt without further confirmation. Run
`build/updater/runs/20260915T220516Z-amquptwe/` reused the identical frozen
candidate and again passed preflight identity/core checks, serial acceptance,
disk-free ROM enumeration and exact-A identity inspection. The first full-slot
backup read again failed with RP2040 `unknown error`, exit 157, after about
10.10 seconds. Total run time was 10.756 seconds; it stopped at `backing_up`
with `write_started=false` and `reboot_requested=false`. There was no load,
settings write, propagation, post-write verification or safeguard bypass.
A's normal serial port remained absent. No further software reboot was attempted;
another manual power cycle is required before investigating the repeatable
ROM/USB handoff failure. Neither attempt executed the v0.105 batch code.

### Second recovery verification

After Benji's second power cycle, read-only run
`build/updater/runs/20260915T220738Z-ft35ai_x/` passed in 5.434 seconds. Both
Picos are again executing v0.104, with full-slot CRC `befb208b`, boot metadata
CRC `4f648cd9`, fresh boot sessions, and progressing cores. Full-slot scans
returned PASS/FAIL/PASS using correct/wrong/correct expectations on each board;
history and Mac media checks passed. No reboot or flash was attempted. This
supersedes the preceding recovery-required state, not the unresolved cause.

### Controlled CDC-lifetime test passed (no firmware written)

Benji authorized one read-only bootloader experiment. The helper
`build/updater/diagnose_cdc_hold_open.py` kept the old CDC descriptor open through
the original identity/read sequence and normal application reboot. Evidence is
in `build/updater/runs/cdc-hold-open-once-20260915/`. Exact UID, disk-free ROM,
unchanged Mac media, idle updates and advancing cores were checked first.
The same 262,144-byte save succeeded in 0.565 seconds; every byte matched the
accepted v0.104 image, CRC `befb208b`. The normal reboot succeeded in 0.021
seconds, and subsequent fresh both-board verification passed in 5.494 seconds.
Total time was 8.521 seconds. No firmware/settings write or retry occurred.

This supports a host CDC-cleanup timing issue, not an established exact cause;
libusb debug logging was also enabled for the read and can affect timing. The
maintained updater was unchanged at that point, v0.105 remained uninstalled, and production
regression coverage plus an authorized upgrade are still needed before calling
the remedy accepted. The obsolete descriptor's cleanup ioctls reported
`Device not configured`; descriptor closure and fresh diagnostics succeeded.

### Patched updater retry still failed before writing

Benji authorized the lifecycle patch, regression tests, and unattended flashing.
The patch retains CDC through all ROM operations/reboot, closes before the new
diagnostic session, and journals cleanup errors. All 103 host updater tests
passed (`build/updater/cdc-lifetime-tests.log`); A/B success and seven-stage
failure matrices check retention, cleanup and no retries. Firmware/build inputs
and frozen v0.105 artifacts are unchanged from the earlier deep-tested candidate;
fresh host validation is separate from that firmware/source snapshot.

Run `build/updater/runs/20260915T223711Z-v03u3cwe/` again passed serial acceptance,
disk-free ROM and exact-A identity, but the first backup still failed with exit
157 / RP2040 `unknown error` after 10.103 seconds. Total time: 10.768 seconds.
No firmware/settings write or application reboot was requested. No retry or
additional restoration command followed. A's serial port is absent, so manual
power cycling and fresh verification are again required. v0.105 never ran.

This falsifies CDC retention as a sufficient remedy, not every possible CDC
interaction. The earlier successful save enabled `LIBUSB_DEBUG=4`, with possible
timing effects; that remains a hypothesis, not an accepted workaround. The
production upload/propagation path still lacks physical acceptance.

### Recovery after the patched attempt

Benji power-cycled again. Read-only verification run
`build/updater/runs/20260916T000419Z-cv3rgdbb/` passed in 5.446 seconds with both
v0.104 images, CRC `befb208b`, correct/wrong/correct scan outcomes, progressing
cores, identity/history/media checks, and new boot sessions. Neither reboot nor
flash was issued. v0.105 was still uninstalled. Subsequent research was host-side
and read-only; no repeated bootloader experiment was performed.

### Stock-picotool debug-mode validation passed (no flash)

Benji chose supported `LIBUSB_DEBUG=4` in stock picotool child processes instead
of a custom tool. The maintained Mac backend now scopes and journals that one
override without changing the parent environment. All 105 updater tests passed,
including new environment/logging regressions; see
`build/updater/stock-debug-tests.log`.

One ROM visit in `build/updater/runs/stock-debug-once-20260916/` completed two
full firmware/settings read passes. Both firmware reads matched every byte of
accepted v0.104, CRC `befb208b`, in 0.568 and 0.552 seconds; both 4,096-byte
settings reads matched. A normal reboot succeeded in 0.021 seconds, then fresh
both-board verification passed in 5.606 seconds. Total 8.974 seconds; no firmware
or settings writes, load/erase command, or retry occurred. The preflight matched
production (one status/help, no added delay), with debug enabled for all
picotool commands, not only save. The two reads were planned comparisons, not
retries after failures.

This validated the combined no-write procedure, not a precise USB timing cause
or the then-unexercised load/peer-upgrade path. Both Picos remained healthy on
v0.104 at this stage; v0.105 was still uninstalled. No custom picotool code was added.

## Deployment completed; separate verification passed

Benji authorized the real upgrade following stock-tool validation. All 105 host
tests passed again; artifact/source review confirmed that firmware/build inputs
still exactly match the frozen deep-tested candidate. Only the three host
updater modules and two host regression files differ from its source archive.

Run `build/updater/runs/20260916T003027Z-v6d93apw/` successfully backed up A's
v0.104 image/settings, loaded and verified v0.105 (3.889 seconds), independently
read all 262,144 image bytes, and confirmed all 4,096 settings bytes unchanged.
The same unmodified stock picotool used child-only `LIBUSB_DEBUG=4` throughout.
A rebooted normally; B received the complete image automatically and rebooted
into v0.105. The peer-wait/settle phase was 37.659 seconds. This is the legacy
receiver path because B started on v0.104, not a batch-speed benchmark.

The run stopped at final diagnostics with A `UNVERIFIED busy` and B PASS; its
46.903-second result remains marked failed. It was not retried or relabeled.
Separate read-only `make verify`, run
`build/updater/runs/20260916T003133Z-r795gjiq/`, passed in 5.901 seconds with no
flash/reboot. Both boards produced full-slot CRC `e4843d6a`, metadata/boot CRC
`6bffee14`, PASS/FAIL/PASS for correct/wrong/correct expectations, stable boot
sessions/generations, advancing cores, and valid identity/history/media checks.
The deliberately wrong expectation is a negative test, not corrupted firmware.

Current sessions: A `1959d12ac276c3ae`, B `dbf0ebfa7b2023cf`. Both Picos are
firmware-verified on v0.105 without a power cycle. Physical input acceptance and
future new/new batch-transfer measurements remain pending. This successful
stock-tool upgrade does not establish the exact earlier USB timeout cause.
