# Local validation record

Date: 2026-09-14. Branch: `codex/hardware-free-test-framework`, based on local
`d42c930` (v0.92). The candidate is v0.93 with configuration format 10. All runs
below used the isolated worktree. Nothing was flashed, pushed or merged to main;
the Sofle/QMK checkout and target Macs were not changed.

## Completed runs

| Command / experiment | Observed result |
| --- | --- |
| `python3 tests/run.py fast` | Passed. The final deep run also re-executed every fast-tier component with the larger workloads. |
| `python3 tests/run.py deep` | Passed with explicit known counterexamples. Original seven suites, 20,000 generated HID cases, all 256 dispatch IDs per case, 64,128 updater contract cases, 16 storage seeds, real TinyUSB host/device stacks, 19 paired scenarios, 32 generated paired sequences, and 24 fixed four-core priority orders for backpressure. |
| Production source mutations | All 10 storage and 5 paired mutations caused the required runtime failure; compilation failures do not count as detection. |
| Simulator apparatus | Independent node/global reset, callback failure propagation, bounded queue waits and exact successful trace replay passed. The real button-aggregation failure reduced from 21 to 7 steps while preserving its oracle. |
| Baseline differential | Nine valid-input traces matched local v0.92 production source under the same native adapter. This is bounded preservation evidence, not universal equivalence. |
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
version `193`, and CRC32 `0xef55bbed`; recomputing CRC32 over the image preceding
the final 4 KiB metadata sector matched. The UF2 SHA-256 is:

```text
ffcc87bad662fdfa1cbc8aa85d593cf8c7599015ea71b11aedc6b30e0ad5474d
```

The linker reports 151,124 bytes of the 262,144-byte main RAM region used
(57.65%), plus 2 KiB in SCRATCH_X. The `global_state` symbol is 56,640 bytes.
These are build measurements, not peak stack or runtime memory measurements.

## What remains unverified

Three desired properties remain explicit findings: simultaneous cross-device
mouse-button aggregation, output-selection convergence after a lost wire packet,
and safe boot after arbitrary interrupted updates to the running slot. Multiple
keyboard report collections still share state, and partial NKRO handling retains
its documented limitations.

The finite specification does not prove the C implementation or silicon. Native
core scheduling does not explore every instruction or weak-memory interleaving.
Separate USB, storage and paired layers do not establish their complete hardware
integration. Real macOS/Karabiner/Zoom/idle behavior, PIO USB timing, electrical
behavior, and brownout/ROM startup require further evidence.

Linux/macOS CI is configured, including matching LLVM tools and replay artifact
retention. The branch has not been published, so no remote CI success is claimed.
