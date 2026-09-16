# Configuration validation, v0.109

Status: deployed, firmware-verified and user-accepted on both Picos on 2026-09-16.
Benji confirmed "Looks good; please merge and push" after returning from AFK,
authorizing publication of the tested release. Development was on
`codex/configuration-validation-v0.109`, based on hardware-accepted main
`4549ade` (v0.108).
Working tree: `/private/tmp/deskhop-config-validation.I4A81W`.
The original uncommitted draft in the `03bb` worktree is preserved unchanged;
only its relevant changes were ported onto current source.

## Contract

- Reject invalid configuration SET values on both direct WebHID and proxied
  UART paths before publishing a new configuration.
- Output numbers and the configuration-format version are read-only, not
  user-editable routing or migration controls.
- Monitor counts are 1 through INT32_MAX; speeds are 1 through 128; borders
  satisfy `0 <= top < bottom <= 32767`. Booleans and enums must be recognized
  values, and unused incoming value bytes must be zero.
- Updating a monitor count clamps its current screen index. Configuration
  readers take coherent snapshots and release the lock before USB, queue or
  flash work. Mouse consumers also defend against malformed RAM fixtures.
- Pointer calibration updates only the chosen border edge under the lock,
  preserving concurrent changes to the opposite edge. Save and peer traffic
  happen after publication and outside the configuration lock.
- The legacy API carries at most 48 bits for a proxied timer SET, so direct
  SET uses that same limit. An accepted timer SET replaces all eight storage
  bytes, rather than leaving stale upper bits. Existing full-width uint64
  persisted timers remain valid and are preserved on load, migration, unrelated
  edits and Save. The existing GET format still exposes only 56 bits.
- Load validates integrity/version first, then repairs invalid fields using
  field defaults, repairs output identities/border pairs and clamps indices.
  Valid independent settings survive. Current-format repairs stay in RAM until
  explicit Save; existing v8/v9 migration persistence remains in place.
- Save refuses semantically invalid RAM settings. Settings layout/version 10
  and UART frame version 1 remain unchanged. Firmware version increases to
  0.109; compatible automatic propagation succeeded during deployment.

## Configuration-page boundary

Preserve v0.108's seconds-based timer display and exact BigInt conversion.
Validate edits before sending them, retain unchanged legacy-large timers, and
apply each border pair in an order that keeps intermediate values valid.
The page must distinguish a validated connected-device RAM readback from a
request to persist on both devices: the existing protocol has no flash-commit
or peer-SET acknowledgement. Do not claim atomic two-board Save or confirmed
peer persistence. A peer starting with different borders can reject an edit
whose ordering was valid for the connected device; this limitation is not
silently bypassed by weakening receiver validation.

## Validation and release

The complete deep tier passed all 61 steps on 2026-09-16 (269.5 seconds), followed
by a successful ARM release build. Independent source review found a border
read-modify-write race; it was fixed and covered by a production-handler
interleaving regression before the final run. Validation includes:

- 8,546 direct/proxied configuration field and padding cases under ASan/UBSan.
- Both actual accepted-v0.108 API/mouse crashes reproduced against pinned
  `4549ade4bb34084f31825a7cdfd57c20e13f35cd`, then rejected safely in this image.
- Real storage load/save/migration, per-field repair, full-width timer
  preservation, invalid-RAM Save refusal, coherent timer/border publication,
  concurrent calibration and count/index changes, across 16 storage seeds.
- Real TinyUSB host/device tests, paired production firmware, bounded core-order
  exploration, mixed-version transfer and 32 generated-input seeds.
- All 29 storage and 23 paired source mutants caught at runtime, plus upstream
  USB source mutations and historical behavior comparisons.
- Config-page numeric validation, exact timer conversion, safe border ordering,
  serialized readback, timeout/transport failures, and generated HTML/FAT parity.

The baseline crash witness is pinned to accepted v0.108 so an earlier failure or
already-fixed timer bug cannot be misattributed to this release.
The device stack, paired simulation and modeled storage tests remain bounded
evidence, not proof of every hardware interleaving.

Frozen candidate (includes exact source snapshot and validation evidence):
`/Users/benji/Documents/ChatGPT/DeskHop/build/releases/deskhop-v0.109-6wekbwwa/manifest.json`.
Full-slot CRC32: `9ea98c93`; metadata CRC32: `f2de4b19`.
BIN SHA-256: `681f6b69eb2c9e37785242d467346a1677e8c07ee1308102e5b2b263abddbd1d`.
The frozen manifest records the then-uncommitted working-tree snapshot, not
misattributed to its v0.108 HEAD. Subsequent source publication does not relabel
that evidence or change the deployed image. Test/build logs are also retained under
`build/updater/prepare/prepare-udq3endy/` in the working tree.

## Hardware deployment

Benji authorized flashing while AFK. The maintained updater ran once with the
explicit frozen manifest and stock picotool 2.3.1, without a debugger, retry,
power cycle or cable move. Evidence in the canonical repository:
`build/updater/runs/20260916T181039Z-t7wuxvw8/`.

- Preflight identified both expected boards running v0.108. A's firmware and
  settings were backed up before writing. All 262144 firmware bytes matched
  v0.109 on independent readback; all 4096 local settings bytes were unchanged
  (SHA-256 `aa816af793193a7ce487d391b49bf45d514dde09bd77d28255eb782e82a85464`).
  The backed-up configuration version is 10, requiring no format migration.
- A rebooted normally and B's automatic receive/reboot was observed. Both now
  report build 0.109, metadata CRC `f2de4b19`, idle updates and advancing cores.
  UIDs remain A `E6654854574C3E30`, B `E6654854577F2330`; new boot sessions are
  A `5040c95b154b799f`, B `98ed28c1ddd668fe`.
- Both boards passed the full correct/wrong/correct verification sequence:
  expected `9ea98c93` produced PASS, deliberate `9ea98c92` produced
  FAIL/crc_mismatch, then `9ea98c93` produced PASS again. Every scan covered
  all 262144 bytes with stable image generations and valid/advancing cores.
- Updater exited zero: `stage=complete`, `firmware_verified=true`,
  `independent_readback=true`, `settings_unchanged=true`, total 50.627909 seconds.
  Backup took 0.566548 seconds, load/verify 3.892554 seconds, independent
  readback 0.569220 seconds; peer wait was approximately 37.38 seconds.
  Media/USB health checks passed. Three expected disconnected-device errors
  from closing the old CDC handle remain recorded separately from successful
  commands, as in the preceding successful deployment.

Settings preservation above is A's external, pre-application-reboot comparison;
B's settings were not independently read. B's firmware verification is its fresh
on-device full-slot scan, not external picotool readback. This success does not
establish batch-mode acceleration or resolve the parked intermittent ROM hang.

## User acceptance and publication

After the completed deployment, Benji was asked to check typing, mouse/right-click,
switching and config-page Read/Save. His reply was:

> Looks good; please merge and push

This accepts the deployed release and authorizes its commit, merge and push.
The original automatic journal remains `input_acceptance=pending`, since it
predates that reply; separate `user-acceptance.json` in the deployment evidence
directory records the later confirmation. Do not interpret this general
acceptance as an exhaustive live malformed-config, race or keep-awake soak test.
Broader reboot/power-loss safety, updater simplification and the parked ROM hang
investigation remain separate workstreams.
