# Benji's DeskHop setup and maintainer runbook

This is the durable record of the DeskHop work done for Benji's two-Mac setup.
It describes the physical installation, the behavior added to the DeskHop fork,
the coupled Sofle/QMK firmware, how to build and deploy both, and the failure
modes already diagnosed. It is intentionally more specific than the upstream
README.

Snapshot: 2026-09-17

## Current deployment: v0.118; A-to-B paste confirmed

Both boards now run image-verified v0.118 (slot CRC `cbe34825`) after all 74
deep checks and the ARM build passed. Deployment completed in 24.718675 seconds;
both cores progress on both boards, and A's saved settings are unchanged. This
adds `link` and a bounded `link watch` for independent USB observation; it does
not fix or reproduce the input-loss incident below. A's command was checked
three times: fresh cores, no UART receive errors, RX DMA active, cleanup idle.
Evidence: `build/updater/runs/20260917T154452Z-sb68kj2w/result.json` and
`build/tests/link-v118-live-20260917T154538Z/`.

The user cannot run code/helper software on Mac B, but can use built-in `screen`
and transcribe one short line. B's port is `/dev/cu.usbmodem1203`. They have
opened `screen /dev/cu.usbmodem1203 115200` and reported
`C=0/0 U=197/0/3 D=03/31/31 P=0/0` from `link` (R was not transcribed).
The user confirmed the controlled `hello`/L3-V paste into blank TextEdit on B
worked. Metadata history confirms trigger, admission, helper request/result,
payload readiness, release, typing and completion. Both boards retained their
boots, both cores were fresh, and neither history had overwritten entries or
clipboard rejection/cancellation. Evidence:
`build/tests/clipboard-v118-trial-20260917T155400Z/`.
The helper was briefly stopped for that capture and restored on A; PID 35710
owns the port and its sample shows `Session.run()`/`SerialTransport.read` after
the handshake: `build/tests/clipboard-app-v118-restored.sample.txt`.
This verifies one A-to-B paste, not reverse-direction hardware acceptance or
long-term reliability. The earlier v0.117 freeze remains unexplained.
That trial added no new QMK flash, OS helper installation or login item. The helper
read the user-triggered clipboard; the diagnostic capture contains no text.
See [the compact field legend](docs/diagnostics.md) and
[the deployment record](docs/testing/clipboard-deployment.md).

### Permanent helper installation

At the user's request, the existing verified helper is now installed at
`/Users/benji/Applications/DeskHop Clipboard.app`. The build-folder instance was
quit and replaced by the installed instance (PID 37733 at verification). The
installed executable, reader and Info.plist match the tested build, and its
strict deep code-signature verification passed. It launched without arguments,
loaded the saved A port/board ID/build `0.118` and resume preference, and owns
`/dev/cu.usbmodem21203`. The UI confirms it is connected and listening.

The app's **Launch at login** checkbox is enabled through `SMAppService.mainApp`,
with no pending approval shown. This is user-session startup; no logout/reboot
was performed to test it. Future firmware changes still require updating the
helper's expected build. No custom LaunchAgent or root daemon was installed.
Evidence: `build/tests/clipboard-permanent-install.json`.

Build or update the installed helper from the repository root with
`make helper-app` or `make install-helper-app`. The install target builds first,
verifies a staged bundle, quits a running copy normally, replaces it in
`~/Applications`, then restarts it with its saved preferences. A stopped helper
stays stopped. `HELPER_INSTALL_DIR=/Applications` selects another writable
location. Existing login registration is preserved; the target does not enable
it for a new user or change firmware. Both commands have been exercised, and
the installed helper reconnected with Launch at login still checked.

## Historical v0.117 incident: input restored after B power cycle

The user confirms normal input returned after power cycling only B. A read-only
capture shows both exact board identities on v0.117, fresh checkpoints on both
cores and idle updaters. A retains boot `f271f9ee6cf3a917`; B has new boot
`cdf2839c7cc4f189`. Recovery evidence:
`build/tests/clipboard-v117-recovery-20260917T150357Z/`.

During the failure A remained responsive while B was unreachable over the link.
A's full retained history shows the F23 request and a deadline cancellation,
but no grant or helper read. Evidence: `build/tests/clipboard-crash-v117/` and
the deployment record. The root cause is still unknown; B's old volatile history
was lost at power cycle. The helper was stopped during recovery. An independent
way to observe B and bounded UART/DMA diagnostics were required before another
trial; v0.118 supplies that path, with the successful trial recorded above.
Version 0.117 is not physically accepted for clipboard use despite passing image
verification. Ordinary input recovery is confirmed separately.

## Historical clipboard deployments and pending keyboard startup check

Benji authorized running the app and flashing. Both boards ran v0.117 and
passed fresh full-slot CRC `ffde8f66`, exact identity, progressing-core and idle
updater checks. A's saved settings are unchanged. The native menu-bar app was
connected to A but was stopped after the incident above; Launch at login
remains unregistered. The Sofle
Layer 3 V firmware was written to both user-selected Sofle halves on 2026-09-17.
The first re-enumerated normally. The second returned Sofle VID/PID and product
name, but its configured USB/HID interfaces did not appear. The user deferred
the requested keyboard power cycle/reconnect until later; its startup and
physical input/clipboard acceptance remain pending. No reflash is needed merely
to complete that check.

The v0.114 rollout exposed a DMA abort-counter bug, fixed in v0.115 after all
73 deep validation steps and the ARM build passed. Both slow recovery transfers
completed without a manual reset or power cycle. The original host runs remain
failed at peer monitoring; separate read-only both-board verification passed in
3.455363 seconds. Physical input and clipboard acceptance remain pending.
See [the live deployment record](docs/testing/clipboard-deployment.md).

The v0.116 log confirmed F23 admission on A and `led_unknown` rejection on B.
The user cannot toggle their remapped Caps key; more fundamentally, an absent
LED report does not mean Caps is on. Version 0.117 now assumes Caps off until a
valid report from the current USB session says otherwise. Reported Caps on
still blocks/cancels typing. Actual but unreported Caps on can alter letter case.
All 61 fast release checks and 116 clipboard scenarios passed; both-board
update/verification completed in 24.41706 seconds. The subsequent physical retry
caused the incident above; the helper was stopped. No further QMK flash was needed.

The v0.116 follow-up adds metadata-only clipboard stage/rejection events after
that no-op report. All 73 deep steps pass, including 113 paired clipboard
scenarios, and both-board deployment/verification completed in 23.964188 seconds.
The helper is connected with the new version setting. The user has been asked
to retry `hello`/L3-V with their Caps remapping unchanged; that subsequent history established the cause described above; physical
clipboard acceptance remains pending the v0.117 retry.

The user subsequently reported that Layer 3 V on B produces no text. A fresh
event-log capture shows both v0.115 boards healthy, the keyboard interfaces
mounted, synchronized output changes and no retained UART/descriptor errors.
B had restarted about two minutes before that capture; its cause is unknown.
The helper is reconnected. Current history does not record clipboard admission
or rejection reasons, so the cause of the no-op is not yet established. A
controlled ASCII retry with a fresh target Caps Lock report is pending.

## Clipboard development candidate

`codex/clipboard-keyboard` now includes published, accepted v0.113 commit
`6ac4d6080dfcc39030c7232a3631fd8ec933a39c`. Version 0.118 adds bounded,
on-demand clipboard typing from either Mac to the other, with full validation, physical-release gating,
paced HID reports and cancellation. UART 56–61 retain confirmed configuration;
clipboard uses 62. The guarded local `config` and `bootloader A|B` remain intact.

The shipping helper is an all-Swift macOS 13+ menu-bar app with connection status,
Pause/Resume, Quit and explicit Launch at login using `SMAppService.mainApp`.
A universal `.app` and ZIP are built locally; no app installation or login-item
registration is performed by the build. Either or both Macs may run the helper; only the source needs one.
The Python CLI remains a development protocol reference, not an app dependency.

The Sofle shortcut is Layer 3 V (bare F23, held until actual key release). Its
patch is applied in the canonical QMK checkout and the same built UF2 was written
to both user-selected halves. The second half's normal USB startup check awaits
the user-deferred reconnect.
Supported text is 1–1024 bytes of US ANSI printable ASCII plus LF/TAB. No real
clipboard was read by the agent; the helper served the user's successful test.
Hardware deployment and permanent helper installation are tracked above. The
user subsequently authorized committing, merging and pushing both the DeskHop
feature and its matching QMK shortcut. Combined validation and packaging evidence are in
[the clipboard record](docs/testing/clipboard-keyboard.md). See the
[workflow](docs/clipboard-keyboard.md), [native app](macos/DeskHopClipboard/README.md)
and [QMK proposal](docs/clipboard-qmk.patch).

## Clipboard task requirements and original scope

Latest direction: make the feature symmetric. Pressing the shortcut while either
Mac is selected requests the opposite Mac’s current text only while its helper
is connected; otherwise it is a no-op. The keyboard may attach to either Pico.
The original one-way setup below is a supported subset of this design.

Benji requested a new task to implement fixed-size, on-demand text transfer from
personal Mac A to work Mac B. A shortcut on B requests A's current clipboard;
accept the entire supported text only if it fits in 1024 bytes, otherwise reject
it. No truncation, unlimited streaming or old-cache fallback. Validate the full
message before emitting paced, cancellable keystrokes on B. No software is to be
installed on B, and B's clipboard is neither read nor changed.

**Latest explicit helper decision:** Benji chose **Swift**, not Zig, and wants a
native macOS **menu-bar app**, easy installation, and automatic startup. Provide
a normal `.app` bundle and convenient packaging/install instructions, plus a
user-visible **Launch at login** option using the supported macOS login-item
mechanism. A user-session login item is appropriate for clipboard access; do not
make a root/system boot daemon. Include connection status, Pause/Resume and Quit
without displaying or logging clipboard contents. The later “run the app and
flash” instruction authorizes this hardware deployment and normal app launch;
the later “Make the helper permanent” request authorizes installing it in
Applications and enabling its login item. Pin the
supported macOS version and document signing/notarization
limitations honestly; do not disable Gatekeeper or alter security settings.

The new task was requested as `Implement one-way 1 KiB clipboard typing`, in its
own worktree based on main. These requirements supersede the earlier exploratory
discussion of a Zig CLI helper. Keep that feature on its own branch; integrate
the published v0.113 baseline there. The later deployment instruction supersedes
the original development-only scope.

## Current accepted device release: v0.113 confirmed saves and serial config

On the connected Pico's USB serial console, type `config` and Enter. This is a
separate, local-only command: no A/B parameter, firmware upload or change to
`bootloader A|B`. It reboots that Pico into the same configuration mode as L3-C;
open the newly mounted DESKHOP `CONFIG.HTM` in Chrome and connect to DeskHop.
The other Pico remains in normal mode and can receive confirmed settings writes
over UART. Use the config page's Exit action to return to normal operation.

The command waits for actual USB completion of its response and UART drain,
rejects active/dirty updates and competing maintenance, and cancels on a stalled
reply or terminal disconnect before authorization. `accepted` is permission to
reboot, not proof that config USB enumerated; observe the configuration device.
In configuration mode another `config` returns `already_active` without exiting
or rebooting. Invalid arguments are rejected. Unsaved RAM edits can be lost on
entry, as with the keyboard shortcut. The command does not save settings.

This follow-up includes v0.112 confirmed saves. Validation and physical evidence
are tracked in [the v0.113 record](docs/testing/serial-config-v113.md). Benji
separately authorized commit, merge and push after successful verification.

All deep-tier checks passed in 427.151 seconds and the ARM build passed. The
normal upgrade completed in 23.625701 seconds without a retry or power cycle;
both Picos passed fresh full-slot CRC `f530270f`, and A's saved settings were
unchanged. Actual serial `config` on A mounted `/Volumes/DESKHOP` with the exact
current config page. A second command returned `already_active`; neither Pico
rebooted. After a safe disk unmount, the existing configuration Exit report
returned A to normal USB mode. Final status confirmed both v0.113 builds, idle
updaters and an unchanged B boot session. Benji has now accepted typing,
right-click, switching, focus arrows and zoom assist, and authorized commit,
merge and push once verified. Benji then reported **"Save looks good"**, completing
the release acceptance gate. The requested Save check made no settings edits;
this is user-observed acceptance, not an agent-captured browser receipt or an
edited-value persistence test. The device is back in normal mode. The clipboard
feature is being implemented in its separate task, not in this release.

## Previous deployment: v0.112 confirmed two-Pico configuration saves

Branch `codex/confirmed-config-saves` implements acknowledged per-Pico apply and
verified persistence. All 66 deep-test steps and the ARM build passed. The first
two guarded attempts hit the ROM read hang before writing; a checked normal
reboot also failed. Benji power-cycled the pair, then the fresh normal-mode retry
succeeded in 23.728236 seconds. Both Picos then ran verified v0.112 (full-slot CRC
`e49caa16`), with both cores progressing and A's saved settings byte-identical.
B auto-propagated using 1024 pages, zero words and zero retries. Evidence:
`build/updater/runs/20260916T214920Z-zzaryspy/` in
`/private/tmp/deskhop-confirmed-saves.X8z0OT`.
Physical input and configuration-page acceptance were completed on the
superseding v0.113 release above, which includes this implementation. See the
[protocol, UI and validation record](docs/testing/confirmed-config-saves-v112.md).

The updated page probes both Picos before new writes. Ordinary changes report
RAM application separately from Save; border pairs apply together on Save.
Successful Save explicitly reports **A and B saved to flash and verified**.
Timeouts mean unknown, not failed or saved. Partial outcomes identify each
physical board. Retry Save explicitly after connectivity is restored; unconfirmed
edits survive Read so the peer is not skipped. Read itself still shows only the
connected board's values. A warning means both boards saved but their settings
digests differ; untouched historical differences are not silently synchronized.

Use the newly embedded page after upgrading both Picos. An old page retains the
old unacknowledged behavior; a new page refuses writes if either board does not
support confirmation. Save is not atomic across boards or power-loss-safe.

## Previous accepted device release: v0.111 firmware-advertisement startup guard

Benji authorized the narrow follow-up implementation. Firmware metadata
advertisements are now suppressed until monotonic uptime reaches one second;
the heartbeat task continues activity, button, modifier, zoom and selection
synchronization during that grace. There is no sleep, USB-presence requirement,
protocol change, timeout extension or new configuration setting. Existing
config/maintenance and in-progress UF2-drop behavior is preserved.

This is a bounded mitigation for the observed direct-device startup pattern,
not a guarantee that every USB device/hub has finished enumerating. It does not
address later hotplug stalls. At acceptance both Picos ran verified v0.111, and
Benji confirmed "Everything works normally" for the full functional checklist.
The accepted change set was developed on `codex/firmware-transfer-profiling`.
Benji subsequently authorized merging it into main and pushing the fork. See the
[implementation and validation record](docs/testing/firmware-startup-guard-v111.md).

All 64 deep-tier steps (including 141 updater tests) and the ARM build passed.
The prepared candidate is `build/releases/deskhop-v0.111-yq5wa62e/manifest.json`
in `/private/tmp/deskhop-transfer-profile.Zi4FBl`, full-slot CRC `54001839`.
Complete modeled transfers to the exact frozen v0.110 receiver passed in both
directions, with 1024 pages and no words/retries.

The authorized hardware upgrade completed in **23.740171 seconds**, versus
47.821911 seconds for v0.110. A served 1024 pages with zero word requests and
zero page retries. Peer wait/settle fell from 37.759860 to 13.481009 seconds;
source-profile elapsed fell from 36.129854 to 10.746857 seconds. These are two
observed upgrades, not a repeated controlled benchmark or universal speed promise.
Backups, stock `load -v` byte verification, unchanged saved settings, automatic
B propagation/reboot, both fresh full-slot CRCs and progressing cores passed.
Evidence: `build/updater/runs/20260916T202228Z-1oc8iom4/` in that worktree,
including a separate `user-acceptance.json`; the original result is unchanged.

## Previous accepted device release: v0.110 transfer profiling

`codex/firmware-transfer-profiling` investigates the approximately 37-second
peer-propagation phase. It adds sparse sender-side history observations that
survive the receiving Pico's reboot; it does not change transfer policy.
The authorized upgrade completed in 47.821911 seconds using normal verification.
Both Picos passed fresh full-slot CRC checks and Benji confirmed "Everything
works normally" for the full input/switching/arrows/zoom-assist checklist.
The profiling source is included in the accepted v0.111 change set above.
See [the timing baseline and diagnostic plan](docs/testing/transfer-profiling.md)
and [the revised future-project priorities](docs/future-projects.md).

All 61 deep-tier steps (including 141 updater tests) and the ARM build passed.
Frozen candidate: `build/releases/deskhop-v0.110-2ifjuswm/manifest.json` in
`/private/tmp/deskhop-transfer-profile.Zi4FBl`; full-slot CRC `56cf6a40`.
Evidence: `build/updater/runs/20260916T194457Z-1lktwwdf/` in that worktree,
including separate `user-acceptance.json`. Stock byte verification and unchanged
settings passed; B propagated/rebooted without a retry or power cycle.
Use its matching updater for the new history-event names; older host parsers
reject them. The v0.111 publication includes this matching updater.

The new events confirm A served legacy words: zero accepted page requests,
65,537 word requests, 36.129854 seconds from capability-response queueing to the
last word request. The 37.759860-second peer wait remains dominant. USB-host
startup blocks core 1 for 500 ms and is a timing lead, but does not by itself
explain the missing accepted page requests. See the investigation for the
next targeted negotiation/startup diagnostics; no transfer fix is claimed.

The initial hardware-free follow-up reproduced the source's words-only
history using a 500 ms core-1 pause plus real mouse-report traffic. Quiet
traffic alone did not produce that signature. A delayed-advertisement fixture
then completed all 1,024 pages without word fallback or page retries, with
exact image/settings checks. The new startup witness suite and real USB-host
timing assertions are retained in the tests; that investigation did not change
firmware policy. It recommended deferring only the firmware advertisement until
one second after boot, preserving the heartbeat task's other state synchronization.
This is a bounded direct-device startup mitigation, not universal USB readiness.
The now-authorized v0.111 implementation is described above.

## Current host updater policy: normal verification by default

The host-only updater simplification introduces `VERIFY_MODE=normal` (default)
and opt-in `VERIFY_MODE=thorough`; direct CLI calls use
`--verification-mode normal|thorough`. It does not change DeskHop firmware,
bump its version, reflash either Pico, or accelerate peer propagation.

Normal flashing retains the full firmware backup, all 4096 saved-settings
backup bytes, stock `picotool load -v` full byte verification, exact settings
readback, identity/USB/session/media gates, bounded peer rollout, core/history
checks, and one fresh expected-CRC scan on each Pico after reboot. It omits only
the duplicate host firmware readback and the wrong-CRC/repeated-correct-CRC
diagnostic scans. Thorough flashing retains the former complete sequence.

Use `make verify` for the normal read-only health check, or
`make verify VERIFY_MODE=thorough` for correct/wrong/correct CRC testing.
Neither read-only mode enters ROM, writes settings/firmware, requests a reboot,
or performs a ROM firmware readback. The mode does not relax failure handling:
no automatic retry, power cycle, device reselection or recovery bypass.

The original 134 updater tests passed (141 with v0.110 history coverage).
A normal read-only verification passed on both
installed v0.109 Picos in 3.367517 seconds, with one fresh CRC scan each and no
reboot or flash. The v0.110 upgrade above now validates actual-write normal mode
on hardware: 47.822 seconds versus the earlier v0.109 thorough run's 50.628.
Readback/diagnostic phase costs fell about 3.159 seconds; the peer phase did not
improve. This is a comparison across releases, not a controlled same-image trial.
The accepted v0.109 deployment below retains its original sequence and evidence.
See [the updater guide](docs/updater.md) and
[the normal-mode verification record](docs/testing/normal-upgrade-verification.md).

## Previous accepted release: v0.109

Implemented on `codex/configuration-validation-v0.109`, based on accepted v0.108,
in `/private/tmp/deskhop-config-validation.I4A81W`. It resumes the preserved old
draft without replacing the current USB, timer UI, verification, keyboard or
update fixes. All 61 deep-tier steps and the ARM build passed. Benji then
authorized an AFK deployment: both Picos now run firmware-verified v0.109.
After returning, Benji confirmed "Looks good; please merge and push" in response
to the requested typing, mouse/right-click, switching and config-page Read/Save
checks, accepting the release and authorizing publication. The contract and evidence are in
[the configuration-validation record](docs/testing/configuration-validation-v109.md).
Frozen manifest: `build/releases/deskhop-v0.109-6wekbwwa/manifest.json`
(in the canonical repository); full-slot CRC `9ea98c93`.
Deployment evidence: `build/updater/runs/20260916T181039Z-t7wuxvw8/` in the
canonical repository. The single maintained-updater run took 50.627909 seconds;
A's exact firmware readback and all 4096 saved-settings bytes passed, B propagated
and rebooted automatically, and both boards passed fresh correct/wrong/correct
CRC checks with progressing cores. No retry or power cycle was needed.
The automatic journal retains its original `input_acceptance=pending`; a separate
`user-acceptance.json` records Benji's later confirmation. The frozen deployed
image is unchanged by committing or publishing the source. The following v0.108
record is historical and does not supersede this release.

## Previous accepted release: v0.108

Both Picos were upgraded and verified on 2026-09-16 using the frozen v0.108
candidate. Benji then confirmed "Everything works normally" for typing and
modifiers, trackball and keyboard-generated right-click on both Macs, switching
both ways, focus arrows and zoom assist, and authorized merging/pushing to main.
This release includes the four v0.107 upstream fixes and the v0.108 diagnostic
freshness-contention repair. Configuration format 10 and UART frame version 1
are unchanged; QMK was not modified.

Evidence: `build/updater/runs/20260916T173830Z-j3n0vqi7/`. The ordinary maintained
updater completed in 50.815672 seconds, with stock picotool and no debugger,
retry, power cycle or cable move. A's full 262144-byte independent readback
matched the candidate and all 4096 settings bytes were unchanged. B's automatic
receive/reboot was observed, then both boards passed fresh correct/wrong/correct
CRC checks with advancing cores, stable new boot sessions and idle updates.
Full-slot CRC is `80c1302f`; boot metadata CRC is `c60094e1`.

See [the v0.108 deployment record](docs/testing/verification-contention-v108.md)
for exact identities, timing and acceptance limits. The USB-ROM hang remains
unresolved and is parked at Benji's request; this successful upgrade does not
prove it fixed. Batch transfer acceleration remains unproven. Configuration
validation is addressed by the accepted v0.109 release above; broad
reboot/power-loss safety is still outstanding.

The following development/deployment sections are historical. Statements there
that v0.108 was uninstalled, or that main/devices remained at v0.106, describe
those earlier checkpoints and do not override this current release record.

## v0.108 development and pre-deployment history

`codex/upgrade-reliability-v0.108` extends the uninstalled v0.107 upstream
integration with bounded asynchronous retries of the post-scan freshness check.
Transient lock contention must defer a result, not permanently turn it into
`UNVERIFIED busy`; real image changes and expired deadlines still fail closed.
The source investigation also separates this firmware bug from the earlier ROM
USB backup timeout. The repair was built/tested without hardware; the later
approved no-flash experiment is recorded below. Both stored images and `main`
remain v0.106; the old frozen v0.107 image is retained as historical evidence,
not replaced or relabeled. No main merge or push has occurred.

Implementation commit `34ed079` passed all 57 deep-tier steps (119 updater tests)
and the ARM build. The new frozen candidate is
`build/releases/deskhop-v0.108-07hkmacr/manifest.json`, full-slot CRC `80c1302f`.
This is the pending repair candidate, superseding v0.107 for future deployment.
It has not been installed or hardware-accepted. The production ROM transport
is unchanged. The approved one-shot no-flash experiment
`build/updater/runs/20260916T152856Z-uid-first-c26huglr/` read A's exact v0.106
image in 0.573 seconds without preceding `info`, then timed out during the sole
normal-reboot command. No firmware/settings writes or retries occurred. A's
serial port was absent afterward. Following the user's manual power cycle,
read-only recovery `build/updater/runs/20260916T153320Z-el69g00x/` passed the
complete correct/wrong/correct CRC sequence on both boards, plus sampled core
progress, identities, history, idle update and media checks. Both are back on
accepted v0.106. The original experiment remains failed; its reboot process
stalled on a preliminary four-byte ROM identification read before sending the
actual reboot command. This is not a validated workaround or permission to
retry flashing, and v0.108 remains uninstalled.

A subsequent approved debugger diagnostic
`build/updater/runs/20260916T155413Z-observe-stock-_3__pscp/` stopped at picotool's
`main+0` because the observer rejected extra shared-library breakpoint matches.
It never resumed picotool or sent a ROM command: this was a diagnostic setup
failure, not another hardware hang. A had already entered disk-free ROM, so the
user power-cycled both boards. Read-only recovery
`build/updater/runs/20260916T155907Z-392krms2/` passed the full verification
sequence on both unchanged v0.106 images. No firmware/settings writes or ROM
retries occurred. The corrected observer passed synthetic multi-location and
late-entry rejection tests plus exact entry/arming on stock picotool's non-USB
`help` path. The underlying intermittent hang remains unresolved.

The separately approved corrected diagnostic
`build/updater/runs/20260916T163257Z-observe-stock-andjfs10/` then passed its
exact v0.106 read, normal A reboot and application health checks. Independent
`verification/` passed the full correct/wrong/correct CRC sequence on both boards.
No firmware/settings writes or power cycle were needed. The capture showed no
halted endpoint or failed transfer, so it contains no failure-status reply.
The debugger slowed the read to about 2.2 seconds versus the earlier 0.57-second
uninstrumented read and may have masked a timing issue. This is not a hang fix
or acceptance of v0.108. No further live test or flash was attempted.

The subsequently approved failure-only diagnostic
`build/updater/runs/20260916T165604Z-error-only-69_ewv92/` also passed the exact
read (0.546 seconds), normal reboot and both-board application health without
triggering an error breakpoint. Its independent verifier passed both correct
CRCs, then A returned the known `UNVERIFIED busy` on the deliberate wrong-CRC
query; B correctly rejected that wrong CRC. The verifier stopped before the
final scan and remains failed. No firmware/settings writes, automatic retries
or power cycle followed. Both still run v0.106; the pending v0.108 contention
fix is not installed, and the USB-ROM hang remains unresolved.

The next explicitly approved batch
`build/updater/runs/20260916T171133Z-error-batch-9x5q9iao/` completed all five
no-flash exact reads, normal A reboots and both-board health checks. Ten stock
processes exited zero without triggering the error observer; reads took
0.544–0.555 seconds. A's final boot is `eadf5e163654fec3`; B stayed
`8d9032bbb7032771` throughout. The separate final verification then passed A's
correct CRC but stopped on B's `UNVERIFIED busy`, with no retry. All five ROM
cycles are complete, but verification and the overall batch remain failed.
No firmware/settings writes or power cycle occurred; no extra cycle or hardware
command followed. This is additional successful diagnostic evidence, not a
USB-hang fix or hardware acceptance of v0.108. The five-cycle authorization is
exhausted.

See [the v0.108 repair record](docs/testing/verification-contention-v108.md)
and [the ROM backup investigation](docs/testing/rom-backup-timeout-investigation.md)
for validation, boundaries and the no-flash experiment evidence.

## Historical candidate: upstream fixes for v0.107 (released in v0.108)

`codex/upstream-fixes-v0.107` is a separate candidate based on accepted `main`
`1da1af2`, incorporating upstream through `c220d0c` selectively: #369's
byte-safe USB DPRAM copies, #370's PIO USB transaction retries, the remaining
#359 per-report keyboard layouts, and #364's screensaver timer units in seconds.
The USB vendor fixes are unchanged from upstream; keyboard capacity guards and
exact timer conversion are adapted to this fork. The custom hotkeys, keyboard
LED indication, zoom assist, keep-awake controls, UART framing and updater are
retained. Existing timer durations are not migrated or rewritten.

This is not a new hardware-accepted publication: `main` and both stored images
remain v0.106. The authorized flash attempt failed before writing, during the
first ROM backup read. After a power cycle both applications returned and passed
a fresh full-slot CRC scan, but the complete verification sequence remains
blocked by A's intermittent post-scan `busy` verdict.
No firmware write, retry, main merge or push has been performed for this candidate.
The integration scope, test evidence, protocol limits and required physical
acceptance checks are recorded in
[the v0.107 integration record](docs/testing/upstream-fixes-v107.md).
All 55 deep-tier steps and the ARM build pass. Frozen candidate:
`build/releases/deskhop-v0.107-607q82je/manifest.json`, full-slot CRC `6579b48f`.
Use this explicit manifest for a subsequently authorized flash; the canonical
checkout's earlier `latest.json` pointer is not this candidate.

Failed attempt: `build/updater/runs/20260916T144538Z-wt0lug3l/`, first backup
timeout after 10.127 seconds (picotool exit 157). Identity/session pinning passed;
`write_started=false`, `reboot_requested=false`. CDC retention, libusb debug and
pinned selection did not eliminate the intermittent ROM read failure.

Recovery checks `20260916T144742Z-mkmywcjh` and `20260916T144841Z-8bmnj2nl`
under `build/updater/runs/` issued no write/reboot. The second run passed both
v0.106 images against CRC `68eba065`, then both negative controls, but A's final
correct-CRC scan returned `UNVERIFIED busy`; both journals remain failed. Both
applications have stable new boot sessions and advancing cores. Source review
identified a post-completion nonblocking-lock recheck that latches transient
BUSY as a terminal verification failure. This is distinct from the ROM backup
timeout. No further flash or verification loop was attempted; v0.107 remains
uninstalled. See the detailed integration record above before further writes.

## Previous publication: accepted v0.106

Benji confirmed "looks good" after the requested physical input checks and
authorized committing, merging and pushing. `main` now includes the operationally
accepted v0.105 batch implementation plus the version-only v0.106 bump actually
running on both Picos, and the hardware-exercised pinned stock-picotool updater.
The source firmware is unchanged from the frozen v0.106 candidate. No extra
flash accompanies publication. Batch use/speedup remains unproven; this merge
does not convert successful deployment into a performance claim.

The detailed current evidence is in
[the v0.106 hardware record](docs/testing/batched-transfer-hardware-v106.md).
Clean integration validation passed all 42 fast-tier steps (118 updater tests
included) and a fresh ARM build. Rebuilt BIN and UF2 match the installed frozen
v0.106 candidate byte-for-byte. The host fix is `b1d1c8b`; the accepted version
bump/hardware record is `40d4c8a`.
Earlier deployment/publication sections below are historical; their then-current
version, acceptance and branch statements do not override this section.

Still outside this integration: unfinished configuration-validation changes
in its worktree; broader reboot/power-loss safety work; the v0.107 upstream
candidate described above; and proposed batch-mode,
negotiation/retry/fallback diagnostics. The old upstream replay/selective
branches are superseded alternatives, not additional fixes to merge. Seventy
untracked ` 2` copies match historical `03d85db` blobs exactly and remain
untouched; they are not unique work and were excluded from validation/publication.

## Latest upgrade: pinned updater succeeded; both Picos verified on v0.106

Benji subsequently authorized integrating the successful no-flash procedure
into the maintained updater, adding safety regression tests, and retrying the
prepared v0.106 upgrade. This supersedes the earlier diagnostic-only boundary
below. After deployment and verification, Benji confirmed "looks good" and
authorized committing, merging and publishing the device-tested changes.
The host change establishes physical identity with one command, binds the
observed USB session, uses stock bus/address selection for later commands, checks the pin
before every operation and after reads/writes, and expires it on failure or
reboot. It preserves backups, independent readback, unchanged-settings checks,
fresh both-board diagnostics, disk-free mode and the no-retry policy. All **118
updater tests** pass, including 36 platform tests for malformed/ambiguous identity,
changed connections, expired sessions, transport failures and no retries. An
independent safety review found no blocker. The frozen version-only v0.106
candidate remained unchanged; no firmware rebuild was needed for host edits.

The authorized upgrade, `build/updater/runs/20260916T011723Z-s6a3wjja/`, completed
one serial ROM entry, one identity command, firmware/settings backups, verified
load, exact independent readback, unchanged settings and normal reboot. Every
post-identity ROM command used pinned bus/address selection. Firmware backup
took 0.559 seconds, load/verify 3.948 seconds, independent firmware readback
0.558 seconds, and reboot 0.011 seconds. The normal application/peer-watch phase
began 7.567 seconds after preflight. No power cycle, cable move or ROM retry was
needed. A's settings SHA256 stayed
`aa816af793193a7ce487d391b49bf45d514dde09bd77d28255eb782e82a85464`.

B automatically propagated v0.106 and rebooted. The first full-slot scan
passed on both boards, but A returned `UNVERIFIED busy` during the intentionally
wrong-CRC scan (B correctly returned `FAIL crc_mismatch`). The original run
therefore remains **failed at diagnostics**, elapsed 48.332 seconds, not
relabeled as a fully successful run. Separate read-only verification in
`build/updater/runs/20260916T011840Z-krk7l5zw/` passed in **5.860 seconds**:
fresh correct/wrong/correct checks on both boards, full-slot CRC `68eba065`,
metadata/boot CRC `2cd31c9d`, exact identities, stable boot sessions and advancing
cores. It issued no flash or reboot. Current sessions are A `3ad7279fab2beea7`
and B `3da3479a30e83d0f`. Benji confirmed "looks good" after the requested input
checks. This is operational acceptance, not evidence of a batching speedup.

**Batch acceleration is not yet demonstrated.** The 64 same-attempt receiving
samples span 255,744 bytes in 34,478 ms (about 7.4 kB/s). None of their 2,016
ordered sample pairs exceeds the conservative legacy bound below; even the
largest margin is negative (-4,976 bytes). This is inconclusive about actual
batch use or fallback, not positive evidence of either. B's complete settle
phase took 37.347 seconds. Investigate negotiation and/or pacing before claiming
a speedup. Benji has now authorized merging the operationally accepted v0.106
implementation despite this explicitly documented performance uncertainty.
The host fix is being committed separately before firmware integration.

Read-only source review found no intentional version/role gate excluding this
105-to-106 transfer. Possible silent fallback paths are an unanswered single
capability probe after 100 ms or exhaustion of three page attempts. Normal
queued UART traffic takes priority over batch data, and source lock/flash timing
also affects throughput. These are hypotheses, not diagnosed causes. The next
useful firmware test should expose batch mode, negotiation, page retries and
fallback reason/counters in status; another uninstrumented version bump would
not distinguish them. No such firmware change or further flash was performed.

## Prior hardware diagnostic: pinned USB read and reboot succeeded once

Benji approved one no-flash experiment using stock picotool: identify A by its
physical flash UID, pin that ROM USB session, then select its bus/address for
one full-slot read and one normal reboot. The one-shot helper is retained at
`build/updater/probe_bus_address_once.py` (SHA256
`6de617deba82b2031627bad10dc8cedb5e2f14b011e0dc50398acb47939a911d`).
This is an experiment, not a change to the maintained updater. It retains CDC,
child-only `LIBUSB_DEBUG=4`, identity/media checks and the deployment lock.

Evidence: `build/updater/runs/bus-address-once-20260916/`. After one
`info -a --ser E6654854574C3E30`, stock picotool read all 262,144 firmware bytes
using `--bus 2 --address 5` in **0.557 seconds**. The read exactly matches frozen
v0.105, SHA256
`f4ac4870a80e0b7fc405cfe03bde67c4a117e75154e6710e12592f28b8443580`.
Normal reboot with the same selector succeeded in **0.011 seconds**. Registry
ID `0x100010224`, session `2306720922114`, location `34734080` and USB address
`5` remained unchanged in all four pin snapshots. These values apply only to
this USB session and must never be reused as persistent board identities.
There was no load, firmware/settings write, ROM-command retry or power cycle.

The original experiment remains marked failed because its final recovery
verification returned A `UNVERIFIED busy`, not a checksum mismatch; B passed.
A separate read-only verification,
`build/updater/runs/20260916T010755Z-uqvk8ve1/`, completed in **5.821 seconds**.
Both v0.105 images passed fresh correct/wrong/correct full-slot checks with CRC
`e4843d6a`, metadata CRC `6bffee14`, exact identities and advancing cores.
A's boot session changed to `e5a19899118972cf`; B's remained
`bdc0c71e9600a24b`. Both applications are restored and firmware-verified;
physical input acceptance remains separate.

This is promising evidence for identifying once and pinning the USB session,
not proof of a reliable fix. The probe also inserted identity/media checks:
identity completion to read start was about 87 ms, versus about 1 ms in the
failed attempt below. Thus this experiment does not isolate selector choice
from timing effects. Pins are checked observations, not an atomic held USB
handle; changed enumeration must abort. No production updater modification
or further flash is implied by the experiment's approval. Safely integrating
and testing session selection would precede a separately authorized upgrade.
The v0.106 candidate has **not** been installed, physical batching remains
untested, and published `main` remains limited to v0.104.

## Previous hardware test: backup timeout; unchanged v0.105 recovered and verified

Benji requested testing v0.105's batch path after publishing only v0.104.
Fresh read-only verification of both v0.105 images passed in
`build/updater/runs/20260916T004627Z-vpogrmr5/` (5.907 seconds). An isolated
worktree, `/private/tmp/deskhop-v105-hwtest.gUovwq`, on
`codex/test-v105-batch` changes only the version from 105 to 106 relative to the
batching branch. This makes the native newer-version rule trigger propagation;
it adds no feature, instrumentation, downgrade or safety bypass. Fast validation
and ARM build passed. The frozen candidate is
`build/releases/deskhop-v0.106-m7cxjizq/manifest.json`, full-slot CRC `68eba065`.

The authorized test attempt, `build/updater/runs/20260916T004919Z-9jsqm65k/`,
accepted one serial `bootloader A`, observed disk-free ROM and independently
matched A's physical UID. Its first backup then timed out after 10.074 seconds,
exit 157 (`RP2040 unknown error`), despite retaining CDC and child-only
`LIBUSB_DEBUG=4`. Total run time was 10.746 seconds. The journal records
`write_started=false`, `reboot_requested=false`, and failure at `backing_up`.
No firmware/settings write, load, propagation or retry occurred.

After failure A remained in disk-free ROM and `/dev/cu.usbmodem21203` was absent;
read-only USB inspection confirmed one vendor interface and active/nonbusy Mac
media clients. No automatic software restoration or flash retry was attempted.
Neither Pico was upgraded to v0.106, and the batch feature has still not been
exercised on hardware. **The stock-debug invocation is not a reliable remedy
for the intermittent ROM transfer timeout.**

Benji subsequently power-cycled and reconnected both sides. Initial read-only
recovery run `build/updater/runs/20260916T005106Z-r9_fwn_7/` stopped with B
`UNVERIFIED busy`, not a checksum mismatch. A separate fresh read-only run,
`build/updater/runs/20260916T005207Z-6rnyatsw/`, passed in 5.844 seconds: both
v0.105 images still match full-slot CRC `e4843d6a`, metadata CRC `6bffee14`,
correct/wrong/correct expectations, exact identities and progressing cores.
No firmware write or reboot was issued in either recovery check. New boot
sessions are A `614c4ce949545922` and B `bdc0c71e9600a24b`. Recovery is now
firmware-verified; physical input acceptance remains separate. Do not blindly
repeat the unchanged failed upgrade sequence.

For a future successful run, retain B's pre-reboot status samples from the
updater journal. With matching boot session, update attempt, peer source,
target version and receiving phase, an interval satisfying
`delta_received > 16 * (delta_uptime_ms + 2) + 512` exceeds the legacy protocol's
maximum progress and proves actual batch use. This follows from one four-byte
request per 250-microsecond updater task, no scheduler catch-up, page-rounded
progress and a frozen same-core status snapshot. It does not prove every page
used batching or that no fallback occurred. No additional serial observer is
needed. Keep `main` at v0.104 pending hardware results and user input acceptance.

## Historical publication boundary: main was v0.104; devices ran v0.105

Benji requested merging only physically tested changes and explicitly limited
`main` to v0.104. The completed dependency chain through `2509929` is included:
hardware-free testing and input fixes, keyboard-state recovery, UART integrity
and its v0.102 migration, the Bootloader button fix, serial bootloader commands,
and the reusable updater. Firmware/build/vendor/disk inputs match the accepted
v0.104 snapshot exactly (full-slot CRC `befb208b`, metadata CRC `4f648cd9`).

The host-only stock-picotool workaround is included as `499ee96`, cherry-picked
from `07fa0b4`. It retains CDC through ROM operations/reboot and enables
`LIBUSB_DEBUG=4` only for stock picotool children. It has been exercised on the
physical devices; no firmware changes are required. All 105 updater tests pass.

v0.105 batching (`0245392`) remains on `codex/batched-firmware-transfer`, not
merged into `main`. Although both Picos have v0.105 installed and verified, the
first upgrade used the old v0.104 receiver's legacy transfer path. The new
batch-transfer behavior still needs a physical new/new upgrade before merging.
The branch retains the detailed deployment record in
`docs/testing/batched-transfer-v105.md`.

This publication does not flash or downgrade either Pico. Both still run
v0.105, full-slot CRC `e4843d6a`, metadata CRC `6bffee14`. The normal updater
refuses a v0.104 downgrade; do not bypass that guard. Latest flash evidence is
`build/updater/runs/20260916T003027Z-v6d93apw/`: backup, verified write, independent
readback, unchanged settings, normal reboot and automatic peer propagation
completed. Its final diagnostic stopped on A `UNVERIFIED busy`; the separate
read-only run `build/updater/runs/20260916T003133Z-r795gjiq/` then passed fresh
correct/wrong/correct full-slot checks on both Picos. The original failed result
is preserved, not relabeled. Physical input acceptance remains separate.

Also excluded: unfinished configuration-validation work in its own worktree,
broad reboot-safety work, superseded selective/replay integration alternatives,
and newly fetched upstream `bff4d0c` (the integrated upstream baseline remains
`ce8abb6`). These are not hardware-tested additions to this publication.

Duplicate files with ` 2` suffixes appeared during the earlier branch operation;
70 were byte-identical to their corresponding files when inspected. They were
left untracked and untouched, not included in any commit. Review them before
preparing a new release, because source snapshots include untracked inputs.

The deployment notes below retain historical evidence; this section supersedes
their then-current branch, hardware and acceptance statements.

## Repository-owned updater (verification remains mandatory)

Use the root Makefile and `scripts/update_firmware.py` for future upgrades;
release-specific scripts under `build/flashing/` are retained historical evidence,
not the maintained entry point. `make` shows help. `make release` tests/builds
and freezes a validated candidate; `make flash-plan` previews without touching
USB. An explicitly requested `make flash` performs serial ROM entry, UID-targeted
backups, stock verified load, unchanged-settings checks, normal reboot, bounded
peer propagation and fresh both-board verification. Normal mode is now the
default; `make flash VERIFY_MODE=thorough` also retains the duplicate firmware
readback and correct/wrong/correct diagnostic sequence. `make verify` is read-only
in both modes. The new normal-mode path is not yet hardware-tested.

The profile in `config/updater.json` pins A/B's physical UIDs and A's current
callout port. See [the updater guide](docs/updater.md) for tool paths, explicit
frozen-manifest selection, manual disk-free entry, compatibility limits and
failure recovery. Do not use mass-storage ROM, `reboot -u`, or automatic retries.
Normal upgrades require a candidate newer than both boards; an already-current
pair is verified without rewriting. UART framing changes require a separate
migration, not this automatic workflow.

Evidence is retained under `build/releases/` and `build/updater/runs/`, including
the selected verification mode. Both modes keep the backups and programmed-image,
settings, identity and health checks described above; thorough mode collects
additional diagnostic evidence. Preparation can reuse the same validated
candidate when inputs are unchanged. Historical live upgrades exercised the
sequence now called thorough, including independent firmware readback. Offline
test doubles alone are not evidence of physical USB/ROM behavior. Ask Benji to
check normal input after any future successful device verification.

Host-only validation on 2026-09-15 passed all 99 updater tests, all 40 fast-tier
steps, ARM configuration/build, default Make help, and hardware-free preview.
A second preparation reused the validated candidate. The frozen
`build/releases/deskhop-v0.104-0x2bs1w1/manifest.json` still identifies full-slot
CRC `befb208b` and BIN SHA-256
`32ebaeada1de04f5bc347caa0f4b5d6173e1afe433a9a86dc2d1d7c00ad241e8`,
identical to the accepted deployment. No USB inspection, serial command or
picotool invocation against hardware occurred in this validation.

On this Mac, picotool is currently not on `PATH`; pass `PICOTOOL` explicitly
for a future authorized flash/verify. The previously used installation is
`/Users/benji/.codex/worktrees/9235/DeskHop/build/tools/picotool-2.3.1/picotool/picotool`.
That external tool dependency is not hardcoded in the maintained Python scripts.

### First live updater run: already-current pair, 2026-09-15

On Benji's subsequent "Flash" request, `make flash PICOTOOL=…` reused the frozen
v0.104 candidate and correctly took the already-current verification path. No
bootloader command, flash write, or reboot occurred. The first run stopped on
A's `UNVERIFIED reason=busy` after reading all 262,144 bytes; B passed. Evidence:
`build/updater/runs/20260915T210348Z-a826uiym/`. The post-scan firmware recheck can
conservatively invalidate a complete scan on a single failed nonblocking lock
acquisition. This was not a reported checksum mismatch, and no safety check was
bypassed or flash command retried.

A separate read-only `make verify PICOTOOL=…` then completed in 5.617 seconds.
Both boards passed the full correct/wrong/correct verification sequence with
CRC `befb208b`, unchanged boot sessions and advancing cores. Combined history
checks and unchanged active/nonbusy Mac media checks passed. Evidence:
`build/updater/runs/20260915T210433Z-y3kn8rst/result.json`, with
`firmware_verified=true`, `write_started=false`, and `reboot_requested=false`.
At this stage only the live diagnostic path had been exercised; serial ROM
entry, upload and auto-propagation were tested subsequently as recorded above.
No firmware or QMK changes were needed for this diagnostic run, and no new
user-input acceptance was claimed.

## Accepted v0.104: serial bootloader entry (physical command check pending)

`codex/serial-bootloader` adds `bootloader A` and `bootloader B` to the production
USB serial console. These commands select the **physical Pico role**, not the
focused Mac or the console's local/remote position. No keyboard gesture is needed
once this firmware is running. Both boards now execute v0.104. A was flashed after
the user pressed Layer 3 A; full external readback matched and saved settings were
unchanged. B automatically updated from v0.103 without a power cycle or cable
move. Fresh full-slot scans on both boards match `befb208b`, with both cores
advancing. Benji confirmed "Everything is working fine" after the requested
typing, trackball-button and switching checks. Actual serial-command bootloader
entry remains separately untested; no `bootloader` command was sent during this
deployment.

The command only enters disk-free USB ROM/PICOBOOT (`reset_usb_boot` interface
disable mask `1`). It does not transfer firmware. Picotool must upload through
the selected Pico's computer-facing USB cable on that cable's Mac. In particular,
`bootloader B` from A's console does not make B flashable through A's UART.
The usual path is to enter A, flash/verify/reboot A, then observe and verify B's
automatic update. The initial v0.103-to-v0.104 update used the existing keyboard
bootloader entry method; v0.103 does not understand the new console command or
its new remote request. There is no fallback to the legacy immediate-reset packet.

Local entry waits for actual USB completion of the reply and UART drain. Remote
entry uses a dedicated, correlated request/ACK; `peer_admitted_not_boot_proof`
means acceptance, not observed ROM enumeration. An absent, old, or silent peer
can produce `unconfirmed`. A timeout or terminal close cannot retract a request
already transmitted, so inspect the target before retrying. The command rejects
active/dirty updates, existing reservations/reboots, and recently served firmware
word requests (a conservative three-second source-side holdoff). The holdoff is
not proof that an offline or long-paused peer has a clean image. Reservations
exclude new firmware-update claims while the bounded maintenance operation waits.

See [serial maintenance details](docs/diagnostics.md#serial-maintenance-v0104-deployed-physical-command-acceptance-pending)
for response meanings, timing, cancellation, compatibility, and coverage boundaries.
The [v0.104 deployment record](docs/testing/serial-bootloader-v104.md) records tests,
artifact identity, readback/propagation evidence and pending hardware acceptance.
The flashed source was an uncommitted snapshot on `codex/serial-bootloader`, based
on `216a7f8`; it is not attributed to that base commit alone. This changeset
publishes the unchanged runtime/build sources from that snapshot. Exact files,
including new source files, and the patch are retained with the frozen image.
Saved configuration, UART-v1 framing, QMK, and ordinary diagnostic commands are
unchanged; the original config-validation and broad reboot-safety work remains separate.

## Previous deployment v0.103: Bootloader-button fix (input acceptance pending)

`codex/bootloader-button-fix` corrects the Web Config Bootloader button's boolean
payload and the missing firmware command allowlist entry. The firmware retains
the peer request through a full queue and defers local ROM entry until queued
UART traffic, DMA and the UART FIFO/shifter have drained. An active update or
dirty firmware image on the initiating Pico blocks this path. Actual-click/mock-HID
and 20 paired-production regressions cover the complete path in both test tiers;
generated pages and the embedded image are updated together. No remote execution
acknowledgement is added.
See the [v0.103 fix record](docs/testing/bootloader-button-v103.md) for validation,
artifact identity and safe hardware acceptance steps. UART/configuration formats,
settings and QMK are unchanged. Both Picos now run v0.103 from source `bbac34f`.
A's full external readback matched the artifact and its saved settings were
unchanged. B automatically updated from v0.102 without a power cycle. Fresh
full-slot scans on both boards matched `0c2fdeb0`; both-core progress passed.
User input/switching checks and a controlled physical Bootloader-button test
remain pending. v0.102 below is the previous user-accepted deployment;
the config-validation and reboot-safety fixes remain separate unfinished work.

## Previous accepted firmware: v0.102 completed fixes

The user requested publication and deployment of the completed fixes. The
`codex/release-v0.102` branch contains keyboard recovery `b8992dc`, followed by
UART integrity `5499e8f`, plus a release version bump. Both fix branches have
been pushed separately to the fork. Configuration validation (#4) and reboot
safety (#1) are **not included**. Main is unchanged.

See the [v0.102 release record](docs/testing/release-v102.md) for validation,
artifact identity, known limitations, and the required two-board migration.
Fresh release validation passed: fast 38/38, deep 46/46, ARM 2/2. The frozen
artifact's full-slot CRC is `dabb9b75` (boot metadata CRC `d9e9f64d`); the
hardware check is `verify 0.102 dabb9b75`.
**Both Picos were independently programmed, readback-verified, and normally
rebooted on 2026-09-15.** All firmware bytes matched; saved settings were unchanged.
The paired console check passed: both execute v0.102, have fresh stable boot
sessions and advancing cores, and pass fresh full-slot scans over the new UART
link. A repeat check passed after B's host cable was returned to its original
Mac. Benji confirmed "Working great" in response to the requested typing,
trackball/right-click, and switching checks on both Macs. v0.102 is now the
user-accepted deployment. LEDs/arrows, zoom assist, keep-awake soak and future
automatic firmware updates were not separately accepted in this check.

The draft notes below describe the original unversioned commits; v0.102 supplies
their required release version. Release source/artifact commit: `fea46a8`.

## Historical UART command integrity draft (released in v0.102)

The second ordered bug-fix draft branches from keyboard draft
`b8992dc16370bbb1ce00a238463ec6e8207d0883` on `codex/uart-command-integrity`.
See the [transport/migration note](docs/testing/uart-integrity-draft.md).
UART now protects version, length, type and payload with CRC32 and resynchronizes
after malformed frames. There is no legacy UART fallback. Initial rollout must
deliberately program both boards independently; subsequent compatible releases
retain protected automatic peer updates. The Web Config page uses a separate
12-byte CRC8 format. This draft preserves the keyboard state envelope and keeps
firmware 0.101/configuration 10: **UNFLASHABLE as a release** until the stack's
explicit version/migration plan. The v0.102 release above completes that migration.
Validation passed: fast 38/38, deep 46/46, ARM 2/2, 12 UART integrity scenarios,
72 inherited keyboard cases, and the additional queue/remount scheduling checks.

## Historical keyboard reliability draft (released in v0.102)

The first bug-fix-stack draft starts at `af100bc` on
`codex/keyboard-state-recovery`. See the [design and test note](docs/testing/keyboard-reliability-draft.md).
Ordinary releases now have a durable USB tail and session/focus-checked peer
snapshots. A 500 ms peer lease bounds remote holds during link outages; a 20 ms
synthetic lease repairs a lost lock-shortcut all-up. Keyboard backpressure no
longer requests a reboot. Mouse timeout/reboot safety is a later stack task.
This RAM-only draft keeps version 0.101/configuration 10 and is **not for rollout**;
its keyboard protocol requires both upgraded boards and a deliberate release
version bump. The v0.102 release above supplies both.

## Previous accepted deployment: v0.101 full-slot verification

Both boards ran v0.101 and completed input acceptance before v0.102. The
[deployment record](docs/testing/verification-v101.md) documents the new
`verify <build> <crc32>` command, native validation, and physical evidence.
It checks both boards using fresh incremental flash scans, executing-build
metadata, and both-core progress. Use the **full-slot CRC** from the artifact
manifest; the boot metadata CRC in `status` covers a different range. Results
explicitly describe scan snapshots.

Frozen artifact: `build/flashing/deskhop-v0.101-verification.uf2`, SHA-256
`571e1b24d59e36da1dfce58a05c26bdefdb4c2312e5a4982424f1c0b31c3ec87`.
The command is `verify 0.101 2db89640`; separate boot metadata CRC is
`be404f8f`. The source/artifact manifest is `build/flashing/v101-candidate.json`.
All 46 deep steps, six coverage layers, final affected checks, and ARM build
passed.

Pico A was flashed through disk-free PICOBOOT and normal reboot requested at
2026-09-15 09:07:13.250085 UTC. All 262,144 firmware bytes matched an independent
readback, and all 4,096 saved-settings bytes were unchanged. Only the vendor
bootloader interface (class 255) appeared. No RP2 boot object remained after
reboot, and the same 20 Mac media clients including subclasses (five direct
clients) were active and nonbusy.

A 44-status rollout capture over 23.223 seconds observed B receiving target
v0.101, reaching `reboot_pending`, then starting a new boot and advancing both
core counters to `update=confirmed`. There was one bounded peer timeout during
the transition. A's session is `37dad6e6dd90a647`; B changed from
`67023817d0aa5245` to `049fe6e4d5d3495e`. UIDs remain `E6654854574C3E30` (A)
and `E6654854577F2330` (B).

The subsequent serial smoke check passed in 5.537 seconds (11,448 bytes): five
statuses, two combined histories retaining nine A and three B rows without gaps
or overwrites, and three fresh image checks. Correct/wrong/correct expected CRCs
produced PASS/FAIL/PASS on both boards; every full-slot scan measured `2db89640`
with stable generations and both cores advancing. Scan durations were
1.046346–1.067426 seconds. B's complete flash was measured by its own firmware;
it was not read externally with picotool.

Benji confirmed "Everything works" after the requested typing, trackball/buttons,
and Layer 3 S switch to the other Mac and back check. Replugging was not separately
reported. No push has been performed.

## Previous accepted deployment: v0.100 update and core observations

The [deployment record](docs/testing/update-observations-v100.md) describes new
status rows for both core checkpoints and updater state, sparse history events,
and query-derived boot/update observations. The existing dual-core roles remain.
All 41 deep-tier steps, all six native coverage layers, final affected-suite
rechecks, and the ARM build passed. The frozen UF2 is
`build/flashing/deskhop-v0.100-update-observations.uf2`, SHA-256
`70f7d258d66e804ecd32596f70e42fbf00f052d67b2d7eaf55619dcf3ad16565`,
with boot metadata CRC `8397b53c`. Its source/artifact manifest is
`build/flashing/v100-candidate.json`. Pico A was flashed and normally rebooted
at 2026-09-15 00:46:02 UTC. Full independent A firmware readback matched;
all saved settings were unchanged. Disk-free enumeration and Mac media checks
passed. Two early serial snapshots showed B's old v0.99 session through legacy
status fallback; seven subsequent snapshots showed both running v0.100 with
stable new sessions and both core counters advancing. A session is
`e37cfda649031433`; B changed from `5b8f298bd669b78a` to `67023817d0aa5245`.
Both report boot metadata CRC `8397b53c`. Four combined histories retained eight
A and three B rows, including the new peer boot/progress observations, without
gaps or overwrites. The checker passed in 5.906 seconds (13,385 bytes).
Benji confirmed "Everything works" after the typing/trackball/buttons and
Layer 3 S switch-and-back check on both Macs. The initial
rollout correctly reports `update=not_observed`: v0.99 supplied no pre-update
runtime/target information. This does not independently verify B's flash.
That release was locally checkpointed as `989192b`; it did not independently
verify the full images. The v0.101 deployment above adds that assurance.

## Previous accepted deployment: v0.99 peer history

`history [count]` collects both boards by default and interleaves their fixed
snapshots, labeling every row A or B. Timing across boards is approximate;
each board's sequence stays ordered. A missing peer still permits local
history. The [deployment record](docs/testing/peer-history-v099.md) describes
the format, bounds, protocol, and validation. v0.98 was accepted and locally
checkpointed as `8560d5f`; it is the previous accepted release.
v0.99 passed all 39 deep-tier steps, six native coverage layers, and
the ARM build. The frozen UF2 is
`build/flashing/deskhop-v0.99-peer-history.uf2`, SHA-256
`0aa74de576024944c7c8195be922bd5dcc48c7f9826b5c528884381f127db9d7`,
with boot CRC metadata `ce70e3d6`.

Pico A was flashed and normal reboot requested at 2026-09-15 00:09:48 UTC
(September 14 locally), after Layer 3 A entered disk-free PICOBOOT. Only the
vendor interface (class 255) appeared, with no mass-storage interface. Its old
image exactly matched v0.98; all 262,144 new firmware bytes matched an
independent readback and all 4,096 saved-settings bytes were unchanged. Mac
checks before and after found five active, nonbusy media clients and no RP2
boot object after reboot.

Seven serial status snapshots all returned `peer=ok`:

| Board | Physical flash UID | Executing build | Boot session | Sampled uptime range |
| --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `0.99` | `bf0cb9dbdd5d6c39` | 34,948–39,868 ms |
| B | `E6654854577F2330` | `0.99` | `5b8f298bd669b78a` | 14,192–19,112 ms |

Both reported boot CRC metadata `ce70e3d6`. Four combined-history responses
across default/16/64 counts and terminal reopen returned the same five A
records and three B records, with matching boot sessions and no overwrites or
gaps. A recorded boot at 17 ms, USB mount at 295 ms, and HID interfaces at
543/548/555 ms. B recorded boot at 35 ms, USB mount at 443 ms, and its trackball
HID interface at 576 ms. B's later boot put these A events before its B events
in these initial merged lists. B's own history
and executing identity are now read remotely, while its flash integrity remains
independently unverified.

The checker passed in 5.731 seconds with 8,577 received bytes, including
fragmented/queued commands, invalid-count rejection, brief read pauses, and
reconnect. Benji confirmed "Everything works" after the requested typing,
trackball/buttons, and intentional Layer 3 S switch-and-back check on both Macs.
Replugging was not separately reported. A subsequent read-only status/history
capture at 00:12:17 UTC passed with the same builds and sessions. `history 16`
returned 13 A and 11 B records, including eight output changes per board:
both directions and both local and peer events on each. These physical rows
interleaved with no overwrites or gaps; the peer capture bound was 24,320
microseconds. Approximate ages do not prove cross-board causality or attribute
every transition to a particular input action. Evidence is in
`build/flashing/console-v099-switch-check.json` and its `.txt` transcript.
The v0.95–v0.99 changes remain unpushed.

## Previous accepted deployment: v0.98 local RAM history

Pico A was flashed and rebooted on 2026-09-14 at 21:28 UTC using frozen
`build/flashing/deskhop-v0.98-history.uf2` (SHA-256
`2214e8a1e206f3e6b7e14c7b69f0b4560842cbd1f8c6bab7ee8d6644b917bf55`).
Layer 3 A entered disk-free PICOBOOT: vendor interface class 255, no
mass-storage interface. All 262,144 firmware bytes matched an independent
readback, and all 4,096 saved-settings bytes remained unchanged. The Mac had
five media clients, all active and not busy, before and after; no RP2 boot
object remained after reboot.

All seven serial status snapshots returned `peer=ok` and increasing uptimes:

| Board | Physical flash UID | Executing build | Boot session | Sampled uptime range |
| --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `0.98` | `4d718a3937cb36d7` | 31,765–35,140 ms |
| B | `E6654854577F2330` | `0.98` | `19877e4a63cdaeae` | 11,086–14,461 ms |

Both reported boot CRC metadata `4e15626f`. B's executing identity/build is
confirmed; there was no independent B flash readback or integrity check.

This slice adds `history [count]` for the connected board, backed by 64
compact records in RAM. Each event/gap row names its board; closing the
terminal preserves the history, and reboot clears it. Peer history follows
in the next slice. Four serial history responses, including after reconnect,
returned the same six A-tagged records: boot at 17 ms, USB mount at 268 ms,
HID interfaces 0/1/2 at 543/548/555 ms, and a peer output change A to B at
841 ms. The overwrite count was zero. No B history was retrieved.

Fragmented and queued status commands, invalid count rejection, brief read
pauses, and terminal close/reopen passed. Standard macOS `/usr/bin/screen` at
115200 also displayed help, the same local history, and two-board status with
the same sessions, then closed normally. Benji confirmed "looks good" after
the requested typing, trackball/buttons, and Layer 3 S switch-and-back/history
check. Those later history rows were user-reviewed; replugging was not
separately reported.
See the [deployment record](docs/testing/history-v098.md).

The 36-step deep tier, all six native coverage layers, and ARM build passed;
v0.98 has completed user input acceptance.
The v0.95–v0.98 changes have not been pushed.

## Previous accepted deployment: v0.97 peer status

Pico A was flashed and rebooted on 2026-09-14 at 21:03 UTC using frozen
`build/flashing/deskhop-v0.97-peer-status.uf2` (SHA-256
`dfd8ea758d430dd2683186de12b69006b4543d0410e88d74dcc2de20cea5c375`).
Layer 3 A entered the disk-free PICOBOOT path: only the vendor interface
appeared, with no mass-storage interface or additional media client. Official
picotool 2.3.1 selected A's flash UID. Its old firmware matched v0.96; after
loading, all 262,144 new firmware bytes matched an independent readback, and
all 4,096 saved-configuration bytes remained unchanged. The post-reboot Mac
check found no retained RP2 object or inactive/busy media client.

`status` now queries both boards through a bounded, request-correlated UART
exchange. The connected board prints first, followed by the peer's own
identity/build/session/uptime or an explicit error. The first query reported
A running v0.97 and `peer=timeout_or_unsupported`. A subsequent query returned
B running v0.97. At 21:06 UTC, all six serial smoke snapshots returned
`peer=ok` with these stable identities and increasing uptimes:

| Board | Physical flash UID | Executing build | Boot session | Sampled uptime range |
| --- | --- | --- | --- | --- |
| A | `E6654854574C3E30` | `0.97` | `4d4f8996e257d47d` | 187,376–189,571 ms |
| B | `E6654854577F2330` | `0.97` | `2440d7c7e9f8cd10` | 166,601–168,796 ms |

Both reported boot CRC metadata `9a2b3827`. Fragmented commands, two queued
status commands, a brief application read pause, and close/reopen passed on
`/dev/cu.usbmodem21203`. B's fresh executing identity/build is now confirmed;
there was no independent readback of B's flash. The CRC field is boot metadata,
and every status correctly reports `verification=not_implemented`.

The standard `/usr/bin/screen` terminal at 115200 also passed `help` and
two-board `status`, retaining the same sessions, and closed normally. No
special client software is required to use the console.

Benji confirmed typing, trackball movement/buttons, and Layer 3 S switching
on both Macs: "Working great. No replug needed." The v0.97 slice passed its
interactive input check before the v0.98 local-history deployment above. See
the [deployment record](docs/testing/peer-status-v097.md) for evidence and
[diagnostics.md](docs/diagnostics.md) for the protocol and planned slices.
The v0.95–v0.97 changes have not been pushed.

## Previous deployment: v0.96 disk-free maintenance

Pico A was flashed and rebooted on 2026-09-14 at 20:39 UTC using frozen
`build/flashing/deskhop-v0.96-maintenance.uf2` (SHA-256
`4e36e037ebb30c03c1707b8cd04c59bc79b227051aff4980285f7e732e17402e`).
Its old firmware exactly matched the v0.95 artifact. The new firmware passed
picotool verification and independent comparison of all 262,144 bytes; all
4,096 saved-configuration bytes remained unchanged. Configuration format is 10.

The local serial check confirms executing build `0.96`, boot CRC metadata
`d4fdee05`, fresh boot session `13d7da4d302c9de8`, and uptime increasing from
15,282 to 17,021 ms. UID remains `E6654854574C3E30`, port
`/dev/cu.usbmodem21203`. This transition used the old disk-enabled bootloader,
but it disappeared cleanly: no retained RP2 object and no inactive/busy media
clients were present after reboot. This does not establish that the earlier
Mac panic cannot recur. See the [deployment record](docs/testing/maintenance-v096.md).

The A/B maintenance entry points now request PICOBOOT without USB mass storage.
Physical BOOTSEL and invalid-image recovery still provide the UF2 disk.
Benji reported "Works well" after the input check. A second Layer 3 A entry
was inspected at 20:41 UTC: only the vendor interface (class 255), no mass
storage interface, and no additional media clients. Direct picotool access
worked. Pico A returned to normal operation without a flash; its subsequent
serial check passed and no stale USB/media state remained. This verifies the
disk-free maintenance path on A. At this stage, Pico B's executing version
remained independently unverified, and the serial console was read-only and
local-only. The v0.97 deployment above subsequently added and verified peer
status. A successful local flash or local-only response does not establish
peer propagation.

## Previous deployment: v0.95 first serial-console slice

After v0.95 the Mac panicked on an inactive `IOMediaBSDClient` timeout two
minutes after wake. An older bootloader disk had already remained inactive/busy
for hours. A flashing-related storage teardown issue is a credible suspect,
not proven attribution. See the [panic investigation](docs/testing/macos-panic-20260914.md).
The subsequent 20:33 UTC health check found no retained RP2 object or inactive/
busy media clients; Pico A's serial checks passed with session
`3152032dc4f341dd` and uptime 2,336,528–2,338,263 ms. Its reset cause is unknown.
Benji confirmed typing, trackball movement/buttons, and Layer 3 S switching on
both Macs before the v0.96 transition.

The v0.95 firmware added a read-only USB
serial console in normal and configuration modes. `help` and `status` report
the connected board's identity, compiled version, metadata CRC captured at boot,
random boot session, and uptime. Peer queries, history, and flash verification
were later slices; v0.95 explicitly reported them as unimplemented.
The [incremental design and hardware checks](docs/diagnostics.md) record the
agreed sequence, including both-board defaults and interleaved A/B histories.
Pico A was flashed and rebooted on 2026-09-14 at 19:35 UTC using the frozen
`build/flashing/deskhop-v0.95-console.uf2` artifact. Its SHA-256 is
`c08d7237d442ef23465b2e7ab1ba7517152c02a19ca6089c801c464e55ff1cf5`.
All 262,144 firmware bytes matched the independent readback; all 4,096 saved
configuration bytes remained unchanged. Configuration format remains 10.

macOS bound the new CDC function to its built-in AppleUSBACM driver and exposed
`/dev/cu.usbmodem21203`. Read-only physical serial checks passed for fragmented
help/status, stable identity, increasing uptime, a brief paused reader, and
close/reopen discarding a partial command without changing the boot session.
The standard `/usr/bin/screen` terminal also displayed help/status correctly;
the same session was observed at 77 seconds uptime. The executing version was
`0.95`, boot metadata CRC `237a0b65`, and physical UID `E6654854574C3E30`.
See the [validation/deployment record](docs/testing/console-v095.md).

Pico B's propagated version was not independently verified at this stage.
Initially the trackball was completely dead on both outputs while typing worked. Unplugging
and reconnecting only the trackball restored normal operation; Benji reported
"Works great after replug." This suggests a peripheral re-enumeration issue,
but its cause is not established. Repeat the check on the next update; the
passing serial checks alone do not establish input correctness.
The v0.94 entry below is retained
as deployment history. The remote branch has not yet been updated for v0.95.

## Previous deployment: v0.94 regression fixes

On 2026-09-14 at 18:41 UTC, Pico A was flashed from commit `6ddc1e1` on
`codex/hardware-free-test-framework`, using
`build/arm-validation/deskhop.uf2` in this task's worktree. Its SHA-256 is
`8e1471c435f38feebf1e4f9bb48e1eb9e07cc9fff4e807fbd748ee183e3be507`.
All 262,144 firmware bytes were read back and matched the validated v0.94 binary;
all 4,096 saved-configuration bytes were unchanged. Pico A then rebooted and
re-enumerated as `DeskHop Switch`, serial `E6654854574C3E30`.

The previous image was independently read as v0.92 (192), CRC `0x92f56c36`.
Firmware/config backups and readback logs are retained under `build/flashing`
in the isolated worktree. The main checkout remains at its prior version;
no QMK firmware or target-Mac settings were changed. Pico B is expected to pull
v0.94 automatically, but its version has not been independently read back.
Benji reported "Looks good" after the post-flash input/switching check. This is
a basic hardware smoke result, not an independent readback of Pico B's version
or an extended test of every mode.

The user entered A's bootloader with Layer 3 A. macOS retained a stale busy
`RPI-RP2` disk and failed to mount the new one, so the update used Raspberry Pi's
official picotool v2.3.1 via direct USB. `picotool --ser` selected the flash ID
`E6654854574C3E30`; the ROM USB serial `E0C9125B0D9B` does not select this device.
The tool was unpacked temporarily, without a system installation.

## Hardware-free validation framework (v0.94)

The `codex/hardware-free-test-framework` branch adds the executable framework
in [docs/testing/README.md](docs/testing/README.md). It builds v0.94 with
configuration format 10. Fast, deep, six-layer coverage and ARM validation all
passed before the Pico A deployment above. The v0.92 notes below retain the
previous main-branch/hardware snapshot; the deployment note above supersedes
their version status.

Run `python3 tests/run.py fast` before ordinary changes, `python3 tests/run.py deep`
for generated workloads, bounded core orders and source mutations, and
`python3 tests/run.py arm` for a hardware firmware build. `python3 tests/coverage.py`
produces source coverage. All commands run on the development computer/CI with
no software installed on either target Mac.

Read the [architecture decision](docs/testing/architecture.md) and
[coverage/fidelity matrix](docs/testing/coverage.md). Two isolated production-C
Picos, actual SDK queues and UART framing run under virtual time; separate tests
execute the real TinyUSB device/host stacks and production storage/update code.
An executable two-emulator UART experiment confirms that topology is feasible,
but current emulator peripheral/core limitations prevent full firmware boot.

The new tests drove fixes for HID/input bounds, mouse-button aggregation and
handoff/detach, lost selection-message recovery, and configuration save races.
Both Picos need v0.94 for the full new protocol semantics. Arbitrary update
power-cut recovery and multiple keyboard report collections remain limitations.
A passing software tier does not establish macOS/Karabiner behavior or physical
PIO USB timing. See the [validation record](docs/testing/validation.md).

## Source of truth

| Component | Local repository | Remote | Source / verified state |
| --- | --- | --- | --- |
| DeskHop | `/Users/benji/Documents/ChatGPT/DeskHop` | `git@github.com:benji-york/deskhop.git` | `main`: accepted v0.104 firmware plus the hardware-exercised host updater workaround; upstream `ce8abb6` integrated. Devices remain on verified v0.105 from the separate batching branch. See the publication boundary above. |
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

The v0.92 build flashed on 2026-09-14 was produced at this mutable build path
(which does not necessarily still contain that image):

- `/Users/benji/Documents/ChatGPT/DeskHop/build/deskhop.uf2`

Its SHA-256 is
`5f7a6703dab1e0ccb65eba71861ea7fd78c5ca03c435953ff859eab4ba41728d`.
The local Pico rebooted and re-enumerated as `DeskHop Switch` after the UF2
transfer, despite macOS copy warnings as the bootloader drive disconnected.
After a settling period, Benji reported normal operation in response to checks
of the trackball, switching, and keyboard right-click on both Macs. This is a
basic deployment smoke test, not an independent readback of both Pico versions.
Extended v0.92 zoom assist, timed jitter, and coordinated reboot tests have not
yet been recorded.

The current `main` snapshot builds v0.104; both Picos remain on the separate
v0.105 batching branch's image. The v0.92/v0.94 notes above are historical.
To reproduce hardware-verified v0.91,
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
- Before v0.95, normal host input to DeskHop was the standard one-byte
  keyboard LED report. v0.95 also accepts read-only serial
  diagnostic commands; those commands do not control the keyboard or mouse.
- Both computers normally need to power their respective Pico. If one side is
  unpowered, cross-Pico input routing, coordinated reboot, and firmware
  propagation cannot work.

Both Picos default to output A at a cold start. With v0.94 on both sides, a
rebooted Pico can rejoin the surviving peer's selected output.

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
attached host. In v0.94, versioned generation/origin tokens reject older
selections; accepted changes are mirrored and periodically reconciled through
the heartbeat. Duplicate synchronization does not repeatedly release held keys.
Actual focus changes also release both mouse HID interfaces while retaining
physical source button masks for subsequent input on the new host.

The immediate output-selection message uses a blocking queue; periodic
reconciliation repairs lost wire messages when both upgraded peers resume
delivery. In the accepted v0.101 deployment, critical keyboard and switch/detach mouse
releases wait up to 100 ms for a queue slot. If one cannot be enqueued, that
version requests a watchdog reboot. The deployed v0.102 keyboard recovery replaces
the keyboard wait with durable state; the mouse path is unchanged.

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
node tests/test_webconfig_bootloader.js

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

The test-framework branch now adds paired production-code regression coverage
for pointer-sync, remote non-motion transport, button ownership and handoff,
selection reconciliation and other behaviors. See `tests/sim` and the detailed
[coverage matrix](docs/testing/coverage.md).

If configuration fields/UI change, update the generated web configuration and
the embedded disk image as well as C source. CI runs `make` in `webconfig/`, then
`disk/create.sh`, then the native tests and ARM build. The disk script uses Linux
mount tooling and `sudo`, so the repository's Docker/CI path is usually easier
than invoking it directly on macOS.

## Flashing DeskHop and peer propagation

### Preferred update path after v0.96 is installed

1. Build and test a firmware with a higher version number.
2. Keep both DeskHop Picos powered and UART-connected.
3. Use Layer 3 A/B for the selected Pico's PICOBOOT-only ROM bootloader. Once
   v0.104 has been deployed, `bootloader A` or `bootloader B` on its serial console
   is the keyboard-free alternative; keep the terminal open and reading the reply.
   A remote target must also support the new command. No `RPI-RP2` volume should
   appear. Identify the physical flash UID before writing; remote ACK acceptance
   alone does not establish ROM entry. Upload on the Mac wired to the target Pico.
4. Prefer the maintained updater, which uses official picotool to back up the
   firmware/settings, load and byte-verify the frozen candidate, verify unchanged
   settings, and reboot normally. Add `VERIFY_MODE=thorough` for a second full
   firmware readback and the extra diagnostic scans; normal mode retains one
   fresh expected-CRC scan per Pico after peer propagation.
5. Leave both sides powered for several one-second heartbeat cycles. The older
   peer should pull and install the 256 KiB running image automatically.
6. Check the trackball, keyboard right-click on both Macs, F24 switching, Pico
   LEDs, and Sofle arrow direction. On v0.97+, use repeated `status` commands
   from one console to confirm both executing builds, distinct identities,
   stable boot sessions, and increasing uptimes. Require a fresh peer reply;
   a timeout leaves its build unverified. v0.95/v0.96 `status` is local-only
   and requires each board's console. Input checks alone do not establish the
   peer's version, and status CRC metadata does not verify flash integrity.

This path is unavailable on a board still running v0.95 or older. Do not use
`picotool reboot -u` to obtain disk-free mode: on RP2040 it enables the disk.
Physical BOOTSEL, automatic invalid-image recovery, and the Layer 3 C
configuration drive still expose mass storage. They remain recovery/configuration
facilities, not the routine flashing path on this Mac after the storage panic.

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
at least the 30-second stall/recovery interval. If it remains bad, identify B's
version and use the appropriate direct flashing/recovery procedure; an older B
may still expose `RPI-RP2` when its hotkey is used. Do not casually remove power from a
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
| v0.104 `bootloader A/B` says `unconfirmed` | Peer is absent, old, busy/stalled, or its ACK did not arrive | Do not assume nothing happened. Inspect the selected Pico's USB/console state before retrying; an already-transmitted request cannot be recalled. |
| v0.104 `bootloader A/B` says `update_active` after propagation | This board is updating/dirty, or recently served firmware words to its peer | Let propagation and verification finish. Recent source service causes a three-second holdoff; expiration alone does not establish peer health. |

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
| Serial bootloader maintenance | `src/console.c`, `src/maintenance.c`, `src/include/maintenance.h` |
| Peer update/recovery | `src/fw_update.c`, `src/tasks.c`, `src/handlers.c`, `src/ramdisk.c`, `src/utils.c` |
| Config/defaults/API | `src/include/structs.h`, `src/include/config.h`, `src/include/user_config.h`, `src/defaults.c`, `src/protocol.c` |
| Native regression tests | `tests/test_zoom_tracker.c`, `tests/test_fw_update.c`, `tests/test_screensaver_policy.c`, `tests/test_reboot_hotkey.c`, `tests/test_config_migration.c`, `tests/test_webconfig_autostart.js`, `tests/test_hid_regressions.c` |
| Web Config | `webconfig/form.py`, `webconfig/templates/`, generated `webconfig/config*.htm`, `disk/disk.img` |
| Sofle integration | `/Users/benji/qmk_firmware/keyboards/sofle/keymaps/benji/keymap.c`, `config.h`, `rules.mk`, `readme.md` |
