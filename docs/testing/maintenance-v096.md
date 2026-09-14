# v0.96 disk-free maintenance deployment

Prepared on 2026-09-14 from `8800d79` plus the changes described here. Pico A
was flashed and rebooted at 20:39 UTC and is confirmed running v0.96. The new
disk-free shortcut passed its physical enumeration check. Benji confirmed
normal input operation; the peer build remains independently unverified.

## Device health before continuing

At 20:33 UTC Pico A (`E6654854574C3E30`, `/dev/cu.usbmodem21203`) passed the
bounded read-only serial check: fragmented help/status, stable identity,
increasing uptime, a paused reader, and close/reopen cleanup. It reported:

```text
board=A
build=0.95
image_crc_at_boot=237a0b65
boot_session=3152032dc4f341dd
uptime_ms=2336528 ... 2338263
```

This is a different boot session from the check before the Mac panic. It is
consistent with the Pico restarting around the Mac restart, but the reset
cause is not captured by v0.95 and is unknown. The CRC is boot metadata, not a
fresh integrity calculation. The earlier independent flash readback remains
the evidence for the programmed bytes.

At 20:34:58 UTC there were no RP2 bootloader USB objects and no inactive/busy
media clients (20 clients inspected). Benji confirmed typing, trackball
movement/buttons, and Layer 3 S switching all work on both Macs. Pico B's
executing version is still not independently verified.

Evidence is retained in `build/flashing/console-v095-smoke-20260914T203313.041765Z`
(`.json` and `.txt`) and `health-postpanic-20260914T203458Z.json`. These checks
establish current behavior; they do not prove the storage panic cannot recur.

## Behavior change

Both the local A shortcut and the received B maintenance request call
`reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 1)`. Bit 0 disables the ROM mass-storage
interface while retaining PICOBOOT for official picotool. The B request's UART
format is unchanged. Both boards must install v0.96 before both shortcuts use
the new mode; an old peer retains its old behavior until updated.

This changes only the two routine entry points and the build version. Physical
BOOTSEL and invalid-image recovery retain UF2 disk recovery. Configuration mode
and debug-only entry paths are unchanged. The existing immediate-reset behavior
of the A/B shortcuts is unchanged; do not use them during a peer update.
There are no new console commands, scheduler changes, or input-path changes.

## Validation

- `python3 tests/run.py fast`: all 26 recorded steps passed, including 42 paired
  scenarios, real TinyUSB device/host stacks, storage transactions, and native
  sanitizer tests. Results/log: `build/tests/results-fast.json`, `fast-v096.log`.
- `python3 tests/run.py arm`: configure and build passed. Main RAM remains
  172,192 bytes (65.69%), plus 2 KiB in SCRATCH_X. Log: `build/tests/arm-v096.log`.
- New paired scenarios send actual Layer 3 A/B keyboard reports through
  production handlers and UART. Each checks the target's ROM interface mask,
  the other board's continued watchdog progress, and the unchanged B packet.
  Both scenarios also passed seeds 1–8. Temporary source copies restoring each
  old mask failed the corresponding new test.
- Existing corrupt-UF2 and corrupt-peer-image tests now assert recovery mask 0.
  This preserves the manual recovery route.
- `git diff --check` passed. The deep tier was not rerun for this two-call change.

The simulator observes the requested mask at the ROM boundary. Physical USB
enumeration without a disk also passed on Pico A, as recorded below.

## Frozen artifact

- UF2: `build/flashing/deskhop-v0.96-maintenance.uf2`
- SHA-256: `4e36e037ebb30c03c1707b8cd04c59bc79b227051aff4980285f7e732e17402e`
- Binary SHA-256: `eee12d588e1489571e18654df2f4bded66839f6f4d6ae47759114f217b5ce74d`
- Metadata: magic `0xf00d`, version `196`, CRC `d4fdee05`.

An independent check recalculated the CRC over the first 258,048 bytes and
validated all 1,024 unique RP2040-family UF2 blocks against the binary. The
262,144-byte image occupies `[0x10000000, 0x10040000)` and excludes saved
configuration at `[0x101ff000, 0x10200000)`. The manifest is
`build/flashing/v096-candidate.json`.

Official picotool v2.3.1 is available under
`build/tools/picotool-2.3.1/picotool/picotool`. Its downloaded macOS archive
matches the [published release](https://github.com/raspberrypi/pico-sdk-tools/releases/tag/v2.3.1-0)
SHA-256 `7bb3cea5d9f1b000fc758cf4b54774fda5c16a9899d342ef6b0c30138988793b`.
It is local build tooling, not a system installation.

## One-time transition from v0.95

The current read-only console has no command to enter PICOBOOT-only mode.
`picotool reboot -u` enables mass storage on RP2040, and its force options need
a reset interface that this firmware does not implement. Neither Linux nor an
SWD probe is available for this session. Installing the candidate therefore
requires one legacy bootloader entry that will still expose a USB disk to this
Mac. Direct picotool transfer does not remove that initial exposure or guarantee
against another panic. See the [incident analysis](macos-panic-20260914.md).

The remaining exposure was explained, and Benji authorized the transition
with "go" after entering the bootloader. The completed deployment is recorded
below. This procedure remains relevant for another board still on v0.95:

1. Recheck that the USB/media registry has no stale bootloader state. Keep both
   boards powered; close any serial session. Ask Benji to use Layer 3 A when
   ready for this specific transition.
2. Identify the active Pico using flash UID `E6654854574C3E30`. Inspect the
   registry for new inactive/busy media state. Do not retry hung mount commands
   or use Finder/config-drive copying. Stop if the earlier stale state returns.
3. With bounded picotool calls selected by `--ser E6654854574C3E30`, save the
   firmware range and saved-configuration range into new before-v096 files.
4. Load the frozen UF2 with `load -v`, without automatic reboot. Read both
   ranges again and compare every firmware byte with the frozen binary and
   every configuration byte with the backup. Stop on any mismatch.
5. Reboot normally only after readbacks pass. Wait for DeskHop CDC, check v0.96,
   CRC metadata `d4fdee05`, a fresh boot session and increasing uptime, and
   inspect for stale media services after bootloader removal. If stale state
   appears, stop further device transitions; leave the Picos powered and let
   Benji save work and restart the Mac before resuming.
6. Allow peer propagation to settle and repeat the input check on both Macs,
   including whether the trackball needs reconnecting. Local status alone does
   not verify the peer's executing version.
7. In a separate interactive check, use A's shortcut again and verify that ROM
   exposes PICOBOOT with no mass-storage interface/media object. Reboot normally
   with picotool without writing. Repeat for B only after its update is known.

Once this prerequisite passes, resume the peer-status diagnostic slice.

## Completed Pico A deployment

The active bootloader was identified by flash UID `E6654854574C3E30`. All 22
media clients were active and idle before writing. The 262,144-byte backup
exactly matched frozen v0.95 (encoded version 195, CRC `237a0b65`); saved
configuration was separately backed up. Official picotool `load -v` passed.
Independent firmware/configuration readbacks then confirmed every new firmware
byte and every unchanged setting byte before normal reboot at 20:39:14 UTC.

The first post-reboot USB/media snapshot at 20:39:23 UTC had no RP2 object and
no inactive/busy clients (20 clients total). DeskHop HID and CDC re-enumerated
on the same physical UID and callout port. The bounded serial check passed:

```text
board=A
board_id=E6654854574C3E30
build=0.96
image_crc_at_boot=d4fdee05
boot_session=13d7da4d302c9de8
uptime_ms=15282 ... 17021
```

The session differs from the pre-flash session. Fragmented/repeated commands,
a paused reader, and close/reopen cleanup all passed. CRC displayed by the
console remains metadata; the independent full readback establishes flashed
bytes. The peer's executing build is not independently established. The later disk-free
entry check is recorded below.

Retained artifacts under `build/flashing` include `pico-a-v096-result.json`,
before/after firmware/configuration binaries, per-step picotool logs,
`pico-a-postreboot-v096-health.json`, and
`console-v096-smoke-20260914T203930.035066Z.{json,txt}`. The manifest records
`hardware_flashed: true` with these remaining acceptance limits.

## Disk-free shortcut acceptance

Benji reported "Works well" after the input/switching check, then confirmed
"L3-A pressed." At 20:41:36 UTC A was active in ROM boot mode with exactly one
vendor interface (`bInterfaceClass=255`) and no mass-storage interface. The Mac
still had 20 media clients, all active and idle: no bootloader disk had appeared.
Official picotool identified the same flash UID through that interface.

A normal picotool reboot returned the board to DeskHop without another flash.
The serial check `console-v096-smoke-20260914T204323.559169Z.{json,txt}` passed
with a fresh boot session. A later registry check found no retained RP2 object
or inactive/busy media clients. The deployment result now records this hardware
check and basic input acceptance as passed. Whether a trackball replug was
needed on this update was not stated separately and is not inferred.

Pico B's version and its maintenance enumeration still require independent
verification. The next diagnostic slice adds the peer query needed to inspect
its executing build without moving cables.
