# Hardware-free firmware testing architecture

Decision date: 2026-09-14. Baseline: DeskHop v0.92/configuration 10 at `d42c930`.
The three maintainer/integration documents were read before choosing an
approach. They identify seven useful native suites, but explicitly exclude
queues, UART transport, mouse decoding, USB timing and concurrency. Passing
those tests or the recent basic physical smoke check is not comprehensive
verification.

## Decision

Use a hybrid whose principal engine executes production C against deterministic
peripheral models. Keep separate test layers for full-size NOR/UF2/config
storage, pure policy contracts, generated malformed HID inputs, and a small
explicit-state concurrency specification. Preserve the hardware architecture
and ARM build. Treat instruction emulation as a supplementary research target
until required RP2040 devices are demonstrated.

This choice follows three working prototypes: real production units can run in
a native harness without replacing the firmware algorithm; the current
rp2040js emulator exchanges UART bytes between two instances but fails the per-instance multicore capability probe; and a finite flash
ownership model exhausts 3,395 states while detecting five removed safeguards.
The native approach is sufficient to add real behavior coverage now. A complete
rewrite does not supply missing USB PHY, host OS, or electrical truth and would
create a much larger equivalence obligation.

## What full fidelity would require

There are two Picos, each with independently scheduled Cortex-M0+ cores,
interrupt controllers, PIO state machines, DMA, USB controller, UART, watchdog,
flash and power state. They share bytes across an isolator and expose USB HID
to separate macOS hosts while hosting a composite Sofle and a trackball.
An honest full-system model must include every one of those components and the
observable ordering/timing at their boundaries. Matching the C output for
specified inputs is a narrower, valuable claim.

| Layer | What can be established without target hardware | What remains outside that claim |
| --- | --- | --- |
| Production-code simulation | Actual parser/routing/policy/queue/protocol/storage decisions for supplied inputs and schedules | ARM instructions, ABI-specific layout unless separately checked, weak memory, unmodeled accesses |
| Peripheral/protocol model | Specified queue capacity, UART delivery/faults, endpoint acceptance, NOR erase/program and resets | Silicon implementation, electrical effects, exact physical frame timing |
| Instruction-level emulation | Behavior of an ARM binary under an implemented machine model | Accuracy/completeness of the machine, undocumented errata, actual Mac stack |
| Bounded/finite formal model | Named invariants for all states reached within stated bounds/assumptions | Automatic correspondence to production C, arbitrary system sizes or timing |
| Hardware/macOS validation | Actual USB enumeration, HID interpretation, Karabiner modifier behavior and real power/timing effects | Still only the cases and environment exercised |

No hardware-free result establishes that Karabiner never sticks a modifier,
that macOS interprets every report as intended, that the host sleep counter
resets, that zoom assist always matches real Accessibility Zoom state, or that
PIO USB meets electrical/timing margins. Host helper software is unnecessary
for the firmware and for these local/CI tests; no test installs software on
either target Mac.

## Options compared using current primary sources

| Option | Feasibility/evidence | Decision |
| --- | --- | --- |
| Native production C with virtual devices | Existing seven suites demonstrate portable seams; the new paired and storage prototypes exercise substantially more production units | Primary engine; fast sanitizers, deterministic fault injection, real callbacks and state |
| Wokwi / rp2040js | Wokwi documents one simulated core, PIO, and USB CDC. Current npm 1.3.4 instantiates one core and has no working SIO inter-core FIFO in the executable probe | Cannot run this unchanged dual-core workload; retain a pinned probe so improvements can be reassessed |
| Stock QEMU | ARM/M-profile instruction support exists, but its current board index lists no RP2040; its Raspberry Pi machines are different boards. No QEMU executable was present | CPU support alone is insufficient. Writing the missing SoC and PIO-USB devices would be a separate emulator project |
| Renode with community RP2040 model | The community model reports working multicore examples and partial PIO, but no USB, no PIO IRQ/DMA, and stubbed XIP/cache behavior. The model calls itself frozen/WIP | Closest route to future binary-level testing, but it does not provide DeskHop's required USB host/device system today |
| Renode + HDL/PIO co-simulation | Could pair firmware CPUs with a better PIO/USB model and deterministic synchronization | Potential second phase after validating devices; integration and timing calibration remain significant work |
| Functional-core refactor / complete rewrite | Could make every dependency explicit and establish cleaner ownership | Small seams may help; a full rewrite is unjustified merely to avoid stubs. It would need differential equivalence and hardware builds across all preserved features |
| CBMC on C | Supports C memory-safety and assertions with bounded loops; unwinding assertions are required to establish adequate bounds | Good future narrowly scoped parser/queue proofs. Do not claim whole-firmware proof from a bounded helper harness |
| TLA+/TLC or explicit-state specification | Useful for source ownership, lock ordering, reset/update exclusion and recovery state design | Included as a reduced model with assumption and production mapping; not a substitute for production execution |
| Lean/Coq/TLAPS theorem proving | Can prove carefully stated inductive properties of a specification with a substantial formalization/refinement effort | No theorem proof is claimed; greatest near-term value lies in executable code tests and a small reviewable state model |

The emulator support claims come from [Wokwi's Pico reference](https://docs.wokwi.com/parts/wokwi-pi-pico),
[QEMU's ARM machine documentation](https://www.qemu.org/docs/master/system/target-arm.html),
and the [RP2040 Renode model's own supported-peripheral/test list](https://github.com/matgla/Renode_RP2040).
Renode's official [supported board list](https://renode.readthedocs.io/en/latest/introduction/supported-boards.html)
did not list RP2040 when checked; the community model should not be presented
as official complete support. Its successful multicore examples are real
counterevidence to a blanket assertion that every emulator lacks dual cores.

Renode's [time framework](https://renode.readthedocs.io/en/latest/advanced/time_framework.html)
uses virtual time, configurable CPU MIPS and synchronization quanta. That can
make repeated emulations deterministic, but it does not prove physical CPU/bus
cycle timing. [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
uses PIO state machines plus a repeating host timer; generic PIO instruction
support alone is not a tested low/full-speed USB bus or peripheral model.

[CBMC's documentation](https://www.cprover.org/cbmc/) explains its C assertion
and memory checks; its [unwinding guidance](https://www.cprover.org/cprover-manual/cbmc/unwinding/)
explains why a too-small bound can miss bugs. The [TLC documentation](https://docs.tlapl.us/using%3Atlc%3Astart)
describes finite explicit-state checking. The local Python checker uses that
style of exploration but is its own small specification tool, not TLC/CBMC.

## Executed prototype evidence

| Prototype | Reproducible command | Observed result |
| --- | --- | --- |
| Current RP2040js package | `node tests/emulation/probe_rp2040js.cjs /tmp/deskhop-rp2040js-probe/package` | ARM arithmetic executes (`r0=43`), and two separate instances exchange `0x41`/`0x42` using ARM UART register writes/reads. Each instance still has one core; CPUID=0; SIO FIFO accesses unimplemented (`0xffffffff`) |
| Two-core finite model | `python3 tests/model/check_flash.py --mutations --power-loss --output /tmp/deskhop-model-traces` | Baseline graph exhausted: 3,395 states / 7,832 transitions. Five mutations detected and automatically replayed |
| Actual updater helper | compile/run `tests/model/policy_contract.c` with `src/fw_update.c`, per model README | 64,128 finite timing/owner/response contract checks pass, including 32-bit clock translations |
| Actual TinyUSB device stack | `python3 tests/usb_stack/run.py` | Real device/control/HID/MSC/FIFO code, application USB callbacks/descriptors; queued virtual DCD events, enumeration/class requests and endpoint completion under ASan/UBSan |
| Actual TinyUSB host stack | `python3 tests/usb_host/run.py` | Real host/HID/hub/FIFO code, actual parser/keyboard callbacks; virtual HCD/peripheral enumeration, polling/rearm, LED OUT and detach under ASan/UBSan |
| Actual storage/update units | `python3 tests/storage/run.py` | Full image transfer/storage scenarios, interrupted writes and serialized core operations; consult storage README for exact scope |

The [emulator experiment](../../tests/emulation/README.md) includes package hash,
commands and recorded JSON. It is deliberately outside offline CI dependencies.
No DeskHop boot in QEMU/Renode/rp2040js was achieved or claimed. The conclusive
rp2040js gate is missing multicore launch support, before a full firmware boot
would become useful. Interconnecting two emulated Picos is possible: the same
probe now does so using UART0 callbacks plus a shared virtual clock and confirms
both directions in actual emulated ARM registers. The unmet requirement is each
Pico's internal dual-core/USB system, not the ability to connect emulator
instances. The ideal serial byte duration and 1,000 ns propagation in this probe
are chosen peripheral-model parameters, not measured electrical timing.

The [model documentation](../../tests/model/README.md) names its abstraction and
assumptions. A source-lock mutation yields a six-transition conflicting claim;
a flash-lock mutation yields a nine-transition read/write overlap; reboot,
checksum-pin and config-guard mutations fail in 12, 21 and 11 transitions.
These are model mutations. They are distinct from source mutations exercised by
the production-code test framework.

## Execution/scheduling contract

The implemented node build links the checked-in Pico SDK `queue.c` and the real
application units listed in `tests/sim/build.py`; queues have production element
sizes and capacities. Two copied shared libraries preserve independent global
and file/function-static state. Numeric accessors avoid assuming identical ARM
and host structure layouts. Setup, `main` loops, the physical TinyUSB stack and
PIO are modeled or excluded, while `ramdisk.c` runs in the separate storage
harness. This is materially broader than the original HID stub suite.

Virtual time must advance only under harness control. Each simulated Pico owns
its complete firmware state and queue/peripheral state. Tests observe outgoing
HID packets independently of internal pointer estimates and UART packet bytes
independently of the sender's own serialization oracle. Inject USB callbacks,
UART bytes and timer deadlines at explicit event boundaries. Seeded workloads
must retain their seed and event trace; explicit replay must preserve event
ordering. Saturation tests need finite-capacity queues and observable completion
or failure paths, rather than unconditional successful enqueue stubs.

Scheduling at C calls or hardware API boundaries does not expose every
machine-instruction interleaving. A test may claim a bounded set of core/event
schedules only if those points are enumerated; it may not claim all races are
covered. Internal memory accesses, actual interrupt nesting, RP2040 spinlock
semantics and DMA arbitration remain separate obligations. The production
flash wrappers have a real cross-core lock and mask interrupts; Raspberry Pi's
[flash API documentation](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/hardware_flash/include/hardware/flash.h)
requires preventing concurrent XIP accesses. `PICO_COPY_TO_RAM=1` supports the
intended design, but the ELF/map and every remaining XIP read still need review.
TinyUSB's [concurrency guidance](https://docs.tinyusb.org/en/latest/reference/concurrency.html)
is another reason to keep USB stack callbacks and IRQ work distinct from
application events.

Use elapsed-time boundary assertions for keep-awake, zoom quiet timers,
watchdogs and retries. A configured flag proves no elapsed behavior. A liveness
assertion must state link/host availability and scheduler fairness, for example:
with successful endpoint completions and bounded link delay, all-up reaches the
old output before the deadline. No framework can prove eventual peer delivery
under permanent link loss.

## Important architectural gap exposed

The current updater programs the running slot while executing from RAM.
The controlled reset path checks dirty/active state, and invalid final images
explicitly enter ROM recovery. Those are useful safeguards. They do not make
arbitrary power loss safe: after boot2 has been rewritten successfully, a cut
before the remaining image finishes may leave a valid boot entry and invalid
body, while the dirty flag disappears with RAM. The storage prototype preserves
NOR bytes across injected cuts and verifies invalidity after volatile reset;
the specification model emits the corresponding counterexample. Neither one
pretends to execute ROM boot.

An independently validated boot-time full-image check, durable commit record or
A/B update layout would be a separate production architecture change. Testing
has made that gap visible; this framework does not silently redesign the
bootloader or claim interrupted-power atomicity.

## How to extend fidelity without replacing the test suite

First add production scenarios and fault schedules for uncovered behavior.
When a bug depends on a boundary the harness does not model, add that boundary
and a regression. Retain source/model mutations to check that the corresponding
oracle can detect a representative failure.

The separate `tests/usb_stack/` prototype now compiles the checked-in TinyUSB
device/control/HID/MSC stack against a virtual DCD, drives real queued SETUP and
completion events, and checks enumeration, descriptors, stalls, HID/LED handling,
suspend/resume, MSC control requests and SCSI bulk reads under ASan/UBSan. The paired simulator
still uses its simpler endpoint model. The separate `tests/usb_host/` prototype also runs the real host/HID/hub stack
through a virtual HCD/peripheral and actual parser/keyboard callbacks. Connecting
real UF2 backing storage through the MSC bulk transport and running both stacks
with the paired topology are distinct next increments.
Do not imply that separate passing layers already establish their composition.

Revisit binary emulation when a candidate passes a gate suite: launch both
cores through the SDK FIFO handshake; run concurrent UART/DMA; execute the
checked-in PIO USB program with timed low/full-speed peripherals; enumerate
DeskHop's HID endpoints; and perform a flash update/reset cycle. Calibrate
against a physical capture before making physical-timing claims. Model checking
can later target a redesigned persistent update protocol, with explicit
implementation trace/refinement checks instead of assuming the model is C.


## Preservation check after regression fixes

`python3 tests/sim/differential.py` builds both the original `d42c930` sources
and this branch through the same native adapter. All nine scenarios run their
independent endpoint/state/deadline assertions on both builds. Four retain exact
timestamped USB/LED/reset/wake traces: backpressure, LED acknowledgement, zoom
quiet-time/debt and system-wide keep-awake. Five compare ordered effects for each
host: pointer input/synchronization, UART faults, F24 and coordinated reboot.
That comparison suppresses only repeated identical keyboard states and allows
one modeled HID poll (1 ms) of event-time drift; observed drift was at most
500 microseconds. It retains every mouse report, required key release and other
effect. Cross-host interleaving and callback core tags are outside that contract.

New reconciliation packets intentionally change the UART trace, so it is excluded
from cross-version comparison. Same-image replay still checks the full trace.
The actual old receiver accepts the extended immediate selection message and
ignores the new sync message. These are finite preservation and compatibility
checks, not universal equivalence, physical timing or complete mixed-version
semantics. New regression scenarios separately assert the intended fixes.
