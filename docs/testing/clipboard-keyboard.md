# Clipboard typing: hardware-free validation

Development branch: `codex/clipboard-keyboard`, based on published v0.113 `6ac4d6080dfcc39030c7232a3631fd8ec933a39c`.
Current version: 0.118, integrated with v0.113 confirmed configuration and
local serial configuration changes. The original v0.115 validation is retained
below; subsequent sections record v0.116–118. Hardware deployment is tracked separately
in [the deployment record](clipboard-deployment.md). All clipboard inputs were fixtures.
These hardware-free tests access no real clipboard, serial device or flash.

## Reproduction

```sh
python3 tests/run.py deep
python3 tests/run.py arm
```

The deep runner builds the Swift reader on macOS and invokes only its `--fixture`
mode. It runs Python helper tests on other hosts without invoking AppKit. The
isolated clipboard paired runner is:

```sh
python3 tests/sim/build.py
python3 tests/sim/test_clipboard.py --library build/tests/sim/node.so
```

## Symmetric transfer coverage

Both destination roles and both physical keyboard attachment points are tested.
The matrix covers live helpers on both sides, missing/paused source helpers,
no deferred read after a helper starts later, real trigger hold/release,
destination helper close/restart, source helper loss, Caps/focus cancellation,
1024-byte pacing and successive transfers after reversing focus. A regression
checks that releasing a cancelled destination-local shortcut cannot release a
later source-local shortcut. Swift and Python identity tests accept either local
board while retaining exact UID/build checks; the CDC adapter accepts either role.

## Coverage

* Portable production state machine under ASan/UBSan: all 256 input byte values,
  independent U.S. key-position decoding for all 97 supported bytes, standard
  CRC32 vector, 0/1024/1025 boundaries, every negative status, duplicate/gapped/
  overflowing/incomplete chunks, invalid controls, CRC errors, timeout, no
  advancement under failed USB submission, actual-release gate and cancellation
  at every down/up boundary. All 1024 text bytes are checked zero after use.
* Paired production firmware: fresh request/current fixture, each binding field,
  held trigger, a different interface's release, repeated/second-source F23,
  a consumed-hotkey hold, partial HID snapshots, sparse F23 position, stale
  completed triggers, UART corruption/drop/truncation/reordering/duplication,
  full 1024-byte assembly, paced output, failed/stalled USB, complete ordinary
  UART queue priority, and no resumption after a long stall.
* Cancellation is exercised both before release and during typing for physical
  keyboard and mouse-button input on either side, focus, Caps, suspend, host
  disconnect, physical keyboard disconnect, configuration, maintenance, firmware
  update, reboot, helper close/restart, and partial keyboard reports. Physical
  Ctrl+D remains held after cancellation and subsequently releases normally.
* A regression first reproduced the stale synthetic-release race: B accepted
  a synthetic key-down, stalled, focus changed to A, and B-local physical Ctrl+D
  became held. On B recovery the cancellation must send all-up, never that
  A-focused physical report. Physical-state publication also uses the same
  short firmware lock as cancellation reconciliation.
* In the original v0.115 coverage, fresh B host LED knowledge is required after initial mount, unplug/reconnect
  and mount-only bus reset. A's LED reports and malformed B reports (wrong
  instance, ID, type, length or null data) cannot establish it. Ordinary focus
  changes retain valid knowledge, and a valid Caps-on report cancels typing.
  The v0.117 section records the later Caps-off default when a report is absent.
* Real TinyUSB virtual controller: bounded binary reads/writes, exact entry
  grammar on either role, HELLO/request frames, split frames, negative outcomes,
  wrong session/padding/magic/opcode, no payload echo, no maintenance command
  parsing after malformed binary input, disconnect erasure and fresh re-entry.
  Existing console/backpressure/USB reset/maintenance tests remain in the suite.
* USB transport erasure: actual CDC FIFO implementation with wrap, bounded
  reads, callback/rearm ownership, flush/reset/deinit; RP2040 receive-bank and
  abandoned-bank reset erasure, byte-access disassembly check, and mutation
  tests that fail when erasure is removed.
* UART transport erasure under ASan/UBSan: consumed clipboard frames and parser
  temporaries, producer wrap into old slots, in-flight RX and reload bus writes,
  hardware-cleared abort counter, zero-count restart, exact write-address preservation, at most 63 erased bytes
  per pause, at most 32 junk bytes scanned per pass, absolute 50 ms partial-frame
  expiry, delimiter recovery and no DMA pause for ordinary active frames. Idle
  cleanup uses 32-byte slices after 50 ms, preserves newly arrived unread input,
  stops pausing once complete, and detects residue after an exact producer lap.
* Helper: bounded provider output including a million-byte adversarial fixture,
  stalled/exited reader, all result types, current reads per fresh request,
  session/boot/nonce replay checks, exact framing/padding, mutable-buffer wiping,
  no transcript/event recording, exclusive descriptor release, safe cleanup on
  disconnect, Ctrl-C/SIGTERM, fresh-session reconnect and native Swift fixtures.

## Pacing and limits

Portable 1024-byte fixture: 2048 accepted reports over 10.240 simulated seconds,
five milliseconds between each down/up transition. Paired firmware measured
10.230 seconds from first to last key-down, with at least ten milliseconds
between key-downs; a 300 ms endpoint stall delayed output without dropping text.
These are deterministic simulation results, not measured macOS throughput.

On the source the helper response must arrive within three seconds of
trigger admission. On the destination complete assembly must arrive within
three seconds of its admission. A destination-local pending PULL expires
after three seconds without a matching handshake. These use separate local clocks; initial UART
admission delay is not included in B's deadline. Helper reading has a two-second
native timeout and a 2.5-second response budget from receipt of REQUEST.
Typing has a 15-second bound. Helper PINGs every 250 ms maintain a three-second
lease; reciprocal request-bound peer ALIVEs every 250 ms expire after 750 ms.
Local cancellation is immediate at the next guarded submission; a disconnected
remote device is detected within its lease, and already delivered keys cannot
be undone. Delayed or incomplete responses never schedule a future paste.

## Memory accounting

No clipboard queue or heap allocation is added on either Pico. The text state
is 1064 bytes including its single 1024-byte payload. The runtime has a compile
assertion limiting its complete state to 1536 bytes; the CDC adapter has one
64-byte RX frame, one 64-byte TX frame and request metadata. All input/control
traffic retains priority over clipboard UART and USB work.

ARM-target measurement of all 13 SDK queues gives **11,741 bytes** of
payload allocation including every SDK sentinel slot. Reserving 32 bytes per
allocation gives 12,157 bytes including conservative allocator overhead. The
linker now requires at least **16 KiB** from `__end__` to `__StackLimit` (12 KiB
queue/allocator allowance plus 4 KiB headroom); stacks occupy separate scratch
RAM. The linker RAM figure excludes dynamic queues and its non-allocated 2 KiB
minimum-heap marker; it is not post-allocation free RAM. The v0.113 configuration
queue adds nine 16-byte slots (including its SDK sentinel); its ARM disassembly
confirms the element size. There are 13 queue allocations in the integrated source.

The integrated ARM build uses **245,216 bytes (93.54%)** of main RAM.
`__end__ = 0x2003bde0` and `__StackLimit = 0x20040000` leave **16,928 bytes**
for heap allocations, or **4,771 bytes** after the conservative queue allowance.
Size optimization applies to clipboard, console, configuration confirmation,
maintenance and peer diagnostic control modules; ordinary HID paths retain
their existing optimization flags.
The clipboard runtime occupies 1,336 bytes and its CDC adapter 200 bytes.
Each core reserves a 2 KiB stack in separate scratch RAM. This is a static
accounting result, not a measured stack high-water mark. The integrated image
passes the unchanged 16 KiB reserve assertion.

## Native macOS app validation

The universal arm64/x86_64 app and bundled Swift reader build successfully.
Ad-hoc signatures pass `codesign --verify --deep --strict`; bundle metadata,
both architectures and six bounded reader fixtures pass packaging checks.
Sixteen Swift fixture groups cover protocol/identity, owned-buffer clearing,
absolute partial-frame expiry (including a late final chunk), subprocess
bounds, actual socket I/O/backpressure, fresh-session Pause/Resume, bounded
reconnect and explicit-only fake login registration. No XCTest installation
or third-party packages are needed.

The earlier one-way app was launched with `--ui-fixture` and then closed. The native UI
inspection service disconnected, so visual layout and menu interaction review
remain outstanding. Real login registration, OS permission behavior and
physical serial/clipboard acceptance were not exercised. The archive is an
ad-hoc development build, not a Developer ID/notarized distribution.

## Validation evidence and remaining acceptance

The v0.115 correction passed all **73 deep runner steps**, including 144 updater
tests, 32 helper fixtures, 16 Swift fixture groups, **98 paired clipboard
scenarios**, ASan/UBSan boundaries, USB transactions, confirmed configuration
transport, scheduling permutations, generated inputs and historical comparisons.
All 8 USB, 29 storage and 23 paired mutations were detected. The runner recorded
462.749 seconds of subprocess time. The ARM build and unchanged 16 KiB reserve
passed; universal app packaging/signature/bundle/native-reader checks passed.
`git diff --check` is clean.

The corrected hardware DMA model first reproduced the v0.114 abort-counter bug;
the fix passed the same ownership and resume-position fixtures. The earlier
one-way 73-step deep run and symmetric 61-step fast run predated that model
correction and did not establish correct physical DMA behavior.

Frozen v0.115 manifest: `build/releases/deskhop-v0.115-p8w9g5g1/manifest.json`.
Its fresh validation evidence is in `build/updater/prepare/prepare-100f65wn/`,
with step results in `build/tests/results-deep.json` and app packaging in
`build/tests/clipboard-app-v115.log`. Reproduction/fix logs and live hardware
outcomes are linked in [the deployment record](clipboard-deployment.md).

Hardware acceptance remains outstanding for actual USB/serial enumeration,
input layout, dead-key/composition assumptions, Karabiner mappings, key pacing,
physical disconnect timing, and updater/helper handoff. The QMK patch has been built in the canonical checkout and written to both
user-selected halves; the second half's normal startup and physical shortcut
acceptance remain pending. Follow the
[integration and physical acceptance plan](../clipboard-keyboard.md).


## v0.116 diagnostic follow-up

The no-op investigation adds fixed clipboard event labels and a per-interface
F23 edge mask used only for diagnostics. All 73 deep runner steps pass, including
146 updater tests and 113 paired clipboard scenarios. New tests cover Caps state,
missing helpers in both directions, non-bare/incomplete input, held-repeat
suppression, helper result categories, deadline cancellation, and identical
metadata stage counts for 4-byte and 1024-byte fixture text. The real USB-console
formatter tests every phase/reason label; host parsing rejects additional fields.
ARM RAM use is 245,224 bytes with 16,920 bytes before runtime queue allocations;
the 16 KiB linker reserve is unchanged. Both boards passed the guarded v0.116
hardware update and fresh full-slot scans. See the deployment record for exact
artifacts, identities and evidence. The physical no-op still requires the user's
new diagnostic attempt; passing simulations are not a claim that it is fixed.


## v0.117 confirmed LED gate fix

The v0.116 hardware history identified `led_unknown` as the rejection. A new
positive regression fails on the v0.116 library, reproducing the no-request
outcome. Version 0.117 accepts a destination without an LED report using the
Caps-off default, but still blocks/cancels on a locally reported Caps-on state.
New coverage includes both focus/trigger directions with no LED reports,
reported Caps retention, invalid reports, and removal of stale Caps knowledge
on reconnect/bus reset. All 116 clipboard scenarios and 61 fast release checks
pass; ARM reserve is unchanged. Both physical boards passed the guarded update
and fresh CRC scans. Physical typing acceptance remains pending the user's
retry. The explicit limit is that unreported actual Caps-on state can change
case. Exact release and hardware evidence are in the deployment record.

## v0.118 diagnostics and physical A-to-B success

The local read-only UART/DMA diagnostics passed all 74 deep runner checks,
including 150 updater contracts and 116 paired clipboard scenarios. ARM build
and the unchanged 16 KiB reserve passed. Both boards passed the guarded update
and fresh full-slot CRC scans; see the deployment record for exact artifacts.

After B's independent `screen` baseline, the user confirmed the controlled
`hello`/L3-V paste from A into blank TextEdit on B worked. Metadata history
records the full request, helper response, key release, typing and completion.
Both boards remained on their existing boots with fresh cores and no retained
errors or overwritten history. Evidence:
`build/tests/clipboard-v118-trial-20260917T155400Z/`.
The helper is restored on A after capture. This verifies one A-to-B paste; it
does not establish reverse-direction hardware acceptance or sustained
reliability, and the earlier v0.117 input-loss cause remains unknown.
