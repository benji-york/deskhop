# Stock picotool initial-backup timeout investigation

Date: 2026-09-16. Status: diagnosis only; proposed experiments are **unproven**.
No transport implementation change or additional hardware operation was made
for this investigation.

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
  resets the interface, then requests exclusive access. Thus the failed ACK is
  the initial **EXCLUSIVE_ACCESS**, not the first READ or EXIT_XIP.
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
