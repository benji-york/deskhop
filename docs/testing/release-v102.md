# v0.102 completed-fix release and deployment

Prepared 2026-09-15 at the user's request to commit, push, and flash the finished
fixes. **Both Picos are programmed, externally verified, and running v0.102.**
Paired runtime verification passed both before and after returning B's cable to
its original Mac. Benji confirmed "Working great" after the requested typing,
trackball/right-click, and switching checks. **v0.102 is the current user-accepted
paired deployment.**

## Scope and history

The already committed fixes are retained as separate ordered commits:

1. `b8992dc16370bbb1ce00a238463ec6e8207d0883`: authoritative keyboard recovery.
2. `5499e8fe7c297b33c4890de870f47125a0433790`: protected UART command frames.

Both descend from the accepted v0.101/testing framework commit `af100bc`.
The two existing fix branches were pushed and their remote hashes verified.
The release branch adds firmware version **0.102**, encoded as **202**, and
keeps configuration format **10**, its layout and saved settings unchanged.
Main is not merged or moved. Unfinished configuration validation (#4) and
reboot safety (#1) remain excluded; their worktrees are preserved.

## Known limitations

- Old and new UART frames are incompatible by design. A one-sided upgrade
  cannot communicate with, update, or issue maintenance commands to the old
  peer. Both Picos must be programmed independently for this migration.
- The configuration page's Bootloader button currently passes a boolean where
  the report encoder expects a byte collection and throws before sending.
  Keyboard maintenance shortcuts are unchanged. This known UI regression is
  disclosed to the user; its unfinished follow-up fix is not included here.
- After migration, open the Web Config page bundled with this firmware rather
  than a cached old page: USB configuration reports now use the CRC8 format.
- Configuration semantic validation and controlled-reboot safety are not fixed
  in this release. Do not experiment with invalid settings or invoke resets
  while firmware is being written. The existing updater is not power-loss atomic.
- CRC32 is accidental-corruption detection, not authentication or general replay
  protection. Native tests do not establish physical USB/UART/macOS acceptance.

## One-time migration procedure

Do not follow the older runbook instruction to flash A and wait for propagation.

1. Complete validation and freeze one versioned binary/UF2 pair. Record the
   payload metadata CRC, full-slot CRC, and SHA-256 separately.
2. Confirm both live boards are stable on the old firmware, not updating.
   Preserve their settings. The user confirmed B can be connected directly to
   this Mac and accepted deployment with the Bootloader-button limitation.
   Confirm independent input is available while DeskHop is in maintenance.
3. Move B's computer-facing USB connection to this Mac while both still run
   the old firmware. If that restarts B, let it finish normal startup and
   reconfirm both identities/stability. While the old UART link still works,
   use **Layer 3 B** to put B into the disk-free PICOBOOT bootloader, then use
   **Layer 3 A** for A. Never assume the B shortcut works after A is new.
4. Use official picotool selected by UID. For each board, back up firmware
   `[0x10000000, 0x10040000)` and settings `[0x101ff000, 0x10200000)`.
   Program/verify the frozen UF2, independently read both ranges back, require
   all firmware bytes to match and every settings byte to remain unchanged.
   Stop on errors or stale/busy USB/media services. No automatic reboot while
   verification is incomplete.
5. Reboot both normally once independently verified, while both host cables
   remain on this Mac. Check their new protected UART link before returning B's
   host cable to its original Mac; repeat the runtime check after relocation.
6. From a confirmed DeskHop console, request fresh statuses and
   `verify 0.102 <full-slot CRC from the candidate manifest>`. Require both
   identities, new/stable sessions, advancing core counters, and both complete
   flash scans to match. This supplements external readback, not merely boot
   metadata comparison.
7. Ask the user to test typing/releases, trackball and keyboard right-click on
   both Macs, switching, Pico LEDs/Sofle arrows, zoom assist and keep-awake.
   Record the result; do not infer it from a successful flash.

Known physical identities from prior accepted deployment (reconfirm live):
**A `E6654854574C3E30`**, **B `E6654854577F2330`**.

Disk-free keyboard maintenance is preferred because a previous mass-storage
teardown coincided with a Mac panic. Do not use `picotool reboot -u` or a mounted
UF2 copy as an implicit substitute. Finish cable relocation before maintenance;
if a board unexpectedly restarts later, stop and re-establish a safe maintenance
path before touching either image.

### Pico B programming

At 2026-09-15 17:04:33 UTC, B's direct programming and external verification
completed. `picotool load -v` passed. An independent 262,144-byte readback
exactly matches the frozen v0.102 binary; all 4,096 settings bytes match the
pre-write backup (SHA-256
`aa816af793193a7ce487d391b49bf45d514dde09bd77d28255eb782e82a85464`). The old
firmware backup exactly matched the accepted v0.101 binary.

B was deliberately left in disk-free PICOBOOT pending A's upgrade. Evidence
and backups: `build/flashing/pico-b-v102-retry.AbTELH/`, including `result.json`.
The initial backup attempt returned an unspecified RP2040 error and created no
backup files; no writes occurred then. Reconnecting B and reentering maintenance
resolved both small and full backup reads. The underlying cause is unproven;
the earlier error record is `build/flashing/pico-b-v102.ZXWOfu/result.json`.

### Pico A programming and paired runtime verification

At 2026-09-15 17:08:37 UTC, A's direct programming and independent readback
completed. All 262,144 firmware bytes matched the frozen v0.102 binary; all
4,096 settings bytes remained unchanged. Its pre-write firmware matched the
accepted v0.101 binary; its settings backup has the same SHA-256 recorded above
for B. Evidence: `build/flashing/pico-a-v102.kHhVDP/`, including `result.json`.

Both devices were selected by their exact flash UIDs, and each exposed only
the class-255 PICOBOOT interface, with no mass-storage interface. After successful
external verification, `picotool reboot -a --ser <UID>` returned success for A
and then B, by 17:09:05 UTC. Both normal DeskHop USB identities reappeared and
no RP2 Boot device remained. The same 20 Mac media clients remained active and
nonbusy. No settings writes or automatic peer firmware update were needed.

The bounded read-only checker passed at 17:09:27 UTC in 5.728 seconds, receiving
11,064 bytes. Five statuses confirmed both builds, stable sessions, idle updaters,
and advancing counters on all four cores. Correct/wrong/correct expected CRCs
produced PASS/FAIL/PASS for three fresh full-slot scans on each board. The
intentional wrong-CRC failure is a negative control, not a firmware defect.
Every scan measured `dabb9b75`, separately from boot metadata CRC `d9e9f64d`,
and took 1.121921–1.140241 seconds. The peer responses demonstrate communication
over the new protected UART protocol, not automatic-update acceptance.

Both boot sessions changed from the last pre-maintenance check:
A `37dad6e6dd90a647` → `b0bce00bb487c0ee`;
B `278b9e1e3603606d` → `78a478e1fcdf0bdd`.
Evidence: `build/flashing/console-v102-smoke-20260915T170927.300864Z.json` and
matching `.txt`. The checker was adapted from v0.101 and passed its offline
full-sequence self-test, including 42 rejected invalid fixtures, before use.

Both host cables were still attached to this Mac during that first runtime check.

### Post-relocation check and user acceptance

On 2026-09-15, Benji replied "Working great" to the request to return B's host
cable to its original Mac and test typing, trackball/right-click, and switching
on both Macs. A fresh USB inventory then showed only A attached to this Mac,
with no RP2 Boot device. The same read-only console checker passed again via A
at 17:13:57 UTC in 5.748 seconds (13,027 bytes), with B responding over UART.
Five statuses confirmed both v0.102 builds, stable sessions, idle updaters and
advancing counters on all four cores. Three fresh full-slot checks again gave
PASS/FAIL/PASS for correct/wrong/correct expectations; all measured `dabb9b75`
with boot metadata CRC `d9e9f64d`. Scan durations were 1.110871–1.136939 seconds.
A retained session `b0bce00bb487c0ee`; B had restarted into `93c98994a187e251`
and remained stable throughout the check.

Evidence: `build/flashing/console-v102-smoke-20260915T171357.580848Z.json` and
matching `.txt`. The migration and basic input acceptance are complete. The
user's report does not separately establish LEDs/arrows, zoom assist, keep-awake
soak or future automatic-update acceptance. The known limitations above remain.

## Validation and deployment evidence

The completed source stack recorded fast **38/38**, deep **46/46**, ARM **2/2**,
97 paired scenarios, 12 UART scenarios, 72 keyboard cases, and additional
scheduling checks. Those runs preceded this version-only release preparation.
The v0.102 release tree was rebuilt and independently rerun: fast **38/38**,
deep **46/46**, ARM **2/2**, with all results successful. Deep includes the
97 paired scenarios, 24-order scheduling explorations, 32 generated-input seeds,
21 storage and 17 paired mutation checks, and nine historical behavior contracts.
Native fixtures retain explicit historical build identities where appropriate;
the actual ARM artifact's metadata was independently checked as version 202.
RAM use remains **220,092 / 262,144 bytes**. Physical runtime and basic input
acceptance are recorded separately above, not inferred from these test results.

Frozen files are under `build/flashing/` in the main workspace:

- `deskhop-v0.102-completed-fixes.uf2`, SHA-256
  `31f878e9595b84f4c86e7ec3fcad421717f459a956f0bbf6a9c9b57c2a49282d`.
- `deskhop-v0.102-completed-fixes.bin`, SHA-256
  `5269f04202cdf8e76e232206c2d73b333a5bd33c9f07ef3c17a9480cadf16bcb`.
- `deskhop-v0.102-completed-fixes.elf`, SHA-256
  `284523d3de42636c67daada75faee43eca0aeae7c2b7610e6b705ba438f4c63f`.
- `v102-candidate.json`: source hashes, build/test evidence, artifact identities,
  settings exclusion, migration requirements and hardware status.

The freezer independently validates all 1,024 UF2 block addresses/headers,
reconstructs and compares the entire 262,144-byte binary, verifies the embedded
65,536-byte disk image, and recalculates the metadata checksum. The payload
metadata CRC is **`d9e9f64d`**; the separate full-slot CRC is **`dabb9b75`**.
The post-flash console command is `verify 0.102 dabb9b75`.

Results: `build/tests/results-{fast,deep,arm}.json`. Frozen artifacts never
include the settings sector. `build/flashing/freeze_v102.py` refuses to replace
an existing artifact with different bytes. Do not rerun that preparation helper
after deployment starts: per-board hardware results now update the manifest.

### Pre-migration hardware check

At 2026-09-15 16:46 UTC, the existing bounded read-only console checker passed
in 5.816 seconds. Five statuses showed both boards executing v0.101, stable
sessions, idle updaters, and advancing counters on both cores. Three fresh
full-slot checks using correct/wrong/correct expected CRCs returned
PASS/FAIL/PASS on both boards, with the expected artifact CRC `2db89640` and
boot metadata CRC `be404f8f`. The intentional wrong-CRC failure is a negative
control, not an installed-image defect.

Sessions remained `37dad6e6dd90a647` (A) and `049fe6e4d5d3495e` (B). Evidence:
`/Users/benji/.codex/worktrees/9235/DeskHop/build/flashing/console-v101-smoke-20260915T164612.734816Z.json`
and its matching `.txt` transcript. The Mac had 20 active, nonbusy media clients
and no RP2 Boot object. This is pre-upgrade evidence only.
