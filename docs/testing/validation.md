# Local validation record

Date: 2026-09-14. Branch: `codex/hardware-free-test-framework`, based on local
`d42c930` (v0.92). The candidate is v0.94 with configuration format 10, following
the v0.93 test framework at `fdc0f48`. All runs
below used the isolated worktree. At validation time nothing was flashed, pushed or merged to main;
the Sofle/QMK checkout and target Macs were not changed.

## Deployment follow-up

On the user's subsequent request, Pico A was flashed on 2026-09-14 at 18:41 UTC.
Raspberry Pi picotool v2.3.1 loaded and verified the exact UF2 documented below,
then an independent 262,144-byte readback matched `deskhop.bin` byte-for-byte.
The 4 KiB configuration sector matched its pre-flash backup unchanged. The
previous image was v0.92/192 with a valid CRC `0x92f56c36`.

Pico A (flash ID `E6654854574C3E30`) rebooted and re-enumerated as `DeskHop Switch`
at the same USB location. Pico B's automatic update is not independently
version-verified; functional checks are pending user feedback. Backups and
load/readback logs are under `build/flashing`. No push, main-branch merge, QMK
flash or target-Mac configuration change was performed.

## Completed runs

| Command / experiment | Observed result |
| --- | --- |
| `python3 tests/run.py fast` | Passed, including 40 paired scenarios and the new selection helper suite. |
| `python3 tests/run.py deep` | Passed, including the revised baseline comparison: original seven suites, 20,000 generated HID cases, all 256 dispatch IDs per case, 64,128 updater contract cases, 16 storage seeds, real TinyUSB host/device stacks, 40 paired scenarios, 32 generated paired sequences, and 24 fixed four-core priority orders for backpressure. The final run repeated all fast-tier components with the larger workloads. |
| Production source mutations | All 11 storage and 15 paired mutations caused the required runtime failure; compilation failures do not count as detection. Seven of the paired mutations exercise selection reconciliation. |
| Simulator apparatus | Independent node/global reset, callback failure propagation, bounded queue waits, indexed report-byte oracles and exact successful trace replay passed. An intentional lost-delivery failure reduced from 21 to 9 steps while preserving its oracle. All six extra mouse scenarios also passed exact full-trace replay. |
| Baseline differential | All nine scenario contracts pass against local v0.92 and current source. Four host-effect traces match exactly; five retain the same ordered effects per host after suppressing only redundant keyboard states, with maximum 500 microseconds drift (1 ms allowed). Mouse effects remain unchanged in these cases. Intentional UART changes are excluded. The actual v0.92 receiver accepts extended ID3 and ignores ID32. |
| New regression evidence | Mouse aggregation, held-button handoff, in-flight source input, asymmetric capabilities, queue saturation, selection loss and the config SET/save race were reproduced before their fixes. The final 9 mouse and 12 selection scenarios pass. Six extra mouse scenarios also passed seeds 7 and 19. The sanitized selection helper covers 992 ordered delivery pairs plus startup, wire and wrap cases. |
| Finite flash model | Exhausted its reduced graph: 3,395 states, 7,832 edges, no remaining frontier at depth bound 64. All five specification mutations produced replayed shortest counterexamples. Uncontrolled power loss also produced the expected counterexample. |
| `python3 tests/coverage.py` | All six independent layers passed and produced LLVM text, JSON and annotated HTML. See the [dated coverage table](coverage.md); overlapping denominators cannot be combined into a whole-firmware percentage. |
| `python3 tests/run.py arm` | Full RP2040 firmware build passed, including hardware SDK and PIO-USB sources. Metadata and CRC checked independently below. |
| Two RP2040js instances | Both executed ARM instructions and exchanged UART bytes: A received `0x42`, B received `0x41`. The [saved probe output](../../tests/emulation/rp2040js-1.3.4-probe.json) was reproduced. No complete DeskHop boot was achieved. |

Native runs used macOS 26.5.2 on arm64, Apple Clang 21.0.0 and Python 3.14.6.
Sanitizer executables use AddressSanitizer and UndefinedBehaviorSanitizer with
recovery disabled. The Python-loaded paired libraries use the normal native
build; sanitizer coverage of those application units runs in a separate C
executable. Source coverage uses matching Apple LLVM tools.

The test runner preserves per-tier command results in
`build/tests/results-<tier>.json` and the latest tier in `results.json`. Coverage
is under `build/tests/coverage/html/index.html`; failure scripts, model traces
and mutation traces are under the other documented `build/tests` directories.
Generated evidence is ignored by Git and can be regenerated with the commands.

## ARM artifact

The GNU ARM 14.3 build produced `build/arm-validation/deskhop.uf2`, `.elf` and
`.bin`. The binary is 262,144 bytes. Its metadata contains magic `0xf00d`,
version `194`, and CRC32 `0x79af8a82`; recomputing CRC32 over the image preceding
the final 4 KiB metadata sector matched. The UF2 SHA-256 is:

```text
8e1471c435f38feebf1e4f9bb48e1eb9e07cc9fff4e807fbd748ee183e3be507
```

The linker reports 155,348 bytes of the 262,144-byte main RAM region used
(59.26%), plus 2 KiB in SCRATCH_X. The `global_state` symbol is 56,760 bytes.
These are build measurements, not peak stack or runtime memory measurements.
The core/task assignment remains unchanged. The firmware executes from RAM,
so the main RAM increase includes added executable code.

## What remains unverified

Mouse aggregation and selection recovery now have passing regressions. Full
source/synthetic separation and selection reconciliation require both Picos to
run the upgraded firmware. Selection repair assumes recurring polling, eventual
delivery and fewer than 2^31 outstanding generations. Ordinary physical mouse
reports are not acknowledged/replayed, and arbitrary stale traffic across
unlimited peer resets is not proved safe.

Safe boot after arbitrary interrupted updates to the running slot remains an
explicit counterexample. Multiple keyboard report collections still share
state, and partial NKRO handling retains its documented limitations. The config
lock protects the saved snapshot and known writers; it does not establish that
all live readers observe every multi-field update atomically.

The finite specification does not prove the C implementation or silicon. Native
core scheduling does not explore every instruction or weak-memory interleaving.
Separate USB, storage and paired layers do not establish their complete hardware
integration. Real macOS/Karabiner/Zoom/idle behavior, PIO USB timing, electrical
behavior, and brownout/ROM startup require further evidence.

Linux/macOS CI is configured, including matching LLVM tools and replay artifact
retention. The branch has not been published, so no remote CI success is claimed.
