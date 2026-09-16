# Firmware-advertisement startup guard: v0.111

## Contract and scope

Implemented on `codex/firmware-transfer-profiling` after the accepted v0.110
diagnostic release. Only the `HEARTBEAT_MSG` firmware metadata advertisement is
suppressed while `time_us_64() < 1000000`. The next ordinary heartbeat at or
after that boundary advertises the genuine running version, protocol marker
and checksum. No sleep, queue retry, fake metadata or peripheral-presence test
is introduced. Both Pico roles use the same rule.

Activity, buttons, modifiers, zoom assist and selection synchronization remain
immediate. Existing config-mode maintenance, active-pull recovery heartbeats,
queue-full handling and complete heartbeat silence during a host UF2 drop are
unchanged. The version advances to v0.111; configuration and UART versions do not.

This source-side change works with the installed v0.110 receiver. It gives the
directly attached USB setup time to get past its blocking 50 ms reset plus
450 ms debounce before inviting a firmware pull. A fixed grace is not a general
USB-readiness guarantee and does not address arbitrary hub enumeration or later
hotplug stalls. The physical failure mechanism remains a supported hypothesis,
not proven UART loss. See [the original evidence](transfer-profiling.md).

## Hardware-free validation

- `tests/sim/test_fw_advertisement.c` invokes the production heartbeat at 0,
  999999, 1000000 and 1000001 microseconds, and just after 32-bit clock wrap.
  Both roles assert exact metadata and state-sync packets, no sleep/time advance,
  no peripheral requirement, unchanged pull/drop state, config-mode blinking,
  maintenance reservation and bounded full-queue admission. ASan/UBSan enabled.
- `tests/sim/test_fw_startup.py` uses real scheduled production heartbeat calls,
  a 500 ms source-core-1 pause and actual 100/500 Hz HID mouse reports. Both
  roles, no-peripheral startup and old receivers are covered. No transfer packet
  drops, timeout edits or replacement Python protocol are injected.
- The same driver retains five unguarded witnesses against exact local commit
  `6bf9528030bcb3f69cedd665add55d94a7713f82`. Historical tests use wire/flash
  observations because that source predates v0.110 profile hooks. CI fetches
  history; a missing baseline is an error, not a silently skipped test.
- The deep tier completes the historical word-fallback case, both guarded
  source roles and a guarded source with the historical receiver. Every byte,
  all 1024 page commits, source preservation, both settings sectors and the
  production CRC/reboot admission are checked. Guarded transfers require zero
  word requests and zero page retries. This does not emulate a physical reboot.
- Established-transport tests now explicitly warm up before installing unequal
  fixture images. Their original 100/200 ms retry checks and 25-second transfer
  bounds remain unchanged; replay records the warmup. Dedicated startup cases
  still begin at time zero.

The paired scheduler models the effect of enumeration blocking, not the real
TinyUSB host stack inside that same simulator. The separate real-host test
asserts the 50+450 ms calls. Its virtual timings and the paired model's 250 us
polling quantum are not predictions of physical upgrade duration.

Focused runs passed 36 cases across three seeds and another 36 across three
fixed core orders, plus the four full-image comparisons. Their compact results
are in `build/tests/fw-startup-guard-{seeds,orders,full}.json` in the branch
worktree. A separate local check rebuilt the receiver from the exact accepted
`build/releases/deskhop-v0.110-2ifjuswm/source.tar.gz` and completed full transfers
in both source directions, each with 1024 page requests, zero word requests and
zero page retries. `build/tests/fw-startup-exact110-receiver.json` records those
results and library hashes. That archive-dependent compatibility check is
additional local evidence, not a CI prerequisite or hardware test.

## Release and device status

All 64 deep-tier steps (including 141 updater tests) passed in the maintained
release preparation, followed by ARM configure/build and candidate integrity
validation. The test tier took 298.9 seconds. Validation logs are under
`build/updater/prepare/prepare-03dv4oll/`; structured step results are in
`build/tests/results-deep.json`. The linker reports 236388 bytes of main RAM
(90.17%), unchanged from the instrumented v0.110 build.

Frozen candidate in `/private/tmp/deskhop-transfer-profile.Zi4FBl`:

- Manifest: `build/releases/deskhop-v0.111-yq5wa62e/manifest.json`
- Full-slot CRC32: `54001839`; metadata CRC32: `19e4f71b`
- BIN SHA256: `c5b03175f01dd39a25b0a94715cac58adf1cc600dbc387e2579a1db468c7e865`

The candidate contains the exact source snapshot and validation evidence.
It was deployed in the authorized run below. Both physical Picos now run accepted
v0.111. Benji subsequently authorized merging the hardware-tested changes into
main and pushing the fork. The frozen source archive and original deployment
evidence remain unchanged by publication.

## Authorized deployment and functional acceptance

On 2026-09-16 Benji requested "Flash when ready." One maintained normal-mode
upgrade used the frozen candidate above, targeting A through its configured
serial console and allowing automatic propagation to B. There was no retry,
manual cable move, power cycle or weakened verification gate.

Evidence in the same worktree:
`build/updater/runs/20260916T202228Z-1oc8iom4/`. The complete original
`result.json` records firmware verification; separate `user-acceptance.json`
records Benji's "Everything works normally" response for typing/modifiers,
trackball and keyboard right-click on both Macs, switching both ways, focus
arrows and zoom assist. The original journal's pending input field is preserved.

Both boards passed fresh full-slot CRC `54001839`, stable image generations and
progressing cores. Stock `picotool load -v` byte verification and all 4096 bytes
of saved-settings readback passed. A's full backup exactly matches the accepted
v0.110 BIN SHA256. B propagated and rebooted into v0.111 automatically. Normal
mode deliberately omitted the duplicate host firmware readback, as designed.
The journal retains best-effort cleanup errors for the departed pre-ROM CDC
descriptor; fresh diagnostics succeeded. They do not constitute a new fix for
the separately parked intermittent ROM-USB issue.

| Observed metric | v0.110 | v0.111 |
| --- | ---: | ---: |
| Complete upgrade | 47.821911 s | 23.740171 s |
| Peer wait/settle phase | 37.759860 s | 13.481009 s |
| Source profile elapsed | 36.129854 s | 10.746857 s |
| Accepted page requests | 0 | 1024 |
| Accepted word requests | 65537 | 0 |
| Page retries | 0 | 0 |

The observed total decreased by 24.081740 seconds (50.36%). The same normal-mode
host procedure was used for both upgrades. These are two different release/run
observations, not repeated controlled benchmarks or a universal guarantee.
Unlike the earlier rate-only evidence, the complete sender history directly
shows page mode through the full image, with no word fallback or page retries.

A's final `history 64` contains all 23 events without overwritten history.
USB HID mounts occur at 543/549/556 ms; capability response queueing and batch
start occur at 1018 ms, after the one-second guard. Page-progress quarters occur
at 3697/6391/9070/11765 ms. The 14-row transfer profile records total page service
7053858 us, inter-page gaps 3692490 us and maximum page service 7515 us.
Profile elapsed starts at capability-response queueing and ends when the source
first offers the complete image; it excludes the initial grace and is not itself
a receiver-commit/reboot confirmation. The later status/session/CRC checks supply
that evidence.

The capability tag `3584469824` matches B's pre-upgrade boot-session-derived
token. A's history/status/verification all bind to session `0030f77c4c2374eb`;
B's receiving session `32f53f71e753941e` changes to `c94fee81895bca2f` on reboot,
matching its fresh verification and history. Page service plus inter-page gaps
is 509 microseconds below profile elapsed, consistent with initial overhead.
Inter-page gaps include transport, receiver dispatch, flash and request
scheduling; these observations do not isolate flash time alone.

This successful on-device intervention supports the startup-starvation diagnosis
and demonstrates a substantial improvement for this run. It still does not prove
the exact traffic or UART loss in the earlier failure, general USB readiness,
or a solution for arbitrary hubs and later hotplug stalls. No further tuning or
additional flash experiment was performed.
