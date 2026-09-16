# Storage and firmware update production-boundary tests

Run from the repository root:

```sh
python3 tests/storage/run.py
python3 tests/storage/run.py --seed 197 --seeds 32
python3 tests/storage/run.py --seed 197 --trace /tmp/deskhop-storage-197.jsonl
python3 tests/storage/mutations.py
```

The first command builds with AddressSanitizer and UndefinedBehaviorSanitizer and
runs in about one second on the development Mac. `CC` selects the native C
compiler. `--no-sanitize` is available for a debugger or unsupported host. Builds
and mutation variants live in temporary directories. The production files are
never rewritten. Python 3 and a C11 compiler are required; no Pico or software on
either target Mac is involved. Every reported failure includes its original seed,
scenario, virtual time, modeled core, and failing oracle. Repeating `--seed` runs
the same schedule and UF2 permutation. `--trace` emits JSONL flash, dispatch,
peer-word, watchdog, and power-cut events for that run. The trace is an observation
log; replay regenerates the scenario from the seed, not from arbitrary edited logs.

## What executes

The suite compiles the complete checked-in `utils.c`, `ramdisk.c`, `tasks.c`,
`handlers.c`, `fw_update.c`, `config_migration.c`, `constants.c`, `defaults.c`,
`protocol.c`, `selection.c`, `diagnostic_runtime.c`, `diagnostic_history.c`, and
`history.c`. `run.py`'s `SOURCES` is the build inventory.
The linker removes unused functions; no updater or MSC callback is copied into
test code. Production `device_t`, config layout, UF2 layout, protocol enums,
metadata, and TinyUSB HID types are included directly.

`main.h` replaces the hardware include umbrella. It supplies a finite packet FIFO,
virtual clocks, observed critical sections, interrupt-mask state, watchdog/reset
sinks, and a NOR flash model. NOR erases require sector alignment; programs require
page alignment and can only clear bits. Flash commands must run while the current
core owns the flash critical section and has interrupts disabled. Flash partition
offsets match `misc/memory_map.ld`: running image at zero, embedded disk at 188 KiB,
metadata at 252 KiB, staging at 256 KiB, and configuration at 2044 KiB. Native
addresses are relocated; the RP2040 absolute XIP address is not instruction-emulated.

For every word of the two full peer transfers, emitted requests run through the
real source `handle_request_byte_msg` and `read_running_firmware_word`, then the
real receiver response handler and firmware upgrade task. The source has separate
state, image bytes, and TX queue. An adapter switches modeled flash address spaces
between invocations. Update operations run serially and check lock ownership.
The config regression additionally invokes the real SET_VAL handler on modeled
core 1 halfway through core 0's save copy, splitting a 64-bit field after four
bytes. If its real config-lock acquisition blocks, the boundary defers the
side-effect-free setter until the save releases that lock. Removing the setter's
lock allows it to execute inside the copy and produces a torn persisted value.
This suite does **not** run two CPUs or exhaustively interleave C memory accesses.
Configuration validation adds the opposite schedule: a real snapshot reader
preempts a SET, border-pair update, or load publication. Both writer/reader
orders split a timeout after four bytes or a border pair between top and bottom.
The blocked operation resumes only after the owning core releases the real
configuration lock. A stale screen-index update after a count reduction also
runs against the production publication helper. These are selected schedules,
not an instruction-level race proof.
The broader simulator and reduced model checker cover additional
scheduling questions at their separately documented abstraction levels.

Updater observations run through the production runtime snapshot and 64-record
history store. The lock boundary preserves firmware/flash/config IDs and adds
separate history/runtime locks: updater hooks may hold firmware while briefly
using either diagnostic lock, but those two locks may not nest, readers may not
acquire firmware or flash while holding them, and neither may remain held during
flash operations. The ROM-reset sink checks that a failure event already exists
at reset entry; it does not infer that volatile history survives a real reset.
The core 1 diagnostic transport is a boundary stub here that checks checkpoint
publication before transport entry. Dedicated peer suites exercise its UART work.

The verification guard also runs from production `utils.c`. Only
`dh_critical_section_try_enter` is replaced by a native one-attempt lock primitive;
the RP2040 implementation lives in `src/include/critical_try.h`. Guard tests
preserve the other core's lock ownership and both interrupt masks, prohibit any
blocking acquisition inside the API, and observe each actual flash copy. They
read all 1,024 pages, including the metadata sector, and compare the resulting
full-slot CRC with the independent bit-at-a-time oracle. This CRC differs from
the payload checksum stored in boot metadata. These tests cover the flash guard;
console/peer verification policy and scan scheduling have their own suites.

## Coverage and oracles

| Scenario | Actual behavior checked | Independent oracle / model boundary |
| --- | --- | --- |
| Shuffled UF2 plus duplicates | All 1,024 blocks, random order, sector erased once, block programmed once, completion only after every block | NOR operation counters; byte-for-byte fixture comparison; bit-at-a-time CRC versus production lookup-table CRC |
| Invalid UF2 | Truncated block, three bad magic values, payload size, block count, block number, target address, out-of-range LBA | No flash operation, updater claim, or watchdog kick |
| Mixed complete host image | A valid block from a different image among 1,024 otherwise correct blocks | Embedded build CRC must reject; first sector is erased and ROM reset requested |
| Full peer transfer | 65,536 real source reads and receiver writes, retransmitted requests, dropped/duplicate/future responses, 32-bit timer wrap | Exact address progression, unchanged state on rejected responses, final bytes and advertised CRC |
| Queue backpressure | TX refusal at completed page boundaries | No page commit or consumption until next request enqueues; completion emits no out-of-range request |
| Peer corruption | Correctly addressed but modified data word | Independent advertised checksum forces recovery |
| Pause/restart | Clean stalled pull abandons; dirty lost peer pauses; compatible return repairs from zero; stale replies drain; changed checksum restarts | Explicit state/time assertions at timeout minus one and timeout |
| Host/peer/config ownership | Six permutations of host UF2 claim, peer response, and config save; config wipe during drop | Host owns updater after claim; stale peer work cannot touch it; configuration flash is untouched |
| Source/reboot reservation | Bounds/alignment, legacy sentinel word, busy source, rebooted source, late UF2 after reboot decision | No unsafe source read or post-decision flash operation/watchdog extension |
| Metadata formats | Current metadata/version/transferred CRC and packed legacy direct rollback | Direct old images accepted; unaligned legacy peer metadata rejected |
| Config persistence | Actual v8/v9/v10 load, migration, flash save, later reload, manual auto-start disable, corruption fallback | Independent persisted CRC; preserved speed, border and timeout fields; defaults comparison |
| Config SET_VAL/save race | `config_set_during_save_keeps_persisted_crc_coherent`: actual setter runs at a selected snapshot-copy preemption, then a later save/reload | Independent CRC over persisted bytes and a whole-old/whole-new field oracle; later RAM edit remains available; config lock must be released before every flash operation |
| Config semantic repair | Valid-CRC current-format images poison each scalar with an invalid representable value; v8/v9/v10 images poison identity, border relationships and screen indices | Exact repaired field values; unrelated customized OS, speeds, LED, hotkey, timeout and calibration bytes preserved; current-format repair stays in RAM until SAVE; migrations persist after repair |
| Legacy full-width timers | All four persisted timer fields include values from 2^48 through UINT64_MAX across v8/v9/v10 load, unrelated edit, SAVE and reload | Exact original timer bytes survive; only an explicit timer SET replaces the requested field with its new 48-bit wire value |
| Invalid SAVE | Semantically invalid live images, including zero-height borders | No flash operation or RAM mutation; earlier valid settings remain byte-for-byte intact |
| Config publication | SET during reader copy, reader during SET/load/border publication, paired borders and stale index publication | Snapshot is whole old or whole new; border relationship never tears; final index fits the current count |
| Power interruption | Seven partial firmware erase/program cuts; one partial config program | NOR persists, RAM state vanishes; damaged image CRC rejected; torn config loads defaults |
| Watchdog | Core1 hang threshold minus one and threshold, deliberate reboot | Observable kick counts, no wall-clock sleeping |
| Update history | Successful and corrupt full peer/UF2 images; receiving, 25/50/75/100% progress, validating, reboot pending or failure | Exact seven-record timelines in the real ring; failure visible before reset; no duplicate progress-age refresh, invalid-UF2 events, or stale peer takeover |
| Update state changes | Clean abandonment, dirty pause, resume, live-peer stall restart, source version/CRC changes, host takeover | Runtime phase/target/attempt and retained-event assertions against actual updater branches |
| Core checkpoints and observation bounds | Both diagnostic task entries, console disabled, invalid core numbers, duplicate/decreasing/out-of-range progress and paused progress | Exact counters/ages; update begin preserves core counters; rejected progress and phase changes do not refresh progress age; duplicate phase emits no extra row |
| Full-slot verification reads | Actual start/read/finish APIs over all 262,144 slot bytes, including live metadata; mutable updater metadata deliberately differs | Exact 1,024-page contents and independent full-slot CRC; at most one 256-byte copy per call, CRC outside locks |
| Nonblocking verification | Firmware lock busy, flash lock busy, IRQ already disabled, unrelated config lock held, invalid pointers/lengths/ranges, active/dirty update and pending reboot | One attempt per needed lock, no blocking acquisition, no leaked ownership/IRQ state, no copied bytes on refusal |
| Verification generation | Identical programming, previously read bytes, reserved metadata, recovery erase, nonoverlapping settings/staging writes | Old generation refuses subsequent reads/finish; settings/staging writes preserve generation; counter exhaustion remains invalid instead of wrapping |

`mutations.py` compiles and runs isolated production variants. It must kill all
of them at runtime: duplicate UF2 programming, early completion, skipped embedded
CRC, UF2 after reboot reservation, consuming a word on TX failure, premature page
zero commit, missing final page, stale response acceptance, config-save guard
removal, removal of cross-core flash locking, SET_VAL bypassing the config
snapshot lock, missing peer/USB progress observations, missing failure observation
before reset, and suppressing the core 0 checkpoint when the console is disabled.
Verification variants omit program/recovery invalidation, exclude metadata from
invalidation, allow an active update/reboot, ignore generation changes, or wrap
the exhausted counter.
Compile failures do not count.
This demonstrates sensitivity to these concrete errors; it is not a universal
mutation score or proof of correctness.

The generation-exhaustion fixture builds a temporary copy with only the private
startup generation initialized to `UINT64_MAX - 1`. It runs the actual guard and
write hooks through exhaustion and repeated later writes. Production has no
test setter or configurable generation. The wrap mutant uses that same fixture;
normal storage seeds continue to start at zero.

Three configuration variants additionally remove reader snapshot locking,
semantic load repair, and semantic SAVE validation. Their failures must reach
the independent publication or persisted-settings oracles; build failures do
not count.

The configuration format remains 10 in the firmware 0.109 candidate, based on
accepted firmware 0.108. Existing full-width unsigned 64-bit timer values remain
valid on disk and survive migration, unrelated edits and saving. New timer SETs
are limited to 48 bits to match direct and proxied wire capacity. `save_config` copies RAM
under a short config lock and calculates the persisted checksum from that copy;
the lock ends before erase/program. Known config writers use the same lock,
including API SET, screensaver settings, screen borders and desktop screen-index
changes. Runtime consumers take short-lock snapshots to read a complete
configuration, and publication rechecks the current count for screen-index
updates. These tests demonstrate the selected copy schedules above. The updater/flash ownership design and
power-loss recovery guarantees are unchanged.

## Extending scenarios

Add a function in `test_storage.c`, call `fresh("stable_scenario_name")`, construct
an image with `make_image`, and explicitly dispatch `owner(0)` (host device/config)
or `owner(1)` (peer receiver/upgrader). Useful authoring helpers are:

```c
uint32_t crc = make_image(image, 193, 0x51);
heartbeat(193, crc, true);        /* Actual peer heartbeat handler. */
upgrade_tick();                  /* Actual firmware task emits request. */
uart_packet_t request = pop_request();
now += FW_UPDATE_RESPONSE_TIMEOUT_US; /* Advance virtual time, no sleep. */
upgrade_tick();                  /* Deterministic retry. */
source_reply(request, image);    /* Real source read and receiver handler. */
host_block(31, image);           /* Actual UF2 callback claims updater. */
CHECK(global_state.fw.source == FW_UPDATE_SOURCE_DROP);
```

Use fixture bytes or externally defined properties as assertions, not another
copy of the update algorithm. Add intentional fault cases beside successful
scenarios, and add a narrow mutation when it demonstrates a meaningful new oracle.
The fixed six-operation permutations are bounded schedule enumeration, not a
partial-order exploration engine. This suite does not shrink a failing schedule;
the named scenarios and seed already identify a fixed reproducer.

## Fidelity limits and observed recovery gap

The flash model specifies logical erase/program effects and selected torn-write
patterns. It does not establish flash timing, brownout electrical behavior,
RP2040 ROM boot decisions, instruction fetch safety, SDK spinlock instructions,
IRQ preemption, PIO, DMA, USB mass-storage transport fragmentation, USB host
behavior, or macOS behavior. It asserts that software calls lock and interrupt
boundaries in the expected order; it cannot prove the underlying hardware
implements those boundaries correctly. No coverage of discarded functions is
implied by compiling their translation units.

**Arbitrary power loss is not currently fail-safe.** The real updater rewrites the
running slot. If power fails after a valid new first sector but before later
sectors finish, the RAM `image_dirty`/ownership state disappears. There is no
persistent whole-image boot gate in the tested startup/update design. The tests
preserve partial NOR, remove RAM state, and show that the independent whole-image
validation rejects the result while no software recovery has executed. They do
not claim that ROM will always reject the first sector or that the box will boot
into recovery. Powered dirty transfers can pause in RAM and repair when the peer
returns; asynchronous power loss requires a separate bootloader/transactional
update design to guarantee recovery. Config interruption instead loses the new
settings and falls back to defaults through its existing CRC validation.
