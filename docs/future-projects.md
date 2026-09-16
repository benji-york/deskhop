# Potential future projects

Started 2026-09-16 from a discussion of unused flash capacity in then-deployed
v0.108; expanded the same day to include the remaining project docket. The
accepted device firmware is now v0.111, including the profiling/startup-guard
changes developed on `codex/firmware-transfer-profiling`. Benji authorized merging
and pushing this hardware-tested release after functional acceptance. Saving this docket does
not authorize implementation, hardware experiments, or removal of old
files/worktrees.

These are proposals and follow-ups, not promises about current behavior.
Section numbers identify topics; the suggested priority order is below.

## Current priorities and status

1. [Batch-transfer performance](#7-batch-transfer-observability-and-performance):
   the v0.111 startup guard is deployed and functionally accepted. This upgrade
   used 1024 pages, zero words/retries and took 23.740 seconds versus 47.822
   previously. The immediate diagnosis/fix is complete and its publication is
   authorized. Further tuning is optional, not a blocker.
2. [Power-loss-safe settings](#1-power-loss-safe-settings-storage), followed by
   [recoverable firmware installation](#2-recoverable-firmware-installation-and-rollback).
   These are separate projects, not part of the completed configuration validation.
3. [Acknowledged two-Pico configuration saves](#8-reliable-two-pico-configuration-saves).
4. [Composed end-to-end tests](#9-higher-fidelity-end-to-end-tests).

The [normal-upgrade hardware acceptance](#10-normal-upgrade-hardware-acceptance)
follow-up is now complete from the authorized v0.110 deployment.

The former combined reboot/backpressure project is now split into
[small voluntary-reset guards](#61-consistent-voluntary-reset-guards) and a
[larger mouse-buffering redesign](#62-mouse-buffering-and-backpressure-redesign).
Neither is the next priority: consider guards alongside another firmware
change, and defer the buffering redesign absent a reproducer or real symptoms.
Other lower-priority possibilities are [multi-report keyboard aggregation](#11-keyboard-state-across-report-ids),
persistent fault logs, saved profiles and RAM reduction (sections 3–5).
The [USB-ROM hang investigation remains parked](#parked-usb-rom-hang-investigation).
[Housekeeping](#repository-and-documentation-housekeeping) is separate from fixes.

## Historical baseline: v0.108 flash capacity and behavior

The capacity measurements below were recorded for v0.108, not remeasured for
v0.109. Re-measure before using them to size a new feature.

Each Pico has its own 2 MiB flash. The current
[memory map](../misc/memory_map.ld) reserves:

| Region | Size | Current purpose |
| --- | ---: | --- |
| Firmware code and initial data | 188 KiB | Approximately 143.9 KiB occupied in v0.108; 44.1 KiB padding available |
| Configuration webpage disk image | 64 KiB | Browser configuration utility, not saved user settings |
| Firmware metadata | 4 KiB | Version, identifying marker and checksum; record is 12 bytes |
| Firmware staging | 256 KiB | Reserved but unused by the current updater |
| Unassigned space | 1,532 KiB | Available for a future layout |
| Saved configuration | 4 KiB | Persistent settings |

The first three regions form the fixed 256 KiB firmware image. Peer propagation
and config-mode UF2 uploads currently overwrite the main firmware slot while
the application runs from RAM. The staging reservation does not provide
automatic rollback. See [peer update writes](../src/tasks.c) and
[UF2 writes](../src/ramdisk.c).

RAM remains the tighter constraint: v0.108 statically allocates approximately
225.8 KiB of the 256 KiB main RAM region, leaving 30.2 KiB before runtime
allocations. Spare flash is not interchangeable with RAM. Re-measure all figures
before planning against a newer release.

## 1. Power-loss-safe settings storage

**Benefit:** Preserve the last valid configuration if power disappears during a
save. Current saving erases and rewrites one sector; it does not retain a second
persistent copy.

**Possible approach:** Two independently erasable configuration copies, each
with a format version, generation number and checksum. Write and validate the
new copy before retiring the previous valid copy. Define a deterministic boot
selection rule and migration from the existing single-sector layout.

**Acceptance considerations:** Interrupt saves at every erase/program/commit
boundary; recover either the old or fully committed new configuration, never a
partially written one. Cover malformed records, generation handling, migration,
firmware-update preservation and write endurance. This is distinct from
validating configuration field values.

## 2. Recoverable firmware installation and rollback

**Benefit:** Reduce reliance on manual recovery after an interrupted upgrade or
a new image that fails to boot correctly.

**Possible approach:** Receive and verify a complete candidate in a separate
flash slot before replacing the installed image. Retain a known-good image and
design a small recovery/boot-selection mechanism with explicit boot confirmation.
Firmware slots are separate from the two physical Pico roles, A and B.

**Acceptance considerations:** Staging alone is insufficient: final installation,
boot metadata updates and rollback must also tolerate power loss. Test interrupted
transfers and installation, corrupt candidates, failed first boots, compatible
rollback, and recovery when the peer is unavailable. Preserve configuration and
account for schema/UART compatibility between old and new firmware. Coordinate
changes to the linker layout, image/checksum tooling, host updater and peer
protocol; do not assume existing updaters understand a new layout.

## 3. Persistent fault and upgrade diagnostics

**Benefit:** Retain useful evidence after a reboot or power cycle, when the
current RAM-only diagnostic history is lost.

**Possible approach:** A bounded journal of reset causes, fault summaries and
upgrade outcomes, with sequence numbers and checksums. Use buffered, infrequent
writes and rotate sectors rather than continually rewriting one location.

**Acceptance considerations:** No keyboard contents or other input payloads in
the persistent log. Bound storage, RAM and latency costs; tolerate torn writes
and corrupt records. Test that logging does not cause USB/UART timing failures.
Define what can survive an unexpected power cut; unsaved RAM history cannot be
guaranteed to survive. Provide read/clear operations with documented semantics.

## 4. Richer configuration resources and saved profiles

**Benefit:** Store more configuration-page help, named screen arrangements,
device compatibility profiles or calibration data without keeping everything
resident in RAM.

**Possible approach:** Expand the webpage region or allocate separate versioned
data storage, loading only the active profile or required resource into RAM.

**Acceptance considerations:** Keep application assets distinct from user data;
define update, migration, validation and backup behavior. Measure incremental
RAM use and firmware-transfer cost. Choose concrete user needs before expanding
the interface or introducing a general-purpose filesystem.

## 5. Selective flash residency to recover RAM

**Benefit:** Reduce RAM pressure by leaving suitable read-only tables, strings
or carefully selected code in flash instead of copying them into RAM.

**Possible approach:** Measure candidate sections first; retain timing-critical
USB/UART paths and all required flash-update execution paths in RAM. Review
both cores, interrupt handlers and indirect code/data dependencies before moving
anything.

**Acceptance considerations:** Flash accesses must not occur while the device is
erasing/programming flash. Moving executable code also changes the safety of
overwriting the installed image while running. Test configuration saves,
firmware updates and high-load input handling together; measure actual RAM
savings and worst-case timing rather than assuming flash residency is free.

## 6. Reboot and backpressure safety

**Priority reassessment:** The source contains a plausible failure path, but it
has not been reproduced on Benji's devices and its frequency is unknown. The
serious case requires congestion, a critical release and a partial firmware
update to coincide. Do not treat it as a demonstrated daily-use problem or
spend a release on a broad redesign without evidence of benefit.

### 6.1. Consistent voluntary-reset guards

**Scope:** A relatively small defensive improvement, to consider alongside
another firmware change rather than as the next standalone project. Audit
software-requested resets and consistently reject or defer inappropriate ones
while replacing an image, serving firmware to the peer, or performing maintenance.

**Remaining issue:** The critical mouse timeout and configuration-mode hotkey
can request a normal reboot without active/dirty-image checks. Some older
bootloader paths lack comparable admission checks. Direct ROM entry interrupts
an update but does not boot the partial application; distinguish it from a
normal reset. Triple-Q and serial maintenance already have stronger checks.

**Acceptance considerations:** Test reset requests during receiving, serving,
partial-image and maintenance states, including concurrent state changes. Keep
successful-update reboot and deliberate ROM recovery working. Retain the
watchdog for genuine hangs. Guards alone do not guarantee recovery after a
hardware watchdog or power loss during an update; that is section 2's project.

### 6.2. Mouse buffering and backpressure redesign

**Scope:** Larger, lower-priority work with its own complexity and regression
risk. Defer until a reproducer or actual symptoms justify it.

**Remaining issue:** Ordinary reports are dropped when the queue is full;
critical releases wait up to 100 ms and can request a watchdog reboot. Removing
the reboot alone would still lose the release. See [the mouse queue](../src/mouse.c)
and [the earlier reliability boundary](testing/keyboard-reliability-draft.md).

**Possible approach:** Retain pending button state/releases, retry without
blocking the input core, and define bounded overload behavior for motion and
click transitions. Handle focus changes, absolute/relative interfaces and
reconnects without replaying stale button-down state. This is distinct from
adding voluntary-reset guards.

**Acceptance considerations:** Under queue saturation, detach/reconnect and
switching during a drag, avoid congestion-triggered resets and ensure release
after service resumes. Other tasks must keep progressing. Do not promise every
input transition survives an indefinitely stalled host with bounded memory.

## 7. Batch-transfer observability and performance

**Status:** Investigation requested after the reboot/backpressure priority
reassessment. Source-side mode, retry and timing events are implemented on
`codex/firmware-transfer-profiling`; all 61 deep-test steps and the ARM build
passed. The authorized v0.110 upgrade took 47.822 seconds, both Picos passed
fresh CRCs, and Benji confirmed normal operation. That profiling source is
included in the accepted v0.111 change set.
The 37.760-second peer phase served legacy words: zero accepted page requests
and 65,537 accepted word requests. Batch service did not occur in this run.

The paired simulator now reproduces words-only source service from a 500 ms
source-core-1 pause plus ordinary mouse traffic: B sends three page requests
which are overwritten before A can process them. Quiet traffic instead gives
mixed-mode service. Delaying the first advertisement until after that pause
completes all 1,024 pages with no word fallback or retries; full image/settings
checks pass. This demonstrates a plausible cause, not proof of physical UART
loss or an on-device speedup. The real USB-host test pins the stack's blocking
50 ms reset plus 450 ms debounce sequence.

The separately authorized v0.111 implementation adds a nonblocking one-second
boot grace for only the firmware advertisement, preserving other heartbeat-task
state sync. Boundary/state-sync tests and complete modeled transfers in both
directions pass, including the exact frozen v0.110 receiver: 1,024 pages, no
word requests or page retries, exact firmware/settings. Historical unguarded
failure witnesses remain in the tests. All 64 deep-test steps and the ARM build
passed; the frozen candidate is `build/releases/deskhop-v0.111-yq5wa62e/manifest.json`
in `/private/tmp/deskhop-transfer-profile.Zi4FBl` (full-slot CRC `54001839`).
See the branch's `docs/testing/firmware-startup-guard-v111.md` for evidence.
The authorized upgrade is now complete and Benji confirmed normal behavior.
Evidence: `build/updater/runs/20260916T202228Z-1oc8iom4/` in that worktree,
including separate user acceptance. Both full-slot CRCs and unchanged settings
passed. Source profiling confirms 1024 pages, zero words and zero retries.
Total time fell from 47.821911 to 23.740171 seconds; peer wait/settle from
37.759860 to 13.481009 seconds. These are observed separate release runs, not
repeated controlled measurements or a guarantee of every upgrade's speed.
No protocol/USB-stack changes were required. Do not interpret a fixed grace as
general USB readiness or a hotplug/hub fix. Keep the ROM-hang investigation
parked; this docket does not authorize another flash.

**Remaining work:** The immediate performance fix is complete. Optional later work could measure
the remaining 7.054 seconds of page service and 3.692 seconds of inter-page gaps
before tuning pacing; gaps must not be labeled entirely flash time. No further
tuning or disruptive diagnosis is currently required. The earlier
[v0.106 record](testing/batched-transfer-hardware-v106.md) remains historical.

**Acceptance considerations:** Bind observations to the same boot session,
update attempt and target image. Demonstrate batch use and measure end-to-end
improvement while retaining integrity, fallback and input responsiveness.
Successful propagation alone is not evidence of acceleration.

## 8. Reliable two-Pico configuration saves

**Remaining issue:** v0.109 validates values, but the protocol has no acknowledged
peer SET or flash commit. Local RAM readback cannot confirm that both Picos saved
the settings. Different starting border values can also cause the peer to reject
an edit order valid on the connected board. See
[the configuration-page boundary](testing/configuration-validation-v109.md#configuration-page-boundary).

**Possible approach:** Add explicit per-board apply/persist acknowledgements and
error reporting; define consistent handling of paired field changes. Decide
whether atomic two-board Save is a requirement rather than implying it already
exists. Coordinate with, but do not conflate this with, redundant settings storage.

**Acceptance considerations:** Test divergent starting settings, lost or delayed
requests/replies, peer unavailability, failed persistence and reconnects. The UI
must distinguish RAM application, persistence, partial success and uncertainty.

## 9. Higher-fidelity end-to-end tests

**Remaining issue:** Real TinyUSB host/device stacks, paired routing and NOR/UF2
storage are tested separately; their composition is not yet exercised as one
system. See [the architecture roadmap](testing/architecture.md#how-to-extend-fidelity-without-replacing-the-test-suite).

**Possible approach:** First connect real UF2 backing storage through MSC bulk
transport, then integrate the real USB stacks into the paired-device harness.
Retain deterministic schedules, replay and fault injection.

**Acceptance considerations:** Exercise enumeration, input/release delivery,
disconnect/backpressure and interrupted update/storage operations across the
combined boundaries. Use independent oracles and mutations. Do not claim full
silicon or macOS/Karabiner fidelity from hardware-free results.

## 10. Normal-upgrade hardware acceptance

**Status:** Complete. The host policy implementation is merged and pushed.
The authorized v0.110 actual-write normal-mode upgrade passed backups, stock
byte verification, unchanged settings, automatic peer propagation and one fresh
CRC scan per Pico; Benji confirmed normal input/switching/arrows/zoom behavior.
Elapsed time was 47.822 seconds versus the previous v0.109 thorough run's 50.628.
These are different releases/runs, not a controlled identical-image benchmark.
See [the verification record](testing/normal-upgrade-verification.md).

**Follow-up:** No separate validation release is needed. Preserve normal-mode
safety gates and recorded evidence on subsequent upgrades. The peer-transfer
bottleneck is the separate investigation in section 7.

## 11. Keyboard state across report IDs

**Known limitation:** Per-report descriptor layouts are fixed, but held-key
aggregation across multiple report IDs/collections on one physical source and
the six-key output limit remain separate limitations. This is not a reproduced
problem with the current Sofle setup. See
[the upstream integration boundary](testing/upstream-fixes-v107.md) and
[the keyboard reliability contract](testing/keyboard-reliability-draft.md).

**Possible approach:** Track each contributing report's key/modifier state and
combine it before routing, with explicit detach/reset behavior. Treat any output
rollover expansion as a separate compatibility decision.

**Acceptance considerations:** Interleaved reports, overlapping held keys,
per-report releases, source removal, focus changes and peer recovery must not
release another report's held key or retain stale state.

## Parked: USB-ROM hang investigation

The intermittent stock-picotool/ROM USB hang remains unresolved and is parked
at Benji's request. Later successful upgrades do not establish a fix. Preserve
[the investigation and its failed/successful evidence](testing/rom-backup-timeout-investigation.md)
without relabeling outcomes. Resume disruptive diagnostics only with explicit
authorization; this docket is not permission for more ROM cycles or retries.

## Repository and documentation housekeeping

- Reconcile stale runbook repository-state and coverage sections with accepted
  v0.109 and the current host updater. Retain dated historical evidence, but make
  it unambiguous that old configuration-reader and descriptor-layout findings
  are not newly outstanding fixes.
- Review superseded comparison/draft worktrees and the historical ` 2` duplicate
  files. The alternate upstream branches are comparison material, not pending
  deployment targets; relevant configuration-draft changes were already ported.
  Preserve unrelated edits and archive/remove only with explicit approval.
- This docket is included explicitly with the accepted v0.111 publication.
  The unrelated historical duplicate files remain outside that change set.

## Shared constraints

- Flash programming uses 256-byte pages and erasure uses 4 KiB sectors in the
  current implementation. New storage must respect alignment and erase ownership.
- Erase/program operations require coordination across cores and interrupts;
  flash is not a substitute for frequently modified working memory.
- Account for finite write endurance, power-loss behavior and RAM overhead in
  every persistent-storage design.
- Prefer deterministic, hardware-free fault-injection tests before device trials;
  retain real-device timing and recovery acceptance tests.
- Among the storage-layout proposals, prioritize redundant settings storage,
  then recoverable firmware installation. The broader suggested order is in
  Current priorities and status; the other ideas are not prerequisites.

Hardware reference: [Raspberry Pi flash API and access restrictions](https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#hardware_flash).
