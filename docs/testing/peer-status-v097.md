# v0.97 peer-status deployment

Prepared on 2026-09-14 from `c79634f` plus the peer-status changes and flashed to
Pico A at 21:03 UTC through the disk-free PICOBOOT entry. Direct A firmware and
configuration readback passed. Fresh serial replies now confirm that both A
and B are executing v0.97. Benji confirmed the interactive keyboard, trackball, and switching check:
"Working great. No replug needed."

## Behavior

`status` queries both boards by default. The connected board prints immediately
inside `BEGIN status`; a successful peer response adds that board's identity,
executing version, boot session, uptime, and boot CRC metadata. Failures produce
an explicit `peer=timeout_or_unsupported`, `invalid`, or `busy`, followed by
`END status`. No peer fields are inferred from updater caches or local success.

Identity is captured before core 1 starts. UART types 36/37 carry correlated,
checksummed snapshots. One-entry SDK queues cross cores; core 1 owns the
protocol, and core 0 owns console formatting. Work is bounded at one diagnostic
packet per millisecond, 32 console RX bytes and 64 TX bytes per tick. The
client has a 500 ms deadline; the console has a 600 ms fallback. USB backpressure
can delay delivery of text but does not keep a peer transfer alive indefinitely.

Review caught a rapid-repeat defect: a second healthy query could hit the
server's 200 ms rate limit and appear unavailable. The bridge now waits 200 ms
after completion before starting the next queued query, while retaining its
original deadline. This state survives terminal close/reopen.

These commands remain read-only. The displayed CRC is metadata copied at boot;
the snapshot CRC protects transport. Neither is the future `verify` command.
The [diagnostic design](../diagnostics.md) documents the wire layout and limits.

## Validation

- The complete deep tier passed all 33 recorded steps, including every fast
  component, 20,000 generated HID cases, 16 storage seeds, 46 paired scenarios,
  32 generated paired sequences, and all 11 storage/15 paired mutations.
- All nine baseline scenario contracts passed: four exact effect traces and
  five per-host state comparisons, with at most 500 microseconds observed drift
  (1 ms permitted). This is not universal equivalence or a hardware timing test.
- Pure protocol tests under ASan/UBSan cover an independent wire/CRC vector,
  both board roles, reordered/duplicate/missing chunks, corruptions, wrong
  tokens, exact deadlines, queue refusal, immutable snapshots, rate limiting,
  and simultaneous bidirectional requests.
- Paired production firmware exercises real SDK bridge queues, UART framing,
  receive dispatch, and scheduled tasks. It covers simultaneous and immediately
  repeated queries, broken links, full/stalled UART, malformed replies, and
  continued keyboard/mouse output. Request/result actions replay exactly.
- The real TinyUSB device stack exercises the production console with a mocked
  peer bridge: local-first output, delayed success, failures, the fallback
  deadline, stale replies across reconnect, queued commands, HID progress, and
  strict per-call RX/TX bounds. The actual peer bridge/UART path is covered by
  the paired tests, not this USB mock.
- Validly checksummed direct and proxied diagnostic messages are rejected by
  the real USB configuration allowlist, preserving core-1 protocol ownership.
- ARM configure/build passed. RAM is 176,664 / 262,144 bytes (67.39%), 4,472 bytes
  above v0.96; SCRATCH_X remains 2 KiB. Compiler stack frames are bounded, but no
  hardware stack high-water measurement is claimed.

Verification found that the simulator CLI's `--interleavings` option always
selected backpressure even with another `--scenario`. The runner now honors an
explicit scenario and names it in the result; an apparatus regression guards
that routing. After the full deep run, the corrected peer command and complete
apparatus tests passed separately. The peer command executes one normal run
plus all 24 fixed core-priority orders, including simultaneous/repeated queries.
This command is now also part of future deep runs.

Logs: `build/tests/deep-v097.log`, `arm-v097.log`,
`peer-orders-v097-confirmed.log`, and `harness-v097-confirmed.log`.
The earlier `peer-orders-v097.log` exposed the CLI issue and is not evidence of
24 peer-order checks. `git diff --check` passed.

## Frozen artifact

- UF2: `build/flashing/deskhop-v0.97-peer-status.uf2`
- UF2 SHA-256: `dfd8ea758d430dd2683186de12b69006b4543d0410e88d74dcc2de20cea5c375`
- Binary SHA-256: `13f33c9b1047de43c84f267c828c2709adf9b09d5141031551977662258222f1`
- Metadata: magic `0xf00d`, encoded version `197`, CRC `9a2b3827`.

An independent check recalculated CRC over the first 258,048 bytes and checked
all 1,024 unique RP2040-family UF2 blocks against the binary. The image occupies
`[0x10000000, 0x10040000)` and excludes saved settings at
`[0x101ff000, 0x10200000)`. `build/flashing/v097-candidate.json` records all
artifact hashes and source hashes, with `hardware_flashed: true` and a link to
the deployment result.

## Hardware deployment and serial checks

Layer 3 A entered the v0.96 PICOBOOT-only path. The USB registry showed the
vendor interface (class 255), no mass-storage interface, and no additional
media clients. Official picotool 2.3.1 selected physical flash UID
`E6654854574C3E30`. The pre-update firmware exactly matched the frozen v0.96
image. After loading v0.97, an independent readback matched all 262,144 firmware
bytes and all 4,096 saved-configuration bytes remained unchanged. Pico A then
rebooted normally and exposed `/dev/cu.usbmodem21203`. No retained RP2 object or
inactive/busy media client remained in the Mac's post-reboot check.

The initial `status` returned local build 0.97 at 18,629 ms uptime and explicitly
reported `peer=timeout_or_unsupported`. A later query returned B's own v0.97
identity. This is consistent with propagation completing between queries;
the initial timeout alone does not identify what B was doing.

The bounded serial smoke check at 21:06 UTC passed with `--require-peer`:

| Board | Physical flash UID | Executing build | Boot session | Sampled uptime range |
| --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `0.97` | `4d4f8996e257d47d` | 187,376–189,571 ms |
| B | `E6654854577F2330` | `0.97` | `2440d7c7e9f8cd10` | 166,601–168,796 ms |

All six status snapshots returned `peer=ok`, with stable identities/sessions,
increasing uptimes, and boot CRC metadata `9a2b3827` on both boards. Fragmented
help/status, two queued status commands, a 0.75-second application read pause,
and close/reopen all passed. Reopening discarded an unfinished command while
retaining both boot identities. The transcript records local blocks arriving
before peer blocks; host receipt timestamps are not USB wire latency, and the
read pause does not prove USB-level backpressure.

The standard `/usr/bin/screen` terminal at 115200 also displayed `help` and a
complete two-board `status` with `peer=ok`. Sessions were unchanged at A uptime
221,964 ms and B uptime 201,189 ms. The terminal closed normally.

These fresh B responses establish its executing identity/build and boot
metadata. They are not an independent readback of B's flash or a flash-integrity
test. Every response correctly reports `verification=not_implemented`.

Evidence is retained in `build/flashing/pico-a-v097-result.json`, the picotool
logs and before/after binaries in that directory, and
`console-v097-smoke-20260914T210622.154818Z.{json,txt}`. The frozen artifact
manifest now records `hardware_flashed: true`.

## Interactive acceptance

Benji confirmed "Working great. No replug needed" after the typing, trackball
movement/buttons, and Layer 3 S switching check on both Macs. The v0.97
interactive slice passed. This is a basic input check, not a separately timed
comparison of input with the terminal open and closed. Local RAM history is
the next slice.
