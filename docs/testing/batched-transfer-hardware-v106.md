# Physical test of the v0.105 batch-transfer implementation

Status: the pinned-selector upgrade propagated and verified v0.106 on both
boards. Benji confirmed "looks good" after the requested input checks and
authorized commit, merge and publication. This is operational acceptance;
speed measurements still do not establish batch use or acceleration.

## Candidate and test boundary

Branch `codex/test-v105-batch` is based on `03d85db`. Its only firmware/build
change is `VERSION_MINOR` 105 to 106. Both Picos began on verified v0.105, so the
new version label triggers the real newer-version upgrade policy with a
batch-capable receiver. No instrumentation or policy override is added.

`make release TEST_TIER=fast` passed all fast-tier checks (including 105 host
tests, native, storage, real TinyUSB and paired production scenarios), ARM
configuration and build. Frozen candidate:
`build/releases/deskhop-v0.106-m7cxjizq/manifest.json` in the canonical repo.
Full-slot CRC: `68eba065`; BIN SHA256:
`55c78043fbdcf445a9807caef67ca1feff908af5b74c7cf462406639c0672e2a`.
The source archive binds the version-only working-tree change.

## Preflight and failed attempt

Fresh read-only verification in
`build/updater/runs/20260916T004627Z-vpogrmr5/` passed both original v0.105 images,
full-slot CRC `e4843d6a`, identities, core progress and fresh positive/negative
checks. It made no firmware write or reboot.

The authorized flash attempt is
`build/updater/runs/20260916T004919Z-9jsqm65k/`. Serial entry into A's disk-free
ROM and UID inspection succeeded. The first firmware backup failed with exit
157 after 10.074 seconds; libusb records a zero-byte bulk-transfer timeout.
The whole run lasted 10.746 seconds. `write_started=false` and
`reboot_requested=false`: no load, settings write, peer propagation or retry
occurred. A's serial port was absent and it remained in disk-free ROM until
Benji subsequently power-cycled and reconnected both sides.

Retained CDC plus stock `LIBUSB_DEBUG=4` did not prevent this recurrence. The
prior successful upgrade is not proof of a reliable USB workaround. The test
has not yet produced any evidence of physical batching.

The first read-only recovery check,
`build/updater/runs/20260916T005106Z-r9_fwn_7/`, stopped on B `UNVERIFIED busy`.
A separate fresh check, `build/updater/runs/20260916T005207Z-6rnyatsw/`, passed
in 5.844 seconds: exact identities, v0.105 full-slot CRC `e4843d6a`, metadata
CRC `6bffee14`, fresh correct/wrong/correct checks and advancing cores. Both
recovery checks made no write or reboot. New sessions are A `614c4ce949545922`
and B `bdc0c71e9600a24b`. Physical input acceptance remains separate.

## Subsequent no-flash USB-session experiment

Benji separately approved one stock-picotool experiment, not another flash.
Evidence in the canonical repo:
`build/updater/runs/bus-address-once-20260916/`. One UID-targeted identity
inspection was followed by a full-slot read and normal reboot using the
observed, pinned `--bus 2 --address 5`. Registry ID, USB session, location and
address remained unchanged across four snapshots; changed enumeration would
abort. These are session-specific selectors, not persistent board identities.

The read took 0.556811 seconds and all 262,144 bytes exactly matched frozen
v0.105 (SHA256
`f4ac4870a80e0b7fc405cfe03bde67c4a117e75154e6710e12592f28b8443580`).
Normal reboot took 0.011072 seconds. No load, firmware/settings write, ROM
command retry or power cycle occurred. The original result remains failed:
its final recovery scan returned A `UNVERIFIED busy`, while B passed.

Separate read-only verification in
`build/updater/runs/20260916T010755Z-uqvk8ve1/` completed in 5.821471 seconds.
Both v0.105 images passed fresh correct/wrong/correct full-slot checks with
CRC `e4843d6a`, metadata CRC `6bffee14`, exact identities and advancing cores.
A's new boot session is `e5a19899118972cf`; B's remains `bdc0c71e9600a24b`.

This demonstrates one successful pinned-selector procedure, not a proven
causal fix: the added identity/media observations also increased the gap
between identity completion and the read from about 1 ms to about 87 ms.
The experimental helper is ignored build evidence, not a maintained-updater
change. No further hardware attempt is authorized by this diagnostic alone.
v0.106 remains uninstalled and actual batch-transfer acceptance is pending.

## Authorized pinned-selector upgrade and separate verification

Benji subsequently authorized production integration, regression tests and a
retry of the frozen candidate. The canonical updater now establishes identity
with one stock `info -a --ser` command and requires the same observed USB
session around identity/read/write and before reboot. Later commands use
bus/address; failure or reboot expires the binding. No custom picotool or
firmware change was added. All 118 updater tests pass, including 36 platform
tests. An independent safety review found no blocker.

Upgrade evidence: `build/updater/runs/20260916T011723Z-s6a3wjja/`. One backup,
verified load, exact readback and unchanged-settings check completed. Firmware
backup took 0.559159 seconds, load/verify 3.948203 seconds, full-slot readback
0.557699 seconds and reboot 0.011432 seconds. The application returned and
peer-watch began at +7.567 seconds. No power cycle, cable move or ROM retry was
needed. B automatically propagated and rebooted to v0.106.

The first correct full-slot scan passed on both boards. The subsequent
intentional negative-CRC scan returned A `UNVERIFIED busy` and B's expected
`FAIL crc_mismatch`. Preserve the original failed diagnostic verdict, elapsed
48.331896 seconds. Separate read-only verification,
`build/updater/runs/20260916T011840Z-krk7l5zw/`, passed in 5.859616 seconds:
fresh correct/wrong/correct checks on both, CRC `68eba065`, boot CRC `2cd31c9d`,
exact identities and advancing cores. A's session is `3ad7279fab2beea7`;
B's is `3da3479a30e83d0f`. This second run made no write or reboot.

The 64 B receiving snapshots share the old B boot session, peer source,
attempt 1 and target v0.106. They span 255,744 bytes in 34,478 ms (~7.4 kB/s).
None of the 2,016 chronological pairs exceeds the conservative legacy bound;
the maximum margin is -4,976 bytes. Peer settling took 37.347 seconds. Thus
the test confirms upgrade/propagation/integrity but **does not establish batch
use, fallback, or acceleration**. Negotiation/pacing needs investigation before
claiming the batch feature accepted. No automatic further flash is planned.

Source review found no intentional version/role gate blocking this pair.
Unproven fallback candidates are the single capability probe's 100 ms deadline
and exhaustion of three page attempts; ordinary UART traffic, source locking
and flash programming can also reduce throughput without fallback. A future
test should first expose mode, negotiation, retry and fallback counters/reason.
No additional instrumentation or firmware upgrade was made in this attempt.

## Acceptance criteria (operationally accepted; batch-path proof still pending)

- Exact A image readback and unchanged settings, normal reboot and automatic B
  propagation, then fresh full-slot verification and progressing cores on both.
- Positive evidence of actual batching from B's saved, pre-reboot status
  samples. Match boot session, update attempt, peer source, target and receiving
  phase. If `delta_received > 16 * (delta_uptime_ms + 2) + 512`, progress exceeds
  the all-legacy bound and demonstrates the batch path actually delivered data.
- Normal typing, trackball/buttons, keyboard-generated right-click, and
  bidirectional switching checked by Benji after recovery/deployment.

The legacy limit follows from at most one four-byte request per 250-microsecond
task invocation. The scheduler sets `next_run = now + frequency` (no catch-up),
and only one requested word may advance reception. The loose allowance covers
page-rounded observations, an outstanding word and millisecond quantization.
Peer uptime and runtime progress are frozen on the same core; USB response
latency therefore does not distort their interval. Existing status polling
already journals the samples; do not add a competing console reader.

A successful timing bound proves some batch use, not zero fallback or batch
delivery of every page. Slow timing is inconclusive. A flash attempt or image
CRC alone does not establish this feature's hardware acceptance.
