# Normal-upgrade verification policy

## Scope and status

This is a host-only simplification of the maintained Make/Python updater.
`Updater(..., verification_mode="normal")` is the default, exposed as
`--verification-mode normal|thorough` and Make's `VERIFY_MODE` variable.
No firmware source, wire protocol, version, image or device settings change is
part of the original policy change. All 134 original updater tests passed, and
the default normal read-only verifier passed on both Picos. That implementation
did not perform a full upgrade; its historical evidence remains unchanged.
The later authorized v0.110 deployment below now validates the actual-write
normal path on hardware. The expanded host suite has 141 passing tests.

Implemented on `codex/normal-upgrade-verification`, based on main `1080cd4`,
in `/private/tmp/deskhop-normal-upgrades.oz4m7n`.

The accepted physical firmware is now v0.110; see
[the transfer-profiling deployment](transfer-profiling.md).
The prior v0.109 deployment remains recorded in
[the configuration-validation record](configuration-validation-v109.md).

## Checks retained and omitted

Normal flashing retains:

- Frozen artifact integrity and compatibility checks; strictly newer candidate
  admission unless both devices already match, in which case it only verifies.
- Exact physical identities, disk-free ROM selection, session pinning and the
  existing USB/media gates before and after operations.
- A full 262,144-byte old-firmware backup and all 4,096 saved-settings bytes.
- Stock `picotool load -v`, including its full programmed-image byte comparison
  before normal reboot, plus the separate unchanged-settings readback.
- Bounded peer propagation, expected builds and boot metadata, required boot
  sessions, advancing cores, history and media health.
- One fresh expected-CRC full-slot scan on **each** running Pico, including
  coverage, identity/session and generation checks. Boot metadata alone is not
  a successful verification.
- Stop on any failure, retain evidence, and do not retry, reselect a device,
  power-cycle, or reboot a target after failed programming/verification.

Normal flashing omits the separate `firmware-after.bin` readback/comparison and
the wrong-CRC/final-repeat diagnostic scans. Thorough flashing retains both,
requiring exact bytes and correct/wrong/correct PASS/FAIL/PASS verdicts.

Read-only `verify` uses the same scan policy: normal does one expected-CRC scan
per Pico, thorough does the triplet. **Neither read-only mode enters ROM or
performs a ROM firmware readback, flash write, settings write or reboot.** An
already-current `flash` likewise takes the read-only path; selecting thorough
does not force it to rewrite or enter ROM.

## Why this does not remove programmed-image verification

Stock picotool 2.3.1 `load -v` reads the flash contents back to the host and
compares them with the input image, rather than merely checksumming transmitted
bytes. See the [official byte-comparison implementation](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/main.cpp#L5341-L5374)
at commit `2041936441b48a3cc53ae3da9e805229fe8f4e18`. The separate `save`/exact BIN
comparison repeats that integrity check while adding an independent captured
readback; thorough mode keeps that extra evidence.

The post-reboot DeskHop scans independently check both complete running slots,
including B's propagated image. They do not make a CRC accumulated from incoming
bytes equivalent to a flash readback, and they do not make the ROM's boot2-only
CRC a full-application check. No custom picotool or custom RAM helper is needed.

## Estimate from the accepted v0.109 run

Source evidence, in the canonical repository:

`build/updater/runs/20260916T181039Z-t7wuxvw8/`

| Measurement | Time | Treatment in normal mode |
| --- | ---: | --- |
| Entire historical deployment | 50.627909 s | Baseline only; not a normal-mode result |
| Stock `load-verify` | 3.892554 s | Retained, including its byte verification |
| Separate `firmware-after` read | 0.569220 s | Omitted |
| Separate `settings-after` read | 0.021128 s | Retained |
| Peer wait/settle phase | 37.379610 s | Retained and unchanged |
| Entire post-rollout diagnostic phase | 5.804341 s | Partly retained; not all removable |
| First expected-CRC request | 1.217598 s | Retained |
| Wrong-CRC request | 1.198414 s | Omitted |
| Final expected-CRC request | 1.214407 s | Omitted |

Command durations come from `commands.json`; phase intervals from `result.json`;
CRC request durations from TX through the corresponding `END verify` in
`serial-2.jsonl`. These are host-observed command durations, not isolated Pico
CRC CPU timings. Each console verification command checks both Picos.

The omitted command durations sum to approximately **2.982 seconds**, before
their associated pin/status observations and inter-command overhead. A practical
estimate is therefore **3–4 seconds** off a similar run, not a measured speedup
and not a claim that all 5.804 diagnostic seconds disappear. The roughly
37-second peer phase remains the dominant component; this change neither
proves batch transfer acceleration nor resolves the parked intermittent ROM
USB timeout.

## Validation and acceptance boundary

`make test-updater` passed all **134 tests** in 0.753 seconds. These cover
default/explicit mode selection, both target roles, exact operation order and
retained safety gates, expected scan counts, and failure-stop behavior in each
mode. In particular, failed loads, changed settings, corrupt or stale scan
responses, and stopped cores remain fatal. Independent review found no defects
in the workflow mode split. `git diff --check` passed; firmware, stock transport
and strict console-parser sources are unchanged.

On 2026-09-16, the new CLI's default normal `verify` completed against the
accepted v0.109 frozen manifest. Evidence in the canonical repository:

`build/updater/runs/20260916T183931Z-3a_sxmne/`

The preflight-to-complete phase interval was **3.367517 seconds**. Exactly one
`verify 0.109 9ea98c93` request produced fresh full-slot (262,144-byte) PASS
results for A and B, with progressing cores and passing history/media checks.
Both boot sessions remained unchanged. The journal records
`verification_mode=normal`, `firmware_verified=true`, `write_started=false`
and `reboot_requested=false`. Its `picotool_verified`, `independent_readback`
and `settings_unchanged` flags are false because those flash-only operations
were not performed. External commands were limited to USB/media inspection;
there were no picotool operations, bootloader requests, settings writes or
firmware writes.

This validates the new normal read-only path on hardware, not a complete
normal-mode upgrade or its total speedup. A later authorized physical upgrade
can measure that path. The accepted firmware and original v0.109 deployment
evidence remain unchanged.

A subsequent user-requested `make flash` with the explicit frozen v0.109
manifest exercised the already-current path in **3.602051 seconds**. Both Picos
passed the single fresh CRC request; `already_current=true`,
`write_started=false` and `reboot_requested=false`. No flash was needed because
this change is host-only. Evidence in the implementation worktree:
`build/updater/runs/20260916T190235Z-myx15ww1/`. This is not an actual-write
normal-mode upgrade test.

## First actual-write normal-mode validation: v0.110

After authorization, one maintained-updater run upgraded both Picos from v0.109
to diagnostic v0.110. Evidence is in
`/private/tmp/deskhop-transfer-profile.Zi4FBl/build/updater/runs/20260916T194457Z-1lktwwdf/`.
The journal records `verification_mode=normal`, successful firmware/settings
backups, `picotool_verified=true`, `settings_unchanged=true`,
`independent_readback=false` and `firmware_verified=true`. The expected duplicate
`firmware-after.bin` is absent. Exactly one `verify 0.110 56cf6a40` command
produced fresh 262,144-byte PASS scans on both boards; identities, new boot
sessions, progressing cores and history/media checks passed. B propagated and
rebooted automatically. No retry, power cycle or cable move was required.

Total: **47.821911 seconds**, compared with 50.627909 seconds in the prior
v0.109 thorough run, a 2.805998-second reduction. Readback/guard and post-rollout
diagnostic phases together fell about 3.159 seconds; peer wait/settle increased
about 0.380 seconds to 37.759860. These are distinct releases and runs, not a
controlled identical-image performance comparison. The dominant transfer cost
remains: new history confirms legacy word service, not successful batch service.

Benji answered "Everything works normally" for typing/modifiers, trackball and
keyboard right-click on both Macs, switching both ways, focus arrows and zoom
assist. Separate `user-acceptance.json` preserves that response without changing
the automatic run's original pending input flag. The diagnostic source was
uncommitted at that acceptance point. After accepting the subsequent v0.111
startup-guard deployment, Benji separately authorized merging/pushing the combined
hardware-tested change set. The original frozen v0.110 evidence is unchanged.
