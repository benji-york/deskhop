# v0.101 image verification deployment

Both boards now run v0.101. Pico A's flash/readback, saved-settings comparison,
Mac USB/media checks, peer rollout observations, and both-board serial image
verification passed. Benji confirmed the requested typing, trackball/buttons,
and both-Mac switching check. The previous accepted release was v0.100,
checkpoint `989192b`. This release adds `verify <build> <crc32>` to the existing
serial console. The command checks both boards by default and works from an
ordinary serial terminal. No new host application is required.

## Command and checksum coverage

```text
verify 0.101 <eight hexadecimal digits from the candidate manifest>
```

The expected CRC32 covers the exact **262,144-byte binary**, address range
`[0x10000000, 0x10040000)`, including firmware metadata, disk image, and padding.
It excludes saved settings at `0x101ff000`. The build argument names the
executing version; the checksum pins the particular artifact within that version.
Only `major.minor` versions representable by the firmware's 16-bit encoding and
exactly eight hexadecimal CRC digits are accepted.

This is **not** the payload-only checksum stored in boot metadata and displayed
by `status` as `image_crc_at_boot`. The candidate manifest records both values
with distinct names and supplies the complete command. To calculate the expected
full-slot CRC independently from a binary:

```sh
python3 -c 'import pathlib,zlib; p=pathlib.Path("build/arm-validation/deskhop.bin"); b=p.read_bytes(); assert len(b)==262144; print(f"{zlib.crc32(b):08x}")'
```

A response has `BEGIN verify`/`END verify` framing, `scope=both`,
`claim=scan_snapshot`, expected build/CRC, and explicit coverage. Each board
prints its verdict/reason, UID, boot session, executing build, scan start/end,
bytes read, fresh CRC, flash metadata, generation evidence, and both core
checkpoint deltas/ages. The connected board prints first. Every evidence row
identifies its board. Incomplete scans print `crc32=unavailable`; other evidence
may be partial. `generation_end` is meaningful only for a completed stable scan.

## What the verdict establishes

| Result | Meaning |
| --- | --- |
| `PASS match` | A complete stable scan matches the supplied build/CRC, flash metadata agrees with the executing boot identity, and both diagnostic core checkpoints advanced during the scan with final ages at most 100 ms |
| `FAIL build_mismatch` / `crc_mismatch` / `metadata_invalid` | A completed scan establishes a specific mismatch |
| `UNVERIFIED update_active` / `image_changed` | An update, dirty image, pending reboot, or firmware mutation prevents assurance |
| `UNVERIFIED busy` / `timeout_or_unsupported` / `invalid_response` | Necessary work or fresh peer evidence could not be obtained |
| `UNVERIFIED core_unavailable` / `core_progress_unverified` / `expired` | Core progress or timely presentation could not be established |

The final result is PASS only if both boards pass. A definite board failure
takes precedence over an unavailable board; otherwise incomplete evidence gives
UNVERIFIED. A missing or older peer never contributes a fabricated success.

These are snapshots of the reported scan intervals, with separate boot-relative
clocks. They do not certify changes after those intervals or prove every input
device/task works. CRC32 detects accidental mismatches; it is not cryptographic
authentication. The firmware executes from RAM: the result checks flash against
the expected artifact and the boot-captured executing identity, without hashing
all executing RAM. Every command starts fresh scans; there is no stored PASS.

## Work, ownership, and invalidation

Core 1 owns a single scanner and the peer protocol; core 0 owns console output.
The existing task tables and input responsibilities are unchanged. One query
normally scans both boards concurrently. Simultaneous terminal queries serialize
local and peer scans on each board, without allocating more scanners.

At most one 256-byte flash page is copied per millisecond. The scanner tries
the existing firmware lock and then the flash lock once; contention yields to
a later tick. CRC calculation runs after both locks are released. Start/end
metadata reads are separate ticks. Each request has a three-second deadline,
including queueing; a normal scan needs at least 1,024 page ticks. Console expiry
is 3.5 seconds from command acceptance, including paused output. A completed
local result is checked again before formatting and before an overall PASS,
and a completed peer result before being frozen for transfer. Bytes already handed to USB cannot be recalled;
read the final result and rerun after a long terminal pause or firmware change.

A flash-lock-protected 64-bit generation increments before every firmware-slot
erase/program, including identical rewrites, metadata writes, and recovery.
Settings/staging writes are outside this range. Generation saturation rejects
all later scans until startup instead of wrapping. Dirty/update/reboot state is
checked under the firmware lock. A changed generation invalidates the scan even
if the resulting bytes happen to match again.

UART types 40/41 add an independent protocol: a token-correlated request and
a 132-byte snapshot in 44 three-byte chunks, with an independent wire CRC.
Malformed matching replies fail explicitly; stale tokens cannot satisfy a later
query. There is no verification downgrade for older firmware. Status, history,
and verification rotate access to the shared limit of one diagnostic UART
enqueue attempt per millisecond, including refusals. Scanner/provider and
protocol state are fixed-size; result queues copy their values across cores.

## Validation and interactive acceptance

Pure policy tests cover verdict precedence, metadata/build/CRC faults, partial
scans, generation changes, missing/stale/wrapped core counters, and zero CRC.
Peer tests use an independent wire fixture, 176 role/order cases, all 1,056
single-bit corruptions, missing chunks, duplicate/conflicting data, busy/delayed
providers, cancellation, exact deadlines, and refused-enqueue fairness.

Storage tests use real full-slot guards and an independent bitwise CRC, verify
lock order/IRQ restoration and untouched failed outputs, and check firmware
versus settings/staging invalidation. Six added source mutations must fail, and
a temporary-source fixture forces generation saturation without a production
test hook. Paired scenarios run the actual scanner, flash guards, SDK queues,
UART, and verdict policy with simultaneous queries, HID input, image changes,
missing peers, dirty images, and fresh retries. The real TinyUSB console suite
covers strict grammar, maximum fields, all verdicts, stale/duplicate results,
disconnects, paused output, and HID progress. Native timing/locks remain models;
the ARM build checks the hardware adapter and memory fit.

All 46 deep-tier steps passed, including 60 paired scenarios, four scenarios
under all 24 modeled core priority orders, 16 storage seeds plus generation
saturation, 21 storage and 15 paired mutation failures, and nine baseline
comparison contracts. A further paired regression admits a replacement query
while the previous terminal's scans are still active, discards both old results,
and requires fresh local and peer results before the replacement deadline; it
passed separately, bringing the scenario inventory to 61. All six native
coverage layers passed. Logs are `build/tests/deep-v101.log`,
`targeted-v101-final.log`, and `coverage-v101.log`. The final local-guard
refinement passed the real TinyUSB sanitizer suite again (`usb-v101-final.log`)
and affected policy/USB coverage was remeasured
(`coverage-v101-final-targeted.log`). The final ARM build passed and uses
213,764 of 262,144 RAM bytes (81.54%), leaving 48,380 bytes below the heap
limit before runtime allocations. The two new queues allocate 560 bytes;
all application queues total 10,610 bytes before allocator overhead and other
runtime allocations. Verification makes no per-command allocations.

Static ARM inspection totals 664 bytes of C frames on the new core 1 scanner
chain and approximately 1.25 KiB including the inspected interrupt paths, below
its existing 2 KiB stack. Core 0's console task has a 280-byte frame; its
formatting chain is approximately 928 bytes before scheduler/interrupt overhead.
These are compiler/linker-based estimates for the inspected paths, not physical
stack high-water measurements. No new recursion or full-image RAM buffer was added.

The deployed artifact is `build/flashing/deskhop-v0.101-verification.uf2`, SHA-256
`571e1b24d59e36da1dfce58a05c26bdefdb4c2312e5a4982424f1c0b31c3ec87`.
All 1,024 UF2 blocks reconstruct the 262,144-byte binary exactly and exclude
settings. Its independently checked metadata CRC is `be404f8f`; its full-slot
CRC is `2db89640`. The source and artifact hashes are recorded in
`build/flashing/v101-candidate.json`. The exact command is:

```text
verify 0.101 2db89640
```

The optional bounded read-only helper checks help, five statuses, two histories,
and fresh PASS/FAIL/PASS verification from both boards within 30 seconds and
64 KiB. Offline self-tests passed the complete sequence and rejected 33 malformed,
stale, or false-verdict fixtures without accessing any device:

```sh
python3 build/flashing/check_console_v101.py /dev/cu.usbmodem21203 --expected-slot-crc 2db89640 --expected-boot-crc be404f8f
```

Reconfirm this explicit port as DeskHop before any later run.
The helper is optional and lives among the local deployment artifacts; the
ordinary serial command needs no helper.

## Physical deployment and serial results

Pico A was flashed through disk-free PICOBOOT and normal reboot requested at
2026-09-15 09:07:13.250085 UTC. All 262,144 firmware bytes matched an independent
readback, and all 4,096 saved-settings bytes were unchanged. Maintenance exposed
the vendor interface (class 255), without a mass-storage interface. After reboot,
no RP2 boot object remained. The Mac retained the same 20 media clients including
subclasses (five direct clients), all active and nonbusy.

The rollout capture obtained 44 statuses over 23.223 seconds (36,174 bytes):
43 peer replies succeeded and one timed out during B's boot transition. B's
old v0.100 session reported peer-sourced target v0.101, with receiving progress
from 3,840 through 254,976 bytes, then `reboot_pending` at 262,144 bytes.
The transfer was already underway at the first sample. B then reported its new
v0.101 session, initially `awaiting_progress` and subsequently `confirmed`
after both core counters advanced. This captures the version-update observation
sequence; the exact reboot instant was not measured.

The subsequent bounded smoke check passed in 5.537 seconds with 11,448 received
bytes. Five statuses retained these identities and advanced all four core
counters:

| Board | Physical flash UID | Boot session | Sampled uptime range |
| --- | --- | --- | --- |
| A | `E6654854574C3E30` | `37dad6e6dd90a647` | 36,120–40,244 ms |
| B | `E6654854577F2330` | `049fe6e4d5d3495e` | 15,254–19,378 ms |

B's previous session was `67023817d0aa5245`. Both executing builds were
`0.101`, with boot metadata CRC `be404f8f`. Two combined history queries
(`history 16` and `history 64`) retained the same nine A and three B records,
with no gaps or overwrites. A's rows include B's old-build first contact,
new-build boot, and subsequent progress. B's pre-reboot RAM history is gone;
the live rollout statuses supply the receiving/reboot-pending evidence.

Three verification commands each performed new scans of all 262,144 bytes on
both boards. Every measured slot CRC was `2db89640`; metadata matched, generations
were stable, and both cores advanced during each scan with final ages of 0 ms.

| Expected CRC | A result / scan duration | B result / scan duration |
| --- | --- | --- |
| `2db89640` | PASS / 1.046346 s | PASS / 1.064277 s |
| `2db89641` (deliberately wrong) | FAIL `crc_mismatch` / 1.054407 s | FAIL `crc_mismatch` / 1.067426 s |
| `2db89640` | PASS / 1.050361 s | PASS / 1.060340 s |

A also has an independent external readback. B's complete flash was read by its
own firmware and reported through the peer protocol; it was not read externally
with picotool. These successful checks describe the recorded scan intervals.

Local evidence files are `build/flashing/pico-a-v101-result.json`,
`build/flashing/pico-a-postreboot-v101-health.json`,
`build/flashing/console-v101-rollout-20260915T090736.515879Z.json`, and
`build/flashing/console-v101-smoke-20260915T090755.292908Z.json`, with matching
serial `.txt` transcripts. No push has been performed.

Benji confirmed "Everything works" after the requested typing, trackball
movement/buttons, and Layer 3 S switch to the other Mac and back check. This
completes the release's interactive input acceptance. Replugging was not
separately reported.
