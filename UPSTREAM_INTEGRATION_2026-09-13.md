# Upstream integration experiment, 2026-09-13

Fork baseline: `c1e9420a05b32ea67a44f716160dd4286de5f629` (v0.91).
Upstream baseline: `ce8abb69861c6e5d9ffb731e1df557a128d4c222`.
Common ancestor: `59577cc53b311e6ede128402fdaf459d92866ba1`.

This project is an isolated checkout at `/Users/benji/Documents/ChatGPT/DeskHop`.
The previously deployed source remains at
`/Users/benji/Documents/Codex/2026-08-13/i/work/deskhop-benji`.

## Initial merge

The fork contains 18 commits absent from upstream, and upstream contains 12
commits absent from the fork (including merge commits). A trial merge on
`integration/upstream-2026-09-13` conflicted in:

- `src/hid_parser.c`
- `src/include/hid_parser.h`
- `src/keyboard.c`
- `src/tasks.c`

The trial merge was aborted after recording the conflicts. All three resolution
strategies were then completed in separate tasks and worktrees. The recommended
full-merge candidate is now on `integration/upstream-2026-09-13`, with this
comparison added. Both upstream and the fork baseline are ancestors of it.

## Incoming changes

| Upstream change | Integration decision |
| --- | --- |
| #358: consumer/system reports without report IDs | Already backported as `066bc20`; preserve the fork's real-input activity tracking. |
| #361: usage-array bounds | Already backported as `3f25461`; upstream's later corrections still need incorporation. |
| #360: finalize peer transfers and avoid writing an empty first page | Subsumed by the fork's recovery state machine; preserve retries, locking, checksum pinning, and repair behavior. |
| Carry the last declared usage; guard a zero usage count | New fixes. The old fork's comment said “last,” but the carry expression selected the first declared usage. |
| Reject empty consumer payloads | New guard to merge with activity tracking. |
| Full report-ID handler map | Supports IDs 24–255 previously dropped by the 24-entry callback table. Both old and new dispatch are constant-time; the useful change is the ID range. |

The new handler map grows from 24 four-byte pointers to 256 one-byte receiver
IDs per interface on RP2040: 160 additional bytes per interface, or 7,680 bytes
across 4 devices × 12 interfaces. The parser still supports at most 24 distinct
report offsets per interface. Preserve the fork's multi-block NKRO handling.

## Baseline verification

All six existing native regression suites passed in this project. A clean ARM
build using Arm GNU Toolchain 14.3.Rel1 also passed. Linker RAM consumption was
141,580 bytes of 262,144 (54.01%). `global_state` occupied 49,152 bytes.
RAM includes the firmware copied to RAM; flash section figures include reserved
padding and should not be interpreted as executable-code usage.

## Candidate approaches

1. `integration/upstream-full-merge`: resolve a true two-parent upstream merge.
2. `integration/upstream-selective`: apply novel upstream fixes to the fork,
   recording skipped duplicates and retaining fork history.
3. `integration/upstream-replay`: start at upstream and replay the custom work,
   omitting duplicate backports.

Each candidate must retain the six existing suites, add HID regression coverage,
build v0.92, preserve config v10 and custom firmware behavior, and report memory
usage. Hardware validation is separate from compilation and native testing.

## Results and recommendation

| Approach | Candidate commit | Native checks | ARM build | Linker RAM |
| --- | --- | --- | --- | ---: |
| Full merge | `f980b1f` | Six original suites plus new HID regressions passed | Passed | 149,068 B (56.86%) |
| Selective integration | `21d36c7` | Six original suites plus new HID regressions passed | Passed | 149,068 B (56.86%) |
| Upstream-first replay | `985de2d` | Six original suites plus new HID regressions passed | Passed | 149,068 B (56.86%) |

Recommend **the full merge**. Its merge commit `9e212f9` has the exact fork and
upstream tips as parents; `f980b1f` adds tests and documentation. It preserves
both histories, incorporates every incoming fix, and avoids replaying the same
upstream conflicts next time. The initial integration branch was fast-forwarded
to this candidate; `main` and `origin/main` remain at `c1e9420`.

The selective candidate preserves the fork's capacity-based parser arithmetic
while adding upstream's corrected last-usage semantics. It has equivalent
intended behavior, but does not record upstream as merged. The replay candidate
starts from upstream and replays 16 custom commits, skipping two duplicate
backports; it works but replaces the original custom commit identities. Its
production parser and keyboard implementation match the full merge.

All candidates include an empty-USB-transfer guard. Full merge also rejects
device address zero before indexing, and makes the HID type headers independent
of `main.h` so tests can compile production sources directly. The other
candidates retain the circular headers and stage them beside hardware stubs for
native compilation. These differences do not remove custom behavior.

Independent review found no introduced production regression or validation
blocker. The HID suites exercise production parser, keyboard, report extraction,
and USB dispatch code: IDs 0/23/24/255, usage carry/saturation, empty consumer
reports, activity tracking, and split NKRO extraction. Mouse decoding and
hardware transport are stubbed, so these tests do not validate physical
trackball behavior, USB timing, or peer propagation.

The measured RAM increase is **7,488 bytes**, slightly below the 7,680-byte map
estimate because the new interface layout saves four padding bytes per
interface. `global_state` grows to 56,640 bytes. The linker reports 113,076
bytes remaining; allowing for the 2,048-byte minimum heap reservation leaves
111,028 bytes above that reservation. These are static figures, not runtime
heap or stack high-water measurements.

The recommended candidate was also built in this project's `build/` directory
and its new HID suite passed here. The resulting `build/deskhop.uf2` is v0.92.
Detailed full-merge decisions and artifact checksums are in
`UPSTREAM_FULL_MERGE_NOTES.md`; alternative notes remain in their worktrees:

- `/Users/benji/.codex/worktrees/9a34/DeskHop/UPSTREAM_SELECTIVE_INTEGRATION.md`
- `/Users/benji/.codex/worktrees/1f58/DeskHop/INTEGRATION_UPSTREAM_REPLAY.md`

No candidate has been pushed or flashed, and QMK is unchanged. Physical checks
must cover both directions, trackball and keyboard clicks, focus LEDs, zoom
assist, jitter, coordinated reboot, and firmware propagation before calling
v0.92 hardware-verified. All alternatives use version 192; switching between
their binaries without a version bump would not automatically update an
equal-version peer.
