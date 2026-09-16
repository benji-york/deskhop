# Reusable firmware updater

The root Makefile and `scripts/update_firmware.py` replace release-specific
helpers under ignored build directories. They automate preparation and the
existing conservative macOS flash/verify sequence. They do not change firmware,
speed up the UART protocol, or remove any image checks. Firmware already checks
the received/programmed image; the host also retains independent readback and
fresh checks on both Picos.

Firmware v0.105 separately adds [negotiated page bursts](testing/batched-transfer-v105.md)
to reduce peer-transfer round trips. No new host option is needed: the firmware
negotiates them and falls back to legacy words for an older peer. Installing
v0.105 onto the prior v0.104 pair propagated at legacy speed; later
upgrades can use batches when both executing boards already support them.
Both Picos now run verified v0.105. Its simulated batch speedup remains distinct
from physical timing: this initial installation did not exercise the batch path.

## Commands

Run these from the repository root:

```sh
make                         # help only; never flashes
make test-updater             # hardware-free updater unit/contract tests
make release                  # tests + ARM build + frozen candidate; no USB
make flash-plan               # prepare/reuse candidate and preview; no USB
make flash                    # actual upgrade, including final verification
make verify                   # fresh read-only diagnostics; no reboot or flash
```

`make flash` prepares a candidate if inputs changed. Repeated preparation reuses
the exact frozen candidate only if its integrity, source fingerprint and recipe
still match; deep validation can satisfy a fast request, but not vice versa.
Use `make release FORCE=1` to rerun validation/build, including after toolchain
updates. `make release TEST_TIER=deep` runs the extended test tier.

To deploy or preview a previously frozen image without rebuilding:

```sh
make flash-plan MANIFEST=build/releases/deskhop-v0.105-EXAMPLE/manifest.json
make flash MANIFEST=build/releases/deskhop-v0.105-EXAMPLE/manifest.json
```

Replace the example directory with the path printed by `make release`. The
default `build/updater/latest.json` is only a convenience pointer; candidate
directories are unique, never overwritten, and contain read-only artifacts.
SHA-256 validation, not file permissions alone, protects their integrity.

## Prerequisites and physical selection

- Python 3.10+, Make, CMake, a native C compiler, Node.js for existing tests,
  an ARM GNU toolchain, and populated repository SDK/library directories.
- For actual device operations: macOS and an installed official `picotool`
  supporting UID selection, save/load/verify and normal application reboot.
- No packages are downloaded, no privileged command is run, and no software is
  needed on the other Mac. Python host tools use the standard library only.
- Put build tools on `PATH`, or supply `TOOLCHAIN_DIR=/path/to/arm/bin`.
  Supply `PICOTOOL=/path/to/picotool` if it is not on `PATH`.

`config/updater.json` describes Benji's actual pair:

| Role | Flash UID | Default connection |
| --- | --- | --- |
| A | `E6654854574C3E30` | This Mac, `/dev/cu.usbmodem21203` |
| B | `E6654854577F2330` | Other Mac |

Use `PROFILE=/path/to/profile.json` for another pair. The profile requires
`target`, `port`, and both `uids`; it deliberately does not discover/select a
device by wildcard. `PORT=/dev/cu.…` and `TARGET=A` or `TARGET=B` override the
profile. The selected target must be physically USB-connected to this Mac and
must match the local console's role and flash UID. Selecting remote B through
A's console is **not** an upload tunnel and is refused. To target B directly,
move B's computer-facing cable to this Mac and specify its observed serial port.

Normal automatic entry requires local firmware v0.104 or later, both boards
responsive and idle, and unchanged UART-v1 framing. Firmware metadata versions
older than v0.102 are refused; a UART protocol migration needs a separately
reviewed two-board procedure. Candidates with other UART frame versions are
rejected. The firmware configuration version is recorded, not automatically
migrated by this host tool; on-boot migration remains firmware's responsibility.

The candidate must have a strictly greater version than **both** running boards.
If both already run the candidate, the updater only verifies them. Same-version
replacement images, downgrades, a partially upgraded pair and unknown peer state
are not silently repaired. Review the evidence instead of blindly retrying.

## What happens during an upgrade

1. Validate the frozen BIN/UF2, metadata/full-slot CRCs, hashes and source/test
   evidence. Stage private per-run image copies and recheck them before writing.
   Check Mac media clients and both running Pico identities/core freshness.
2. Send the local `bootloader A` or `bootloader B` command exactly once, retaining
   the serial context through its complete response, ROM enumeration, every
   backup/load/readback operation and the normal application reboot.
   Confirm the selected flash UID and **disk-free PICOBOOT only**.
3. Back up the full 256 KiB firmware slot and all 4,096 saved-settings bytes.
   Validate the old image and version, then load the UF2 with picotool verification.
4. Independently read the complete firmware slot and saved settings. Require an
   exact image match and byte-for-byte unchanged settings. Only then request a
   normal application reboot. Close the old serial context before waiting for
   the application port and opening a fresh diagnostic session.
5. Watch peer propagation with bounded status polling, checking identities,
   executing versions/boot CRCs, new boot sessions and both-core progress.
6. Check help, two progressing statuses, history, and three fresh full-slot
   CRC scans using **correct / deliberately incorrect / correct** expectations.
   Require PASS/FAIL/PASS on both Picos, full coverage, stable image generations,
   fresh scans and advancing cores. Recheck history and Mac media health.

The deliberately incorrect CRC is a read-only negative test, not corruption or
a failed upload. It must return `FAIL crc_mismatch`; the next correct scan must
pass. User input acceptance is separate: after success, check typing, trackball
movement/buttons, keyboard-generated right-click, and switching in both directions.
The journal keeps `input_acceptance: pending` rather than claiming these checks.

Media clients must remain active, nonbusy, and unchanged across the operation.
ROM must expose exactly one active PICOBOOT vendor interface, with no storage
interface. This protects the Mac from the previously observed ROM-storage panic.
There is no `picotool reboot -u`, mounted-volume copy, fallback port selection,
automatic retry, power cycle, or safety-check bypass.

Keeping the old CDC descriptor alive matches a successful held-open diagnostic,
but a later patched attempt still failed: it is not a proven timeout fix. No further
CDC commands are sent after bootloader entry. Context-manager cleanup runs on
success and every exception, before any fresh application console opens. The
already-ROM path opens no initial CDC session. Best-effort cleanup errors for
the departed device are retained as `bootloader_console_cleanup_errors` in the
result journal; they do not turn a failed read/load into permission to continue.

The Mac backend now sets `LIBUSB_DEBUG=4` only in stock picotool subprocesses.
This uses libusb's supported diagnostic setting, not a custom picotool build
or transport. Parent environment and unrelated commands are unchanged. The
existing per-command log retains combined stdout/stderr, with the usual
timeout and 4 MiB post-command output check. `commands.json` records only
`environment_overrides: {"LIBUSB_DEBUG": "4"}`, never the ambient environment.
The controlled validation and a real upgrade using this setting passed the
ROM operations. Final both-board checks passed in a separate read-only run
after an initial busy verdict. Do not infer a proven timing mechanism.

## Evidence, timeouts and failures

`build/releases/deskhop-vVERSION-…/` contains BIN, UF2, optional ELF, the manifest,
source archive (including SDK/library and untracked build/test/tooling files),
source-bound successful command evidence, actual CMake cache, and configured
tool versions. The working-tree fingerprint, not merely Git HEAD, identifies
uncommitted source. No claim is made that the source archive plus tool-version
text is a hermetic reproducible toolchain.

`build/updater/runs/TIMESTAMP-…/` contains per-run image copies, source manifest
reference, profile, serial transcripts, command logs, firmware/settings backups,
readbacks and `result.json`. Side-effect intent is journaled **before** flash or
reboot. Phase timestamps and command durations distinguish transfer time from
host orchestration time. Preparation logs live under `build/updater/prepare/`.

A nonblocking lock prevents overlapping updater runs from this checkout. It is
not a system-wide USB lock: close other serial tools and do not operate picotool,
another checkout, or firmware maintenance controls concurrently.

Serial commands/session lengths, response sizes, subprocesses, enumeration and
rollout have bounds. Peer rollout defaults to 90 seconds; explicitly set
`ROLLOUT_TIMEOUT=120` if needed (maximum 300). A timeout/USB disconnect/ACK alone
never proves a flash or reboot succeeded. A failed load/readback leaves the
target in ROM; the tool does not automatically reboot a possibly invalid image.

After any failure, inspect `result.json`, logs, USB state and backups before
deciding the next action. `make verify MANIFEST=…` can check a pair that has
returned to normal firmware without writing or rebooting anything. Recovery
from a partial flash, incompatible pair or invalid backup is intentionally not
automated by this tool.

For an explicitly inspected target already in **disk-free** ROM, including
initial installation on pre-v0.104 UART-v1 firmware:

```sh
make flash-bootloader MANIFEST=/path/to/candidate/manifest.json
```

This mode still checks USB identity, old-image integrity/version, backups,
readback, settings, propagation and final diagnostics. It cannot query the
peer's version or capture either board's pre-upgrade boot session through a
console that is already gone. Establish the peer's compatibility and older
version before using it. It is not a retry/recovery shortcut for unknown state.

## Validation boundary and expected speedup

The offline suite tests BIN/UF2 geometry and settings exclusion, source/evidence
binding, console grammar/freshness, transport failures and the complete A/B
workflow against controlled device doubles. It does not execute RP2040 ROM or
prove physical USB timing. The firmware's existing native/simulator tests also
run during preparation; the extended tier remains available.

The new host workflow's **read-only diagnostic path has passed on hardware**:
on 2026-09-15 the pair was already current at v0.104, so no rewrite was attempted.
An initial conservative `UNVERIFIED busy` on A stopped the first run; a separate
fresh verification completed all checks on both boards in 5.617 seconds. See
the runbook's first-live-run record for evidence. Serial bootloader entry,
upload, reboot and propagation through this new host workflow still need physical
acceptance. The deployed v0.104 firmware remains untouched.

An authorized v0.105 attempt later that day successfully exercised the serial
acceptance reply, disk-free ROM enumeration and exact-A identity check, but its
first backup read failed with RP2040 `unknown error` before any flash write.
A separately journaled normal reboot of the unchanged image also timed out;
A's serial port remained absent. See the runbook's latest-state section and
`build/updater/runs/20260915T212614Z-_c8duiu_/` for the stopped run and restoration
evidence. Manual power cycling and fresh health checks are required; the tool's
actual upload/propagation path remains physically unvalidated. No backup or
verification check was bypassed to continue the upgrade.

After Benji manually power-cycled DeskHop, read-only verification run
`build/updater/runs/20260915T220007Z-qzl7nu4h/` passed both-board v0.104
identity/core/history checks and the full correct/wrong/correct CRC sequence.
Both images still match `befb208b`; recovery is firmware-verified, without any
flash retry. The serial-ROM failure still needs investigation before another
upgrade attempt.

A subsequent explicitly authorized retry with the same frozen candidate
reproduced the first-backup failure, again before any write:
`build/updater/runs/20260915T220516Z-amquptwe/`. No further software reboot or
flash attempt followed. A again requires a manual power cycle; recovery from
the earlier attempt must not be mistaken for recovery from this second one.

Benji then completed that second power cycle. Read-only run
`build/updater/runs/20260915T220738Z-ft35ai_x/` passed all both-board v0.104
identity/core/history and CRC checks in 5.434 seconds. Current firmware health
was verified again without another flash during that recovery check.

A subsequent authorized no-flash test retained the old CDC descriptor through
the same ROM identity/full-slot backup sequence and normal application reboot:
`build/updater/runs/cdc-hold-open-once-20260915/`. The backup passed in 0.565
seconds and matched all 262,144 v0.104 bytes. Reboot succeeded, then both-board
verification passed in 5.494 seconds. This supports changing the maintained
updater's descriptor lifetime; it is not yet a production fix or a successful
v0.105 deployment. Debug logging was enabled during the successful save, so
one trial does not isolate the exact mechanism. No flash write was issued.
The proposed change must preserve all existing safety gates, bounded commands,
single-attempt semantics and cleanup on every exit path.

The lifetime change was then implemented and passed 103 host updater tests,
including A/B success/failure cleanup and no-retry assertions. Its authorized
unattended attempt `build/updater/runs/20260915T223711Z-v03u3cwe/` nevertheless
failed at the same first backup in 10.103 seconds, exit 157, despite retaining
CDC. No write or reboot followed, and A's normal serial port was absent. Manual
power cycling plus fresh v0.104 verification was required. The prior successful
diagnostic enabled libusb debug logging during save; possible timing effects
are still unproven. Do not treat CDC retention alone as a working remediation,
or the 103 simulated contracts as proof of physical ROM/USB behavior.

After another power cycle, both-board v0.104 verification passed. Benji chose
the supported stock-tool environment setting instead of owning a custom
picotool. All 105 updater tests passed (`build/updater/stock-debug-tests.log`),
covering environment isolation, journal privacy, debug output parsing and the
existing failure/timeout/no-retry contracts.

No-flash run `build/updater/runs/stock-debug-once-20260916/` then passed in 8.974
seconds using the maintained backend. In one ROM visit, two complete firmware
reads matched the accepted v0.104 image byte-for-byte (0.568 and 0.552 seconds),
and both 4,096-byte settings reads matched. Normal application reboot succeeded
in 0.021 seconds, with CDC retained until then; fresh both-board verification
passed in 5.606 seconds. There was no load/erase/firmware write or retry.
Debug was enabled for all picotool children, including identity and reboot.
This validated the combined no-write workflow, not the cause or the then-unexercised
load/upgrade path. v0.105 was still uninstalled at this stage.

The subsequent authorized deployment,
`build/updater/runs/20260916T003027Z-v6d93apw/`, successfully performed backup,
stock-tool load/verify, exact full-slot readback, unchanged settings, normal A
reboot and automatic B propagation/reboot. B's prior v0.104 receiver used legacy
transfer; peer wait/settle took 37.659 seconds. The final check conservatively
stopped on A `UNVERIFIED busy` (B PASS), not a checksum mismatch. The original
run remains recorded as failed at `verifying_both` after 46.903 seconds.
Separate read-only verification
`build/updater/runs/20260916T003133Z-r795gjiq/` passed all both-board checks in
5.901 seconds, with no repeated flash or reboot. Both Picos now run v0.105,
CRC `e4843d6a`, boot CRC `6bffee14`; user input acceptance remains pending.

The improvement is eliminating manual pauses between already-tested steps and
reusing unchanged prepared images. Previous retained evidence shows roughly
36 seconds for peer propagation; the host tool alone does not reduce that UART
transfer time. The separate v0.105 batch protocol targets that component.
The successful initial deployment's timings above include the older receiver;
a future separately authorized upgrade can measure new/new batch performance.
