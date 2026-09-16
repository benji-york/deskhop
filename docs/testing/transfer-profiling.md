# Firmware-transfer profiling investigation

## Status and scope

Started 2026-09-16 on `codex/firmware-transfer-profiling`, based on accepted
main `6bf9528`, in `/private/tmp/deskhop-transfer-profile.Zi4FBl`.
Both installed Picos now run firmware-verified v0.111 after the separately
authorized startup-guard deployment. Benji confirmed "Everything works normally"
for the full functional checklist and subsequently authorized merging/pushing
the hardware-tested change set.
That upgrade took 23.740171 seconds versus the instrumented v0.110 run's
47.821911 seconds, and served 1024 pages with no words or retries. See
[the v0.111 hardware record](firmware-startup-guard-v111.md#authorized-deployment-and-functional-acceptance).
Instrumentation alone did not change transfer policy; the earlier v0.110
diagnostic and hardware-free investigation below remain historical evidence.

The first slice adds sparse sender-side evidence without changing transfer
framing, checksums, retry policy, queue priority or flash layout. Host history
parsing must recognize the new events without relaxing existing validation.
Implementation and validation results are recorded below.

## Existing physical timing evidence

The successful accepted upgrades are retained in the canonical repository:

- v0.108: `build/updater/runs/20260916T173830Z-j3n0vqi7/`
- v0.109: `build/updater/runs/20260916T181039Z-t7wuxvw8/`

These used the former thorough verification sequence. Phase boundaries come
from the monotonic timestamps in `result.json`; individual command timings and
serial observations are retained separately.

| Phase | v0.108 | v0.109 |
| --- | ---: | ---: |
| Preflight and ROM entry | 0.773 s | 0.706 s |
| Firmware/settings backups | 0.754 s | 0.719 s |
| Flash and stock verification | 3.951 s | 3.945 s |
| Former additional readbacks | 0.742 s | 0.727 s |
| A reboot/reconnect | 1.244 s | 1.342 s |
| Peer propagation and settling | **37.470 s** | **37.380 s** |
| Former thorough diagnostics | 5.877 s | 5.804 s |
| Total | **50.816 s** | **50.628 s** |

The expensive part is real peer transfer, not just host detection:

- Each run includes 65 receiving samples from one receiver boot/update attempt.
- B progresses steadily at approximately 7,428 and 7,427 bytes/s respectively.
  Most roughly 548 ms intervals advance 4,096 bytes; nearby intervals advance
  3,840 or 4,352 bytes. There is no coarse stall or restart visible in those
  samples; short page retries could still be hidden between them.
- The first samples already contain 1,024/1,792 bytes, at source uptimes
  705/794 ms. The full 262,144 bytes appear by source uptimes 35.856/35.850 s.
- Receiving-complete to confirmed post-reboot core progress adds about 2.12 s.
- Each run has 68 rollout status requests. Typical requests take 39–40 ms,
  separated by 0.5-second host sleeps; one request times out during B's reboot.
  Their summed roughly 3.2 seconds overlap autonomous device work, so they are
  not an additive delay we can simply subtract. Faster polling may compete
  with the transfer on UART.

Rate alone does **not** prove word mode, batch mode or fallback. Existing status
does not report any of those. B's old transfer history disappears when B reboots;
the final B history contains new-boot events, not a preserved transfer profile.

## Source review and hypotheses

The UART is configured for 3,686,400 baud, 8N1. Each protected frame is 32 bytes.
A batch page is a request, 64 four-byte DATA frames, and an END frame for 256
firmware bytes. At ideal wire utilization, 1,024 such exchanges take roughly
5.9 seconds. This excludes scheduling, ordinary input/diagnostics, flash work,
retries and validation; it is a lower-bound comparison, not a promised runtime.

Legacy mode requests one four-byte word at a time. Its 4,000 Hz update task alone
caps useful throughput at 16,000 bytes/s, before round trips and scheduling.
Batch mode removes most request round trips but still sends one frame per
available TX task pass. Ordinary queued traffic retains priority over batch
frames, and the source must acquire the firmware lock without blocking.

The receiver probes capabilities once and falls back after the existing 100 ms
deadline. Three failed page attempts also cause fallback. Those are real code
paths, not established explanations for these physical timings. There is no
confirmed deterministic batch-disable defect from the initial source review.
The 100 ms budget starts at request queue admission, not actual wire delivery,
so queue waiting can consume some of it before the peer receives the request.

### Hardware-free scheduling sensitivity experiment

A fresh native library built from unchanged accepted `6bf9528` exercised the
production transfer through the existing paired simulator. Each normal case
programmed 64 pages; throughput is measured between the first and last program
events (63 intervals, 16,128 bytes). Seed 1, default seeded ordering, a common
core-pass quantum for all four simulated core loops, and 2,000 us advance steps
were used. The firmware images are synthetic v205/v204 fixtures, not deployed
release artifacts. All sampled programmed bytes and saved settings matched.

| Core-pass quantum | Modeled flash time | Sampled throughput | Legacy requests |
| --- | --- | ---: | ---: |
| 100 us | Zero | 37.54 kB/s | 0 |
| 250 us | Zero | 15.29 kB/s | 0 |
| 500 us | Zero | 7.61 kB/s | 0 |
| 1,000 us | Zero | 3.80 kB/s | 0 |
| 500 us | 50 ms erase / 1 ms program | 6.97 kB/s | 0 |
| 2,000 us | Zero; one-page fallback case | Three page attempts, then words | 65 |

Thus a rate near the physical result is compatible with genuine batching plus
slow task service. At 2 ms, negotiation succeeds but a 65-frame page outlasts
the response budget, and the actual fallback code runs. None of the chosen
scheduling/flash durations was measured on the Picos. Short-window extrapolation
is not a full-slot benchmark or a diagnosis of the physical cause.

Local experimental evidence/reproducer:
`/private/tmp/deskhop-pacing-sim.nu4RXu/pacing-results.json` and
`/private/tmp/deskhop-pacing-sim.nu4RXu/pacing_probe.py`. The fresh production
library at `node.so` has SHA-256
`65ac1478ad4b4fd793ca66d5bfdf7b6d80958960a2de77d46b82c38b4e146bad`.

## Why source-side instrumentation first

On the next genuine upgrade, the newly flashed source runs v0.110 while the
receiver still executes v0.109 during reception. Source observations therefore
work on that first rollout, survive the receiver reboot, and are available in
the updater's existing final history collection.

The intended evidence distinguishes capability-response queue admission,
actual page/word requests, observed retries, coarse service progress and timing
between requests versus page service. Source admission is not physical wire
delivery, receiver CRC acceptance or flash completion. Exact receiver fallback
reasons and erase/program time cannot be measured by old receiver firmware.

Do not log every word/page/frame or continuously fetch full peer histories.
History is a bounded 64-entry RAM ring; a 64-row remote snapshot can require up
to 802 diagnostic response chunks competing with the firmware transfer. Sparse
events and accumulated source timing avoid that traffic. No persistent logging
or protocol-layout change is required.

### Implemented event semantics

Three new history event types reuse the existing fixed record and v2 transport:

- `transfer_source`: capability reply queued, first batch/word request,
  first repeated-page request, 25/50/75/100% service-address milestones and
  first last-page END selection or last-word acceptance. Mode is `none`,
  `pages`, `words` or `mixed`; capability tags and addresses remain explicit.
- `transfer_timing`: source elapsed time, accumulated page-service time,
  inter-page gap time and maximum page-service time, in microseconds.
- `transfer_count`: accepted page requests, accepted word requests and observed
  repeated-page requests.

An ordinary all-pages episode adds 14 records; unprobed all-words service adds 13.
Counters/timing saturate at UINT32_MAX rather than wrapping. Page service is
measured from an accepted request, before source flash copy/page CRC, to END
selection for DMA or replacement by a retry/word request. Gaps are END selection
to the next accepted page request.
They include scheduling and do not isolate wire or receiver flash time.

The summary is a **snapshot through the first offered end of the image**, not
confirmation that the receiver finished. A lost final END or response can still
cause later requests. Snapshot totals then remain frozen, but the first later
retry remains observable if no retry was previously logged, and a newly
observed page-to-word transition is logged once. Source byte/address
milestones likewise do not certify receiver coverage or successful programming.
Existing fresh full-slot CRC verification is still the acceptance criterion.

History/event formatting and strict host parsing change together. The status
schema, retry/transfer logic and UART framing stay unchanged. Old receiver
firmware can still make the same requests. Receiver-side exact timeout reasons,
flash erase/program totals and physical DMA completion timing are not captured
by this first slice and must not be interpreted as zero.
Use this branch's updater when deploying/verifying v0.110: an older host parser
will correctly reject the new event names rather than silently accept them.

## Next decisions after an authorized instrumented upgrade

- No page requests: investigate capability exchange or observed word fallback.
- Page requests followed by words: investigate the transition and retry evidence.
- Pages throughout, long source service time: investigate TX scheduling, ordinary
  queue pressure, source-lock contention and DMA pacing.
- Pages throughout, long inter-request time: inspect the receiver's scheduling,
  flash commit/erase cost and request admission. A gap alone is not proof of
  flash overhead; receiver-side timing may be the next diagnostic slice.

Retain normal-mode firmware/settings backups, stock `load -v`, settings
preservation, identity/session/progress gates and fresh CRC verification. Use
the same rollout polling for a comparable baseline. Do not remove safety checks
or tune timeouts before the observations distinguish the limiting mechanism.
The intermittent ROM USB hang remains parked and separate from transfer speed.

## Validation

Targeted checks passed: 141 updater tests, native ASan/UBSan storage tests with
two seeds plus generation saturation, the real TinyUSB device/CDC stack tests,
and the existing paired full-image batch scenario. New regression coverage
includes final-page retry/fallback, a lone last-page request, stale/malformed
requests, source changes/restarts, queue pressure and saturating counters.

The storage harness previously required interrupts enabled on diagnostic-lock
entry. Its existing firmware try-lock correctly models IRQ masking, so that
blanket assertion rejected the new history hook despite valid production lock
ordering. The correction permits IRQ-off only when the current core owns that
firmware try-lock; flash/config exclusion, diagnostic non-nesting and reverse
lock-order checks remain enforced. Production locking was not changed.

A first ARM configure/build passed. Static main-RAM use is 236,388 bytes of
262,144 (90.17%), versus 234,268 in the accepted v0.109 build: an increase of
2,120 bytes including copied code/data. The added runtime profile itself is
72 bytes. These are linker figures, not a runtime stack/heap high-water test.
The firmware slot remains the same fixed 262,144 bytes.

All 61 deep-tier steps passed, including the 141 updater tests, native sanitizer
checks, paired-device scenarios, source mutations and baseline behavior
comparisons. The release preparation's test phase took 272.5 seconds; ARM
configuration/build also passed. Evidence is in
`build/updater/prepare/prepare-zp12q_tr/tests.log` and
`build/tests/results-deep.json` in the profiling worktree.

Frozen candidate:
`build/releases/deskhop-v0.110-2ifjuswm/manifest.json` in
`/private/tmp/deskhop-transfer-profile.Zi4FBl`.
Full-slot CRC: `56cf6a40`; boot metadata CRC: `13231899`.
Firmware binary SHA-256:
`867d7286dcea13ea5d1a44e1a1c1167d7fe95d5acb849c8b0de1f5cf17d32779`.
The manifest binds the uncommitted source snapshot to the artifacts and
validation evidence; HEAD alone does not identify this candidate.

The initial hardware-free investigation did not access either Pico. The
subsequent authorized deployment and its measurements are recorded below.

## Authorized v0.110 deployment and measured outcome

Benji authorized the instrumented upgrade with "Proceed". The maintained
updater completed one normal-mode attempt without retry, power cycle, cable
move or safety bypass. Evidence in the profiling worktree:
`build/updater/runs/20260916T194457Z-1lktwwdf/`.

Both Picos passed fresh 262,144-byte CRC scans matching `56cf6a40`, boot metadata
`13231899`, new boot sessions and progressing cores. Stock `load -v` verified
A's programmed bytes; firmware/settings backups passed and all 4,096 saved
settings bytes were unchanged. Normal mode correctly omitted the duplicate
firmware readback and negative/repeat CRC requests. Benji subsequently answered
"Everything works normally" to typing/modifiers, trackball and keyboard
right-click on both Macs, both switching directions, focus arrows and zoom
assist. `user-acceptance.json` records that separately; the original updater
journal's pending input flag is preserved.

| Phase | v0.110 normal |
| --- | ---: |
| Preflight and ROM entry | 0.722 s |
| Firmware/settings backups | 0.711 s |
| Flash and stock verification | 3.929 s |
| Settings readback and guards | 0.108 s |
| A reboot/reconnect | 1.323 s |
| Peer propagation and settling | **37.760 s** |
| Normal post-rollout diagnostics | 3.265 s |
| Total | **47.822 s** |

Compared with the earlier v0.109 thorough run, total elapsed time decreased
2.806 seconds. Readback and post-rollout diagnostic phases together decreased
about 3.159 seconds, while the peer phase increased about 0.380 seconds. These
are separate releases/runs, not a controlled same-image benchmark. This is
physical acceptance of normal-mode upgrading, not a peer-transfer speedup.

### What the source history proves

A's final history had 24 rows, no overwritten entries, and one source profile
in boot session `c34175b6d0fe596d`:

- Capability reply queued at source uptime 18 ms, tag `1161510912`.
- First accepted legacy-word request at 519 ms, address zero.
- Word-service quarter milestones at 9,427 / 18,329 / 27,232 / 36,148 ms.
- `mode=words`, zero accepted page requests, 65,537 accepted word requests,
  zero observed page retries. One more word request than unique image words
  implies at least one duplicate; the aggregate does not locate it.
- Summary elapsed time 36,129,854 us from capability-response queue admission
  through the final-word request; page service/gap/max totals are zero because
  A accepted no page service. These zeros do not measure receiver flash time.
- B's new boot session `32f53f71e753941e` and v0.110 were then observed and
  independently verified. No transfer was retried by the host.

This establishes that the observed image service used the legacy four-byte
path, not that B never briefly selected batch mode. Requests sent by B but lost
or rejected before A admitted them cannot appear in these counters. Queuing a
capability reply likewise does not prove its DMA transmission or B's receipt.

### Startup-starvation lead and next experiment

The real TinyUSB host enumerator has blocking 50 ms reset and 450 ms debounce
delays (`pico-sdk/lib/tinyusb/src/host/usbh.c`, `enum_new_device`), implemented by
`sleep_ms` for `OPT_OS_PICO`. USB-host work is first in core 1's task loop, ahead
of UART receive processing. The approximately 501 ms gap from caps queueing to
the first admitted word request, followed by keyboard HID mount at 543 ms,
fits that startup stall. Core 0's USB-device mount event at 251 ms shows this
is not evidence of a stopped core 0 or necessarily a delayed capability reply.

The receiver has a 100 ms capability deadline and three 100 ms page attempts.
Either negotiation failure or unserviced initial page requests could cause
fallback during startup. The 1 KiB receive ring holds only 32 frames; loss of
requests during a long receive-processing stall amid other traffic is another
possibility, not an observed overrun. Source history cannot distinguish these
cases. Do not report the USB startup delay as the proven fallback trigger yet.

Next bounded work: reproduce source-core-1 startup starvation in the paired
simulator; observe capability response DMA selection and receiver capability/
page-timeout fallback reasons. Any receiver-only evidence must be collected
before its reboot or summarized to the source. Prefer a narrow startup-ready
advertisement or bounded negotiation retry over weakening page integrity,
unbounded timeouts, or a USB-stack rewrite. Choose the change after the
reproducer identifies which requests fail. No further flash or source-policy
change was made after collecting this evidence.

## Hardware-free follow-up: startup starvation

After Benji authorized the next investigation, a paired-production-code probe
varied the timing of a 500 ms pause on source A's core 1. A's core 0, both B
cores and UART DMA remained live. Tests used real heartbeat/negotiation,
protected frame encoding/decoding, bounded receive storage, retry policy and
flash commits; they did not copy the protocol into Python or inject dropped
transfer packets.

The quiet post-capability pause caused B to receive the capability reply,
transmit three page requests, then fall back to words. A eventually admitted the
buffered page requests, producing a mixed-mode source history. Adding 100 Hz
alternating mouse motion through B's production HID path caused the stalled
A receive ring to wrap: B still sent its page requests, but A's eventual history
contained only capability queueing followed by words. That matches the physical
profile's event shape. It demonstrates a possible mechanism, not proof of the
traffic or loss during the actual device upgrade.

The separate real-TinyUSB host harness now asserts the exact 50 ms reset and
450 ms debounce calls within each single root-attach task invocation. That
ASan/UBSan test passed with the composite keyboard/mouse and standalone
trackball fixtures. Its OS abstraction advances virtual time; production
`OPT_OS_PICO` uses `sleep_ms` for the same calls. This pins the pause's code
basis without claiming a physical scheduling measurement or composing the two
test harnesses into one complete hardware emulator.

### Narrow mitigation to evaluate

Defer only the first outgoing firmware `HEARTBEAT_MSG` advertisement until at
least one second after boot. Keep the heartbeat task's activity, button, zoom,
selection and maintenance work unchanged. The already-installed v0.110 receiver
can benefit on the next upgrade because this is a source-side change, with no
new message format or receiver-version requirement.

This is a bounded warm-up allowance for the directly attached setup, not a
general proof of USB readiness. It does not solve arbitrary hub enumeration,
later hotplug stalls or indefinitely delayed USB devices. Waiting for a mounted
keyboard would be worse: it can prevent updates with no keyboard connected or
failed enumeration, and does not prove all hub children have settled. A USB
stack rewrite is disproportionate to the current evidence. Longer receiver
timeouts or retries may be useful later, but require the receiver to already
run that change and can merely move the failure boundary.

Before changing firmware, retain a controlled delayed-advertisement comparison
under the same pause/traffic, plus no-peripheral, role reversal, old-receiver,
state-sync and ordinary steady-state transfer checks. A physical upgrade must
still prove page service and an end-to-end gain; simulator virtual times are
not upgrade-time predictions.

### Reproducer and intervention results

`tests/sim/test_fw_startup.py` retains eight bounded scenarios. Eight scenarios
across seeds 1–3 passed; representative scenarios also passed three fixed core
orders. The regular fast tier now runs the seed-1 quick matrix, and the deep
tier additionally completes the two full-image comparisons. These are v0.110
behavior witnesses, not a requirement that future firmware retain fallback:
when implementing a startup guard, preserve the old witness against the
archived v0.110 source and add success assertions for the changed firmware.

```sh
python3 tests/sim/build.py
python3 tests/sim/test_fw_startup.py --seeds 1 2 3 --output build/tests/fw-startup-probe.json
python3 tests/sim/test_fw_startup.py --full --seeds 1 --output build/tests/fw-startup-full.json
```

The full comparisons both used 500 Hz alternating motion for the 500 ms pause
and another 100 ms after it, then quiet transfer completion. They independently
checked the entire receiver image, every page commit, source preservation and
both settings sectors, plus production CRC/reboot admission. The simulator
does not perform a physical reboot.

- Early advertisement: B transmitted three page attempts, but A accepted none;
  the final source profile was words-only with 65,536 accepted word requests.
  B transmitted 65,538 word requests because startup losses also affected words.
  This does **not** reproduce the physical duplicate count of 65,537 accepted
  requests exactly. Completion took 50.190 seconds of modeled time.
- Delayed-advertisement intervention: 1,024 pages, zero word requests, zero page
  retries, complete image accepted in 17.723 seconds of modeled time.

The intervention fixture simply avoids the early manual production heartbeat;
its first naturally scheduled heartbeat runs after the paused core resumes at
500 ms. It does not implement or validate an actual one-second firmware gate.
Neither modeled time predicts hardware upgrade duration. The result supports
selecting the bounded source startup guard for implementation and subsequent
hardware measurement, while exact physical fallback causation remains open.

No production policy, firmware version or device state changed in this
follow-up. The accepted frozen v0.110 artifact and deployment evidence remain
unchanged; no new release artifact was prepared. The new host-stack timing
assertions and standalone probe were tested separately from that release's
original 61-step validation.

## Implemented follow-up: v0.111

The one-second nonblocking guard is now implemented only around outgoing
firmware advertisements. The current test driver executes the actual guarded
heartbeat with normal production scheduling, rather than the manual delayed
advertisement used by the intervention above. It separately compiles historical
commit `6bf9528030bcb3f69cedd665add55d94a7713f82` to retain the unguarded failure
witness. That v0.109 source has the same transfer behavior as v0.110 but lacks
the new source-profile events; historical assertions therefore observe actual
wire requests/data and flash writes instead of expecting those events.

The original intervention results above remain historical evidence, not results
from the new implementation. See [v0.111's validation record](firmware-startup-guard-v111.md)
for guard boundary, compatibility, state-sync and complete-image checks. The
subsequent authorized physical upgrade now demonstrates page service and reduced
elapsed time for this run; both Picos run accepted v0.111. The hardware record
preserves the evidence and limitations rather than retroactively changing the
earlier v0.110 outcomes.
