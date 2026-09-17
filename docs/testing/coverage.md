# Coverage, fidelity and remaining gaps

Snapshot: 2026-09-15. This is a boundary/behavior inventory, not a claim that all
firmware paths are covered. It describes the checked-in test artifacts and the
models they use. The runner's generated execution coverage, when enabled, is a
separate measurement; compiling a translation unit does not execute every branch
in it. No source-line percentage is inferred from suite counts.

The [v0.112 confirmed-save addition](confirmed-config-saves-v112.md) adds browser
intent/retry tests, actual paired command routing from both USB origins, and
native persistence/expected-value checks. The paired coverage layer executes
the new service using its instrumented library; storage covers the snapshot,
digest and readback helpers. Existing percentages below are historical and
are not extrapolated to the new code. The JavaScript endpoint double and paired
C transport tests remain separate layers, not a real browser-to-silicon test.

The deployed [v0.105 batch-transfer addition](batched-transfer-v105.md) is covered
at three layers: pure `fw_batch.c` protocol/collector tests, production
`firmware_batch.c` and page-commit logic over the storage oracle, and paired
UART/scheduler scenarios. The source-coverage runner includes both new units
in their relevant layers; the percentages below remain historical. Deep tests
add actual v0.104 mixed-version transfers and bounded four-core priority
exploration. The optional flash-delay model keeps peer cores and UART DMA live
while pausing both local cores; replay tests retain those durations and compare
complete traces. This models a conservative flash blackout, not RP2040
instruction/interrupt timing or physical USB behavior.

The subsequent [v0.106 hardware test](batched-transfer-hardware-v106.md) used
the same firmware with only a version bump. Propagation, image verification and
user input acceptance passed; measured progress did not establish actual batch
use or acceleration. Those performance/observability gaps remain after merging
the operationally accepted release.

## Reproducible source coverage

Run `python3 tests/coverage.py` with Clang and LLVM `llvm-profdata`/`llvm-cov`
(the runner finds Xcode tools through `xcrun` on macOS). No Python packages are
required. `--layer paired --layer storage` selects a subset; `--hid-iterations N`
changes the generated HID workload. Default measurements use scenario seed 1,
storage seed 1, the five original pure-policy suites plus selection, peer status,
history storage, peer history, peer observations, verification policy and protocol,
64,128 updater contract cases, and 2,000 generated HID descriptors at seed `0x484944`.

The generated `build/tests/coverage/html/index.html` links each layer's source
HTML. Every layer retains binaries, raw profiles, merged profiles, a text report,
LLVM JSON and exact requested/mapped source lists. Export includes every binary's
coverage mapping before any cleanup. Application and SDK/stack sources have
separate denominator groups. `summary.json` is a layer inventory; it deliberately
has no whole-firmware union percentage.

Measured on 2026-09-14 for the v0.99 candidate after all six layers passed
(`python3 tests/coverage.py --output build/tests/coverage-v099`):

| Layer / source group | Executed lines / mapped lines | Covered branches / mapped branches |
| --- | ---: | ---: |
| Paired application + native boundaries | 2,611 / 3,537 (73.82%) | 1,222 / 1,893 (64.55%) |
| Paired SDK queue implementation | 77 / 83 (92.77%) | 14 / 19 (73.68%) |
| Storage application units | 450 / 1,251 (35.97%) | 170 / 499 (34.07%) |
| HID regression + generated inputs | 629 / 900 (69.89%) | 333 / 548 (60.77%) |
| Pure policy units | 786 / 806 (97.52%) | 520 / 578 (89.97%) |
| USB device application callbacks/descriptors/console | 515 / 673 (76.52%) | 258 / 410 (62.93%) |
| USB device TinyUSB stack | 1,335 / 2,330 (57.30%) | 564 / 1,325 (42.57%) |
| USB host application units | 516 / 905 (57.02%) | 243 / 548 (44.34%) |
| USB host TinyUSB stack | 999 / 2,178 (45.87%) | 344 / 1,157 (29.73%) |

These percentages overlap and **cannot be summed or averaged into total
firmware coverage**. For example, storage links complete `tasks.c`/`handlers.c`
units while exercising their updater paths; its unexecuted mouse/hotkey paths
remain visible in that layer's denominator. Report groups omit test code, HAL
models, headers/inlines, Python/JavaScript, formal specifications, ARM startup,
and code that has no LLVM coverage mapping. Constants-only translation units
may have zero mapped executable lines. The generated report is authoritative
after code/compiler/test changes; these figures are a dated snapshot.

The v0.100 candidate adds `diagnostic_runtime.c` to the paired and storage
layers, and `peer_observation.c` to paired execution and pure-policy tests.
Storage now executes the real runtime/history publishers and asserts lock
ordering, sparse update events, progress freshness, and reset behavior. Four
additional mutation tests remove those hooks. Pure units cover legacy/new wire
formats and query-derived boot/target/core comparisons. The console still uses
mock peer/runtime snapshots in the real TinyUSB device layer; physical task
latency and a v2-to-v2 update remain hardware checks. The candidate's generated
report is `build/tests/coverage-v100/html/index.html`, with final policy and
USB-device refinements remeasured in
`build/tests/coverage-v100-final-targeted/html/index.html`; the table above remains
the dated v0.99 measurement.

The v0.101 candidate adds `verification.c`, `peer_verify.c`, and
`diagnostic_verify.c` to the paired layer, the policy/protocol to pure units,
and the actual verdict policy to the real TinyUSB console layer. Storage tests
execute the new full-slot read guards in `utils.c`, with independent CRC and
generation/invalidation checks, forced lock contention, and six additional
mutations. The paired scenarios run real scanners over modeled flash and
transfer the results through real queues and UART dispatch. Console snapshots
remain mocked in the USB layer. The production single-attempt hardware spinlock
adapter is compiled for ARM; native tests substitute an instrumented lock.
These checks do not measure physical scan timing or prove arbitrary memory
interleavings. See the [candidate record](verification-v101.md).

## Layers and independent oracles

The [UART integrity work released in v0.102](uart-integrity-draft.md) adds independent
CRC32 wire fixtures, whole-frame bit corruption, stream resynchronization,
mixed-protocol rejection and command-dispatch regression evidence. Historical
claims of accepting legacy UART frames below describe earlier versions; the
current firmware accepts only protected UART v1, including for legacy payload shapes.

The v0.99 diagnostic additions are exercised at separate boundaries: pure
history/peer protocol units use independent CRC and wire oracles; the paired
suite runs real bridges, SDK queues and UART dispatch; the real TinyUSB device
suite runs the console with mocked peer results. It checks capture under CDC
backpressure, merging, borrowed-result release, malformed input, and HID progress.
The [candidate record](peer-history-v099.md) documents work and clock bounds.
These layers do not prove physical lock contention or interrupt latency.

| Layer | Production code executed | Models / independent oracles | Principal limit |
| --- | --- | --- | --- |
| Original seven suites | zoom tracker, updater helper, screensaver policy, reboot recognizer, config migration; HID parser/report/keyboard/USB callback units; Web Config JavaScript | Explicit expected states/bytes, instrumented hardware/queue doubles | Useful retained regressions; original HID mouse decoding and queues are stubs |
| `tests/hid_fuzz.c` | Actual HID parser, scalar extraction, keyboard/consumer/system extraction and USB report dispatch | Bit-at-a-time signed scalar oracle; exact-sized buffers; descriptor mutation/truncation; 256 dispatch IDs per generated descriptor | Sanitizers and finite generated cases; no parser completeness proof, original HID boundary stubs remain |
| `tests/sim/` | Actual application units plus checked-in SDK `queue.c`, separate globals and statics for A/B | Independent standard HID fixture bytes, exact outgoing reports, production UART serialization/receiver with modeled DMA arrival, controlled clock and host acceptance | HAL/task boundary schedules; modeled USB stack, CPU and hardware |
| `tests/sim/test_native_boundaries.c` | Same native application build, focusing on mouse decoding and protocol output-index bounds | ASan/UBSan exact allocation, 256 mouse IDs, short/empty reports, independently asserted state preservation and output | Native ABI; no ARM instruction or arbitrary memory-interleaving coverage |
| `tests/storage/` | `utils.c`, `ramdisk.c`, `tasks.c`, `handlers.c`, protocol map, updater/migration/selection helpers and production types | NOR bits/alignment/counters; independent bitwise CRC; full image bytes; finite TX queue; real SET_VAL interleaved inside a save snapshot | Update operations are serialized; one explicit config-copy preemption; no USB mass-storage wire stack or arbitrary instruction-level concurrency |
| `tests/model/` | `policy_contract.c` links the actual updater helper; `check_flash.py` separately checks a specification | 64,128 finite C contract cases; exhaustive reduced two-core graph; mutation counterexamples and shortest-path replay | Specification assumptions are not automatically proved for C or RP2040 |
| `tests/emulation/` | Small ARM instruction sequences in rp2040js, not DeskHop firmware | Arithmetic oracle; two emulator instances exchanging UART register writes/reads; multicore capability probe | Each instance has one CPU and no working SIO launch FIFO; no PIO-USB or DeskHop boot |
| `tests/usb_stack/` | Checked-in `tusb.c`, device/control, HID, MSC and FIFO units plus production `usb.c` / `usb_descriptors.c` | Queued virtual DCD SETUP/XFER_COMPLETE events; independent host control requests, descriptor lengths, stalls and class transfer assertions | ASan/UBSan pass; device-controller transaction model, modeled MSC backing callbacks; no PHY, PIO or real host OS |
| `tests/usb_host/` | Real TinyUSB host/HID/hub/FIFO units plus production USB callbacks, parser, report extraction, keyboard and reboot recognizer | Virtual HCD/peripherals; real reset/debounce/address/configuration/report/protocol enumeration; interrupt-IN rearm, LED OUT, malformed keyboard reports and detach releases | ASan/UBSan pass; host controller/PIO hardware and pointing transform modeled; separate from paired routing |

Native paired simulation loads two distinct shared-library copies. It executes
production SDK queue algorithms and application functions, rather than writing
a second keyboard/mouse/router implementation. Spinlocks, clock reads, USB
controller effects, DMA delivery, and flash hardware remain explicit models.
The independent source oracles matter: merely comparing two Picos executing the
same bug, or comparing an internal pointer to its own emitted report, would not
establish correctness.

## Production translation-unit inventory

Every C file directly under `src/` is accounted for below. “Included” means
linked into a test layer, not complete branch coverage. The bundled Pico SDK,
TinyUSB and Pico-PIO-USB contain additional source outside this inventory.

| Production units | Test inclusion / exercised responsibility | Remaining responsibilities |
| --- | --- | --- |
| `hid_parser.c`, `hid_report.c` | Original HID suite, generated HID tests, paired/native boundary builds; usage carry/list capacity, report offsets, NKRO, bit extraction, malformed/truncated inputs | All legal HID combinations, collection semantics and an independent full HID parser |
| `keyboard.c`, `keyboard_sync.c`, `reboot_hotkey.c` | Original recognizer/HID suites and paired scenarios; report routing, modifiers, F24 and exact reboot sequence | Real host virtual-keyboard state; all possible multi-keyboard interactions |
| `mouse.c` | Paired scenarios and sanitized native boundaries; descriptor-driven decoding, absolute/relative output, per-interface/peer button masks, detach, focus all-up, nonmotion and synthetic desktop reports | All pointing-device descriptors, arbitrary loss of ordinary physical reports, physical host button semantics |
| `zoom.c`, `zoom_tracker.c` | Original tracker suite plus paired debt/quiet-exit/manual-mode scenarios | Whether macOS is actually magnified; host scroll interpretation; exhaustive shared-edge paths |
| `screensaver_policy.c` | Original policy suite and actual `tasks.c` timed paired keep-awake | Real macOS idle counter and lock/sleep policy |
| `tasks.c` | Paired scheduler/policy/queue/heartbeat/watchdog calls; full storage updater | Actual loop instruction timing, every scheduler/IRQ/DMA interleaving, full Pong trajectory coverage |
| `handlers.c` | Paired routing/switch/reboot handlers; storage heartbeat/source/reply/updater guards | Complete API, maintenance-hotkey, calibration and bootloader-command coverage |
| `selection.c` | Paired production selection requests and reconciliation; independent wire fixtures for loss/recovery, ordering, joins, concurrent intent and wrap; targeted mutations | Assumes eventual delivery and fewer than 2**31 outstanding generations; unlimited resets with arbitrarily stale traffic are not proved |
| `uart.c` | Real packet writer, queueing, checksum dispatch, DMA-ring receiver with modeled byte arrival | Actual UART/isolator electrical behavior, hardware DMA wrapping/overrun semantics |
| `protocol.c` | `consumer_system` routes real reports to both outputs; `vendor_config` exercises guarded SET/GET, GET_ALL (44 fields), invalid checksum/length/ID/type, read-only fields and endpoint backpressure | Every mutable-field value, malformed/proxied config combination and persistence interaction |
| `usb.c`, `usb_descriptors.c` | Actual app callbacks and descriptors in paired/HID layers; real TinyUSB device/control/HID/MSC stack in `usb_stack` | Physical USB/PIO, all descriptor/host combinations; real host and device stacks are separate from paired application simulator |
| `led.c` | Paired focus indication and five-transition acknowledgement pulse scenario | Actual Sofle/QMK LED behavior and exhaustive transport failure/acknowledgement semantics |
| `fw_update.c` | Existing helper suite, 64,128 contract cases, full storage updater | Unbounded protocol liveness under arbitrary link failure |
| `config_migration.c` | Original migration tests plus real flash load/save/migrate/reload | Every historic unsupported layout and arbitrary application config consistency |
| `utils.c` | Paired utilities; storage CRC/read/write/lock/config/recovery paths; coherent persisted config snapshot under a real concurrent setter | Physical XIP safety, boot image execution, all live config-reader interleavings, each untested utility branch |
| `ramdisk.c` | Actual UF2 MSC callbacks in storage suite | FAT/macOS copy behavior, SCSI transport and host request fragmentation through a complete stack |
| `constants.c`, `defaults.c` | Production constants/default configuration linked in relevant native layers | Compiled data is not proof every option works in every combination |
| `setup.c` | ARM build only | Role-probe GPIO/isolator logic, USB/PIO/DMA/clock initialization, startup flash/RAM behavior |
| `main.c` | ARM build; task tables extracted for native scheduling; explicit keyboard init-before-launch and core0 announcement ownership assertions | Production startup, two real loops/core launch and exact in-core instruction order |

`disk/webconfig.html` auto-start behavior retains the existing JavaScript suite.
The generated disk image, browser UI, field persistence, and full USB mass-storage
workflow are distinct boundaries; testing the checkbox alone does not establish
timed keep-awake behavior.
No QMK checkout or target Mac is modified by these tests.

## Requested behavior matrix

| Requirement | Concrete test/scenario | Coverage and remaining gap |
| --- | --- | --- |
| Position-neutral clicks and wheel on both Macs | paired `pointer` | Poisons inactive cached coordinates, checks owner coordinates for absolute reports and zero deltas for relative reports in both output directions |
| Cross-Pico pointer synchronization / zero-motion composite reports | `pointer_sync` | Actual physical input decode, serialized UART and owner report; checks unchanged physical-activity timestamp for empty composite report |
| Button aggregation | `mouse_button_aggregation`, `mouse_button_sources_and_detach`, `mouse_button_split_reports`, `mouse_incomplete_fragments_keep_holds` | OR of independent local interfaces and peer sources; same-bit holds, releases/detach, motion/wheel/pan, idle composite input, split IDs and rejected truncated fragments; both outputs and absolute/relative modes |
| Held-button focus changes | `mouse_output_switch_held`, `mouse_switch_inflight_source`, `mouse_switch_full_queue` | Actual F24 releases both old-host HID interfaces; retains source masks for later input, supports detach after switching, rejects stale physical delivery to the inactive host, and requests recovery at the 100 ms critical-queue bound |
| Mouse capability and synthetic reports | `mouse_asymmetric_capability`, `mouse_synthetic_desktop_preserves_sources` | Asymmetric startup cannot reinterpret a queued physical report as synthetic; actual macOS desktop edge handling emits five zero-button relative nudges while subsequent physical input retains both holds. Full behavior requires upgraded peers |
| IDs 0–255, offsets and bounded usage lists | original HID `test_all_report_id_receivers`, `test_distinct_report_capacity`, generated HID cases; native mouse ID loop | Receiver-map/offset distinction preserved; 24 report offsets and four NKRO blocks remain representation limits |
| Multi-block NKRO, usage carry, consumer/system reports | original HID named tests plus `hid_fuzz` | Independent outputs/activity guards; exact allocations expose memory errors. Multiple keyboard reports/collections still share collapsed state |
| Malformed/truncated reports and false activity | generated HID, scalar oracle, `test_native_boundaries` | Exact short inputs and independent state/activity invariants; finite corpus, not every malformed descriptor |
| Ordinary keyboard state recovery | `keyboard_*` in `test_keyboard_reliability.py` | Real queue saturation, loss/corruption, source FIFO overload, duplicate/stale/session/context rejection, focus and USB reconnect, peer lease, multiple sources, consumed hotkeys, synthetic lock release and diagnostics; both directions. [Draft contract and limits](keyboard-reliability-draft.md) |
| F24/held modifiers and all-up | `f24_releases_held_modifiers`, `critical_queue`, `uart_queue_switch` | All modifier bits, exact output releases, actual queue saturation and durable all-up without keyboard reset; mouse timeout remains separate; no Karabiner proof |
| USB enumeration, disconnect, reconnect, suspend and backpressure | paired `disconnect`, `backpressure`; USB stack `enumerate`, `hid_data_and_leds`, `suspend_and_unplug` | Device/config/HID/string requests, partial/multi-packet control transfers, address/configure/unconfigure, invalid-request stall, real HID busy/completion, LED SET_REPORT, remote wake and unplug/re-enumeration. DCD/host transaction model; no physical host-driver execution |
| Keyboard LED focus and acknowledgement pulses | `led_focus_and_acknowledgement`; USB stack `hid_data_and_leds` | Actual host SET_REPORT passes through TinyUSB to application; paired focus policy/five 80 ms transitions with sync suppression. Outgoing Sofle SET_REPORT acceptance remains modeled |
| Zoom activation, debt, overscroll and quiet time | `zoom_scroll_debt_and_quiet_exit`, original `test_zoom_tracker.c` | Checks active relative reports, debt repayment, six-unit overscroll and quiet-deadline restart/expiry; helper tests cover debt saturation and stale modifiers |
| Zoom/manual gaming independence | `zoom_and_gaming_are_independent` | Actual hotkeys, both inferred scroll directions, manual reset and output report mode; full edge-switch stress remains separate |
| System-wide keep-awake timing | `timed_system_wide_keepawake` | Real A/B physical input, 10-second alternating jitter, unchanged direct timestamps through synthetic output/peer sync, stop after configured global idle and restart on real activity |
| Inactive-output idle policy | `keepawake_inactive_only`, screensaver policy suite | Output focus changes and timeout-disabled case; per-output maximum/Pong combinations are not comprehensively exercised |
| Persisted auto-start and migrations | storage `config_persistence_and_migration`, original migration/JS tests | Actual v8/v9/v10 flash load/save/reload and CRC; disabled mode persists; corruption defaults; separate from actual timed jitter scenarios |
| Config save concurrent with SET_VAL | storage `config_set_during_save_keeps_persisted_crc_coherent` | Real API setter attempts to update a multibyte field halfway through the save copy; independently validates CRC and whole-old/whole-new value, retains the later RAM edit, and saves/reloads it subsequently. The config lock is absent during flash operations; this is a selected preemption, not all memory races |
| UART delay/loss/corruption/partial packets | `uart_faults`, `uart_queue_switch`, storage peer tests | Modeled byte scheduling enters real ring/parser; exact source/receiver full-image traffic in storage; physical framing/parity errors are not simulated |
| Selection loss, ordering and recovery | `selection_loss`, `selection_link_recovery`, `selection_reordering`, `selection_concurrent`, `selection_duplicate_keeps_keys`, `selection_queue_retry`; join/wrap/legacy cases in `test_selection.py` | Runtime generation/origin ordering and recurring ID32 reconciliation restore agreement within the tested post-recovery bound; duplicates do not release held keys again, full queues retry. Requires both upgraded peers, recurring polls, eventual delivery and less than half the counter range outstanding; permanent partitions cannot converge |
| Full peer update / completion boundaries | storage `peer_transfer_queue_loss_duplicate_wraparound` | 65,536 actual source word reads and receiver operations per full transfer, exact final-page/image boundaries, queue backpressure and retry |
| Source pinning / recovery / stalled peer | `peer_corrupt_word_recovery`, `peer_stall_pause_and_restart`, `source_reads_and_metadata` | Corrupt words, pinned checksum/version, paused dirty state, repair on return, source refusal, legacy sentinel/metadata |
| UF2 ordering/duplicates/invalid/mixed image | `uf2_reordering_and_duplicate`, `uf2_reject_invalid`, `uf2_complete_mixed_image_enters_recovery`; USB stack MSC requests | All 1,024 blocks, shuffled order, independent CRC and NOR byte/counter oracle through real MSC callback. Separate real MSC stack handles class control requests, SCSI INQUIRY/READ_CAPACITY/READ10 bulk data and invalid-CBW stalls with modeled storage; these are not yet one integrated end-to-end UF2 stack test |
| Two-core flash/config/update ownership | storage `host_peer_config_serializations`; finite model | Six operation permutations plus modeled phase interleavings, lock owner/interrupt assertions, five specification mutations; not all C memory operations |
| Controlled reset/update races | storage `reboot_reservation_rejects_update_work`, `watchdog_deadline_and_reboot`; model | Late writes/source replies refused and watchdog kick boundaries; unrelated hardware reset causes remain outside the model |
| Interrupted update power / config write | `power_cut_preserves_nor_but_loses_ram_ownership`, `config_power_cut_falls_back_to_defaults`; model power gap | Seven firmware cut points and torn config program. Config falls back; running-image updates are not atomically power-loss safe |
| Exact coordinated reboot | `reboot_three_completed_taps`, `reboot_rejects_extra_modifiers`, original reboot suite | Completed-tap counting, repeat suppression, extra modifiers, release ordering, peer notification and two watchdog stops; ROM restart/re-enumeration not instruction-emulated |
| Core hang/watchdog | `core_watchdog`, storage watchdog contract | Pause one modeled core and observe watchdog stop; check exact helper deadline; no silicon watchdog calibration |

Names in the matrix match functions/scenario keys in
[paired transport tests](../../tests/sim/test_transport.py),
[paired behavior tests](../../tests/sim/test_behaviors.py),
[mouse source tests](../../tests/sim/test_mouse_buttons.py),
[mouse handoff tests](../../tests/sim/test_mouse_extra.py),
[selection tests](../../tests/sim/test_selection.py), and
[storage tests](../../tests/storage/test_storage.c).
The selection scenario inventory is generated from its `scenario_` functions;
`python3 tests/sim/run.py --help` lists all currently registered paired cases.

## Scheduling, replay and mutation evidence

The paired simulator uses a seeded deterministic event scheduler. Its task,
UART-byte, host-change and selected HAL checkpoint order is reproducible.
The model explicitly bounds blocking waits and the core schedule; it does not
preempt at every load/store. Each core pass preserves the task order extracted
from production main.c. Independent passes use an explicit polling quantum and
seeded tie ordering, not a measured RP2040 loop execution profile. The deep tier
also runs the backpressure scenario under all 24 fixed priority orders for
simultaneous core passes. Clock advances and explicit task invocations are
useful for exact policy boundaries, but do not become physical nanosecond timing
because the timestamps are precise.

Saved paired steps include input operations and serializable `expect`,
`expect_report` and `check` assertions. Substantive elapsed-time, report-suppression,
age-range and deadline assertions use the recorded predicate API, so generic
replay/minimization preserves those oracles. Harness contract tests verify exact
trace replay and that an intentionally impossible delivery property survives minimization.
Storage replay uses seed plus named scenario, and its JSONL is an observation
trace rather than an arbitrary input script. The finite model produces shortest
counterexample paths and validates replay automatically. See each runner's help
for current shrinking/minimization support; do not infer it from a saved log.

The storage mutation suite executes twenty-one deliberately broken production variants
and requires runtime detection, not compile failure. The model detects five
separate specification mutations. These demonstrate sensitivity to representative
bugs such as premature finalization, queue consumption on failure, invalid CRC
acceptance, missing flash exclusion and a SET_VAL writer bypassing the config
snapshot lock. The paired suite defines seventeen production mutations, including
seven selection mutations, mouse peer-mask/echo/detach checks, and two UART
integrity/resynchronization regressions. Each runner
reports its actual runtime results; these inventories are not completed-run
claims or a universal mutation score. The intentional lost-delivery apparatus
case checks replay/minimization independently of an unfixed production bug;
source execution percentages above measure exercised code, not mutation quality.

## Outstanding findings and fidelity obligations

Cross-device mouse aggregation and bounded selection convergence after link
recovery are passing regression properties for upgraded peers. The compatibility
path still accepts legacy selection commands and physical mouse packets; it
cannot give old firmware the new reconciliation or explicit-synthetic behavior.
Ordinary input packets are not an acknowledged, lossless transport, and these
tests do not promise delivery through permanent partitions or every queue/link
failure. Multiple keyboard report collections still collapse onto shared
keyboard state. The updater's volatile ownership flag cannot guarantee safe boot
after arbitrary loss of power while rewriting the running slot.

Configuration format remains 10 in firmware v0.94. The new short RAM lock covers
the persisted snapshot and known config writers; it is released before flash
work. It does not make all application reads atomic or prove every two-core
memory interleaving. Neither this inventory nor the dated source-coverage table
substitutes for the final overall run recorded in `validation.md`.

There is no blanket proof of USB enumeration on macOS, Karabiner modifier state,
Accessibility Zoom state, real idle timer resets, Sofle LED behavior, isolator
margins, USB signal integrity, flash brownout behavior, ARM compiler correctness,
PIO/DMA arbitration or every race. The actual device-stack tests against a virtual controller improve USB
transaction coverage while retaining those physical/host gaps. The separate host-stack suite exercises real TinyUSB host enumeration/HID code
against a virtual HCD; it does not execute the physical PIO-USB controller driver.
Passing host, device, storage and paired application layers separately does not
prove their complete integration.
The [architecture decision](architecture.md) explains why production simulation,
protocol/peripheral models, instruction emulation and formal specifications are
kept separate, and what evidence is required before raising a fidelity claim.
