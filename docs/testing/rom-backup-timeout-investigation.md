# Stock picotool initial-backup timeout investigation

Date: 2026-09-16. Current status: **investigation parked at Benji's request; the
intermittent ROM hang remains unresolved**. A later separately authorized
ordinary upgrade to v0.108 succeeded without debugger, retry or power cycle,
and both boards passed full verification and user input acceptance. Evidence:
`build/updater/runs/20260916T173830Z-j3n0vqi7/`; see
[the deployment record](verification-contention-v108.md). This success does not
prove a transport fix. No production ROM-transport change was made.

The following is the pre-upgrade diagnostic history, preserved unchanged in
its evidence directories. The latest authorized no-flash batch completed all five exact
firmware reads and normal reboots without a ROM failure. Its separate final
verification passed A but stopped on B's `UNVERIFIED busy` verdict during the
first correct-CRC scan, consistent with the known v0.106 contention bug. That
verification and the overall batch remain failed, not full verification passes.
The intermittent USB hang has not been reproduced by these observers or fixed.
No production transport change,
firmware/settings write or automatic ROM retry was made during that diagnostic
batch. Both stored images were v0.106 at that point. The alternatives below
remain **unproven**, not an adopted workaround.

## Five-cycle failure-only batch: all ROM cycles passed; verifier BUSY on B

The user explicitly authorized up to five no-flash read/reboot cycles, stopping
at the first failure. Evidence:
`build/updater/runs/20260916T171133Z-error-batch-9x5q9iao/`.

- The temporary runner has a fixed five-cycle cap, one deployment lock, fresh
  backend/identity state and a separate evidence directory per cycle. Offline
  tests exercised a failure in each cycle, incomplete results and a failed final
  verification; an independent source review found no blocking issue. No
  debugger/picotool process remained before launch.
- All five cycles completed an exact UID-filtered 262144-byte read of accepted
  v0.106, matching SHA256
  `55c78043fbdcf445a9807caef67ca1feff908af5b74c7cf462406639c0672e2a`,
  followed by a single normal reboot and both-board application-health checks.
  No identity helper was sent before the read; disk-free enumeration, fresh
  physical pins and media checks were retained for every cycle.
- All ten stock processes exited zero. Every error-only capture contains just
  one `armed` record: zero error-wrapper/control calls or pending observations.
  Reads took 0.544–0.555 seconds and reboots 0.031–0.034 seconds under observation;
  complete cycles took 5.136–5.398 seconds. No firmware/settings writes occurred.
- Retained statuses confirm cross-cycle continuity: each A starting session
  equals the previous cycle's ending session, with five expected new sessions;
  final A is `eadf5e163654fec3`. B remained `8d9032bbb7032771` throughout,
  including the intervals between cycles. Both cores advanced and updates
  remained idle. No power cycle was required.
- Only after all five cycles passed, a separate strict `verification/` began.
  Its first correct-CRC query (`68eba065`) returned A `PASS match`, but B
  `UNVERIFIED busy`, with 262144 bytes scanned, generation 0→0, valid/advancing
  cores and unchanged boot metadata `2cd31c9d`. B's computed CRC was unavailable.
  No wrong-CRC or final correct-CRC query followed. Verification stopped after
  2.17535 seconds; its failed record has `firmware_verified=false` and
  `write_started=false`. The parent also remains failed, specifically at
  `verification`, after 28.617851 seconds with `cycles_completed=5`.

This adds five successful low-impact ROM round trips, not a reproduced failure
or a proven transport fix. Debugger startup still affects timing. The B BUSY
verdict matches the separate freshness-contention bug addressed by uninstalled
v0.108; it is not evidence of a USB-ROM failure. No automatic recovery/retry or
further hardware command followed. The authorized batch is exhausted; another
disruptive experiment or firmware deployment requires a new authorization.

## Failure-only diagnostic: ROM round trip passed; independent verifier BUSY

The user approved one lower-impact no-flash run, retained at
`build/updater/runs/20260916T165604Z-error-only-69_ewv92/`.

- The new observer traps only `picoboot_cmd_status`, which the relevant stock
  C++ failure paths call after a failed operation. Only within that function
  does it temporarily observe the nested raw control transfer; it decodes the
  status only for an exact 16-byte result. It injects no USB commands or retries.
- All seven offline LLDB cases passed in
  `build/updater/lldb-status/failure-observer-tests-_873m98k/`: 221 successful
  fake bulk calls generated no USB observation events; valid/zero/short/error
  replies, budget exhaustion and identity mismatch behaved as required. The
  actual stock `help` path also passed error-only arming/admission without USB
  access (`error-stock-help-v1.json`). An earlier synthetic launch timeout before
  observer entry left only an owned fixture process; it was identified and
  terminated before the serial offline suite. No fixture/debugger was left
  running before the live diagnostic.
- Both live stock processes exited zero. Save took 0.545943917 seconds under
  observation, comparable to previous native reads rather than the full trace's
  2.19 seconds. Reboot took 0.033005708 seconds. Each capture contains only its
  `armed` record: zero error-wrapper or nested control calls, no failed/pending
  observation, no watchdog or child kill. The libusb logs retain the transfer
  sequence for independent analysis. Startup is still debugger-mediated, so
  timing is not wholly transparent.
- Exact 262144-byte v0.106 read, expected UID, unchanged disk-free USB pin and
  media checks passed before the sole normal reboot. Application health passed
  with A's new boot `0a3307211fed4c5a` and B's unchanged boot
  `8d9032bbb7032771`; both cores advanced and updates were idle. The no-flash
  experiment record is complete, with `read_verified=true`,
  `application_health_verified=true`, `write_started=false`.
- The separate `verification/` run first passed both boards' correct full-slot
  CRC `68eba065`. In the deliberate wrong-CRC query (`68eba064`), A returned
  `UNVERIFIED reason=busy` after scanning 262144 bytes; B correctly returned
  `FAIL reason=crc_mismatch`, reporting actual CRC `68eba065`. A's generation
  remained 0→0 and both cores were valid/advancing. The parser stopped before
  the final correct-CRC query. Its record remains failed with
  `firmware_verified=false`; the parent script exited 1. No scan, ROM, reboot
  or flash retry followed.

This reproduces the separate v0.106 verification-contention symptom addressed
by the pending v0.108 repair, **not** the intermittent USB-ROM hang. No power
cycle was required and no image/settings were written. Two different observer
runs passing do not prove reliability; a further live attempt or bounded series
needs explicit authorization, fresh per-session identity/pin checks and a stop
on the first failure. Do not relabel the failed verifier as a pass or install
the debugger as a production workaround.

## Corrected debugger diagnostic: complete pass, no failure reproduced

The user separately authorized this one-shot run:
`build/updater/runs/20260916T163257Z-observe-stock-andjfs10/`.

- Preflight, disk-free A entry, UID-filtered first save and an unchanged physical
  USB pin passed. All 262144 firmware bytes matched the accepted v0.106 image
  exactly (SHA256 `55c78043fbdcf445a9807caef67ca1feff908af5b74c7cf462406639c0672e2a`).
- Save captured 221 calls / 443 records with no unmatched calls or errors;
  observed child runtime was 2.190790458 seconds. Reboot captured 11 calls /
  23 records and exited zero in 0.1088625 seconds. Both endpoint status checks
  reported not halted: OUT `0x03`, IN `0x84`. There were no clear-halt calls,
  failed bulk transfers or CMD_STATUS failure replies. No extra probe was sent.
- Save's UID helper consumed tokens 1–6, then its connection reset and requested
  EXCLUSIVE at token 7. The first READ was token 8 (four bytes). Reboot's separate
  process began with READ token 1/four bytes, READ token 2/one byte, then actual
  REBOOT token 3. These are now captured command headers, not only source-path
  inferences. No firmware or other bulk data payload was stored in the observer
  trace; the ordinary backup file contains the verified firmware as intended.
- The no-flash read/reboot experiment finished in 7.027401 seconds, with
  `write_started=false`, `read_verified=true`, and
  `application_health_verified=true`. A returned with boot session
  `04e41e0f09f7fa0f`; B kept `8d9032bbb7032771`. No power cycle was needed.
- Its independent `verification/` run passed correct/wrong/correct full-slot CRC
  tests on both boards in 5.88582 seconds. Correct CRC was `68eba065`; deliberately
  wrong `68eba064` was rejected with `crc_mismatch`; each scan covered 262144
  bytes. Both boards reported v0.106, boot metadata `2cd31c9d`, advancing cores
  and idle updates. That separate record has `firmware_verified=true`,
  `write_started=false`, `reboot_requested=false`.

This is a useful successful baseline, not root-cause evidence from a failure.
The same observer pauses at every bulk call/return and selected control calls.
Its roughly 2.2-second read versus the previous uninstrumented 0.57-second read
demonstrates a meaningful timing change. The absence of HALT remains consistent
with prior successful sessions, but a timing-sensitive bug may have been hidden.
Do not install the debugger as a production updater workaround, add arbitrary
delays, or promote v0.108 based on this v0.106 diagnostic pass. Another live test
requires separate authorization; first consider an error-path-only observer
that does not stop at successful USB transfers.

### Lower-impact observer design (implemented and exercised above)

The unchanged arm64 binary retains `picoboot_cmd_status` and the
`picoboot_cmd.token` data symbol. Its EXCLUSIVE and READ failure paths explicitly
call that status function; the source's success-status path is disabled by
`#if 0`. A failure-only observer can therefore wait without breakpoints on
successful transfers, then capture the caller, host token counter minus one,
and the already-requested status reply. Main-entry setup still changes process
startup timing, so this is lower impact, not perfectly transparent.

Do not trust the status wrapper's zero return alone: its implementation returns
the raw length on a short transfer, meaning a **zero-byte short reply also looks
like return zero**. A valid capture must observe the nested libusb control
return and require **exactly 16 bytes** before decoding the buffer. Activate that
nested observer only after the error-path function is entered. It must still
inject no USB calls, stop on logging/identity errors, and never add a retry.
This covers the two historically observed wrapped EXCLUSIVE/READ failures,
not every direct-C UID-helper or destructor-cleanup failure. Earlier native
sessions also passed; this successful paced run does not justify adding sleeps
or claiming a demonstrated timing cure.

## Debugger setup failure and recovery

The user approved one no-flash diagnostic with unmodified stock picotool under
LLDB, to capture existing control-response buffers. The observer first passed
offline synthetic success, output-budget and identity-failure cases. The live
attempt is retained at
`build/updater/runs/20260916T155413Z-observe-stock-_3__pscp/`.

Preflight and disk-free A ROM entry succeeded. LLDB then stopped picotool at
`main+0`, before its first instruction. The name-based breakpoint gained 15
extra locations as shared libraries loaded. The observer's overstrict check
of **every** location rejected these unrelated symbols and killed its owned
child without resuming it (`continued=false`, `final_state=exited`). There was
no flash read, captured ROM status reply, firmware/settings write, or normal
reboot command. This is a diagnostic-harness failure, **not another observed
PICOBOOT hang**. The failed journal remains failed; no ROM retry followed.

After the user power-cycled both boards,
`build/updater/runs/20260916T155907Z-392krms2/` passed the full read-only
correct/wrong/correct CRC sequence in 5.857084 seconds, with the expected
`68eba065` full-slot CRC and `2cd31c9d` boot metadata on both v0.106 images.
Identities, sampled core progress, idle updates, history and media checks passed.
New boot sessions were A `e6820fc07665ea29` and B `8d9032bbb7032771`.
`write_started=false`, `reboot_requested=false`, `firmware_verified=true`.
The preceding `20260916T155855Z-16hix46u/` invocation stopped at a missing
host tool-path precondition before accessing either Pico; the configured path
was supplied for the successful read-only invocation.

The corrected launcher restricts the entry breakpoint to the actual executable
and disables prologue skipping. The observer validates the exact stopped
frame/module/main-entry address and breakpoint/location in the thread's stop
reason rather than assuming a name breakpoint has no other locations.
Seven mocked guard regressions passed. A real LLDB run of the synthetic fixture
with a deliberately multi-location breakpoint captured all four expected replies;
a stop after main's entry was rejected without continuing.

The actual unchanged stock tool's `help` subcommand also passed exact-main
binding and libusb observer arming, then exited zero with no status calls, as
expected for a non-USB path (`build/updater/lldb-status/stock-help-v3.json`).
Its observer summary intentionally remains `ok=false` / `no status transfers
were observed`: a no-transfer help check must not masquerade as a successful
hardware capture. The earlier `stock-help-v2.json` used unsupported bare
`--help` and returned an argument error without USB access. Another live ROM
session requires separate authorization. This observer remains temporary
diagnostic tooling, not a modified picotool or a production updater dependency.

## Approved first-save experiment

Evidence: `build/updater/runs/20260916T152856Z-uid-first-c26huglr/` in the
canonical checkout. The exact one-shot script and its hash are preserved there.
Offline ordering/failure tests and an independent review preceded execution.

- Fresh A/B identities, accepted v0.106 build/boot checksums, idle update states,
  advancing cores and healthy media passed before one local A ROM-entry request.
- The first stock command was UID/VID/PID/address-filtered `save`, with no
  preceding `info`. It succeeded in **0.572525 seconds** and read exactly
  262144 bytes matching the accepted v0.106 binary (SHA256
  `55c78043fbdcf445a9807caef67ca1feff908af5b74c7cf462406639c0672e2a`).
- Actual opened selector `--bus 2 --address 5`, the full unchanged USB pin,
  valid image metadata and unchanged healthy media all passed before reboot.
- The one normal `reboot -a --bus 2 --address 5` process hit its **10-second
  host deadline**. Its log again shows OUT endpoint `0x03` halt clearing,
  a completed interface reset, and a successful first 32-byte OUT command,
  followed by an IN transfer with no completion before process termination.
  Source inspection identifies that first command as **READ of four bytes
  from ROM address `0x10` for model identification**, not EXCLUSIVE_ACCESS or
  REBOOT. The trace does not contain command payload bytes; this identification
  follows the stock command path described below.
  This is not a recorded libusb timeout result: the outer bounded runner ended
  the process. A's application serial port was absent afterward.
- `result.json` remains failed: `write_started=false`, `read_verified=true`,
  `reboot_requested=true`, `application_health_verified=false`. No further
  USB command, verification loop or recovery reboot was attempted. CDC cleanup
  errors are retained. The user subsequently power-cycled both boards; the
  separate read-only recovery record below does not change this failed result.

This supports investigating USB/ROM state across stock-tool close/reopen/reset
boundaries, not a firmware-data-transfer failure. Removing `info` allowed this
particular first read, but did **not** make a complete session reliable and is
not proved to have caused the read's success. The firmware and saved settings
were not rewritten; v0.108 remains uninstalled.

### Recovery after the manual power cycle

`build/updater/runs/20260916T153320Z-el69g00x/` completed the full read-only
verification sequence in 5.815882 seconds. Both boards passed full-slot CRC
`68eba065`, rejected deliberately wrong CRC `68eba064` with `crc_mismatch`
while still observing `68eba065`, then passed a second correct-CRC scan.
Each scan covered 262144 bytes on each board. Fresh identities, boot metadata,
sampled core progress, idle update states, history and Mac media checks passed.

Both new boot sessions stayed stable during verification: A
`39184f9a1b8db4ca`, B `ceb36c688afcbd5b`. The successful recovery journal has
`firmware_verified=true`, `write_started=false`, `reboot_requested=false`.
It issued no picotool or bootloader command. Both boards are running accepted
v0.106; this is recovery evidence, not a fix for intermittent ROM transport or
hardware acceptance of v0.108.

## Hang-fix investigation after recovery

The goal remains to eliminate the hang, not to turn a timed-out deployment into
a reported success. No hardware operation accompanied this source review.

### What the additional source and trace review establishes

- The actual bundled library and Homebrew library both report libusb
  `1.0.30.12037`; picotool is `2.3.1`, on macOS `26.5.2`. These match the current
  official releases. Swapping to another copy of the same library is not an
  identified fix. Current libusb's explicit Darwin clear-halt path calls
  `ClearPipeStallBothEnds`; a historical cancellation/toggle workaround is
  already included and does not replace that path. See
  [released Darwin implementation](https://github.com/libusb/libusb/blob/v1.0.30/libusb/os/darwin_usb.c#L1886).
- Every complete successful ROM session in the retained libusb-debug evidence
  has no clear-halt call. The failed backup sessions' initial identity commands
  succeed after one OUT-only clear; their subsequent saves time out. Importantly,
  the latest UID-first save also clears OUT immediately after its UID helper's
  final NOT_EXCLUSIVE ACK, **before any process close/reopen**. It then reads the
  full image successfully. Thus neither close/reopen nor one observed halt alone
  establishes a sufficient cause.
- DeskHop's ROM-entry call uses the standard watchdog-reset path. It resets
  both processors/peripherals, parks core 1, rebuilds ROM clocks and resets USB
  state/DPRAM. The disk-free mask only disables mass storage. No evidence supports
  adding application DMA, IRQ or multicore teardown as the hang fix. See
  [ROM entry and clock setup](https://github.com/raspberrypi/pico-bootrom-rp2040/blob/ef22cd8ede5bc007f81d7f2416b48db90f313434/bootrom/bootrom_main.c#L184).
- The ROM's OUT-only HALT is not a normal busy/NAK indication. An unexpected
  buffer completion with no current transfer halts that individual endpoint;
  malformed PICOBOOT commands halt both endpoints. NOT_EXCLUSIVE merely changes
  the virtual-disk task queue. See
  [buffer-completion handling](https://github.com/raspberrypi/pico-bootrom-rp2040/blob/ef22cd8ede5bc007f81d7f2416b48db90f313434/usb_device_tiny/usb_device.c#L545)
  and [exclusive-access handling](https://github.com/raspberrypi/pico-bootrom-rp2040/blob/ef22cd8ede5bc007f81d7f2416b48db90f313434/bootrom/async_task.c#L132).
- Protocol reset deliberately preserves endpoint DATA0/DATA1 progression.
  CLEAR_FEATURE hard-resets it. Stock picotool clears only endpoints reporting
  HALT; an independent IN-side host/device mismatch could survive OUT-only
  recovery. This is a falsifiable hypothesis, not a demonstrated defect in
  macOS, libusb or the ROM. See
  [ROM reset semantics](https://github.com/raspberrypi/pico-bootrom-rp2040/blob/ef22cd8ede5bc007f81d7f2416b48db90f313434/usb_device_tiny/usb_device.c#L321).

### Separate documented silicon candidate: RP2040-E16

The observed chip revision is B2, excluding older E2/E5 errata but not E16.
The manufacturer documents missed USB status events when the system clock is
not sufficiently faster than the USB clock, including unreliable USB-ROM
operation with its equal 48 MHz clocks. The specified workaround is a system
clock at least 10% faster; the RP2040 ROM itself is not fixed. This is a relevant
candidate, not proof that our particular hang is E16. Changing DeskHop's normal
application clock cannot apply the workaround: ROM entry replaces that clock
configuration. See [RP2040 datasheet, E16, page 634](https://datasheets.raspberrypi.org/rp2040/rp2040-datasheet.pdf#page=635).

### Next discriminating observation, before another proposed fix

Capture the existing stock command's 16-byte PICOBOOT_IF_CMD_STATUS reply
**before its exception handler resets the protocol**. Existing debug traces
record the response length, not its contents; the low-level verbose flag is
private/disabled, so an ordinary CLI verbosity switch cannot recover it.

| Observed status for the failed command | Next hypothesis to test |
| --- | --- |
| Token and opcode still zero after a known reset | No correctly framed command recorded since that reset; malformed length/magic or another reset can also explain zero status |
| Matching token/opcode, OK, in progress | Accepted work **or the data transfer/completion** remains outstanding; this does not isolate a stuck worker |
| Matching token/opcode, OK, not in progress | ROM entered the protocol ACK phase, or that ACK already completed; interpretation requires the host's transfer phase |
| Nonzero status | Inspect the actual opcode, validation/data-task failure and halt state; do not classify this as a lost response |

The status flag is not a host-delivery receipt. In
[`_picoboot_ack()`](https://github.com/raspberrypi/pico-bootrom-rp2040/blob/ef22cd8ede5bc007f81d7f2416b48db90f313434/bootrom/usb_boot_device.c#L206),
the ROM clears `bInProgress` **before** arming the final ACK. For EXCLUSIVE this
is an IN ACK after worker completion. For READ this is an OUT ACK after the ROM
believes all IN data completed. A first READ whose host IN transfer timed out,
but whose matching status is OK/not-in-progress, would therefore favor lost or
discarded data completion over a stuck worker. It would not prove a particular
DATA-toggle or silicon-erratum mechanism. ACK completion itself does not change
the recorded status. Non-data task callbacks also do not copy their task result
into status, so OK is not a universal guarantee of operation success.

Correlate the actual outgoing token/opcode, host endpoint/length/result and
status **before reset**, not just opcode or an assumed token of one: `--ser`
runs six UID-helper commands first, and protocol reset does not reset the host
token counter. The temporary observer records those narrow metadata fields
without recording firmware or other bulk data payloads. A full-size synthetic
run covering 221 calls passed with 443 entry/return/arming records in 1.395
seconds, using 247560 bytes. Its explicit limits are 512 records and 512 KiB
for this experiment. Stock `save` performs 64 separate 4-KiB reads of the full
slot; retaining the old 128-record limit would have aborted an otherwise
healthy capture. Error/budget paths still stop rather than truncate or resume
blindly. The expanded observer also armed both real stock libusb symbols and
completed the non-USB `help` path with zero observed calls, as expected
(`build/updater/lldb-status/stock-help-v4.json`). This is offline diagnostic
validation, not evidence that the physical hang is fixed.

Standard endpoint SET_FEATURE can also cause HALT; an OUT-only halt alone does
not prove an unexpected completion IRQ. Explicit endpoint feature requests and
vendor resets can be logged at `libusb_control_transfer`, but Darwin's
`libusb_clear_halt` calls IOKit `ClearPipeStallBothEnds` directly. Absence of a
captured CLEAR_FEATURE is therefore **not** evidence that no kernel-level
endpoint reset occurred; retain the libusb debug trace alongside the capture.

The observer uses a debugger on the unmodified stock executable and reads
existing response buffers and narrow command-header/transfer metadata. It
must not inject commands, change registers, skip code, retry a failed operation
or reset hardware. Validate it on a synthetic process before authorizing any
live run. A captured response is diagnostic evidence, not a fix; a fix still
requires repeated no-flash round trips and an independently verified upgrade.
No physical-BOOTSEL/MSD comparison is permitted: the earlier ROM-storage Mac
panic remains a reason to preserve disk-free enumeration throughout.

## Evidence

All paths below are under
`/Users/benji/Documents/ChatGPT/DeskHop/build/updater/runs/`.

| Run | Operation | Result |
| --- | --- | --- |
| `20260916T144538Z-wt0lug3l/` | Attempted v0.107 deployment, initial firmware backup | Failed before any firmware/settings write or application reboot; picotool exit 157 after 10.126562 s |
| `20260916T011723Z-s6a3wjja/` | Successful v0.106 deployment, initial firmware backup | Exact 262144-byte read in 0.559159 s |
| `bus-address-once-20260916/` | Earlier no-flash diagnostic firmware read | Exact 262144-byte read in 0.556811 s and normal reboot; its initial postcheck was BUSY, with a separate later read-only verification passing |

The deployed tool was the stock bundle at
`/Users/benji/.codex/worktrees/9235/DeskHop/build/tools/picotool-2.3.1/picotool/picotool`.
The failing run retained the CDC descriptor, set child-only `LIBUSB_DEBUG=4`,
verified A's flash UID `E6654854574C3E30`, and used a disk-free ROM session pinned
to the same IORegistry entry/session/location/address and actual opened
`--bus 2 --address 5`. Neither prior workaround guarantees reliability.

The most useful contrast is in these exact logs:

- Failure: `20260916T144538Z-wt0lug3l/012-firmware-before.log`.
  At line 124 picotool clears a halt on OUT endpoint `0x03`. The interface-reset
  control transfer succeeds (line 136), and the first 32-byte bulk command is
  sent successfully (line 149). Its IN ACK times out at line 161. **No firmware
  payload has been read.** The later status query/reset is picotool's built-in
  exception handling, not an updater retry.
- Success: `20260916T011723Z-s6a3wjja/012-firmware-before.log`.
  Startup does not clear a halt. The first 32-byte command and ACK succeed
  (lines 146 and 159); the first 4096-byte payload follows at line 289.
- Identity phase: the normalized transfer sequences in the two runs'
  `009-identity.log` files are identical except that the failing run clears
  OUT halt at line 309, after the initial UID helper and before the info
  command's main work. Both identity commands return success and the same
  expected program/flash UID.

The command journals show only about 41.85 ms between identity process exit and
save process launch in the successful deployment, versus 44.18 ms in the
failure. These samples do not support a simple "increase the inter-process
delay" explanation. Earlier failure logs without libusb debug output cannot
establish whether their endpoint-halt behavior matched.

## Source-backed interpretation

The independently inspected source is official picotool tag `2.3.1`, commit
`2041936441b48a3cc53ae3da9e805229fe8f4e18`, checked out for read-only audit at
`/private/tmp/picotool-audit.EuTuAG`.

- [`save` entry](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/main.cpp#L4940)
  constructs a connection before memory inspection. Its
  [constructor](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/picoboot_connection/picoboot_connection_cxx.h#L30)
  resets the interface, then requests exclusive access. Thus the failed ACK in
  the earlier `20260916T144538Z-wt0lug3l/012-firmware-before.log` is the initial
  **EXCLUSIVE_ACCESS**, not the first READ or EXIT_XIP.
- The subsequent experiment's
  [`reboot` entry](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/main.cpp#L10076)
  explicitly requests a **nonexclusive** connection, which still resets the
  interface. Its
  [memory-access constructor](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/main.cpp#L2386)
  calls
  [`determine_model`](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/main.cpp#L2320),
  reading four bytes at `BOOTROM_MAGIC_ADDR` (`0x10`) before the later reboot
  command can execute. `get_model()` itself only returns the cached model.
  In `20260916T152856Z-uid-first-c26huglr/016-reboot-application.log`, halt
  clearing occurs at line 124, reset completes at line 136, the first 32-byte
  OUT command completes at line 149, and the IN transfer starts at lines
  151–156 without completing. This is the first READ's **data phase**, not a
  reboot ACK or an exclusive-access ACK. No PC_REBOOT command was reached.
- [Reset handling](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/picoboot_connection/picoboot_connection.c#L255)
  clears reported endpoint halts before the vendor interface reset. The
  [command implementation](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/picoboot_connection/picoboot_connection.c#L300)
  uses a ten-second timeout for this ACK, matching the trace.
- [`--ser` on RP2040](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/picoboot_connection/picoboot_connection.c#L201)
  runs a flash-UID helper before command dispatch. The
  [helper](https://github.com/raspberrypi/picotool/blob/2041936441b48a3cc53ae3da9e805229fe8f4e18/picoboot_connection/picoboot_connection.c#L692)
  toggles exclusivity, exits XIP, uploads and executes a RAM stub, then reads
  the UID. Device-info output invokes it again.

The strongest current hypothesis is a ROM/libusb endpoint or command-state
problem across UID-helper/reset/close/reopen transitions. The newly observed
OUT-halt correlation supports that hypothesis but does not identify the
underlying cause. Info's overall RP2040 connection is nonexclusive; its helper
changes exclusivity independently. The failed save must acquire it immediately.
No stock CLI flag skips that reset/acquisition, and stock picotool offers no
general single-process command batch. Smaller backup chunks, longer bulk
timeouts, or candidate firmware changes do not address the observed boundary.

## Two minimal stock-only experiments

Both require a fresh, explicitly authorized ROM session. Neither is a proven
fix, and neither should be followed automatically by a firmware write.

1. **UID-filtered first backup.** Make `save -r 0x10000000 0x10040000
   <unique-backup.bin> --ser <expected-flash-UID> --vid 11914 --pid 3
   --address <freshly-observed-address>` the first stock operation. Do not guess
   the bus. Stock picotool checks the UID before saving: the helper must succeed
   and its numeric UID must equal the requested value before the save can use
   that handle. This removes the separate info process, its second
   UID helper, and the first close/reopen transition. The constructor still
   resets and requests exclusivity after one helper, so failure remains
   possible. Require the successful exit, unique actual opened selector,
   unchanged pin, and exact expected current image before accepting the read.
   The exact image match is additional integrity evidence, not a substitute for
   UID matching: both physical boards can have identical firmware. Journal the
   identity method as `UID-filtered save`, not as a printed info/UID check.

2. **Single-UID filtered identity.** Replace `info -a --ser ...` with stock
   `info -b -d --vid 11914 --pid 3 --address <observed-address>` and, if safely
   known, `--bus <observed-bus>`. Require exactly one disk-free RP2040 ROM
   candidate globally, one actual opened bus/address, one expected flash UID,
   expected DeskHop program identity, and an unchanged pin. Address alone is
   not globally unique across buses: ambiguity must abort. This removes the
   pre-dispatch UID helper and unnecessary `-a` probes while retaining the
   explicit identity gate before backup. A later pinned save still crosses
   close/reset/exclusive, so this also may fail.

Recommendation: if further hardware diagnosis is desired, authorize **one
bounded no-flash experiment**, preferably option 1 because it removes the
first process handoff entirely. Approval must explicitly include ROM entry and
the planned normal application reboot after a successful exact read. On any
failure, preserve evidence, stop, and seek recovery direction; do not retry
save, reboot, or flash automatically. Option 2 is an alternative experiment,
not an automatic fallback after option 1 fails.

An independent source/safety review agreed that the stock UID-filtered save is
an actual identity gate under those constraints, without needing a printed UID
from a separate info command. It also requires an unchanged healthy media
baseline immediately before admitting the normal reboot, whose intent must
be journaled first. Retain CDC through that reboot and record cleanup errors.
This is a **no-flash** experiment: stock UID identification still uploads and
executes a small RAM helper, but it does not write firmware or saved settings.

## Safety gates retained

- Verify both application identities, versions, health and idle update state
  before ROM entry, and hold the single deployment lock.
- Require disk-free ROM, no unexpected mounted Pico disk, exactly one eligible
  ROM candidate, and the expected flash UID before any future write.
- Capture and compare registry ID, session ID, location, USB address, VID/PID
  around identity and each operation. Accept only one actual opened selector;
  any re-enumeration, mismatch, ambiguity or lost evidence aborts the session.
- Keep CDC lifetime management and debug logs; no arbitrary sleeps, force
  options, custom picotool, weakened identity checks or automatic retries.
- A future real deployment must still complete firmware and settings backups,
  load/verification, exact firmware readback, settings comparison and both-board
  live verification. A successful diagnostic alone does not satisfy those
  gates or constitute hardware acceptance of a new firmware candidate.
