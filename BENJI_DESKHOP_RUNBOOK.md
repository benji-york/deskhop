# Benji's DeskHop setup and maintainer runbook

This is the durable record of the DeskHop work done for Benji's two-Mac setup.
It describes the physical installation, the behavior added to the DeskHop fork,
the coupled Sofle/QMK firmware, how to build and deploy both, and the failure
modes already diagnosed. It is intentionally more specific than the upstream
README.

Snapshot: 2026-09-13

## Source of truth

| Component | Local repository | Remote | Source / verified state |
| --- | --- | --- | --- |
| DeskHop | `/Users/benji/Documents/ChatGPT/DeskHop` | `git@github.com:benji-york/deskhop.git` | `main`, firmware v0.92 with upstream `ce8abb6` merged; seven native suites and ARM build pass. Hardware remains on verified v0.91. |
| Sofle/QMK | `/Users/benji/qmk_firmware` | `git@github.com:benji-york/qmk_firmware.git` | `master` at `469f5dc815` (`Map DeskHop reboot to Layer 3 Q`) |
| Physical carrier project | n/a | [jfedor2/screen-hopper](https://github.com/jfedor2/screen-hopper) | The installed two-Pico board shown in the setup photo |

Both remotes use SSH for fetch and push. The numbered QMK checkouts under
`/Users/benji/projects/qmk_firmware/` are superseded; do not build from them.
The full upstream merge was adopted on `main` on 2026-09-13. Alternative selective
and replay branches remain for comparison; they are not deployment targets.
The previous DeskHop clone at
`/Users/benji/Documents/Codex/2026-08-13/i/work/deskhop-benji` is retained at the
v0.91 state; build current firmware from the project path in the table above.

Known-good archived binaries are:

- `/Users/benji/Documents/Codex/2026-08-13/i/outputs/deskhop-v0.90-triple-q-reboot.uf2`
- `/Users/benji/Documents/Codex/2026-08-13/i/outputs/sofle-rev1-benji-deskhop-triple-q-reboot-rp2040.uf2`

The hardware-verified auto-start build is:

- `/Users/benji/Documents/Codex/2026-08-13/i/outputs/deskhop-v0.91-auto-start-jitter.uf2`

The current, not-yet-flashed v0.92 build is:

- `/Users/benji/Documents/ChatGPT/DeskHop/build/deskhop.uf2`

The current working tree builds v0.92. To reproduce hardware-verified v0.91,
use commit `c1e9420` or its archived binary. For the older hardware-tested v0.90
state, use commit `6d1cd12` or its archived binary.

## What the box is doing

The installed hardware comes from the `screen-hopper` project; the firmware in
this repository is Benji's fork of `hrvach/deskhop`. DeskHop is a hardware USB
KVM made from two Raspberry Pi Picos. Each Pico is a USB device to one computer
and a PIO-USB host for attached input devices. The Picos exchange fixed-size,
checksummed UART messages across the board's digital isolator. The computer sees
a `DeskHop Switch` composite device, not the original keyboard or trackball
directly.

Normal pointer output is absolute HID. DeskHop accumulates the relative deltas
from a physical mouse/trackball into a logical `pointer_x`/`pointer_y`, emits the
corresponding absolute position, and swaps outputs when that logical position
crosses the configured shared edge. Keyboard focus follows pointer focus.

This is deliberately not a software KVM:

- Nothing needs to run on either computer for normal operation.
- Clipboard and arbitrary data are never shared between computers.
- The host-facing sides are galvanically isolated.
- The only normal host-to-peripheral information DeskHop accepts is the standard
  one-byte keyboard LED report.
- Both computers normally need to power their respective Pico. If one side is
  unpowered, cross-Pico input routing, coordinated reboot, and firmware
  propagation cannot work.

Both Picos select output A after a reboot.

## Physical topology

In the installed screen-hopper/DeskHop board, viewed in the same orientation as
the setup photo:

```text
               host USB                 host USB
             computer B               computer A
                  |                         |
          +-------+-------------------------+-------+
          |   Pico B       UART       Pico A        |
          |   (left)    <isolator>    (right)       |
          |      |                         |         |
          |  mouse-marked             keyboard-     |
          |  USB-A input              marked USB-A  |
          +------+--------------------------+--------+
                 |                          |
              trackball                  Sofle
```

The working arrangement is keyboard/Sofle on board A and trackball on board B.
There is one keyboard-marked input port and one mouse-marked input port; A and B
name the two output/Pico roles, not two keyboard sockets. The firmware currently
has `ENFORCE_PORTS=0`, so it can parse HID devices on either side, including the
Sofle's composite keyboard-and-mouse interfaces, but the bootloader shortcuts
and this runbook assume the physical arrangement above.

The monitors are logically arranged B on the left and A on the right. Moving off
B's right edge should select A; moving off A's left edge should select B.

Board role is auto-probed from the isolator wiring at boot, rather than compiled
into separate A and B images. The same DeskHop UF2 is used for both Picos.

## Daily-use controls

### Native DeskHop hotkeys

These are the chords recognized by the current DeskHop firmware. Except for slow
mouse, recognized chords are consumed and should not reach the computer.

| Keyboard input | Action | Notes |
| --- | --- | --- |
| bare `F24` | Switch A/B | Preferred explicit switch command. |
| Left Control + Caps Lock | Switch A/B | Legacy compatibility only; do not use in new keyboard mappings. |
| Right Control + Right Option/Alt | Toggle slow mouse | Deliberately also passes through to the OS. Speed is approximately quartered. |
| Right Control + K | Toggle edge-switch lock | Also prevents explicit F24 switching. |
| Right Control + L | Lock both computers | Uses each output's configured OS; both must be set to macOS in this installation. |
| Left Control + Right Shift + G | Toggle gaming mode | Relative mouse; pointer-edge switching disabled. F24 still works. |
| Left Control + Right Shift + Z | Reset zoom assist | Clears the active output's inference and relearns wheel direction. |
| Left Control + Right Shift + S | Enable Pong | Changes only the currently selected output's runtime keep-awake mode. |
| Left Control + Right Shift + J | Enable Jitter | Changes only the currently selected output's runtime keep-awake mode. |
| Left Control + Right Shift + X | Disable Pong/Jitter | Changes only the currently selected output's runtime mode. |
| Right Shift + F12 + D | Erase saved configuration | Destructive; the Sofle mapping adds triple-tap protection. |
| Right Shift + F12 + Y | Save vertical edge calibration | Use at the top/bottom alignment point described below. |
| Left Control + Right Shift + C + O | Enter/exit configuration mode | Reboots the local/keyboard-side Pico. |
| Left Shift + Right Shift + A | Put board A in the ROM UF2 bootloader | Actual DeskHop matcher; F12 is not required. |
| Left Shift + Right Shift + B | Put board B in the ROM UF2 bootloader | Actual DeskHop matcher; F12 is not required. |
| exact Left Control + Right Shift + Q, three taps | Reboot both Picos | Three complete press/release taps from one keyboard within one second. |

Ordinary DeskHop hotkey matching requires the listed keys and modifiers but can
also match a report containing extras. The coordinated reboot recognizer is the
exception: it requires exactly Left Control, Right Shift, and Q.

### Sofle Layer 3 mappings

Hold both tri-layer thumb keys (`TL_UPPR` + `TL_LOWR`), then tap the physical key:

| Layer 3 key | DeskHop action | HID report emitted by QMK |
| --- | --- | --- |
| `A` | Boot DeskHop board A | Left Shift + Right Shift + F12 + A |
| `B` | Boot DeskHop board B | Left Shift + Right Shift + F12 + B |
| `C` | Enter/exit config mode | Left Control + Right Shift + C + O |
| `D`, `D`, `D` | Erase DeskHop config | Right Shift + F12 + D, emitted only after QMK accepts the third tap |
| `G` | Toggle gaming mode | Left Control + Right Shift + G |
| `J` | Enable jitter on selected output | Left Control + Right Shift + J |
| `L` | Lock both computers | Right Control + L |
| `Q`, `Q`, `Q` | Reboot both Picos | Three separate Left Control + Right Shift + Q taps |
| `S` | Switch computers | bare F24 |
| `X` | Disable Pong/Jitter on selected output | Left Control + Right Shift + X |
| `Y` | Save cursor-height calibration | Right Shift + F12 + Y |

Important distinctions:

- A/B boot the DeskHop Picos, not the two Sofle halves.
- QMK includes F12 in the A/B report even though the current DeskHop
  implementation only requires both Shifts plus A/B. The extra key is harmless
  because the ordinary matcher permits extras.
- D is guarded in QMK: three uninterrupted taps, each no more than one second
  after the previous one. Another key cancels it, and the first two taps send
  nothing to DeskHop.
- Q is guarded in DeskHop, not QMK. Every Layer 3 Q tap emits one full chord;
  DeskHop counts three completed taps within one second.
- The keyboard right-click key is `LSFT_T(MS_BTN2)`: tap for button 2, hold for
  Left Shift. That button event travels through the Sofle's composite mouse
  interface and exercises the remote non-motion fix described below.

QMK's `tap_deskhop_hotkey()` constructs the whole chord in a single HID press
report, holds it for 20 ms, and sends a single release report. It preserves weak
modifiers. This atomic shape is intentional: incrementally sending modifiers and
keys caused macOS/Karabiner to observe partial combinations or leave modifiers
latched. Avoid holding unrelated physical keys while invoking maintenance
commands, because physical state can still accompany the synthesized report.

## Output switching and stuck-key safety

Bare F24 replaced Left Control + Caps Lock because an escaped F24 is inert,
whereas an escaped modifier or Caps Lock event can poison Karabiner's virtual HID
state. The old Ctrl+Caps chord remains accepted only for transition compatibility.

On every output change, the initiating Pico updates its state, restores LEDs,
blocking-queues `OUTPUT_SELECT_MSG`, and clears keyboard state on its locally
attached host. The receiving Pico adopts the selection without echoing it,
clears its own locally attached host if connected, and then restores its LEDs.
The paired handling therefore clears both hosts.

The output-selection UART message uses a blocking queue so a momentarily full
queue cannot leave the two Picos routing to different computers. The critical
all-up report waits up to 100 ms for a keyboard queue slot. If it still cannot be
enqueued—typically because a stalled endpoint has filled the queue—the board
requests a watchdog reboot instead of silently dropping the release and risking
a stuck key.

If Caps-like behavior remains after DeskHop is physically unplugged and clears
only after restarting Karabiner, the bad state is in the Mac's virtual input
stack. Restarting Karabiner is the recovery. F24 switching, atomic QMK chords,
and critical all-up reports are the firmware-side prevention.

Because the Macs see DeskHop's identity (`1209:c000`, product `DeskHop Switch`)
rather than the Sofle's identity, Karabiner rules with device filters must match
DeskHop or be device-agnostic. Configuration mode enumerates under a different
VID/PID (`2e8a:107c`).

## Pointer correctness and the keyboard right-click fix

There are three related but distinct protections.

### Composite-device zero-motion filtering

QMK exposes a mouse interface even when a keyboard action contains no mouse
change. DeskHop drops reports with zero X/Y, zero wheel/pan, and unchanged button
state. Without that filter, ordinary keyboard activity could emit spurious
absolute pointer reports.

### Cross-Pico cursor synchronization (PR #357)

Each Pico stores coordinates, but there is only one visible cursor. Previously,
movement handled locally by the active Pico never updated the other Pico. Using a
pointing interface on the other side could therefore restore its stale position,
often the corner used to park the cursor while switching, and immediately jump
back across the edge.

The fork sends `POINTER_SYNC_MSG` after local active-side movement and after an
output switch. The peer adopts that authoritative X/Y. This makes alternating
between the B-side trackball and the A-side Sofle mouse interface stable.

### Position-neutral remote buttons and wheels

A keyboard-generated right-click contains a changed button but no real position.
When it originates on the inactive Pico, forwarding a full absolute report would
attach that Pico's cached/bogus coordinates and jump the pointer to a corner. The
fork instead sends `MOUSE_NONMOTION_MSG`, containing only buttons, wheel, pan, and
mouse mode. The active Pico attaches its current coordinates in absolute mode or
zero deltas in relative mode.

This is the fix for: physical trackball right-click works everywhere, but Sofle
right-click jumps to the upper-right or works only on A. If that symptom returns,
the first suspicion should be that the two Picos are running different firmware;
`MOUSE_NONMOTION_MSG` must be understood by both.

`Force Mouse Boot Mode` only simplifies parsing a physical mouse descriptor. It
does not fix stale coordinates on a QMK-generated button event.

## Mouse modes and macOS zoom assist

### Normal and gaming modes

Normal mode uses accumulated absolute coordinates so DeskHop can know when the
pointer crosses an output edge. Gaming mode uses an ordinary relative mouse and
disables pointer-edge switching. It fixed macOS Accessibility Zoom immediately,
but manually toggling it for every zoom session was undesirable.

Manual gaming mode remains independent from zoom assist. F24 is available in
both modes even when pointer-edge switching is disabled.

### Why fast macOS zoom used to stutter

With Accessibility Zoom configured to move the zoomed view when the pointer
reaches its edge, slow motion appeared to work, but fast motion stuttered and the
viewport stopped once the logical cursor reached the physical display edge.
DeskHop was repeatedly clamping/reasserting an absolute coordinate at that edge;
macOS needs continuing relative deltas to pan a zoomed viewport beyond it.

### Inferred zoom assist

On an output configured as macOS, the firmware watches for a nonzero wheel event
while either USB GUI modifier is down—that is macOS Command, not Control at the
HID level. The first qualifying Command-scroll direction after boot is assumed to
be zoom-in, which automatically accommodates Natural Scrolling.

While inferred zoom is active:

- Mouse reports use the separate relative HID interface.
- Pointer-edge computer switching is disabled.
- Explicit F24 switching remains available.
- Logical pointer coordinates continue to be mirrored between Picos so returning
  to absolute mode does not reveal stale state.

Raw wheel magnitude adds to a per-output "zoom debt." Scrolling the opposite
direction repays it. Once debt reaches zero, the user must overscroll by six raw
wheel units and then stop for 250 ms. DeskHop then assumes macOS is back at 1x,
returns to absolute reports, and re-enables edge switching. This deliberately
matches the habit of over-scrolling when zooming out.

The state is runtime-only and separate for A and B. The owning Pico mirrors it to
the peer; mirrored modifier state expires after 2.5 seconds so a stale Command
cannot classify a later scroll as zoom. Debt saturates rather than wrapping,
because falsely remaining in relative/locked mode is safer than unexpectedly
switching computers.

Limitations of the no-host-software heuristic:

- It cannot know whether macOS Accessibility Zoom is actually enabled.
- An application-specific Command-scroll can falsely activate it.
- If the first Command-scroll after boot is zoom-out, the learned direction is
  backwards. Use the native chord (`Left Ctrl + Right Shift + Z`) manually to
  clear and relearn; Z is not presently mapped on the Sofle Layer 3 table.
- Stopping precisely at 1x without the six-unit overscroll leaves zoom assist
  active; F24 and the reset chord remain available.

The Sofle maps Command/Control in its own Mac-oriented layout, so reason about
the actual HID GUI modifier when debugging the gesture rather than only the
keycap label.

## Keep-awake behavior

DeskHop calls this feature `screensaver`, but it generates harmless mouse motion
to prevent a computer's screensaver/lock from starting.

| Mode | Generated motion |
| --- | --- |
| Disabled | None |
| Pong | Absolute bouncing pointer; a 5 ms gate, currently capped by the 120 Hz task loop |
| Jitter | Relative Y movement alternating +2/-2, once every 10 seconds |

Configuration has two layers:

- Per output: mode, activation idle time, maximum run time, and
  `Only If Inactive`.
- System-wide: stop synthetic motion after neither output has seen real input for
  a configured number of seconds.

Current compile-time defaults are mode Jitter on both outputs, 240-second
per-output activation delay, no per-output maximum, `Only If Inactive=0`, and a
300-second system-wide real-idle cutoff. Saved web configuration on the Picos can
and does override these defaults. “Auto-start” means Jitter is selected when the
firmware boots; motion still waits for the configured per-output idle delay.

Real keyboard, mouse, consumer-control, and system-control traffic counts as
activity. Synthetic Pong/Jitter movement does not. Each Pico broadcasts the age
of real activity it physically observed once per second, without re-broadcasting
the peer's copy; this prevents synthetic timestamp creep.

The system-wide policy means real activity delivered to either machine keeps
both machines eligible for their configured keep-awake behavior. Web Config's
`Auto-start Jitter on both outputs` checkbox is a single control over the two
persisted output modes: checked saves both as Jitter, while unchecked saves both
as Disabled. If the advanced per-output Mode controls disagree, the checkbox is
shown indeterminate. Layer 3 J/S/X only alter the selected output's runtime mode
and do not save it.

Auto-start was verified on the installed hardware on 2026-09-12 without using
Layer 3 J. Output A's idle delay was temporarily shortened to 5,000,000 µs, the
Pico was restarted, and focus was switched to B. macOS's `HIDIdleTime` on A
reset at approximately 10, 20, 30, and 40 seconds, exactly matching Jitter's
ten-second cadence. The ordinary 240,000,000 µs delay should be restored after
this accelerated test.

After no real input on either output for the system timeout (300 seconds by
default), all synthetic motion stops so the machines may sleep. Set the system
timeout to `0` for the old unlimited behavior. Input attached directly to a Mac,
rather than through DeskHop, is invisible to this policy.

The host must still be ready to receive HID. This feature is intended to prevent
suspend, not recover a host that has already suspended.

## LED focus indication and Sofle arrows

`KBD LED as Indicator` repurposes the Caps Lock LED bit as the selected-output
signal:

| Selected DeskHop output | Caps LED bit sent to keyboard | Sofle offhand OLED |
| --- | --- | --- |
| A / personal | Off | `<---` |
| B / work | On | `--->` |

DeskHop still caches each host's genuine LED state independently. When indicator
mode is enabled, it overrides only the physical Caps bit for focus; other LED
bits retain the active host's request. The onboard LED on the Pico belonging to
the selected output is steadily on, and the other is off.

Hotkey acknowledgements blink the Pico LED and the keyboard LED state together:
five transitions, 80 ms apart. During the blink, DeskHop sends all three standard
lock bits as `0x07`/`0x00`, then restores the focus/host state. A 30 Hz retry task
keeps attempting a failed keyboard LED update but does not overwrite an active
acknowledgement blink. This mirroring applies to acknowledgement blinks; direct
`toggle_led()` progress flashes during a firmware copy affect only the Pico GPIO.

The Sofle's USB/master half receives the Caps bit through QMK's standard LED
callback. A value must remain stable for 250 ms before QMK treats it as focus, so
the faster acknowledgement blink does not make the arrow flicker. QMK retries the
accepted state across its split link every 100 ms until the offhand half accepts
it. Only the offhand OLED draws the arrow.

On USB initialization/reconnect the arrow starts blank. A 2.5-second claim window
allows either the DeskHop LED signal or the optional Raw HID personal-host helper
to identify the host; otherwise it falls back to work/B. Once a stable DeskHop LED
state arrives, it is authoritative for that USB session and later Raw HID claims
are ignored. Synthetic all-off during suspend is ignored and state is sampled
again on wake.

The optional Raw HID fallback in the QMK repo uses Sofle VID/PID `FC32:0287`,
usage page/usage `FF60:61`, and a 32-byte claim beginning `42 01 01`. Its helper is
`util/qmk_kvm_host_claim.py`, with an optional LaunchAgent beside it. It is no
longer required for the LED-primary arrangement and is unsuitable for the work
Mac if arbitrary helper software is prohibited.

Important recovery fact: `KBD_LED_AS_INDICATOR` is compiled as `0` in DeskHop's
defaults. The working installation enabled it in persisted Web Config. Erasing
configuration therefore turns the signal off until it is re-enabled. If the
offhand OLED remains constantly `--->`, verify this setting and verify that both
Picos are actually running the LED fix.

When the Sofle is plugged straight into a computer, QMK cannot distinguish real
Caps Lock from DeskHop's focus bit while `DESKHOP_LED_FOCUS_ENABLE` is compiled
in. Remove that define for a Raw-HID-only/direct-connect build. If A/B roles ever
change, update `DESKHOP_OUTPUT_A_HOST` and `DESKHOP_OUTPUT_B_HOST` in QMK; changing
only the displayed strings is not enough.

## Locking both Macs

Right Control + L loops over both configured outputs and emits an OS-specific
lock chord:

- macOS: Control + Command + Q
- Linux/Windows: GUI/Super + L

The compiled DeskHop defaults are A = macOS and B = Linux. Both attached machines
in this installation are Macs, so persisted config must set both A and B to
macOS. If only one Mac locks, inspect B's OS setting first. This exact mismatch
has happened after configuration/default changes.

## Coordinated reboot

The reboot gesture is intentionally hard to trigger accidentally. DeskHop
requires three complete exact Left Control + Right Shift + Q taps from the same
keyboard source within one second of the first completed tap. The modifiers may
stay held between Q taps. A Q press containing an extra modifier, another
non-modifier, an overlong Q hold, or changing keyboard source cancels progress;
a modifier-only report between taps does not.

Consumed Q reports remain suppressed until physical release, so a malformed
follow-up report or another keyboard cannot recombine and leak Q/modifiers to the
host. Each accepted tap blinks the LEDs.

On the third tap, the initiating Pico:

1. Locks firmware-update state and refuses the reboot if an update is active or
   the running image is known dirty.
2. Successfully queues `REBOOT_MSG` to the peer before arming its own reboot.
3. Releases all host-visible keys.
4. Stops feeding its 500 ms watchdog while HID and UART queues continue draining.

The peer does the same without echoing the command. Both return on output A. This
is reliable queue ordering, not a two-phase acknowledgement protocol; a broken
physical UART link can still prevent the peer from receiving it. Once reboot is
armed, UF2 writes, update replies/pulls, and new heartbeat-triggered pulls are
blocked so a partial image cannot be booted. Outbound heartbeat advertisements
may continue during the short watchdog grace period.

## Configuration mode

Use Layer 3 C or native Left Control + Right Shift + C + O. The keyboard-side Pico
reboots and mounts a synthetic `DESKHOP` drive containing `config.htm`.

1. Open `config.htm` in Chrome/Chromium; Firefox lacks the required WebHID path.
2. Click Connect and select DeskHop.
3. Make changes.
4. Click Save to write the configuration.
5. Click Exit to reboot safely into normal mode. Configuration mode otherwise
   times out after five minutes.

Save has no success toast, so apparent silence is normal. It sends changed fields
to the peer first and then saves locally. `Read` reports only the connected Pico,
not both. If a side appears to have missed a setting, simply clicking Save again
is insufficient because unchanged fields are not retransmitted. Change each
affected field to a temporary value and Save, then restore the desired value and
Save again before Exit—or wipe and reconfigure. Independently reading the peer
requires entering config through that side or purpose-built proxy tooling.

The drive is a synthetic interface, not general storage. The HTML is local and
does not load code from the Internet.

Settings that matter for this installation after a config wipe:

- Output A operating system: macOS.
- Output B operating system: macOS.
- Screen positions: B left, A right.
- `KBD LED as Indicator`: enabled.
- `Auto-start Jitter on both outputs`: enabled for hands-off keep-awake.
- `Only If Inactive`: enabled on both outputs.
- Per-output idle time: 240,000,000 µs (four minutes); maximum time: 0.
- System idle timeout: 1,800 seconds in the saved installation configuration;
  the compiled default is 300 seconds and 0 means unlimited keep-awake.
- Screen count, speed, jump threshold, and edge calibration as appropriate.

Saved settings live only in Pico flash and can differ from `user_config.h`.
Config format v9 reused the reserved v8 word for the system idle timeout.
Format v10 keeps the same binary layout and migrates valid v8/v9 data in place;
it preserves HID, LED, OS, calibration, and timeout values while deliberately
setting both startup screensaver modes to Jitter once during this upgrade. This
also replaces an explicitly saved Disabled or Pong mode from v8/v9; use the new
checkbox or advanced Mode controls and Save if that is not desired.

### Erasing config

Use Layer 3 D three times. This erases both Picos' saved configuration and reloads
the compiled defaults. Re-enter Web Config afterward to restore both-mac OS
selection, LED focus, keep-awake preferences, and any calibration.

### Vertical edge calibration

For monitors with different heights, place the pointer on the larger screen at
the vertical height corresponding to the smaller screen's top or bottom edge,
then use Layer 3 Y. DeskHop treats a point above screen center as the top boundary
and a point below center as the bottom boundary, synchronizes it, and saves it.

## Building and testing DeskHop

Standard build:

```sh
cd /Users/benji/Documents/ChatGPT/DeskHop
export PATH=/opt/homebrew/opt/arm-gcc-bin@14/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
cmake -S . -B build
cmake --build build --parallel
```

The artifact is `build/deskhop.uf2`. The current Mac has Homebrew's
`arm-none-eabi-gcc` and binutils available under `/opt/homebrew`.

Every behavior change intended to auto-propagate must also raise
`VERSION_MINOR` in `CMakeLists.txt`. Internally v0.92 is encoded as 192. A healthy
peer starts an automatic pull only when it sees a strictly newer numeric version;
an equal-version rebuild with different code/checksum will not normally replace
the peer.

Six original host-native suites cover the custom state machines, configuration migration,
and configuration UI:

```sh
node tests/test_webconfig_autostart.js

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/include tests/test_zoom_tracker.c src/zoom_tracker.c -o /tmp/deskhop-zoom-tracker-test
/tmp/deskhop-zoom-tracker-test

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/include tests/test_fw_update.c src/fw_update.c -o /tmp/deskhop-fw-update-test
/tmp/deskhop-fw-update-test

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/include tests/test_screensaver_policy.c src/screensaver_policy.c -o /tmp/deskhop-screensaver-policy-test
/tmp/deskhop-screensaver-policy-test

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/include tests/test_reboot_hotkey.c src/reboot_hotkey.c -o /tmp/deskhop-reboot-hotkey-test
/tmp/deskhop-reboot-hotkey-test

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/include tests/test_config_migration.c src/config_migration.c -o /tmp/deskhop-config-migration-test
/tmp/deskhop-config-migration-test
```

The adopted v0.92 full merge adds a seventh native HID regression suite for
parser usage carry/bounds, full report-ID dispatch, consumer/system payload and
activity guards, and split NKRO extraction. Run the sanitizer command documented
in [tests/hid_stubs/README.md](tests/hid_stubs/README.md); CI runs it after the
six original suites. See [UPSTREAM_FULL_MERGE_NOTES.md](UPSTREAM_FULL_MERGE_NOTES.md)
for its integration decisions, build results, and remaining hardware checks.

There is not yet an in-repo native regression test for pointer-sync or remote
non-motion transport; those changes were verified on the real hardware.

If configuration fields/UI change, update the generated web configuration and
the embedded disk image as well as C source. CI runs `make` in `webconfig/`, then
`disk/create.sh`, then the native tests and ARM build. The disk script uses Linux
mount tooling and `sudo`, so the repository's Docker/CI path is usually easier
than invoking it directly on macOS.

## Flashing DeskHop and peer propagation

### Preferred update path

1. Build and test a firmware with a higher version number.
2. Keep both DeskHop Picos powered and UART-connected.
3. Use Layer 3 C for the `DESKHOP` configuration drive, or Layer 3 A/B for the
   selected Pico's `RPI-RP2` ROM bootloader drive.
4. Copy the new `deskhop.uf2` to the mounted volume and let it reboot/eject.
5. Leave both sides powered for several one-second heartbeat cycles. The older
   peer should pull and install the 256 KiB running image automatically.
6. Verify the trackball, keyboard right-click on both Macs, F24 switching, Pico
   LEDs, and Sofle arrow direction before declaring propagation successful.

Layer 3 A/B uses the canonical keyboard-on-A topology: A enters the local Pico's
bootloader, while B asks the peer over UART. If the topology or UART is broken,
use the Pico BOOTSEL button while reconnecting that board.

### How automatic propagation works

Each Pico sends a heartbeat once per second with its version, a protocol marker,
and full-image checksum. A compatible older peer pulls four bytes at a time,
writes complete 256-byte pages, verifies the full image, and reboots. The current
protocol includes these safeguards:

- Responses for old or unexpected addresses are ignored.
- An unanswered word is retried after 100 ms.
- The transfer is pinned to one source version and checksum to prevent hybrids.
- No progress for 30 seconds abandons a still-clean pull, restarts a dirty pull
  from a live peer, or pauses a dirty pull if the peer disappeared.
- A paused dirty board keeps executing safely from RAM and resumes repair when a
  compatible peer returns.
- A completed invalid image erases its first sector and enters deterministic ROM
  bootloader recovery.
- Host UF2 drop and UART pull cannot run concurrently. The first valid host UF2
  block claims the updater.
- Duplicate/out-of-order UF2 blocks are accepted safely: each of 1,024 expected
  blocks is programmed once and each sector erased once.
- A board receiving a host UF2 stops advertising its changing image until reboot.
- Config save/wipe and flash reads/writes are protected across both RP2040 cores.
- A new receiver will not pull from an old peer lacking the checksum protocol
  marker. Direct ROM/config-drive flashing remains the compatibility escape hatch.

Firmware before v0.85 could hang forever on a dropped transfer response. Current
responses are blocking-enqueued for compatibility with such old receivers, while
current receivers have retry/stall recovery. The v0.82 boundary fix also answers
the legacy sentinel request at exactly the end of the image without reading past
it.

### If propagation fails

Symptoms include a dead B-side trackball, no LED after reconnecting B, or a Sofle
right-click that works on A but never appears on B. Leave both sides powered for
at least the 30-second stall/recovery interval. If it remains bad, put B into
`RPI-RP2` and flash the same UF2 explicitly. Do not casually remove power from a
current receiver that may be executing from RAM with a partial flash image. A
plain power cycle historically fixed the pre-v0.85 case in which every page had
already arrived but finalization was waiting forever; it is not the generic
recovery for the current updater. Verify both boards before changing unrelated
Karabiner or mouse settings.

## Building and flashing the Sofle

Build only from the canonical QMK checkout:

```sh
cd /Users/benji/qmk_firmware
qmk compile -kb sofle/rev1 -km benji -e CONVERT_TO=rp2040_ce
```

Output is `sofle_rev1_benji_rp2040_ce.uf2` under the checkout/build output. If
Homebrew toolchain selection fails, explicitly put the current unversioned
toolchain prefixes first:

```sh
env PATH=/opt/homebrew/opt/arm-none-eabi-gcc/bin:/opt/homebrew/opt/arm-none-eabi-binutils/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  qmk compile -kb sofle/rev1 -km benji -e CONVERT_TO=rp2040_ce
```

Flash both Sofle halves separately with the same UF2. The RP2040 CE conversion
uses a 500 ms double-tap-reset window; enter each half's `RPI-RP2` volume and copy
the file. Do not use Layer 3 A/B for this—those commands boot the DeskHop Picos.

For a coupled change, deploy DeskHop first, allow/verify peer propagation, then
flash both Sofle halves. This prevents the keyboard from emitting a chord or
interpreting an LED convention the DeskHop firmware does not yet understand.

After QMK flashing, test:

- Both physical halves type normally.
- Base-layer right-click works on A and B without pointer movement.
- Layer 3 S switches via F24.
- Offhand arrows change A `<---` / B `--->` without flicker during hotkey ACKs.
- Karabiner remappings, including Alt-Tab, still work.
- No modifier remains logically down after every Layer 3 command.
- Triple D does nothing on the first two taps and triple Q reboots both Picos.

## Troubleshooting matrix

| Symptom | Most likely cause | First response |
| --- | --- | --- |
| Keyboard always types on A, mouse stays on B, cursor disappears at the edge, only B LED changes | Picos are not sharing correct state: stale/mismatched firmware, failed UART, or incorrect host/input cabling | Confirm the physical topology, power both hosts, reconnect both Picos, then explicitly flash both with the same current UF2 if needed. |
| Neither LED reacts and a keyboard plugged "in line" with one side only types on that side | The keyboard is bypassing DeskHop's intended input/routing path or one/both Picos are not running | Use the single keyboard-marked USB-A input and both host-facing Pico USB ports; check both boards power and blink on peripheral enumeration. |
| Trackball is dead immediately after a one-sided flash | Peer did not complete propagation or cannot parse the new UART messages | Keep both powered for at least 30 seconds; then explicitly flash B if still stale. Avoid interrupting a current partial repair. |
| Trackball causes B to watchdog/restart continuously | Old HID parser overflows on the Gameball's large vendor report count | Use v0.84+ / commit `3f25461` or newer on both Picos. |
| Keyboard right-click jumps to upper-right | Full absolute report carried stale coordinates | Ensure both Picos contain pointer sync and `MOUSE_NONMOTION_MSG` (v0.82+); let propagation settle or flash the peer explicitly. |
| Keyboard right-click works on A but B's Karabiner EventViewer sees nothing | B is running firmware too old to understand the position-neutral message | Let propagation settle or explicitly update B; do not change the QMK mapping first. |
| Physical trackball right-click works on both, keyboard right-click does not | Same cross-Pico composite-button path as above | Verify both DeskHop versions and UART propagation. `Force Mouse Boot Mode` is unrelated. |
| Pointer snaps to a corner when alternating pointing devices or immediately switches back | Peer has stale X/Y or lacks PR #357 | Install `c68078f` or newer on both Picos. |
| Zoomed viewport stutters or stops panning at the display edge | Absolute coordinates are clamped at the edge; zoom assist is not active | Confirm output OS is macOS and zoom is initiated with Command-scroll; test gaming mode G, then reset zoom assist direction with Z if necessary. |
| Edge switching stays disabled after zooming out | Zoom debt was not fully repaid/overscrolled, or manual gaming mode is still on | Command-scroll farther out and pause 250 ms; use Z to clear inference; toggle G separately if gaming mode was enabled. |
| Other Mac does not stay awake | Auto-start Jitter is off, its per-output idle conditions are not met, the host suspended USB before Jitter began, or the configured global real-idle cutoff was reached | Enable `Auto-start Jitter on both outputs`; inspect idle/max/Only If Inactive/system timeout. Temporarily use a 5,000,000 µs idle time and monitor macOS `HIDIdleTime` to test it quickly. |
| Layer 3 L locks only one Mac | The other output is still configured Linux/Windows | Set both output OS values to macOS, Save, Exit, and retry. |
| Sofle arrow is constantly `--->` | Caps focus signal is absent, unstable, or treated as B fallback | Enable `KBD LED as Indicator`, confirm current firmware on both Picos and QMK, then reconnect/power-cycle. |
| Arrow flickers during every DeskHop acknowledgement | QMK LED debounce is missing/too short | Use current QMK with 250 ms settle time; DeskHop ACK transitions are 80 ms. |
| Alt-Tab or other Karabiner rules stop matching | Device filter still names the physical keyboard rather than DeskHop | Match `DeskHop Switch` / `1209:c000` or make the rule appropriately device-agnostic. |
| All keyboard input dies after a synthetic chord | Partial HID reports left Karabiner's virtual keyboard in bad modifier state | Immediate recovery: restart Karabiner. Permanent fix: current atomic 20 ms QMK reports plus current DeskHop all-up handling. |
| Caps behavior persists after DeskHop is unplugged | Stuck state is in macOS/Karabiner, not either Pico | Restart Karabiner; retain F24 switching and current all-up protection. |
| Config Save seems ineffective | Settings were not saved/exited, a peer missed a changed SET, or a firmware update was in progress | Read the connected Pico. To force retransmission, save a temporary value and then the desired value before Exit; alternatively wipe/reconfigure. Runtime SETs may apply during an update, but flash persistence is intentionally refused. |
| Coordinated reboot resets only one Pico | Peer unpowered, UART broken/full on obsolete firmware, or update guard refused the peer | Ensure both powered/current and no update active; test UART via dual LED blink on peripheral reconnect, then retry. |

When a USB peripheral enumerates, the local Pico blinks and asks the peer to
blink. Seeing the two LEDs blink in succession is a useful power/USB/UART smoke
test before deeper debugging.

## What is custom in this fork

The current branch is upstream v0.78 plus 17 commits. In chronological order:

| Commit | Purpose |
| --- | --- |
| `59577cc` | Upstream ramdisk mount fix; ignore ordinary filesystem writes before validating UF2 block metadata. |
| `c68078f` | PR #357: synchronize cursor coordinates between Picos. |
| `a2d641f` | Correct keyboard LED focus overrides, retry behavior, and acknowledgement restoration. |
| `b061c99` | Position-neutral remote clicks/wheels plus peer updater end-boundary/queue fixes. |
| `daff1d6` | Inferred macOS zoom assist and cross-Pico modifier/state synchronization. |
| `066bc20` | PR #358-style fix: consumer/system controls without report IDs. |
| `c7ebc73` | PR #359: retain multiple keyboard bitmap sections and bound short reports. |
| `ac3cea0` | PR #359: recognize valid short key bitmap sections. |
| `3f25461` | PR #361: bound HID usage-array access for very large report counts/Gameball. |
| `a90a0cd` | Adapt PR #329: compute config CRC through `offsetof(checksum)`. |
| `4093479` | Firmware-version bump for the HID-hardening bundle. |
| `c020c34` | Self-recovering, checksummed, cross-core-safe peer firmware propagation. |
| `08db0d1` | Firmware-version bump proving propagation of the prior release. |
| `329761d` | Decide NKRO from total bitmap size, avoiding false classification of small bitfields. |
| `6367f9c` | System-wide real-activity policy for keeping both configured outputs awake. |
| `1e90719` | F24 switching, reliable selection packets, and critical all-keys-up handling. |
| `6d1cd12` | Guarded triple-Q coordinated reboot of both Picos. |

The corresponding QMK history begins with host-arrow support and then adds the
configuration chord, complete Layer 3 controls, atomic 20 ms reports, triple-D
safety, DeskHop LED focus as the primary signal, F24 switching, and triple-Q
reboot. The key milestones are `32c100bda9`, `a65d54f1f2`, `4d24e8ae81`,
`6438cfe0ea`, `7f2dcabfef`, and `469f5dc815`.

Do not blindly re-apply similarly named upstream PRs during a future sync. This
fork already contains equivalent or extended implementations, often with later
interaction fixes layered on top.

### Upstream relationship at this snapshot

As of 2026-09-11, upstream `hrvach/deskhop` had advanced to `1e31d10`, so it and
this fork diverge after `59577cc`. Upstream has merged PR #360's original updater
boundary fixes and PR #361's Gameball bounds fix. This fork already contains
their equivalents in `b061c99`/`c020c34` and `3f25461`, respectively;
`329761d` is separate follow-on NKRO-classification hardening. Those upstream
commits will overlap during a future merge.

PRs [#357](https://github.com/hrvach/deskhop/pull/357),
[#358](https://github.com/hrvach/deskhop/pull/358),
[#359](https://github.com/hrvach/deskhop/pull/359), and
[#329](https://github.com/hrvach/deskhop/pull/329) were still open. The fork
contains selected commits or adaptations from all four, but PR #359 has since
grown additional multi-keyboard-collection work that is not in this snapshot.
Review by behavior and diff, not PR number, before importing anything.

## HID compatibility work carried by the fork

- Consumer and system-control handlers skip byte zero only when that HID
  interface actually uses report IDs. This fixes media/system keys on devices
  such as the Cherry KC 6000.
- Multiple NKRO key bitmap sections are retained, including a valid eight-bit
  section, fixing Wooting Two HE layouts.
- A keyboard is classified as NKRO from aggregate bitmap width, not one small
  keyboard-page bitfield.
- The fixed 128-entry HID usage array is bounds-checked, and the final usage is
  reused for oversized report counts per HID semantics. This prevents the
  Gameball gesture descriptor from corrupting memory and watchdog-looping B.
- Configuration CRC ends at the actual checksum field rather than assuming no
  compiler tail padding.
- Keyboard unmount clears only that source, recombines remaining keyboards, and
  republishes modifiers instead of spuriously releasing unrelated held keys.

Known parser limits remain: only four NKRO blocks are represented; the parser is
not a general-purpose HID implementation; and sufficiently pathological report
descriptors may still exceed its representation or watchdog time budget. The
later PR #359 change that assigns separate `keyboard_t` state to multiple
report-ID keyboard collections is not present, so devices such as the 8BitDo
Retro Mechanical Keyboard remain a candidate for that future fix.

## Maintenance rules

- Keep the DeskHop hotkey table, QMK `process_record_user()` cases, Layer 3
  layout, and both repositories' documentation synchronized.
- Bump the DeskHop firmware version for every deployable behavior change, or the
  peer will not auto-update.
- Run all host-native regression suites and an ARM build before flashing.
- If a config field changes, update its struct layout/version/migration, API map,
  Web Config source, generated HTML, and embedded disk image together.
- Preserve position-neutral button/wheel transport. Buttons do not contain
  coordinates.
- Preserve logical pointer synchronization through every relative-mode path.
- Preserve the distinction between manual gaming mode and inferred zoom assist.
- Never let synthetic keep-awake traffic refresh real activity.
- Never silently drop output-selection or key-release state transitions.
- Do not allow host UF2 writes, peer pulls, config writes, or coordinated reboot
  to race over the same flash image.
- In QMK, keep DeskHop chords atomic. `tap_deskhop_hotkey()` currently supports at
  most two simultaneous non-modifier keys.
- Keep the 250 ms QMK LED settle interval shorter than the 2.5-second fallback
  window and longer than DeskHop's 80 ms acknowledgement transitions.
- Test on both output directions. The important edge cases occur when an input
  device is physically attached to the Pico opposite the active computer.

## Code map

| Area | Files |
| --- | --- |
| DeskHop hotkeys and keyboard routing | `src/keyboard.c`, `src/handlers.c` |
| Mouse coordinates, switching, non-motion transport | `src/mouse.c`, `src/handlers.c` |
| macOS zoom assist | `src/zoom.c`, `src/zoom_tracker.c`, `src/include/zoom_tracker.h` |
| Keep-awake policy | `src/tasks.c`, `src/screensaver_policy.c`, `src/include/screensaver_policy.h` |
| LED state/focus | `src/led.c`, `src/usb.c` |
| UART protocol | `src/uart.c`, `src/include/protocol.h`, `src/include/packet.h` |
| Peer update/recovery | `src/fw_update.c`, `src/tasks.c`, `src/handlers.c`, `src/ramdisk.c`, `src/utils.c` |
| Config/defaults/API | `src/include/structs.h`, `src/include/config.h`, `src/include/user_config.h`, `src/defaults.c`, `src/protocol.c` |
| Native regression tests | `tests/test_zoom_tracker.c`, `tests/test_fw_update.c`, `tests/test_screensaver_policy.c`, `tests/test_reboot_hotkey.c`, `tests/test_config_migration.c`, `tests/test_webconfig_autostart.js`, `tests/test_hid_regressions.c` |
| Web Config | `webconfig/form.py`, `webconfig/templates/`, generated `webconfig/config*.htm`, `disk/disk.img` |
| Sofle integration | `/Users/benji/qmk_firmware/keyboards/sofle/keymaps/benji/keymap.c`, `config.h`, `rules.mk`, `readme.md` |
