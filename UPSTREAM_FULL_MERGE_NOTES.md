# Full upstream merge candidate — v0.92

Adopted on `main` on 2026-09-13 after comparison with the selective and replay
approaches. The notes below record the candidate's validation before adoption;
hardware validation is still pending.

Branch: `integration/upstream-full-merge`.
Worktree: `/Users/benji/.codex/worktrees/8bf3/DeskHop`.

Merge commit `9e212f9f5643f9f682babc7cef258dc38ef19138` has the requested parents:

- Fork baseline: `c1e9420a05b32ea67a44f716160dd4286de5f629` (v0.91).
- Upstream: `ce8abb69861c6e5d9ffb731e1df557a128d4c222`.
- Common ancestor: `59577cc53b311e6ede128402fdaf459d92866ba1`.

## Resolution decisions

- Adopt upstream's parser corrections, including repeating the last declared
  usage, carrying the last usage into the next block, and guarding an empty
  local-usage list. Preserve the existing fixed-array bounds protection through
  upstream's equivalent implementation. The parser source matches this upstream
  revision exactly.
- Adopt the upstream byte-valued 256-entry receiver map and shared function
  table. Any report ID value from 0 through 255 can dispatch; the independent
  descriptor-offset table still tracks at most 24 distinct reports per interface.
- Preserve the fork's four NKRO ranges, padding offsets, short-bitmap bounds,
  exact usage-per-bit eligibility, and aggregate width threshold of **more than
  32 bits**, including valid eight-bit ranges. Multiple keyboard collections are
  still subject to the existing shared-state limitation.
- Combine consumer/system payload guards and conditional report-ID skipping
  with the fork's real-activity recording and local/peer routing. Empty consumer
  reports and report-ID-only consumer reports do not generate activity or output.
- Keep `src/tasks.c` identical to the fork. The recovery-capable updater already
  subsumes upstream #360's initial-page and completion fixes, with retries,
  checksum validation, recovery, and cross-core flash locking. #358 and #361
  overlap the fork's existing backports.
- Guard empty USB callbacks before reading a report ID, rearming the endpoint;
  reject device address zero before indexing the interface table. Remove circular
  `main.h` dependencies from the HID type headers so native tests compile the
  production parser, report, keyboard, and USB translation units with hardware
  stubs.

Pointer synchronization and position-neutral remote clicks, focus LEDs, inferred
macOS zoom assist, F24/all-up safety, global real-input keep-awake, v10 migration
and auto-start Jitter UI, coordinated triple Ctrl+Right Shift+Q reboot, and the
configuration checksum offset remain intact. Configuration version/layout and
QMK are unchanged. Firmware version is v0.92 (numeric version 192).

## Build and memory comparison

Both revisions were built from clean build directories with bundled SDK/PIO
sources and ARM GCC **14.3.1**, CMake Release defaults:

```sh
export PATH=/opt/homebrew/opt/arm-gcc-bin@14/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
cmake -S . -B /tmp/deskhop-fullmerge-candidate-arm
cmake --build /tmp/deskhop-fullmerge-candidate-arm --parallel 4
```

Baseline source was exported with `git archive` to
`/tmp/deskhop-fullmerge-baseline.45DE91`; no other worktree or branch was changed.

| ARM allocation | v0.91 baseline | v0.92 candidate | Change |
| --- | ---: | ---: | ---: |
| Linker RAM usage | 141,580 B (54.01%) | 149,068 B (56.86%) | +7,488 B |
| `global_state` | 49,152 B | 56,640 B | +7,488 B |
| `parser_state` | 732 B | 732 B | 0 B |
| RAM remaining after static allocation | 120,564 B | 113,076 B | −7,488 B |
| Space above the minimum 2,048 B heap reservation | 118,516 B | 111,028 B | −7,488 B |

Each receiver map grows from 96 to 256 bytes. ARM layout probes show that replacing
function pointers with bytes also removes four padding bytes per interface:
`sizeof(hid_interface_t)` changes from 1,004 to 1,160 bytes. Across 48 interfaces,
the net growth is **156 × 48 = 7,488 bytes**, rather than the nominal 7,680 map
bytes. Reduced code size and alignment leave the start of `.bss` unchanged.

The linker fills the configured flash partitions by design, so its 100% flash
line is not an executable-capacity measurement. Stacks occupy separate scratch
RAM. Static memory headroom does not measure worst-case runtime heap/stack use.

## Validation

All six original native suites pass: zoom tracker, firmware update, screensaver
policy, reboot hotkey, configuration migration, and Web Config auto-start Jitter.
The five C suites ran with C11, `-Wall -Wextra -Werror`, AddressSanitizer, and
UndefinedBehaviorSanitizer using the existing CI commands. The JavaScript suite
used the bundled Node runtime.

The added seventh suite, `tests/test_hid_regressions.c`, also passes with ASan
and UBSan configured to terminate on failure. It compiles the production parser,
report extraction, keyboard, and USB callback code against hardware stubs. It
checks last-usage carry, empty and oversized usage lists, 1,024-element reports,
all four receivers at IDs 0/23/24/255, unknown IDs and the 24-report capacity,
empty callbacks, consumer/system conversion and activity guards, four NKRO
ranges with padding and an unaligned eight-bit block, aggregate thresholds,
short NKRO payloads, and outgoing six-key limits. The exact command is in
`tests/hid_stubs/README.md` and CI; all six existing test commands remain.

The ARM build passes without compiler warnings. The generated 256 KiB binary
has metadata version 192, valid magic, and a verified image CRC32 of `92f56c36`.
UF2 SHA-256: `5f7a6703dab1e0ccb65eba71861ea7fd78c5ca03c435953ff859eab4ba41728d`.

The UF2, ELF, binary, build logs, baseline log, and ARM layout-probe results are
retained in the ignored `build/integration-full-merge/` directory. The candidate
UF2 is `deskhop-v0.92-full-merge.uf2`.

## Remaining limitations

This candidate has not been flashed or tested on hardware. Both output
directions, pointer movement/clicks, LEDs, zoom transitions, keep-awake,
coordinated reboot, and peer firmware propagation still need the runbook's
hardware checks before deployment. Native hardware stubs do not exercise USB
timing, UART transport, or actual flash operations.

Existing parser limits remain: four NKRO blocks, 24 tracked report offsets,
multiple keyboard report collections collapsing onto one keyboard state, and
incomplete handling of malformed descriptors and truncated generic boot/array
reports. The full report-ID map expands ID dispatch, not these representations.

No main-branch movement, push, flash, QMK change, configuration change, or Web
Config/disk regeneration was performed.
