# Reusable firmware updater

The root Makefile and `scripts/update_firmware.py` replace release-specific
helpers under ignored build directories. They automate preparation and the
conservative macOS flash/verify sequence. The default `normal` verification mode
retains stock picotool's full byte verification, unchanged-settings checks, and
one fresh full-image CRC scan on each Pico. It omits a duplicate host firmware
readback and the diagnostic wrong-CRC/repeat-CRC scans. Opt-in `thorough` mode
retains that previous sequence. Neither mode changes firmware or speeds up the
UART protocol. All 134 updater tests and a normal read-only check on both
installed Picos pass; a full normal-mode upgrade is not yet hardware-tested.
See [the validation record and timing estimate](testing/normal-upgrade-verification.md).

The current accepted release is [v0.109 configuration validation](testing/configuration-validation-v109.md).
Both physical Picos passed automatic propagation and fresh firmware verification;
after returning from AFK, Benji confirmed "Looks good; please merge and push"
following the requested input, switching and config-page checks. This authorizes
publication of the hardware-tested source. The earlier
[v0.108 hardware record](testing/verification-contention-v108.md) is retained as
history. Publication does not reflash the devices; same-version replacements and
downgrades remain refused by the updater.

The subsequent v0.107 attempt failed at the first ROM backup, before any write.
After power cycling, both v0.106 images passed a fresh CRC scan, but the complete
verification sequence remained unsuccessful because A intermittently reported
post-scan `busy`. These are separate failures. The subsequently deployed
[v0.108 firmware repair](testing/verification-contention-v108.md) addresses the
verification contention; the [USB timeout investigation](testing/rom-backup-timeout-investigation.md)
records subsequent successful no-flash experiments, not a proven transport fix.
Identity, media, session, backup, settings and failure-stop gates, and the
stock-picotool transport invocation remain unchanged. The `verify` CLI prints a
diagnostic-only plan; it does not perform the ROM-entry/load steps used by `flash`.

## Stock-picotool invocation and hardware evidence

The host workaround retained on `main` keeps the old CDC connection open through
all ROM operations and normal reboot, then closes it before fresh diagnostics.
Every stock picotool child receives `LIBUSB_DEBUG=4`; this is a supported libusb
diagnostic setting, not a custom tool or driver. Only this override is journaled,
not ambient environment values. Output remains in bounded-command log files.
Failures still stop without automatic retries and clean up the serial context.

The updater now issues one identity command (`info -a --ser`) and
binds that identity to the current disk-free ROM USB session. Subsequent
save/load/reboot commands use stock picotool's `--bus` and `--address`, avoiding
another flash-UID helper execution before each command. The selector comes
from the identity command's libusb debug output, not a guessed bus number or
the nonunique RP2040 ROM serial. Missing or ambiguous output fails closed.
Stock `info -a --ser` itself invokes the UID helper for selection and for its
device-information output; "one command" does not mean one helper invocation.

IORegistry identity, session, location, USB address and VID/PID must match
before and after the UID check, before every ROM operation, and after each
read/write. The binding is discarded on any operation failure and on reboot;
it cannot be silently refreshed or reused. `rom-session.json` records the
original binding, not a reusable configuration. There are no custom picotool
dependencies or new picotool transport options. Keep cables connected and other
USB tools idle:
these checks observe enumeration changes but do not hold an atomic USB handle
across separate picotool processes.

This procedure passed one no-flash hardware experiment: full v0.105 readback in
0.557 seconds, normal reboot in 0.011 seconds, and separate successful both-board
verification without a power cycle. The original run's final `UNVERIFIED busy`
verdict remains recorded. Evidence:
`build/updater/runs/bus-address-once-20260916/` and
`build/updater/runs/20260916T010755Z-uqvk8ve1/`. The extra identity observations
also change timing, so this is not proof of a complete or isolated causal fix.
The maintained implementation has since completed a full physical upgrade:
`build/updater/runs/20260916T011723Z-s6a3wjja/`. Firmware backup took 0.559 seconds,
verified load 3.948 seconds and exact independent readback 0.558 seconds; saved
settings were unchanged. Normal reboot and automatic B propagation succeeded,
without a power cycle. The first full-image check passed on both boards, but
the later negative test returned A `UNVERIFIED busy`; the original run remains
failed at diagnostics. Separate read-only run
`build/updater/runs/20260916T011840Z-krk7l5zw/` passed all fresh both-board checks
in 5.860 seconds, CRC `68eba065`, boot CRC `2cd31c9d`. Benji subsequently confirmed
"looks good" after the requested input checks. All 118 host updater tests pass.
This successful upgrade does not establish reliability across every intermittent
USB failure.

CDC retention alone did not resolve the earlier timeout. The combined stock-tool
invocation succeeded in a no-flash v0.104 validation and the subsequent physical
upgrade, but its precise USB timing mechanism remains unproven. Do not interpret
one successful upgrade as a guarantee that every timeout is fixed.

Evidence: `build/updater/runs/stock-debug-once-20260916/` read the full v0.104
firmware and saved settings twice, rebooted normally and verified both boards
without any flash write. `build/updater/runs/20260916T003027Z-v6d93apw/` then
completed backup, verified load, exact readback, unchanged settings, reboot and
legacy peer propagation. Its final scan stopped on A `UNVERIFIED busy` (B PASS).
Separate read-only verification in
`build/updater/runs/20260916T003133Z-r795gjiq/` passed both-board identity, progress,
history and correct/wrong/correct full-slot checks. Keep the original failed
verdict separate; physical input acceptance and new/new batch timing are not
established by those checks. All 105 host updater tests pass.

The older deployment narratives below retain historical outcomes; the current
status and acceptance above supersede their then-current statements.

## Commands

Run these from the repository root:

```sh
make                         # help only; never flashes
make test-updater             # hardware-free updater unit/contract tests
make release                  # tests + ARM build + frozen candidate; no USB
make flash-plan               # prepare/reuse candidate and preview; no USB
make flash                    # actual upgrade, normal verification by default
make verify                   # one fresh both-board CRC check; no reboot or flash
make flash VERIFY_MODE=thorough # upgrade with duplicate readback + diagnostic scans
make verify VERIFY_MODE=thorough # read-only correct/wrong/correct CRC checks
```

`make flash` prepares a candidate if inputs changed. Repeated preparation reuses
the exact frozen candidate only if its integrity, source fingerprint and recipe
still match; deep validation can satisfy a fast request, but not vice versa.
Use `make release FORCE=1` to rerun validation/build, including after toolchain
updates. `make release TEST_TIER=deep` runs the extended test tier.

To deploy or preview a previously frozen image without rebuilding:

```sh
make flash-plan MANIFEST=build/releases/deskhop-v0.106-EXAMPLE/manifest.json
make flash MANIFEST=build/releases/deskhop-v0.106-EXAMPLE/manifest.json
```

Replace the example directory with the path printed by `make release`. The
default `build/updater/latest.json` is only a convenience pointer; candidate
directories are unique, never overwritten, and contain read-only artifacts.
SHA-256 validation, not file permissions alone, protects their integrity.

## Normal and thorough verification

`VERIFY_MODE=normal` is the default for Make's `flash-plan`, `flash`,
`flash-bootloader`, and `verify` targets. The equivalent CLI option is
`--verification-mode normal` or `--verification-mode thorough`; the Python
`Updater` argument is `verification_mode`, also defaulting to `normal`.

| Check | Normal | Thorough |
| --- | --- | --- |
| Full firmware and 4,096-byte settings backups before writing | Retained | Retained |
| Stock `picotool load -v` full byte verification | Retained | Retained |
| Separate post-load 256 KiB firmware readback and exact comparison | Omitted | Retained |
| Post-load settings readback and exact comparison | Retained | Retained |
| Fresh full-slot CRC scans after both boards settle | One expected-CRC scan per Pico | Expected / wrong / expected CRC per Pico |
| Identity, USB/session, both-core progress, history, peer rollout and media gates | Retained | Retained |

`picotool load -v` reads the programmed image back over USB and compares its
bytes; it is not merely a checksum of outgoing data. Normal mode therefore
still checks the target's programmed image before reboot, then checks both
running images with fresh firmware-computed CRCs. It does not replace stock
picotool, upload a custom RAM checker, or rely solely on boot metadata.

Choose `thorough` when deliberately testing the verifier's rejection path or
collecting a second, independently compared firmware readback. It is not a
retry or recovery mode. In either mode, an already-current pair follows the
read-only verification path. `make verify`, including `VERIFY_MODE=thorough`,
**never enters ROM or performs a USB firmware readback**: thorough read-only
verification adds only the wrong-CRC and final expected-CRC scans.

## Prerequisites and physical selection

- Python 3.10+, Make, CMake, a native C compiler, Node.js for existing tests,
  an ARM GNU toolchain, and populated repository SDK/library directories.
- For actual device operations: macOS and an installed official `picotool`
   supporting UID and bus/address selection, save/load/verify and normal
   application reboot. Its libusb debug output must identify the opened
   bus/address; unfamiliar output is refused rather than guessed.
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
   DTR/the serial connection through its complete response, ROM operations and
   normal application reboot; close it before fresh application diagnostics.
   Confirm the selected flash UID and **disk-free PICOBOOT only**, then pin
   that connection for all subsequent ROM commands.
3. Back up the full 256 KiB firmware slot and all 4,096 saved-settings bytes.
   Validate the old image and version, then load the UF2 with stock
   `picotool load -v` byte verification.
4. Read saved settings and require them to be byte-for-byte unchanged.
   In `thorough` mode, also independently read the complete firmware slot and
   require an exact candidate-image match. Only then request a normal
   application reboot. Close the old serial context before waiting for the
   application port and opening a fresh diagnostic session.
5. Watch peer propagation with bounded status polling, checking identities,
   executing versions/boot CRCs, new boot sessions and both-core progress.
6. Check help, two progressing statuses, history, and one fresh full-slot
   expected-CRC scan per Pico. Require PASS on both, full coverage, stable image
   generations, fresh scans and advancing cores. `Thorough` mode additionally
   performs the deliberately incorrect and repeated correct scans, requiring
   PASS/FAIL/PASS on each Pico. Recheck history and Mac media health.

In thorough mode, the deliberately incorrect CRC is a read-only negative test,
not corruption or a failed upload. It must return `FAIL crc_mismatch`; the next correct scan must
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
reference, profile, selected verification mode, serial transcripts, command logs,
firmware/settings backups, retained readbacks and `result.json`. Normal flashing
does not create `firmware-after.bin`; thorough flashing does. Side-effect intent
is journaled **before** flash or reboot. Phase timestamps and command durations distinguish transfer time from
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

This entry path still checks USB identity, old-image integrity/version, backups,
stock load verification, settings, propagation and final diagnostics; the
separate firmware readback depends on `VERIFY_MODE`. It cannot query the
peer's version or capture either board's pre-upgrade boot session through a
console that is already gone. Establish the peer's compatibility and older
version before using it. It is not a retry/recovery shortcut for unknown state.

## Validation boundary and expected speedup

The normal/thorough split is a host-only change: no firmware version bump,
firmware rebuild requirement, or device reflash is introduced by changing this
policy. All 134 updater tests pass, and a normal read-only verification passed
on both installed v0.109 Picos in 3.367517 seconds, without rebooting or flashing.
No live normal-mode upgrade has been claimed. The accepted v0.109 hardware
record used the previous, now-thorough sequence and remains unchanged.

That v0.109 run measured 0.569 seconds for the duplicate firmware readback,
3.893 seconds for the retained stock load/verify, and 5.804 seconds for the full
post-rollout diagnostics. Each of its two extra CRC requests took about
1.2 seconds, not the entire 5.804-second diagnostic phase. Removing those two
requests plus the duplicate readback suggests roughly **3–4 seconds saved**,
including associated request/observation overhead. This is an estimate, not a
measured normal-mode improvement. The 37.380-second peer wait/settle phase is
unchanged. See [the timing breakdown](testing/normal-upgrade-verification.md).

The offline suite tests BIN/UF2 geometry and settings exclusion, source/evidence
binding, console grammar/freshness, transport failures and the complete A/B
workflow against controlled device doubles. It does not execute RP2040 ROM or
prove physical USB timing. The firmware's existing native/simulator tests also
run during preparation; the extended tier remains available.

### Historical updater validation and transport investigation

The following records describe the firmware and verification policy at the
time of each run, not the current default mode or accepted release above.

The host workflow has exercised serial bootloader entry, backup, verified load,
independent readback, unchanged settings, reboot and legacy propagation on
hardware. Both boards currently have verified v0.106 images. The pinned-selector
implementation has completed the physical upgrade above. B's receiving samples
show about 7.4 kB/s, which does not prove use of the accelerated batch path;
its performance claim remains unvalidated. See the runbook for each preserved
result, the separate diagnostic verification, and the batching evidence limit.

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
