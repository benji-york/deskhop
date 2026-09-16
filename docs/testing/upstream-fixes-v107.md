# Upstream bug-fix integration for v0.107

Status: implementation and hardware-free validation complete; frozen candidate
ready for device testing. No hardware has been flashed, and `main` and the two
devices remain at accepted v0.106. Main merge and push are deferred until
physical acceptance.

## Scope and provenance

Branch `codex/upstream-fixes-v0.107` starts at accepted `main` commit `1da1af2`.
Upstream was refreshed to `c220d0c` on 2026-09-16. The integration selectively
adapts these changes without importing upstream's older version number or
replacing the fork's customized configuration page and generated disk image:

| Upstream change | Source commit | Integration |
| --- | --- | --- |
| #369: RP2040 USB DPRAM copies | `801d388` | Byte-wise volatile copies at the TinyUSB DPRAM boundary. |
| #370: PIO USB transaction retries | `9782d5b` | Count consecutive failed transactions, stop on the third, and preserve SETUP dispatch on retry. |
| #359: separate keyboard report layouts | `fff129f` | Parse-time registration per report ID, with non-allocating lookup at decode time. |
| #364: screensaver timer units | `9f193de` | Display/edit seconds while retaining microseconds on the wire and in settings. |

Earlier #359 split-bitmap, short-block and aggregate-NKRO work is already in
the fork and is not reapplied. Upstream comment/readability-only changes are
not imported wholesale. The generated artifacts from upstream are not copied;
the customized local templates are regenerated instead.

## Local safety adaptations

The keyboard integration preserves the fork's report bounds, descriptor
rejection, keyboard-state recovery, modifier release, synthetic right-click,
hotkeys and boot-protocol fallback. Allocation skips padding-only fields;
the fifth supported keyboard layout can finish parsing even once the slot
count reaches its limit. Additional report IDs cannot overwrite an existing
layout or route through the primary keyboard. The existing exact NKRO
usage-count rule, four-block limit and five-keyboard capacity are unchanged.
This separates descriptor layouts; it does not redesign held-key aggregation
across report IDs on the same physical input source.

Timer conversion must not silently wrap at 32 bits or lose precision through
JavaScript floating point. The existing protocol exposes seven value bytes
on read but only six for proxied writes. Timer-only decimal-string/BigInt
conversion therefore reads the available 56 bits exactly and bounds writes
to 48 bits. Larger unchanged values read from a device must be left unsent
when saving other settings. The upper eight bits of a stored 64-bit timer
are not observable through the current protocol; this integration does not
claim to extend that protocol or complete the separate configuration-validation
workstream.

## Hardware-free validation

Focused HID regressions compile the actual parser, report decoder, keyboard
logic and USB callback under ASan/UBSan. They cover mixed 6KRO and independent
120-bit NKRO collections in either descriptor order, arrays with/without a
reserved byte, per-report modifiers/usages, repeated lookup, all five slots,
ignored excess IDs, and padding-only/unknown reports. The real TinyUSB host
stack harness also enumerates a mixed-report keyboard beside a mouse and
tests interrupt delivery, report rearming and detach all-up behavior.

The vendor-USB harness extracts actual production function bodies verbatim and
uses deterministic bus seams under ASan/UBSan. It covers consecutive-failure
cutoffs, success/NAK resets, STALL, SETUP retry payload/toggle/length, endpoint
isolation, and DPRAM copies for both buffers with pointer offsets 0–7 and
lengths 0–129. An optimized Cortex-M0+ assembly check requires byte loads and
stores. Six compile-valid source mutations are rejected. Three attempts total
are allowed for consecutive transaction failures, not three additional retries.
See [the USB test limits](../../tests/upstream_usb/README.md).

Configuration-page tests cover exact fractional-second roundtrips, values above
2^32 microseconds, 48-bit write and 56-bit read boundaries, unchanged large
values, invalid-input feedback, whole-Save preflight rejection, zero/unlimited,
unscaled fields and generated-page parity. Existing auto-start/CRC and Bootloader
page tests also pass. The customized generated page is embedded in the 65,536-byte
disk image; extracting `config.htm` from that image matches it byte-for-byte.

These two focused suites are registered in the ordinary fast/deep runner; the
USB mutation checks additionally run in the deep tier. All **55 deep-tier steps**
passed (including the 44 fast-tier steps and all 118 updater tests). This includes
20,000 adversarial HID cases, real TinyUSB host/device harnesses, 16 storage
seeds, paired production scenarios, 24 fixed core orders for each of five timing
scenarios, mixed-version transfer, 32 generated end-to-end seeds, 26 storage
mutations, 23 paired-production mutations and baseline behavior contracts.
Independent reviews of the HID allocation and timer protocol changes found
no introduced blocker. These are finite checks, not exhaustive race proofs.

Host-side tests do not execute PIO instructions, USB electrical timing or the
RP2040 USB controller and do not prove physical enumeration reliability.

## Frozen candidate

Prepared with no hardware access:

```sh
make release TEST_TIER=deep \
  TOOLCHAIN_DIR=/opt/homebrew/opt/arm-gcc-bin@14/bin \
  RELEASE_DIR=/Users/benji/Documents/ChatGPT/DeskHop/build/releases
```

Validation took 247.3 seconds; ARM configure/build also passed. RAM usage is
230,164 bytes (87.80%); the fixed linker image regions retain their normal size.
The candidate retains UART frame version 1 and configuration version 10, and
contains no saved-settings page.

- Manifest: `/Users/benji/Documents/ChatGPT/DeskHop/build/releases/deskhop-v0.107-607q82je/manifest.json`
- Full-slot CRC32: `6579b48f`; metadata CRC32: `a35e869c`.
- BIN SHA256: `db33516cb8f81c4a69871a5a0cb297242112ed524be80d75cbe6320a5cee7b9c`.
- UF2 SHA256: `96b232053fd34fd6fe3c9f2ed8a6b36fa55df943b435baad899e65eb2b531d2e`.
- Evidence: `/private/tmp/deskhop-upstream-fixes.FmFwV1/build/updater/prepare/prepare-k3w61n8u/`.

The release was frozen before committing the integration: its recorded Git HEAD
is the base plus dirty inputs, with an exact source archive and file hashes.
Those source fingerprints, not base HEAD alone, identify the validated candidate.
Subsequent documentation-only results do not alter the firmware or test inputs.

## Device acceptance still required

- Ordinary typing, modifiers, all DeskHop/QMK shortcuts and switching both ways.
- Trackball motion/buttons and keyboard-generated right-click on both Macs.
- Keyboard LED focus arrows, zoom assist and keep-awake settings unchanged.
- Config-page timers shown in seconds; save/readback retains the intended
  durations, with `0` still unlimited for the maximum-time setting.
- Keyboard/trackball enumeration after normal reconnect/reboot.

The host updater and UART-v1 framing remain unchanged. A future explicitly
authorized flash must retain backups, independent readback, unchanged-settings
checks and fresh verification on both boards. This integration does not add
batch-negotiation diagnostics or make a new batch-speed claim.
