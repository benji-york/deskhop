# Verification contention repair, v0.108

Status: **deployed, verified and user-accepted on both Picos, 2026-09-16**.
Implementation `34ed079` passed all 57 deep-tier steps and the ARM build. The
frozen candidate was then installed with the maintained updater; full firmware
verification and Benji's normal-input acceptance passed. The old, uninstalled
v0.107 candidate remains immutable and is retained separately.

Branch `codex/upgrade-reliability-v0.108` is based on `a17ef5c`, so it includes
the four upstream fixes documented in
[the v0.107 integration record](upstream-fixes-v107.md). Benji explicitly
authorized this deployment followed by merge to main and push. The release
extends previously accepted main `1da1af2`.

## Hardware deployment and user acceptance

Evidence: `build/updater/runs/20260916T173830Z-j3n0vqi7/` in the canonical checkout.
Before deployment, `make release TEST_TIER=deep` revalidated the exact cached
candidate. Independent review matched all 674 source fingerprints and the
firmware, source-archive and validation hashes. The only dirty tracked inputs
were non-fingerprinted investigation/runbook documentation.

The standard `make flash` ran once with the explicit v0.108 manifest and stock
picotool 2.3.1. There was no debugger, recovery retry, power cycle or cable move.

- Fresh preflight identified both expected Picos on v0.106 with idle updates.
- A's firmware and settings were backed up before writing. All 262144 firmware
  bytes matched the v0.108 candidate on independent readback; all 4096 settings
  bytes were unchanged (SHA256
  `aa816af793193a7ce487d391b49bf45d514dde09bd77d28255eb782e82a85464`).
- Firmware backup took 0.561196 seconds, load/verify 3.900664 seconds, independent
  firmware readback 0.561120 seconds and normal reboot 0.010876 seconds.
- B's actual peer receiving progress and reboot were observed. The peer-wait
  phase lasted 37.469823 seconds. Both boards then reported v0.108, idle updates
  and advancing cores. New stable boot sessions: A `70882dc812bdbfa4`,
  B `22deee28ac61d736`; physical UIDs remain A `E6654854574C3E30` and
  B `E6654854577F2330`.
- Fresh verification queried correct CRC `80c1302f`, deliberately wrong CRC
  `80c1302e`, then correct CRC again. Both boards returned PASS/match,
  FAIL/crc_mismatch, PASS/match respectively. Every scan covered 262144 bytes
  and measured `80c1302f`, with boot metadata `c60094e1`, unchanged generation
  and valid/advancing cores. No BUSY verdict occurred.
- The updater exited zero, `stage=complete`, `firmware_verified=true`,
  `independent_readback=true`, `settings_unchanged=true`, elapsed 50.815672 seconds.
  Mac media/USB health checks passed. B's flash integrity is its firmware's
  fresh full-slot scan, not a separate external picotool readback.
  Old CDC-handle cleanup recorded three expected device-disconnected errors
  after USB re-enumeration; they remain in the journal, distinct from the 35
  successful recorded commands and the completed upgrade.
- Benji explicitly confirmed "Everything works normally" after being asked to
  check typing/modifiers, trackball and keyboard right-click on both Macs,
  bidirectional switching, focus arrows and zoom assist. The automatic result's
  `input_acceptance=pending` predates that reply and is preserved; the separate
  `user-acceptance.json` and this document record the later confirmation.

This accepts the deployed release, not universal race freedom. The timer UI's
save/readback and keep-awake soak were not separately exercised live in this
deployment; their offline tests and unchanged saved settings remain the evidence.
Peer propagation succeeded, but batch-mode use/speedup remains unproven. The
intermittent USB-ROM hang is not fixed by this firmware change and is parked at
Benji's request. Earlier failed diagnostic records remain failed.

## Observed failure and cause

The separate recovery run `build/updater/runs/20260916T144841Z-8bmnj2nl/`
completed a correct-CRC scan on both boards, then correctly rejected a wrong
expected CRC. On the final repeat, A reported `UNVERIFIED busy` after all
262,144 bytes, while B passed. The journal remains failed; no validation gate
is relaxed or retroactively relabeled.

The scanner already yields on transient BUSY while starting, reading pages and
finishing a scan. Its subsequent freshness checks did not: one unsuccessful
nonblocking lock attempt permanently replaced COMPLETE with BUSY. Those
checks run before local/peer result publication and before console output.
Ordinary activity can hold the firmware-update lock without changing flash.
Thus an otherwise valid scan could become unavailable during presentation.

## Repair contract

- A freshness check makes one nonblocking attempt. BUSY returns pending without
  changing the completed evidence; callers yield rather than emit PASS.
- Recheck again at actual local queue publication, including after backpressure.
- A completed scanner remains owned while awaiting a freshness check. Local
  and peer work are bounded by the original three-second request deadline.
- Console rows and final verdict defer only until the original 3.5-second
  command deadline. Expiry cannot emit an overall PASS.
- Generation changes, metadata changes and an active update remain terminal
  failures. The scan times are not refreshed to hide stale results.
- Remote evidence remains a `scan_snapshot`, not a new freshness lease over
  every later response-packet transmission.

This changes firmware diagnostic availability, not the CRC algorithm, flash
layout, UART frame version, settings format, update transaction or host checks.
The host's correct/wrong/correct sequence still requires every verdict to
match; it does not ignore busy results or silently retry a failed deployment.
The read-only `verify` CLI now also prints its actual diagnostic-only plan,
instead of the generic flash plan that misleadingly mentioned ROM entry/load.

## Validation

The new ASan/UBSan scanner harness executes the production diagnostic scanner,
peer verifier and SDK queues with deterministic firmware-I/O responses. It
covers transient and permanent BUSY at scanner completion, actual queue
publication after backpressure, peer completion/cancellation/replacement,
metadata/generation changes and active updates during deferral, exact original
deadlines and unchanged scan timestamps.

The real TinyUSB device/console suite holds row emission and final-verdict
emission busy independently. It checks multiple transient misses before
success, BUSY followed by a real failure, exact 3.5-second expiry with no further
guard attempts, HID progress while waiting, and disconnect/reopen with rejection
of the abandoned query's token. Existing production storage tests exercise both
actual nonblocking lock helpers separately. This is layered, deterministic
coverage, not a physical multicore/USB timing proof.

Independent review found no blocker and separately reran the scanner, USB and
storage tests. The full release validation passed **57 deep-tier steps** in
255.6 seconds, including all **119 host updater tests**, production paired
scenarios, fixed core-order exploration, historical mixed-version/differential
builds and existing source mutations. ARM configure/build also passed; RAM
usage is 231,196 bytes (88.19%). Physical input acceptance and live verification
subsequently passed as recorded above.

## Frozen release

Prepared with the maintained hardware-free command:

```sh
make release TEST_TIER=deep \
  TOOLCHAIN_DIR=/opt/homebrew/opt/arm-gcc-bin@14/bin \
  RELEASE_DIR=/Users/benji/Documents/ChatGPT/DeskHop/build/releases
```

- Manifest: `/Users/benji/Documents/ChatGPT/DeskHop/build/releases/deskhop-v0.108-07hkmacr/manifest.json`
- Full-slot CRC32: `80c1302f`; metadata CRC32: `c60094e1`.
- BIN SHA256: `fd95a666dfe5759f6f1f24e916551d947afdbd144f259e5a32fe4f2a8609b8be`.
- UF2 SHA256: `8a26a77cd2b11e3fe1e7512fc8624b7f8941f4cb86f250988e89a8d7fe80e0ea`.
- Evidence: `/private/tmp/deskhop-upstream-fixes.FmFwV1/build/updater/prepare/prepare-gsv2rwdk/`.

Build/test inputs match committed source `34ed079`. The manifest records dirty
Git state because documentation-only investigation notes were updated during
validation; these are excluded from build fingerprints. The exact source
snapshot and validation-bound hashes remain authoritative. UART frame version 1
and configuration version 10 are unchanged, and the settings page is excluded.
Use this explicit manifest for a future authorized deployment, never the old
v0.107 candidate or the canonical checkout's earlier latest pointer.

## Separate ROM backup timeout

The failed v0.107 flash stopped before any firmware write. That is a different
failure from this firmware verification issue; this repair must not be described
as fixing both. See [the stock-picotool investigation](rom-backup-timeout-investigation.md)
for the evidence and proposed bounded, no-flash experiment. No speculative
transport workaround or custom picotool is installed by this change.
