# v0.103 Bootloader-button fix

Prepared 2026-09-15 on `codex/bootloader-button-fix`, based on accepted v0.102
commit `0d09d27`. Source/artifact commit: `bbac34f`. **Deployed and firmware-verified
on both Picos.** User input and physical Bootloader-button acceptance are pending.

## Fix and regression coverage

The Bootloader menu handler passed boolean `true` as the payload to
`sendReport`. The strict report encoder spreads that payload, so the actual
click failed with `TypeError: payload is not iterable` before sending any HID
report. The handler now supplies an empty byte array, preserving the separate
`sendBoth=true` argument. Both commands have zero data bytes, as the firmware
upgrade handler requires no payload. Review then found additional defects on the
same path: the firmware configuration-command allowlist omitted the bootloader
command, a full UART queue could drop the peer request, and immediate local ROM
entry could strand a queued peer request even after both browser sends succeeded.
The fix includes all four corrections. The earlier JavaScript-only build is not
a release artifact.

`tests/test_webconfig_bootloader.js` loads the production page script, runs its
load callback, and invokes its actual delegated menu click listener. Its mocked
HID boundary checks exactly one peer-proxy command followed by one local
command, report ID 6, 12-byte length, all header/padding bytes and independent
CRC8. It checks no device/closed device, delayed peer and local sends, and peer
send rejection preventing the local send. A three-second watchdog makes an
unsettled handler fail instead of silently exiting. It ran red against the
original code (zero reports sent), green against the fix, and rejected an
in-memory never-completing-handler mutation. Fast/deep test tiers now include it.

The mocked completion is WebHID send completion, **not an acknowledgement that
the remote Pico entered maintenance**. Additional paired-production tests consume
the browser's actual report bytes at the real vendor callback and exercise the
UART queue, encoder, receiver and disk-free ROM-entry handler on both simulated
Picos. Adjacent sends and full-queue losses were reproduced before the sequencing
correction. All 20 focused scenarios pass: local and proxied requests in both
directions, adjacent sends, full queues, independently busy DMA and UART hardware,
config-mode and malformed-report rejection, allowlist boundaries, and update
guards at admission, dispatch and final local entry. Existing keyboard maintenance
scenarios also pass. These tests record ROM-entry calls; they do not emulate ROM
execution or physical USB enumeration.

The scoped design defers configuration-endpoint boot requests on core 0. It
retains a peer request until the queue accepts it, and holds the local request
until pending peer admission, the queue, DMA and UART FIFO/shifter are all clear.
It does not add a browser sleep, a new wire protocol or a watchdog-based fallback.
The initiating Pico's active update or dirty image blocks USB admission, peer
enqueue and final local entry under the existing firmware lock. This cannot
retract a peer frame already queued. Keyboard and UART-received maintenance
handlers remain unchanged and do not gain this guard;
this is not the broad controlled-reboot safety fix. There is no remote execution
acknowledgement or retry after physical UART corruption. A persistently stalled
or saturated UART can defer this button's local action rather than force reset.
The button's physical browser/USB/UART sequence and ROM entry still need acceptance.

## Build and validation

The source template, rendered HTML, self-extracting page and embedded 64 KiB
FAT image were regenerated together. An independent DEFLATE decoder and the
page's actual JavaScript decoder both reproduce the rendered HTML. Extracting
`config.htm` from the disk image matches the generated page byte-for-byte, and
the ARM binary's complete disk region matches that image.

Final validation passed fast **39/39**, deep **47/47**, and ARM **2/2** steps.
Each paired suite includes **117 scenarios**, including all **20** new Bootloader
regressions. Deep validation also passed all four sets of 24 fixed core-priority
orders, 32 generated input seeds, 21 storage and 17 paired production mutations,
and nine baseline/current behavior contracts. These are bounded, modeled tests,
not exhaustive instruction-level race exploration or physical hardware acceptance.
The ARM image is 262,144 bytes, metadata version **203**, configuration format
**10**, with RAM use of **221,116 / 262,144 bytes**.

Final artifact identities:

- Full-slot CRC32: `0c2fdeb0`; boot metadata CRC32: `30eae185`.
- BIN SHA-256: `f531646f34e768063e737bc28a4c5bb9e64641ad83e17ca05615f4bb1135c84f`.
- UF2 SHA-256: `2e5e5e5dc94da59a4b3a2276d6a904cf17c84ccfbabef1f9b34d96f2a2938203`.
- Post-deployment verification command: `verify 0.103 0c2fdeb0`.
- UF2 writes only `0x10000000`–`0x1003ffff`; saved settings at
  `0x101ff000`–`0x101fffff` are not included.

The candidate is built in `build/arm-validation/`. Frozen release filenames are
`build/flashing/deskhop-v0.103-bootloader-button.*` and
`build/flashing/v103-candidate.json`; the manifest records the committed source
identity, validation results and separate deployment/acceptance state.

## Hardware deployment: 2026-09-15

Pico A was already in disk-free PICOBOOT when flashing was requested. Its exact
flash UID was `E6654854574C3E30`; only USB interface class 255 was present.
Its pre-flash firmware matched accepted v0.102 byte-for-byte. After backing up
firmware and settings, official picotool `load -v` passed, and an independent
262,144-byte readback matched the frozen v0.103 binary. All 4,096 settings bytes
were unchanged. A normal application reboot was requested at
**18:29:25.575682 UTC**. No mass-storage mount/copy was used. The same 20 Mac
media clients remained active/nonbusy, and the RP2 Boot object disappeared.

The first bounded rollout capture obtained 56 statuses over 29.592 seconds.
B remained on its v0.102 boot while receiving v0.103 from A; its final sampled
progress was 217,088 / 262,144 bytes. This capture ended on its time limit,
not a transfer failure. A separate status-only continuation observed B on
v0.103 with a new boot session and two same-boot advancing samples across all
four cores. Peer execution became `update=confirmed`. No B cable relocation,
manual flash or power cycle was needed. Completion occurred between captures;
the exact final-page/reboot instant was not sampled.

- A session: `1163db6c8ba18a80`.
- B session: `93c98994a187e251` → `11f9bc0f338e959f`.
- B UID: `E6654854577F2330`.

The final read-only console checker passed in **5.504 seconds** (11,742 received
bytes): five stable paired statuses, two histories, and three fresh full-slot
scans on each board. Correct/wrong/correct CRC expectations yielded
**PASS/FAIL/PASS**, while every measured image CRC was `0c2fdeb0`. Boot metadata
CRC independently matched `30eae185`; generation stayed stable and both cores
advanced during every scan. B's image was measured by its own firmware, not by
external picotool readback; its settings were not separately read back.

Evidence under `build/flashing/`:

- `pico-a-v103.1zhk_b_p/`: backups, flash/readback logs, reboot and USB/media health.
- `console-v103-rollout-20260915T182955.json` and `.txt`.
- `console-v103-progress-20260915T183016Z.json` and `.txt`.
- `console-v103-smoke-20260915T183031.112765Z.json` and `.txt`.

Typing, trackball/buttons and switching are awaiting Benji's check. The new
Bootloader button has deliberately **not** been exercised on hardware: it stops
both boards, so that acceptance test requires independent input and recovery
access to both boards. Successful flashing does not establish button behavior.

## Rollout and remaining scope

UART-v1, WebHID CRC8, saved settings and QMK are unchanged. The version bump
allows the existing compatible peer updater to recognize this as newer than
v0.102; another incompatible-protocol migration is not required. Actual peer
propagation must still be observed, not inferred from the version bump.

Use the normal disk-free keyboard maintenance procedure for deployment, then
open the newly bundled config page rather than a cached v0.102 page. Test the
Bootloader button only when both boards are idle, no update is in progress,
independent Mac input is available, and each board can be recovered/rebooted.
The button targets both Picos, so DeskHop input will stop during maintenance.

Original configuration-validation (#4) and controlled-reboot safety (#1) remain
separate, unfinished work. Their access restriction/scope is not bypassed by
this narrowly scoped UI fix. It does not make interrupted flash writes safe.
