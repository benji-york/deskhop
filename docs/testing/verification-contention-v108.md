# Verification contention repair, v0.108

Status: implemented in `34ed079`; all 57 deep-tier steps and the ARM build pass.
Nothing was flashed or rebooted during this repair. Both physical Picos remain
on v0.106; the old, uninstalled v0.107 candidate is immutable and retained separately.

Branch `codex/upgrade-reliability-v0.108` is based on `a17ef5c`, so it includes
the four upstream fixes documented in
[the v0.107 integration record](upstream-fixes-v107.md). Main remains at
hardware-accepted `1da1af2`. No merge or push is authorized by this repair.

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
remain outstanding until an explicitly authorized deployment succeeds.

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
