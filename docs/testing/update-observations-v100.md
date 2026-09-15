# v0.100 update and core observations deployment

v0.100 is deployed and both boards report executing it. Flash/readback,
saved-settings, Mac USB/media, serial, both-core progress, and combined-history
checks passed. Benji confirmed "Everything works" after the requested typing, trackball
movement/buttons, and Layer 3 S switch-and-back check on both Macs. The previous accepted release is v0.99, checkpointed locally as
`9142c29`.

## What the terminal reports

The existing executing-build, board UID, boot session, uptime, and boot CRC
metadata fields remain. Each board also supplies two core checkpoint rows and
one updater row, for example:

```text
board=B core=0 checkpoints=31781 age_ms=0
board=B core=1 checkpoints=31779 age_ms=0
board=B update seen=1 source=peer phase=receiving received=65536 total=262144 progress_age_ms=1 target=0.101 attempt=1
board=B observation boot=same_boot progress=advancing update=pending_reboot
```

These example values describe an in-progress future update, not measured device
state. `help`, `status`, and `history [count]` are still the only commands.
`status` and `history` include both boards by default. Any ordinary serial
terminal works, including macOS `screen /dev/cu.usbmodem21203 115200` when that
is the current DeskHop callout port. Close other serial clients first.

- **Core checkpoints:** one counter per existing 1 kHz diagnostic task, enabled
  even with the console compiled out. They show those task entry points ran;
  they do not measure CPU utilization or prove every task or peripheral works.
  The accompanying age is sampled on the board and saturates at 2^32−1 ms. Validity bits distinguish an
  unpublished counter from valid zero after wrap. Counters wrap modulo 2^32.
- **Updater:** `idle`, `receiving`, `paused`, `validating`, `reboot_pending`,
  `failed`, or `abandoned`; source `peer`, `usb`, or `none`; received bytes,
  fixed total, last progress age, target version, and per-boot attempt count.
  Peer progress is published after accepted 256-byte pages, USB progress after
  unique UF2 pages. Duplicate/rejected data and retries cannot refresh progress.
  Received bytes are not verified flash bytes. USB target is unknown in this
  slice. `seen=0` means no firmware-side update observed since this boot.
- **Boot observation:** first contact is `first_seen`. The same physical UID
  with a different random boot session is `new_boot`; a changed UID is
  `identity_changed`. Only fresh, CRC-checked status replies contribute.
- **Progress observation:** a first usable sample is `baseline`. A subsequent
  sample from the same boot must have increasing uptime and positive, bounded
  modular deltas on both counters to be `advancing`. Otherwise it is
  `not_advancing` or `unavailable`. This describes the interval between samples;
  the displayed checkpoint ages provide additional context.
- **Update observation:** an in-flight update with a known target must first
  be observed. The old session remains `pending_reboot` even when the updater
  already advertises the new image. A new session executing that target version
  is `awaiting_progress`; another fresh sample with both counters advancing
  becomes `confirmed`. A different target/identity or another reset before
  confirmation is `unexpected_boot`. Failed/abandoned attempts are cleared.
  `confirmed` retains the historical observation; the separate progress field
  describes the current sample interval. It is target-version confirmation,
  not exact image integrity. First contact after an unobserved update remains
  `not_observed`.

The observer runs only when `status` is queried; it adds no background polling.
State survives terminal reconnect but is lost when the observing board reboots.
A failed query never reuses a prior success. Boot identity is captured at
startup and remains independent of mutable `_running_fw` metadata.

## Sparse RAM history

Each board retains 64 records. In addition to the existing boot, input-routing,
USB, and error events, this release records:

| Event | Details |
| --- | --- |
| `update_begin` | Source and target version; a new attempt resets byte progress |
| `update_progress` | Source, received bytes, and 25/50/75/100 percent milestone |
| `update_phase` | Source, target, and changed phase |
| `peer_observed` | Peer role, first contact/new boot/identity change, executing version |
| `peer_progress` | First observed both-core advancement per peer boot |

A row's `board=` remains the board that produced the record. A peer observation
also names the observed peer. No per-key, per-mouse-motion, or per-word logging
was added. A terminal pause does not pause capture. Events, including failure
and reboot-pending events, can disappear at reset before anyone retrieves them;
there is no persistent flash log. The observer's records can outlive a peer
reset while its own board keeps running.

## Compatibility and bounded work

Status protocol 2 adds a 78-byte snapshot in 26 UART chunks. Old protocol 1
clients receive their exact 39-byte layout. New clients try protocol 2 and,
if no matching reply arrives within 150 ms of enqueueing the request, try
protocol 1 with a distinct nonzero wire token. The total deadline stays 500 ms.
A partial or malformed matching response cannot trigger a downgrade. Legacy
identity remains usable; unavailable runtime data is printed explicitly.

History protocol 2 keeps the existing frame size and accepts unknown nonzero
event types as opaque records. The console preserves their type and payload.
For protocol 1 clients, new event types become explicit zeroed GAP slots in
the same sequence window. A v2 client can fall back to v1 after 250 ms without
any matching reply, within the original three-second deadline. A GAP can mean
an overwritten record during capture or a record unavailable in an older
protocol. Late replies to the other wire token are ignored.

The shared bridge still attempts at most one diagnostic UART packet per
millisecond, including refusals. History retains incremental capture, CRC,
and decode bounds. Core 1 owns protocol and observation state; core 0 owns
console formatting. Runtime readers use a short independent critical section,
never the firmware/flash lock. Update hooks preserve the existing firmware
lock and release the runtime lock before recording sparse history; no reverse
lock acquisition or transport/formatting work occurs inside those scopes.
The two production core task tables and input responsibilities are unchanged.

## Validation and physical observations

The native checks cover exact v1/v2 wire fixtures, mixed-version fallback,
reserved bytes/CRC/token errors, unknown history types, counter wrap/stall,
first contact versus a new boot, target mismatch, pre-reboot advertisements,
failed/abandoned updates, subsequent resets, and no retrospective confirmation.
Production storage tests exercise real updater/runtime/history code with full
images, duplicates, retries, source changes, takeover, validation failure,
recovery, and lock assertions. Four new mutation tests remove progress/failure
hooks or the console-disabled checkpoint and must fail at runtime. The real
TinyUSB suite tests maximum output, editing echo, partial writes, a stalled
reader, and fresh status snapshots. Paired production scenarios check core
progress and explicit timeout after a core stops, with local HID still usable.

Validation passed: all 41 deep-tier steps; 54 paired scenarios; three scenarios
under all 24 modeled core-priority orders; 16 storage seeds; 15 storage and 15
paired mutation failures; and nine baseline comparison contracts. All six
coverage layers passed. After the final help/age-display refinement, the pure
observer and real TinyUSB tests passed again with ASan/UBSan, and the affected
policy/USB coverage layers were remeasured. The final ARM build uses 200,916 of
262,144 RAM bytes (76.64%). The compiler reports a 256-byte console task frame,
152-byte peer receive frame, and 80-byte runtime snapshot frame; these are
individual frames, not measurements of the complete physical call stack.

Logs are `build/tests/deep-v100.log`, `targeted-v100-final.log`,
`arm-v100-final.log`, `coverage-v100.log`, and
`coverage-v100-final-targeted.log`. The frozen candidate manifest is
`build/flashing/v100-candidate.json`, containing build-input hashes and
binary/UF2/ELF hashes. The 262,144-byte binary has encoded version 200 and
independently checked metadata CRC `8397b53c`. All 1,024 UF2 blocks reconstruct
that binary exactly and exclude saved configuration.

Frozen UF2: `build/flashing/deskhop-v0.100-update-observations.uf2`, SHA-256
`70f7d258d66e804ecd32596f70e42fbf00f052d67b2d7eaf55619dcf3ad16565`.
Pico A was flashed using disk-free Layer 3 A entry and explicit flash UID
`E6654854574C3E30`. Normal reboot was requested at **2026-09-15 00:46:02 UTC**
(September 14 locally). Its old image exactly matched v0.99; all 262,144 new
bytes matched the frozen image on independent readback, and all 4,096 saved
configuration bytes remained unchanged. The bootloader exposed only vendor
interface class 255. Before and after, the same five direct IOMediaBSDClient
objects (20 including APFS subclasses) were active and nonbusy; no new media
client or residual RP2 boot object appeared.

Two early status reads at A uptimes 789 and 1,967 ms showed A running v0.100,
while B still reported v0.99, boot session `5b8f298bd669b78a`, with explicit
`runtime=unavailable protocol=1`. This physically exercised the legacy fallback.
The subsequent bounded checker passed in 5.906 seconds with 13,385 bytes and
no cleanup errors. All seven statuses and four combined histories succeeded:

| Board | Physical UID | New boot session | Uptime range | Core 0 checkpoints | Core 1 checkpoints |
| --- | --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `e37cfda649031433` | 28,320–33,377 ms | 28,213–33,254 | 27,735–32,781 |
| B | `E6654854577F2330` | `67023817d0aa5245` | 7,514–12,571 ms | 7,451–12,490 | 6,966–12,011 |

Both reported executing build `0.100`, boot CRC metadata `8397b53c`, idle update
state, and checkpoint ages of zero milliseconds at the samples. B's first v2
reply was classified `new_boot`/`baseline`; later replies were
`same_boot`/`advancing`. Its estimated boot offset from A is about 20.8 seconds.
This establishes a changed peer session and fresh execution of the new version,
without independently reading B's flash or checking its actual image digest.

The four history snapshots retained eight A and three B records without gaps
or overwrites. A's new observation rows recorded first contact with B v0.99 at
955 ms, B's new v0.100 boot at 28,348 ms, and B's advancing cores at 28,756 ms.
The interleaved list also contained each board's boot and USB/HID enumeration.
These are the observing board's timestamps, not exact peer reboot times.
Both boot sessions and retained records survived terminal reopen. Peer capture
bounds ranged from 24,033 to 263,929 microseconds across the four reads.

Evidence: `build/flashing/pico-a-v100-result.json`, `console-v100-initial.txt`,
and `console-v100-smoke-20260915T004637.293875Z.json`/`.txt`. Benji confirmed
"Everything works" for typing, trackball movement/buttons, and Layer 3 S
switching to the other Mac and back. No replug was requested.

The first v0.99-to-v0.100 propagation cannot supply v2 telemetry before B's
update. `update=not_observed` is therefore expected and honest. A later v2-to-v2
release with status queries during the transfer can validate the complete
pending-reboot/new-session/progress sequence. Exact image verification remains
the next walking-skeleton slice.

The optional bounded read-only checker passed offline fixtures
(22 malformed status, 12 stale/identity/runtime, 14 malformed history, and three
retained-history failure cases). It then passed the physical check above. To repeat it with the verified
explicit port:

```sh
python3 build/flashing/check_console_v100.py /dev/cu.usbmodem21203 --expected-crc 8397b53c
```

It requires both boards and checks seven status snapshots plus four histories
across reconnect, within 30 seconds and 64 KiB. Standard serial terminals remain
sufficient; this local helper is optional.
