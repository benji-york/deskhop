# Bounded clipboard typing (development candidate)

This is a development candidate, version 0.118, now based on
published and hardware-accepted v0.113 commit
`6ac4d6080dfcc39030c7232a3631fd8ec933a39c`. The baseline was integrated from that
commit, not the earlier dirty worktree. Guarded local serial `config`, existing
`bootloader A|B`, confirmed configuration saves and UART IDs 56–61 are retained.
Both DeskHop boards are flashed and image-verified on v0.118. A v0.117 physical clipboard
retry was followed by loss of input and an unreachable B. A B-only power cycle
restored ordinary input; both boards now answer status queries, but the root
cause remains unknown. Version 0.118 adds independent local `link`/`link watch`
diagnostics. After B's `screen` baseline passed, the user confirmed one
`hello`/L3-V paste into blank TextEdit on B worked. Metadata logs show the full
request and typing sequence completed, with both boards healthy and no reset.
The A helper is running again after the log capture. Both QMK writes completed;
the second keyboard half's separate USB startup check remains pending the user's reconnect;
see the [deployment record](testing/clipboard-deployment.md).
Version 0.114 exposed a hardware DMA
counter bug, fixed in v0.115. Version 0.116 added metadata-only clipboard
diagnostics; these confirmed the no-op was the unknown-LED-state gate. Version
0.117 removes that gate. The v0.118 trial verifies one A-to-B paste; reverse
direction and long-term reliability remain unverified on hardware, and the
earlier freeze remains unexplained.

For Benji's setup, the A helper is now installed in
`/Users/benji/Applications/DeskHop Clipboard.app`, with **Launch at login** enabled
and saved connection settings restored successfully. See the deployment record
for installation evidence; login-session restart itself has not been tested.

## Workflow and scope

Either Mac may supply clipboard text. Run the native Swift menu-bar helper on
each Mac whose clipboard you want available, connecting it to that Mac's verified
local serial port/board ID and exact firmware build. A receiving Mac needs no
helper. With helpers connected on both, the feature works in both directions;
with one helper, only that Mac supplies text; with neither, the shortcut is a no-op.

Select the receiving Mac, place its insertion point in the intended application,
and press/release **bare F23**. The keyboard may be connected to either Pico.
The Sofle mapping is **Layer 3 V**; F24 still switches. F23 is swallowed,
including repeats. Typing waits for the original physical key's complete release.
Additional keys/modifiers cancel. A missing, paused, disconnected or expired source
helper cannot supply text; there is no request queue that resumes when it returns.

Each admitted shortcut reads the **current opposite Mac clipboard** once. No
focus-switch capture or clipboard history is kept. Both Picos validate the entire
response before the receiving Pico emits ordinary keys. The receiving Mac's
system clipboard is not read or modified. A built-in laptop keyboard that is not
connected through DeskHop cannot issue the hardware shortcut.

The [QMK patch](clipboard-qmk.patch) is applied in the canonical QMK checkout and
the UF2 was written to both user-selected keyboard halves. The second half's
normal USB startup still needs checking after reconnect. It registers press/release
rather than a timed tap so holding F23 cannot imply release.

The whole supported plain-text value must be 1–1024 bytes. Empty, non-text,
oversize, unsupported, unavailable, incomplete, malformed and expired results
produce no typing. No truncation or fallback to a previous value occurs.
There is one request at a time. Pressing the shortcut again cannot queue work.

## Character and application contract

* The receiving Mac must use the ordinary **U.S. ANSI** input source. Printable ASCII
  (`0x20..0x7e`), LF and TAB are supported; Unicode, CR (including CRLF), NUL,
  DEL and other control characters reject the entire value.
* LF emits Return; TAB emits Tab. These are real keys and may submit a form,
  execute a command, move focus or trigger application behavior. Choose the
  destination accordingly. This is typing, not an application paste operation.
* Caps Lock must be off for the intended letter case. From v0.117, an absent
  keyboard LED report uses the Caps-off startup default, so hosts/remappers that
  omit reports can use the shortcut. A valid local report saying Caps is on
  blocks/cancels typing. The firmware never toggles Caps. If Caps actually is on
  and the host never reports it, letter case cannot be guaranteed. USB session
  changes discard old LED knowledge; focus changes do not. Held physical keys,
  modifiers or mouse buttons prevent admission.
* Dead-key/composition state, the selected macOS input source, and application
  key mappings cannot be queried through keyboard HID. Start in an ordinary
  text field without an active composition. No Option/dead-key sequences are
  generated, and firmware does not guess an alternate layout.
* Karabiner sees the same DeskHop device and HID positions as ordinary typing.
  Rules that remap those keys also affect this feature. Use rules compatible
  with ordinary U.S. text or disable the conflicting rules for this workflow;
  physical acceptance must verify the actual rules on both Macs.

## Native Mac app and helper lifecycle

The shipping helper is the all-Swift **DeskHop Clipboard** menu-bar app for
macOS 13+ (Apple Silicon and Intel). Build with
`make helper-app`; the output is a universal `.app` and ZIP under
`build/clipboard-app/`. `make install-helper-app` builds and installs it in
`~/Applications`, restarting a running copy and preserving settings/login startup.
Python is needed only to build, not run it.
See the [native app guide](../macos/DeskHopClipboard/README.md) for packaging,
installation, signing/notarization and offline fixture preview instructions.

Connection status, Pause/Resume and Quit are available from the menu bar.
**Pause and wait for serial-port release before diagnostics, configuration or
an upgrade.** Resume afterward with the exact new firmware build. At most three
reconnect attempts use the same port, fresh sessions and repeated identity checks.
Wrong identity/protocol/build stops immediately. Clipboard reads occur only for
fresh correlated requests, never at launch/reconnect or on focus switches.
Pausing/restarting a destination-only helper does not cancel the incoming
transfer; stopping the source helper does. Each Mac controls its own sharing.

Launch at login is an explicit option using `SMAppService.mainApp`; it is never
enabled by building or starting the app. Pending macOS approval is shown. The app
must be in Applications before enabling it. There is no root daemon or handwritten
LaunchAgent. Pause persists across launches; explicit Connect enables resumption
on later launches. Only connection settings and that preference are persisted.

The app and its short-lived Swift reader disable core dumps and avoid payload
logs/display. Owned mutable text/frame buffers are cleared. AppKit can materialize
an oversized immutable string before checking size; OS/runtime copies cannot be
securely erased by the helper, and a killed child relies on OS reclamation.
The [native app guide](../macos/DeskHopClipboard/README.md) details these limits.
The old Python CLI remains a development reference, not the shipping helper.

## CDC protocol v2

Normal-mode CDC is unchanged as a USB interface. Exact console command
`clipboard <session>` takes a nonzero, 16-character lowercase hexadecimal helper
session and is admitted on either local board in normal, idle firmware. It echoes the command
and CRLF, then enters binary mode with no prompt. The official helper terminates
the command with LF. Binary mode stays latched, even after malformed input, until
DTR drops. Payload bytes are never interpreted as console commands or echoed.
The strict updater help parser knows the exact new command list and description;
its read-only command method still cannot issue it.

All binary frames are 64 bytes: `DHC2` at 0–3, opcode at 4, zeros at 5–7, then
56 body bytes. Integers are little endian; unused body bytes must be zero.
The request binding is five uint64 values: source boot, destination boot, source helper
session, destination-generated request nonce and focus token. The token stores
selection counter in its upper 32 bits; low bit 0 is selection origin, bit 1 is
destination role, bit 2 is physical trigger owner, and bits 3–31 are destination
USB host generation. Overflow refuses admission. HELLO matches the helper's
preflight local boot and session. Revision DHC2 rejects old DHC1 helpers.

| Opcode | Direction | Body |
| --- | --- | --- |
| 0 HELLO | source → helper | local boot u64, helper session u64 |
| 1 REQUEST | source → helper | 40-byte request binding |
| 2 BEGIN | helper → source | binding, status u8, length u16, text CRC32 u32 |
| 3 DATA | helper → source | nonce u64, exact offset u16, count u8, 1–40 data bytes |
| 4 END | helper → source | nonce u64 |
| 5 PING | helper → source | helper session u64 |

Statuses: 0 supported text, 1 empty, 2 non-text, 3 oversize, 4 unsupported,
5 unavailable. Negative BEGIN has zero length/CRC and no DATA/END. Successful
BEGIN requires 1–1024, exact contiguous chunks and END. IEEE CRC32 covers the
entire text. This is accidental-corruption/replay protection, not authentication
against a malicious source Mac. Neither peer accepts an unsolicited typing payload.

CDC work is limited to 32 received and 64 submitted bytes per console tick.
Partial writes retain their offset; partial/malformed replies cannot type.
The helper sends session PINGs every 250 ms; expiry cancels the request.

## UART, cancellation and USB output

Dedicated UART message 62 leaves the config-only vendor HID and lock-screen-only
generic synthetic keyboard transport unchanged. IDs 56–61 remain reserved for
the integrated v0.113 configuration protocol. Clipboard messages bypass the ordinary UART FIFO and
are selected only when real input/control and firmware-update traffic have none
ready. The normal UART v1 CRC protects each frame.

A trigger on the source board goes directly to the destination for a fresh
challenge nonce. A trigger on the destination first sends PULL to the source;
only a live source helper permits TRIGGER in response. The destination accepts
that response only for its still-pending physical trigger. Only a matching GRANT
lets the source ask its helper. Full controls bind context and CRC, and destination
nonces are never reset by direction changes. Physical release is local at the
destination or a bound RELEASE from the source, according to trigger ownership.
Text uses strictly ordered bounded fragments and a final matching commit. The
destination validates complete length, CRC and every character before typing. Reciprocal
request-bound liveness expires on a missing peer. Exact timing/constants and
the simulated evidence are recorded in the testing document.

Control kinds are 1 trigger, 2 grant, 3 begin, 4 release, 5 cancel, 6 done,
7 alive, 8 commit, 10 pull. Nine fragments carry 54 bytes, six bytes per fragment,
preceded by kind and fragment index. The body is binding (40 bytes), argument
u32, text CRC u32, control CRC u32, two zero bytes. Control CRC covers the first
48 bytes and kind. The argument is the trigger counter/grant echo, BEGIN length,
the PULL trigger counter, or a strictly increasing directional ALIVE serial,
and otherwise zero.
Data kind 9 is a single byte per frame: kind u8, exact offset u16, byte u8,
CRC32 u32 over binding+offset+byte. This explicitly rejects stale chunks even
when their text matches a newer request. At most 1024 data frames are sent;
they take about 89 ms of wire time at the configured 3,686,400 baud, plus
scheduling/backpressure. There is no unbounded queue or transfer.

Consumed CDC FIFO bytes, completed USB receive banks, UART packet temporaries,
consumed clipboard RX frames and completed TX buffers are explicitly erased.
RX erasure first disables the DMA reload channel, then drains both DMA channels'
in-flight writes with the SDK abort barriers. It preserves newly unread slots
and derives the exact remaining transfer count from the stable post-abort
write address (abort clears the hardware counter); the reload channel restores a
full ring count at the next wrap. A pause contains at most 63 byte stores with
no parsing or dispatch. An incomplete frame expires after an absolute 50 ms.
After 50 ms with an unchanged producer and no readable input, idle cleanup
erases 32 free ring bytes per pass. A bounded scan detects new residue even
after an otherwise invisible exact ring lap. Unread bytes retain ownership;
ordinary active traffic does not invoke these idle pauses. This cannot recover
bytes already lost to a UART overrun.
DMA barrier/blackout timing still needs physical measurement at the configured
baud; hardware-free tests prove ownership and work bounds, not silicon latency.

Focus/USB generation changes, source helper or peer expiry, physical disconnect,
maintenance/configuration/update, reboot, suspend, Caps Lock, and conflicting
keyboard/consumer/system/mouse-button input cancel. Incomplete keyboard reports
cannot prove release. Ordinary mouse motion alone is permitted; switching the destination
cancels through the focus generation. Once the destination knows a cancellation, no future
character is submitted. Across two devices, disconnect detection is necessarily
bounded by the liveness lease; a character already accepted by USB cannot be
recalled.

The destination submits one report directly only when the ordinary keyboard queue and its
durable overflow tail are empty and the HID endpoint is ready. State advances
only on successful USB submission. Key-down and key-up each last at least 5 ms:
nominally 100 characters/second, about 10.24 seconds for 1024 bytes. USB
backpressure slows this instead of dropping characters; a total typing deadline
prevents an indefinitely delayed action. Cancellation retains a release obligation
and restores current physical keys when necessary. No whole-message flood enters
the ordinary coalescing keyboard queue.

## Clipboard diagnostics (v0.116)

After an unsuccessful attempt, pause the helper, collect `status` and `history 64`
on the connected board, and resume the helper afterward. History includes both
boards, so a shortcut physically received on A can show a rejection on B.
Each clipboard row contains only fixed `phase` and `reason` labels. The record
contains no text, length, checksum, request binding or HID report. Repeated held
F23, periodic helper/peer keepalives and individual characters add no rows.

A `trigger` row confirms a decoded F23 press. `non_bare` means another key or
modifier was included; `incomplete_report` means the decoded report cannot prove
complete release. In v0.116, `led_unknown` rejects a destination that has not
received a valid LED report in its current USB session. This overstrict gate
caused the observed hardware no-op and is removed in v0.117. `caps_on` still
means a valid local report says Caps is on.
`helper_missing` identifies a source without a current helper lease. `input_held`
means another physical input is down. `binding` groups rejected request/session
validation; the binding values themselves are never logged.

The successful path proceeds through peer `offer`/`grant`, `helper_request`,
`helper_result`, `payload_ready`, `typing` and `done`. A key received on the
selected destination first sends `pull` to the opposite source. `cancelled`
provides a fixed cause, including expired helper/peer lease, deadline, changed
focus/USB session, physical input or invalid protocol/payload. If a retained
window has no `trigger`, inspect the QMK mapping and input path before assuming
a clipboard or Caps Lock problem. Check overwritten counts and boot sessions
before treating absence as evidence.

## Integration and physical acceptance

Published v0.113 commit `6ac4d608` has been integrated, retaining both existing
serial maintenance commands and UART IDs 56–61. Combined tests and exact
ARM/runtime memory accounting must pass on this branch. Each deployment uses a
frozen candidate from the final validated source. The user authorized running
the helper and flashing DeskHop and QMK in this task; see the deployment record
for completed writes and outstanding physical checks. The later request to make
the helper permanent authorized its Applications installation and login
registration, both now verified. The local package is ad-hoc signed
and not notarized.

Future authorized acceptance must cover: both Picos' identities/build/CRCs;
ordinary typing/modifiers/buttons/switching/arrows/zoom assist; F23 held/released;
fixture text at 1 and 1024 bytes, 1025 rejection, all punctuation, LF/TAB;
Caps/layout/Karabiner behavior; cancellation during assembly and each typing
phase; helper stop/restart, USB/UART unplug and reconnect; stale session rejection;
high-traffic UART latency/overrun checks and measured DMA pause duration;
serial release for updater/config and clean helper recovery afterward. First
use a scratch plain-text document and fixture-only clipboard values. Further
physical acceptance should follow the recorded deployment state; do not repeat
completed flashes merely because a physical check remains outstanding.
