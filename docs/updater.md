# Reusable firmware updater

The root Makefile and `scripts/update_firmware.py` replace release-specific
helpers under ignored build directories. They automate preparation and the
existing conservative macOS flash/verify sequence. They do not change firmware,
speed up the UART protocol, or remove any image checks. Firmware already checks
the received/programmed image; the host also retains independent readback and
fresh checks on both Picos.

`main` intentionally retains v0.104 firmware. Both physical Picos currently run
verified v0.106: the v0.105 batch implementation with only a version bump to
exercise a new/new transfer. Automatic propagation worked, but its measured
speed does not establish batch use or acceleration; batching remains unmerged.
Publishing `main` does not downgrade the devices, and the normal updater refuses
an older candidate.

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
original binding, not a reusable configuration. There are no new options or
custom picotool dependencies. Keep cables connected and other USB tools idle:
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
"looks good" after the requested input checks. All 118 host updater tests pass. This successful upgrade does
not establish reliability across every intermittent USB failure.

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
   Validate the old image and version, then load the UF2 with picotool verification.
4. Independently read the complete firmware slot and saved settings. Require an
   exact image match and byte-for-byte unchanged settings. Only then request a
   normal application reboot.
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

The host workflow has exercised serial bootloader entry, backup, verified load,
independent readback, unchanged settings, reboot and legacy propagation on
hardware. Both boards currently have verified v0.106 images. The pinned-selector
implementation has completed the physical upgrade above. B's receiving samples
show about 7.4 kB/s, which does not prove use of the accelerated batch path;
that feature's acceptance remains pending. See the runbook for each preserved
result, the separate diagnostic verification, and the batching evidence limit.

The improvement is eliminating manual pauses between already-tested steps and
reusing unchanged prepared images. Previous retained evidence shows roughly
36 seconds for peer propagation; this tool does not reduce that UART transfer
time. New per-phase timings will quantify the end-to-end improvement after a
separately authorized hardware run.
