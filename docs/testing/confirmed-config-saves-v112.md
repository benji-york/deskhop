# Confirmed two-Pico configuration saves: v0.112

## Scope

Development branch: `codex/confirmed-config-saves`, based on hardware-accepted
`96ca65e` (v0.111). Benji requested per-Pico apply/persist confirmation and
authorized testing and deployment while AFK. Retries may not bypass identity,
image-integrity or update-state checks. This request does not authorize blind
retries after uncertain flash writes or claim physical input acceptance while
the user is away.

The design adds nonce-correlated requests and per-board responses for capability,
field apply, atomic border-pair apply, settings digest and confirmed persistence.
The configuration page must establish support on both boards before sending new
writes. It must distinguish RAM application, verified saved bytes, explicit
rejection and unknown outcomes after transport loss. It must not call a partial
success “Saved on both.” Retries must not skip a previously unconfirmed peer edit
merely because a local readback matches.

This is not an all-or-nothing transaction across both Picos, a rollback mechanism
or power-loss-safe settings storage. Existing configuration layout, full-width
stored timers, unchanged-field preservation and existing user-visible field
limits remain in force. A border pair is validated and applied together on each
board so different starting intervals cannot reject an otherwise valid final pair.

## Validation and deployment

Implementation and hardware-free validation are complete. After two pre-write
ROM failures and Benji's power cycle, the fresh retry deployed and verified
v0.112 on both Picos. The superseding v0.113 release includes this implementation
and serial configuration-mode entry. Benji subsequently accepted physical input
and the browser Save check on v0.113, authorizing publication. See the
[v0.113 acceptance record](serial-config-v113.md#final-acceptance-follow-up).
Firmware CRC success alone does not establish those user-visible behaviors.

## Protocol and persistence contract

The new v1 service is additive to UART-v1 and the unchanged configuration layout.
Requests use types 56–59, responses 60–61. Each payload is eight bytes, little
endian. META contains a nonzero 32-bit token, physical role (0=A, 1=B), operation,
key and a zero reserved byte; LO and HI each contain the token and four value
bytes; EXEC contains the token and a zero word. ACK_META echoes token, actual
role, operation and key plus status; ACK_VALUE carries token and result. All four
request frames and both reply frames are required. The legacy proxy wrapper
cannot carry these messages, and WebHID cannot inject trusted reply frames.

Operations are capability (1), field SET (2), border-pair SET (3), RAM digest (4),
confirmed SAVE (5) and read-only expected-value check (6).
Statuses are OK, INVALID, BUSY, CONFLICT,
FLASH_MISMATCH, EXPIRED and INCOMPLETE (0–6). Capability returns protocol 1.
SAVE names the expected canonical RAM digest; a successful result returns that
same digest only after the saved page's exact bytes are read back and verified.
The digest includes configuration version and writable fields in API order,
including all eight bytes of stored timers, but not padding, the stored CRC or
the transient selected-monitor index. CRC32 detects accidental differences;
it is not authentication or mathematical proof that two configurations match.

Callbacks enqueue into an eight-entry queue. A core-0 task owns assembly,
forwarding and responses; two-second bounds prevent abandoned transactions
from holding the service indefinitely. The browser waits three seconds and
never automatically repeats a mutation after a timeout. A bridge never invents
a peer result after losing its response. The last completed exact request can
replay its receipt without repeating flash. A fresh Save also skips erase/write
when the desired saved page is already byte-identical.

Persistence holds firmware ownership, rejects busy/dirty/maintenance states,
and snapshots configuration under its existing short lock. It does not hold
that configuration lock across flash: a concurrent edit survives and returns
CONFLICT. A failed readback returns FLASH_MISMATCH. Neither error proves that
flash remained untouched. Settings still occupy one sector; a power cut during
erase/write can still lose them. There is no cross-Pico rollback or atomic commit.

## Browser behavior

Before applying changes or saving, the page probes both physical roles for
support. Old or unreachable firmware causes no legacy-write fallback. Fields
may be applied to RAM before Save; the page distinguishes this from persistence.
Border edits wait for Save and apply each top/bottom pair atomically on each
Pico, even if the two boards start with different valid intervals.

Save applies pending edits, queries both RAM digests and checks every desired
field against the same per-board snapshot digest before requesting each save.
The field checks detect a peer reboot losing an earlier edit between successive
SETs, which comparing only the final SET's digest cannot detect. These checks
do not change RAM or flash. Only two verified successful replies produce the
two-board success message. Missing responses remain unknown. Partial results
name A and B separately. Existing unedited differences are not silently copied
from one Pico to the other: matching digests are reported, or the page warns
that both saved configurations differ. Read still displays the connected
board's values, not a merged view of both boards.

Desired edits remain dirty until both persistent saves are verified, including
when RAM application was acknowledged. They survive a local Read and can be
reapplied by an explicit Save after a peer reboots or a previous save fails.
After applying edits, each queried digest must still match that board's final
apply acknowledgement before any Save is sent; settings changed in between
cause a conflict instead of being called pre-existing differences.
Disconnect or maintenance cancels queued browser work; reconnect
requires fresh capability checks. Existing numeric validation, 48-bit writable
timer limits and preservation of untouched full-width stored timers remain.

## Hardware-free evidence and limits

- Native persistence tests exercise real production helpers with ASan/UBSan,
  an independently encoded canonical digest, modeled NOR corruption, busy
  ownership, stale snapshots, concurrent real SETs, readback and erase avoidance.
- Browser tests execute the real page script against an independent two-board
  endpoint. They cover capability gating, strict receipts, partial results,
  delayed/dropped/malformed replies, blocked sends, disconnect/maintenance,
  dirty-edit retry across peer reboot, apply-to-save snapshot conflicts,
  divergent borders and timers.
- Paired tests execute the real USB admission callback, command engine, task
  tables, UART queues/parser and storage helper from either USB origin, with
  request/reply loss, queue backpressure, old firmware and bounded core orders.
- These layers do not compose a real browser, both TinyUSB stacks, Pico silicon
  and macOS into one system. NOR/concurrency schedules are selected checkpoints,
  not exhaustive instruction interleavings or a power-loss recovery claim.

The final native paired/storage coverage run completed successfully:
`python3 tests/coverage.py --layer paired --layer storage --output build/tests/coverage-v112`.
The new `config_confirm.c` service executed 212/222 mapped lines (95.50%),
180/260 branches (69.23%) and 15/15 functions. These are that translation unit's
mapped execution counts, not whole-firmware coverage or proof of all races.
The per-layer reports and exact source lists are retained under that output
directory; its HTML index links the individual source reports.

An initial full preparation (`build/updater/prepare/prepare-qg_vacbb/`) passed
its deep tier in 435.6 seconds and the ARM build, but review changes landed
during that run. The preparer correctly refused to freeze a candidate because
the source fingerprint changed. No hardware operation followed that run. A
fresh full preparation is required against the final unchanged source snapshot.

That final preparation is now complete: `build/updater/prepare/prepare-3gtvdrot/`.
All 66 deep-tier steps passed in 422.2 seconds, including 337 confirmed-service
scenario runs (seven cases, both USB origins, 24 core orders, plus the old peer).
ARM configure/build and the unchanged-source fingerprint passed. The linker
reports 240,568 bytes (91.77%) in the 256 KiB main RAM region, before runtime
queue allocation; this is not a free-heap measurement.

Frozen candidate: `build/releases/deskhop-v0.112-6dk7c2yk/manifest.json`,
full-slot CRC `e49caa16`, metadata CRC `8e0930ca`, BIN SHA-256
`3294f1f749218707aed0db17ac62d3358457b81aca9684b80736f63eeebc0f4f`.
The source snapshot records the dirty branch and base HEAD rather than claiming
that HEAD alone identifies this firmware. No commit, merge or push has been made.

## Initial deployment failures (resolved by physical power cycle)

The authorized normal-mode flash attempt is retained at
`build/updater/runs/20260916T214400Z-6b4on00n/` in this worktree. Preflight confirmed
both Picos executing v0.111 with idle updates, valid identities and progressing
cores. A entered disk-free ROM and passed flash-UID identification, but the first
full firmware backup failed after about 10.2 seconds with the familiar bulk-IN
timeout / RP2040 “unknown error.” `write_started` is false. No settings backup,
firmware load or settings write was attempted.

After inspecting the journal, USB connection and known older compatible peer,
a fresh guarded already-ROM retry was authorized by the user's AFK retry request.
It established a new identity/session and retained every backup/integrity gate:
`build/updater/runs/20260916T214508Z-4puq9og7/`. It failed at the same first backup;
`write_started` is again false. No invalidated ROM session was reused and no
safety check was bypassed.

A separately identity-checked normal reboot of the unchanged A image also
timed out: `build/updater/runs/recovery-reboot-v112-0_bexlb2/`. A subsequent
read-only USB inspection still showed the same disk-free RP2 Boot device and
no `/dev/cu.usbmodem21203` application console. This is not a successful recovery.

The next required action was a physical power cycle followed by a fresh normal
workflow, not reuse of a failed ROM session. The parked general USB-ROM
investigation was not reopened with a debugger or custom transport.

## Successful retry after power cycle

Benji reported “power cycled.” A fresh `make flash` of the same frozen candidate
completed successfully, recorded at
`build/updater/runs/20260916T214920Z-zzaryspy/` in this worktree. Its preflight
independently observed both original identities executing idle v0.111 with new
boot sessions. Normal-mode backup, stock verified load, unchanged-settings
check, application reboot, automatic B propagation, fresh CRC scans and sampled
both-core progress all passed. No failed ROM binding was reused.

- Total upgrade time: 23.728236 seconds.
- Full old firmware backup: 0.567066 seconds; its SHA-256 matches the accepted
  v0.111 image (`c5b03175f01dd39a25b0a94715cac58adf1cc600dbc387e2579a1db468c7e865`).
- Stock `load -v`: 3.895 seconds; normal application reboot: 0.010931 seconds.
- A's full 4096-byte settings backup/readback match exactly, SHA-256
  `aa816af793193a7ce487d391b49bf45d514dde09bd77d28255eb782e82a85464`.
- Both Picos independently scanned all 262144 firmware bytes and returned
  `PASS`, CRC `e49caa16`, metadata CRC `8e0930ca`, build v0.112.
- A boot session `29f6a6e0ff7e3b46`; B boot session `7f2d2f89c3122e00`.
- Source history records 1024 page requests, zero word requests and zero page
  retries; batch service elapsed 10.721934 seconds. B rebooted and advanced both
  cores without another power cycle.

The original updater result retains `input_acceptance=pending`; no user
acceptance or publication was inferred from this retry. Benji later accepted
input and a no-edit browser Save on the superseding v0.113 release, and
authorized commit, merge and push. See the linked v0.113 acceptance record.
A physical edited-value change/save/reload test is not claimed.
