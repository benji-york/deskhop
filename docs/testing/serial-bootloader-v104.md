# v0.104 serial bootloader deployment

Prepared 2026-09-15 on `codex/serial-bootloader`, based on `216a7f8`.
**Deployed, firmware-verified on both Picos, and user input accepted.** Physical
serial bootloader-command acceptance remains pending. The image was flashed
before committing, so it is attributed to the exact source snapshot, not the
base commit alone. This changeset publishes its unchanged runtime/build sources.
Existing v0.103 deployment evidence is unchanged.

## Contract

`bootloader A` and `bootloader B` select the physical Pico, regardless of focus
or the console's location. They request disk-free PICOBOOT, not an upload.
Upload/readback/normal reboot remain separate picotool operations through the
selected board's computer-facing USB connection. There is no remote USB tunnel.
The first installation used the existing Layer 3 A bootloader entry method.

The implementation uses a bounded core-0 state machine and a core-1 ingress
queue. Local entry needs actual USB reply completion and UART drain. Remote
entry uses dedicated request/ACK types 49/50 with token/session correlation;
the target drains its ACK before entering ROM. Acceptance is not evidence of
ROM enumeration or a successful upload. No fallback to the legacy immediate
reset command is allowed. See [the console contract](../diagnostics.md#serial-maintenance-v0104-deployed-physical-command-acceptance-pending)
for exact outcomes, deadlines and cancellation semantics.

Active/dirty updates, pending reboots and conflicting reservations reject entry.
An updater-locked reservation excludes new firmware work while maintenance waits.
Recent accepted firmware-word serving adds a three-second holdoff, not proof
that a paused/offline peer is clean. This is not the separate broad reboot-safety
or config-validation fix. Saved configuration remains format 10; UART-v1 framing
and the existing automatic update protocol are unchanged.

## Hardware-free coverage

- Real TinyUSB device stack and virtual DCD: strict command grammar and both
  physical roles; framing; actual IN completion; full-packet ZLP followed by the
  final one-byte newline fence; stalled USB, DTR close, bus reset and late
  callbacks; token/target rejection; remote outcomes; HID progress under CDC
  backpressure. Maintenance itself is a contract double at this boundary.
- Paired production maintenance/UART: 33 scenarios for both directions, focus
  independence, dropped and duplicated requests/ACKs, wrong fields, old sessions,
  queue admission, expiry, cancellation, independently stalled DMA/UART hardware,
  simultaneous requests, update/reboot rejection, late dirty-state rechecks,
  source holdoff, reservation guards, and exclusion from WebHID commands.
- Real storage callbacks: UF2/heartbeat/update work has no flash or diagnostic
  effects while maintenance reserves the image; valid UF2 admission resumes
  after release.
- Six new production mutations prove tests detect removal of USB completion,
  ACK wire drain, canceled-packet filtering, dirty-target rejection, ACK token
  checking and ACK session checking. All 23 paired mutations passed their
  baseline, compiled, and failed their designated observable assertion.
- Harness checks preserve core-0 ROM ownership, pre-core-launch initialization,
  and exact replay of a paired serial-maintenance transaction.

The UART encoder oracle was updated because bare queued types 49/50 are now
deliberately rejected without a live transaction. It asserts that rejection and
independently calls the real encoder for those types, preserving all 255 type
values and CRC checks rather than reusing a previous transmission.

These are bounded modeled tests, not exhaustive instruction-level race coverage,
RP2040 ROM execution, or physical USB/UART acceptance.

## Build and pending acceptance

Final validation passed fast **39/39**, deep **47/47**, and ARM **2/2** steps.
Each full paired run includes **150 scenarios**, including the **33** new serial
maintenance regressions. Deep validation includes four sets of 24 fixed core
priority orders, 32 generated-input seeds, 21 storage and 23 paired production
mutations, and nine baseline/current behavior contracts. The final storage
reservation regression also passed a separate 16-seed run. Results are retained
in `build/tests/results-fast.json`, `results-deep.json`, and `results-arm.json`.

The ARM build passes. Frozen deployment artifacts are
`build/flashing/deskhop-v0.104-serial-bootloader.{bin,uf2,elf}` with manifest
`build/flashing/v104-candidate.json`. A source archive includes all relevant
tracked and new untracked files; an accompanying patch records tracked changes.
The binary is 262,144 bytes, metadata version 204,
with RAM use 226,612 / 262,144 bytes. Its 1,024 UF2 blocks remain entirely within
`0x10000000`–`0x1003ffff`, excluding saved settings.

- Full-slot CRC32: `befb208b`.
- BIN SHA-256: `32ebaeada1de04f5bc347caa0f4b5d6173e1afe433a9a86dc2d1d7c00ad241e8`.
- UF2 SHA-256: `92fefd2c16bc6282b27c48dc1365036207176be8f2c4e6bfbfe8a1ee2c59c00b`.

The safe flash procedure, both-image verification and user input checks have
completed. In a separately authorized maintenance
check, exercise each physical target, observe disk-free
ROM enumeration on its own USB-connected Mac, and restore normal firmware
execution. Do not treat an ACK, terminal disconnect, or timeout as proof of the
target's actual USB state. Keep the terminal open through the complete local
reply. v0.103 cannot execute the new serial command.

## Hardware deployment: 2026-09-15

The user entered Layer 3 A. Read-only USB inspection found only the vendor
PICOBOOT interface (class 255), with no ROM mass-storage interface. Official
picotool selected A by flash UID `E6654854574C3E30`. Its old firmware matched the
frozen v0.103 image byte-for-byte. Firmware and all 4,096 saved-settings bytes
were backed up before programming. Picotool load/verify passed, and independent
readback matched all 262,144 v0.104 firmware bytes; settings were unchanged.

A normal application reboot was requested at **20:36:14.358672 UTC**. The same
20 Mac media clients remained active/nonbusy, the RP2 Boot object disappeared,
and `/dev/cu.usbmodem21203` returned. No disk mount/copy or power cycle was used.

The first bounded status capture obtained 56 samples in 29.720 seconds, observing
B receiving v0.104 from A and reaching 212,480 / 262,144 bytes. A 1.310-second
status-only continuation obtained three samples confirming B's new executing
v0.104 boot, stable identities and repeated progress on all four cores. The
observer reported `update=confirmed`. No manual B flash, cable move or power
cycle was needed. Completion occurred between captures, so the final-page/reset
instant was not observed directly.

- A boot session: `492449aea2d4ff41`.
- B UID: `E6654854577F2330`.
- B boot session: `11f9bc0f338e959f` → `888510372aab3f53`.
- Both full-slot CRCs: `befb208b`; separate boot metadata CRC: `4f648cd9`.

The final read-only console checker passed five statuses, two combined histories,
and three fresh correct/wrong/correct CRC scans yielding PASS/FAIL/PASS on both
boards. It checked actual help advertising the new fifth command, but sent only
`help`, `status`, `history` and `verify`. No serial `bootloader` command was sent.
This establishes deployed firmware identity/integrity and sampled core progress,
not user-input acceptance or physical operation of the new maintenance command.
Separately, Benji confirmed "Everything is working fine" after the requested
typing, trackball-button and switching checks. That accepts normal input on the
deployed firmware; it does not imply the unexercised serial bootloader commands
have passed a physical test.

Evidence under `build/flashing/`:

- `pico-a-v104.f_64zjon/`: backups, load/verify, readbacks, reboot and USB/media checks.
- `console-v104-rollout-20260915T203644.088717Z.{json,txt}`: transfer progress.
- `console-v104-rollout-20260915T203652.527746Z.{json,txt}`: new B boot and settled progress.
- `console-v104-smoke-20260915T203708.272969Z.{json,txt}`: final fresh scans and console checks.

The manifest records hardware flashing, firmware verification and user input
acceptance separately from the still-pending physical command acceptance. QMK
was not changed or flashed in this deployment.
