# Clipboard hardware deployment — 2026-09-16–17

User authorization: “run the app and flash,” followed by “Make the helper
permanent” and “commit, merge, and push.” Development branch:
`codex/clipboard-keyboard`, based on published v0.113 `6ac4d608`.

## v0.114 attempt: not accepted

Frozen manifest: `build/releases/deskhop-v0.114-pdko9xtp/manifest.json`.
Full-slot CRC32 `43b69d4a`; BIN SHA-256
`ff003d25288341faba2a525f14b098769e0da8d3adc99a10a7b55bea589a3ad1`.
Preparation reran all 61 fast steps (including 98 paired clipboard scenarios),
then the ARM build, before freezing the source fingerprint and artifacts.

Physical A `E6654854574C3E30` was upgraded through `/dev/cu.usbmodem21203`.
Stock picotool verified the complete programmed image; its 4096 saved-settings
bytes were unchanged. Firmware/settings backups and command evidence are in
`build/updater/runs/20260917T022207Z-gtath7rq/`.
The maintained updater stopped at 97.299382 seconds because B had not finished
within the 90-second rollout deadline. This run remains **failed**, with no
automatic retry or reboot. B `E6654854577F2330` continued receiving v0.114 while
still executing v0.113 from RAM. Both boards were left powered and undisturbed.

A's subsequent history contained repeated packet checksum failures. The root
cause is the new UART ring eraser reading TRANS_COUNT after abort. RP2040 clears
that counter, so the firmware incorrectly restarted a full-ring count while the
physical WRITE_ADDR was partway through the ring. Producer accounting became
misaligned, causing lost/corrupted receive frames. The prior simulator preserved
the counter on abort and therefore missed this hardware behavior.

The [RP2040 datasheet, §2.5.5.3](https://datasheets.raspberrypi.org/rp2040/rp2040_datasheet.pdf)
explicitly specifies counter clearing. The v0.115 fix reads the stable physical
WRITE_ADDR only after both SDK abort/BUSY barriers have drained outstanding
writes, then derives the remaining ring count. Live-DMA pointer reads are not
used; the E12 address adjustment is avoided by waiting for completion.
The simulator now clears TRANS_COUNT on abort and models WRITE_ADDR independently.
The old code fails the corrected fixture; the fix passes. Logs:
`build/tests/clipboard-dma-abort-reproducer.log` and
`build/tests/clipboard-dma-abort-fixed.log`.

## v0.115 recovery preparation

The complete 73-step deep suite and ARM build passed (462.749 seconds of
recorded test subprocess time). Frozen manifest:
`build/releases/deskhop-v0.115-p8w9g5g1/manifest.json`; full-slot CRC32
`42091c5d`, BIN SHA-256
`3528d8d9c3684092280bd46d8af8c8c31b6f9d44c886963c2056620311c6efad`.
Validation logs are in `build/updater/prepare/prepare-100f65wn/`.
The Swift app is rebuilt with v0.115 as its minimum/default supported firmware;
known-defective v0.114 is refused. It was kept disconnected during maintenance.
The original transfer completed without interruption: B reported v0.114, idle,
with new boot session `dfe81f9e8205c3b5` at 2026-09-17 02:40:51 UTC. Its read-only
monitor record is `build/tests/clipboard-slow-rollout.jsonl`.

## v0.115 write and peer transfer

The first normal preflight, `build/updater/runs/20260917T024127Z-043_cu4p/`,
stopped before any write or reboot because B's fresh identity was unavailable.
Read-only observations retained in `build/tests/clipboard-v114-preflight-*.json`
then obtained both exact UIDs, v0.114 builds, idle updaters and unchanged boot
sessions. A separately initiated normal guarded upgrade passed preflight.

Run `build/updater/runs/20260917T024256Z-g8g6bn63/` backed up A, programmed and
stock-picotool verified v0.115, preserved every saved-settings byte, and rebooted
A into boot session `ebec5bac6dd730fd`. B began receiving v0.115 while executing
its v0.114 image from RAM. At 41.314143 seconds the strict status parser stopped
the host run: B returned a protocol-1 runtime-unavailable snapshot rather than
the required complete core/update fields. This run remains failed at monitoring;
it is not a successful pair verification or a failed A write.

No firmware command was retried and neither board was manually reset. The
transfer was observed read-only in `build/tests/clipboard-v115-rollout.jsonl`.
The first monitor reached its 30-minute observation limit; a second read-only
monitor continued observing the same firmware attempt. B completed and rebooted
automatically into v0.115 with boot session `847be6dd2cf9551f`. The source history
records 2,283,372,048 microseconds (38.06 minutes) for this mixed-mode transfer
to the defective v0.114 receiver. This is recovery timing, not a benchmark for
normal updates between corrected boards.

## Both-board firmware verification: passed

Separate read-only run `build/updater/runs/20260917T032200Z-jqigjzs9/` passed in
3.455363 seconds. Both exact physical boards scanned all 262,144 bytes and
returned full-slot CRC32 `42091c5d`, metadata/boot CRC32 `0f0e3d1e`, build v0.115,
unchanged scan generations and advancing cores. Both updaters were idle. A's
boot session is `ebec5bac6dd730fd`; B's is `847be6dd2cf9551f`.
The complete retained current-boot history contains no packet-checksum errors
and reports no overwritten rows. These successful checks do not change either
earlier failed host-run verdict. Physical input and clipboard acceptance remain
pending the keyboard upgrade and user checks.

## Sofle and app

The canonical QMK checkout `/Users/benji/qmk_firmware` received the reviewed
Layer 3 V / held bare-F23 patch and built successfully for
`sofle/rev1:benji` with `CONVERT_TO=rp2040_ce`. The exact UF2 and source snapshot
are retained in `build/clipboard-keyboard/`; UF2 SHA-256:
`78e889c8cce91cea2be99bb3fa79d48679bf68300c1e92611414ede357e90503`.
On 2026-09-17 the first keyboard half was flashed through its directly connected
`RPI-RP2` volume. The exact 83,968-byte UF2 above was hash-checked immediately
before writing; the copy and flush completed. The bootloader disappeared and
the same USB port enumerated as Sofle `FC32:0287`, serial
`E4653873C75151360000000000000000`, in 2.44 seconds. Evidence:
`build/tests/clipboard-qmk-first-flash.json`. This verifies the host write and
normal keyboard re-enumeration, not an independent flash readback or typing test.
After the user confirmed the second half was ready, its new bootloader USB
session received the same hash-checked UF2. The copy and flush completed, and
the same port returned Sofle VID/PID `FC32:0287` in 2.51 seconds. Evidence:
`build/tests/clipboard-qmk-second-flash.json`. Its product name subsequently
appeared, but macOS did not expose its serial string, configured USB interfaces
or HID devices. The host writes to both user-selected halves are complete;
normal startup of the second half is **not yet verified**. The second half's
identity is based on the user's explicit selection, not a distinct USB serial
observed after the write.

The user was asked to remove USB power from both keyboard halves for five
seconds and reconnect the second half directly, without reset, for a normal
startup check. The user chose **“I’ll reconnect it later.”** No further flash,
reset or reconnect was attempted. Continue with the normal USB/HID startup check
when the user reconnects; do not reflash merely because that check is pending.

The menu-bar app was initially launched paused, then stopped for replacement.
After successful pair verification, the rebuilt app was launched with explicit
Foundation settings for `/dev/cu.usbmodem21203`, local UID `E6654854574C3E30`,
build `0.115`, and `resumeOnLaunch=YES`. Process 5987 owned that serial port;
a one-second call-stack sample confirmed `Session.run()` waiting in
`SerialTransport.read`, which is reached only after the identity and HELLO
handshakes. Evidence: `build/tests/clipboard-app-connected.sample.txt`.
Native UI inspection remains unavailable (computer-use pipe closed); this is
process/connection evidence, not a screenshot of the menu. No login item has
been registered and no real clipboard has been read by the agent.

DeskHop firmware, app launch and both QMK writes are complete. The app still
owned its local serial port after both writes (process 5987). The second
keyboard half's normal startup and physical input/clipboard acceptance remain
pending the user-deferred reconnect.

## Reported no-op on Mac B — 2026-09-17

The user reported that Layer 3 V produces no text on B and requested the event
log. Capture `build/tests/clipboard-paste-diagnostic-20260917T135425Z/` retains
raw status/history, parsed results and a compact summary. The helper was in
`Session.run()` and owned the port before capture; it was briefly stopped to
release the exclusive console, then relaunched and confirmed owning the port
again (PID 98146). No device was rebooted, flashed or reconfigured for this read.

Both boards report v0.115, idle updaters and fresh checkpoints on both cores.
A retains boot `ebec5bac6dd730fd`; B has a newer boot `8d4eb4132ca706f2`, uptime
137.078 seconds at capture. B's restart cause is not in this history. A's recent
history shows the Sofle's three HID interfaces mounted. Neither retained history
contains a packet-checksum error, UART drop or rejected descriptor; no rows were
overwritten. Output changes are synchronized between the boards.

The existing v0.115 history has no clipboard trigger/admission/rejection events.
Consequently these logs do not establish whether F23 reached clipboard admission
or why typing was refused. The user was asked to retry with ASCII `hello` copied
on A and Caps Lock toggled on/off on B before pressing/releasing Layer 3 V in a
blank document. That refreshes the target LED-state report required by admission;
unknown LED state is a hypothesis, not a diagnosed cause.


## Clipboard diagnostic candidate v0.116 — 2026-09-17

The user cannot perform the Caps Lock refresh because Caps Lock is remapped on
both Macs. Retrying the copy/select/shortcut steps alone still produced no text.
No remapping change was requested or made. Unknown LED state remains unproven.

The v0.116 candidate adds history event 19 with fixed phase/reason enums. It
records an F23 edge (including non-bare or incomplete decoded input), admission
or rejection, peer handshake, helper response status, release, typing and
completion/cancellation. It records neither clipboard bytes nor lengths,
checksums, request bindings, HID reports or arbitrary strings. All clipboard
history values are zero. A held trigger is deduplicated per interface, and no
per-byte/character/heartbeat events are recorded. The existing 24-byte history
wire format and 64-entry rings remain unchanged.

Admission and cancellation conditions are preserved. Size optimization now also
covers the diagnostic/history control modules so the unchanged 16 KiB minimum
heap reserve still passes. The preliminary ARM build uses 245,224 bytes of main
RAM, leaving 16,920 bytes before runtime queue allocations. The full deep
validation and guarded deployment are pending; v0.115 remains the installed
version until the deployment evidence below says otherwise.


The final v0.116 source passed all 73 deep runner steps (456.715 seconds of
recorded subprocess time), including 146 updater tests, 32 helper fixtures,
16 Swift fixture groups and 113 paired clipboard scenarios. All 8 USB, 29 storage
and 23 paired mutations were detected. ARM validation and the unchanged heap
reserve passed. Frozen candidate:
`build/releases/deskhop-v0.116-pw8e8n1m/manifest.json`; preparation evidence:
`build/updater/prepare/prepare-46yxcfzn/`. Full-slot CRC32 is `6c0c6602`, metadata
CRC32 `aef838df`, BIN SHA-256
`aae0a28f0e0efc10cdfa6a0facec8ee33ceec9131e534e17e20c8791deae18fc`.

The authorized guarded update completed in 23.964188 seconds. Both exact board
identities report v0.116 and pass fresh 262,144-byte CRC scans; both cores on
both boards advance. A boot is `da7cdf4f58b6cea7`; B boot is `4cc6913603501ad3`.
A's 4096-byte settings region is byte-identical before/after. This is not an
independent settings readback on B. Evidence:
`build/updater/runs/20260917T143400Z-o8pithho/result.json`.

The existing universal helper was relaunched with build `0.116` (the protocol
and minimum supported version are unchanged). PID 13231 owns the serial port;
a one-second call-stack sample confirms `Session.run()` after identity/HELLO.
Evidence: `build/tests/clipboard-app-v116-connected.json` and the linked sample.
Connect saves the version/port/board settings for subsequent app launches.
No clipboard was read by the agent, and no further QMK write was made.

The user has been asked for one new `hello`/L3-V attempt with their Caps remapping
unchanged. No successful physical clipboard acceptance or root cause is claimed
until that attempt and its new diagnostic history have been inspected.


## Confirmed no-op cause and v0.117 fix — 2026-09-17

After the v0.116 retry the user again reported no text. Capture
`build/tests/clipboard-paste-diagnostic-20260917T144029Z/` shows A's F23 `trigger`,
`admitted` and physical `release`, and B's matching `offer` followed immediately
by `rejected reason=led_unknown`. Both boots match the deployed images, neither
history was overwritten, and A's helper had remained open. The failure therefore
precedes the clipboard read: the QMK shortcut reaches the firmware, but the
fresh-local-LED-report gate rejects the target. This does not establish that
Caps Lock is on, or whether the host omitted the report versus a remapper
preventing it. The helper was restored and verified owning the port as PID 14262.

The user asked why Caps matters. The feature injects keystrokes, so Caps can
change case; the agent-added requirement for known-off state was too strict.
The explained v0.117 change uses Caps-off as the startup default if no local LED
report arrives. A valid local Caps-on report still blocks/cancels, including
while typing. USB session changes discard stale knowledge, while focus changes
do not. This explicitly changes the guarantee: actual but unreported Caps-on
state can alter letter case. No Caps remapping or QMK change is made.

A new positive regression reproduced v0.116's refusal before the source fix:
`build/tests/clipboard-led-default-v116-reproduction.log`. The fixed-source test
suite and deployment evidence will be appended after validation.


The fix passed all 116 paired clipboard scenarios and all 61 fast release steps
(150.593 seconds of recorded subprocess time), including 146 updater tests,
32 helper fixtures, 16 Swift fixture groups, USB/storage, paired production
firmware, configuration transport and startup guards. The ARM build passes with
245,224 bytes RAM used and the unchanged 16 KiB minimum heap reserve. This run
uses the fast tier; it does not claim a new deep/mutation run (v0.116's deep
validation is recorded above). Candidate:
`build/releases/deskhop-v0.117-6rle5q2x/manifest.json`; preparation:
`build/updater/prepare/prepare-3l4ehoga/`. Full-slot CRC `ffde8f66`, metadata CRC
`fec614e8`, BIN SHA-256
`41c0f6960042fb406dde32e17df91ff9d27824a88df3278f1f74505d5bdc3148`.

The authorized guarded deployment completed in 24.41706 seconds. A and B both
pass fresh full-slot CRC scans, exact identity and progressing-core checks on
v0.117. A's saved settings are byte-identical to backup. Evidence:
`build/updater/runs/20260917T144731Z-dkfr3rw0/result.json`.
A boot `f271f9ee6cf3a917`; B boot `51a0ff980e8562bb`.
The helper was relaunched with build `0.117`; PID 16941 owns the serial port and
is in `Session.run()` after its identity/HELLO handshake. Evidence:
`build/tests/clipboard-app-v117-connected.json` and its referenced sample.

The user has been asked to retry `hello`/L3-V with remapping unchanged. The
confirmed LED gate has been corrected, but successful physical typing is not
yet claimed. No clipboard read, Caps remapping, login registration, application
installation, or additional QMK flash was performed by the agent.


## v0.117 retry: input loss; B unreachable — 2026-09-17

The user reported the next L3-V attempt appeared to crash DeskHop, then clarified
that neither keyboard nor mouse input worked. The local A USB device remains
enumerated with its exact UID. A's original v0.117 boot `f271f9ee6cf3a917` is still
running; both core ages are zero and the updater is idle. B's status/history
queries return `timeout_or_unsupported`. This establishes an unreachable peer,
not the cause or proof that both CPUs crashed. No flash/reset was performed.

The helper PID 16941 was stopped and deliberately NOT relaunched. A bounded
both-board capture stopped at its missing-peer assertion; raw local status is
preserved in `build/tests/clipboard-paste-diagnostic-20260917T145118Z/status.txt`.
A second read-only capture accepted the missing-peer outcome and retained the
local history in `build/tests/clipboard-crash-v117/` (`status.txt`, `history.txt`,
`parsed.json`, initial USB/process/port snapshots and `capture.log`). A has no
history overwrite: F23 `trigger`/`admitted` at uptime 148066 ms, physical release
at 148155 ms, then `cancelled reason=deadline` at 151066 ms. There is no `grant`,
`helper_request` or helper result. The attempt therefore did not reach a helper
clipboard read. The last preceding output switch was at 134806 ms. The available
log cannot establish exactly when or why B stopped answering.

The user was asked to unplug only DeskHop's USB connection to Mac B, wait five
seconds and reconnect without reset/L3-V, then report whether ordinary keyboard
and mouse input returned. The recovery is confirmed below. The helper must remain
stopped while this input loss is investigated. v0.117 is not physically accepted;
the earlier correct CRC scans and offline tests do not establish input reliability.
No speculative source change or further flash has been made for this failure.

## B-only power cycle restored ordinary input — 2026-09-17

The user reports, "We're good after B power cycle." A read-only status/history
capture in `build/tests/clipboard-v117-recovery-20260917T150357Z/` confirms both
exact board identities on v0.117 and `peer=ok`. Both cores on both boards have
checkpoint age zero; both updaters are idle. A retains boot `f271f9ee6cf3a917`
(uptime 979767 ms). B has new boot `cdf2839c7cc4f189` (uptime 33916 ms), consistent
with the user's power cycle. This confirms recovery, not a clipboard fix or a
fresh full-slot verification. The helper remains stopped. There was no further
flash, reset by the agent, shortcut test or clipboard read.

The offline audit has not established the failure's cause. The first request
never reached a helper read; B's inaccessible pre-reset history is no longer
available. The RX cleanup path aborts/restarts DMA for clipboard frames, while
the simulator models completion synchronously and does not model UART FIFO or
DREQ timing. This is a validation gap and an investigation target, not evidence
that DMA caused this incident. No speculative firmware change was made.

Before another physical clipboard trial, arrange independent local access to
B's status/history so a peer-link failure does not also remove the diagnostic
path. Any additional instrumentation should capture bounded transport/core
state only, without clipboard bytes or request identifiers, and be validated
offline before deployment. Preserve normal input and keep the helper stopped
in the meantime. The app's saved resume-on-launch preference is unchanged, so
"stopped" does not imply that manually opening the app cannot reconnect it.

## v0.118 local diagnostic preparation — 2026-09-17

The user authorized proceeding with independent B diagnostics, then clarified
that code/helper execution on B is not allowed. Built-in `screen` interaction
is allowed and results must be short enough to transcribe. B's normal CDC port
is `/dev/cu.usbmodem1203`. No Python recorder or new Mac application is required.

The candidate adds local `link` and bounded `link watch` commands. These read
UART/DMA registers and core checkpoint ages without modifying hardware or
querying a peer; cleanup phase stores are observational. The clipboard
protocol, admission policy and DMA restart procedure are unchanged. Two paced
firmware-transfer modules (`fw_batch.c`, `firmware_batch.c`) use `-Os` to keep
the diagnostic addition within the existing 16 KiB heap reserve; that reserve
and both stack allocations are unchanged. USB-stack checks cover the short
response, no peer queries, strict grammar, watch expiry/Ctrl-C/disconnect,
slow readers and concurrent HID progress. The production register reader is
exercised under sanitizers with a held firmware lock and preserved error/abort
registers. Release validation and hardware deployment are not claimed until
their evidence is appended below. The helper remains stopped.

The final candidate passed 74 deep checks (462.473 seconds of recorded check
time; the full test subprocess took 476.8 seconds), including 150 updater
contracts, 116 paired clipboard cases, native register-reader sanitizers,
the real USB stack, core-order exploration, mixed-version transfers, and
USB/storage/paired source mutations. Preparation:
`build/updater/prepare/prepare-36miz0gs/`. The ARM image uses 245244 bytes of
main RAM, leaving 16900 bytes before queue allocation, with the 16 KiB reserve
unchanged. Manifest: `build/releases/deskhop-v0.118-c2hguaiu/manifest.json`.
Slot CRC `cbe34825`, boot metadata CRC `b87127b8`, BIN SHA-256
`64359e2bfd1f45b7998006b2d7a722e9864f395a8f87c087e39c862f9e60107d`.

The guarded authorized update completed in 24.718675 seconds. Both exact UIDs
report v0.118 and pass fresh full-slot scans and advancing-core checks. A's
settings are byte-identical to backup; no independent B settings readback is
claimed. A boot `3671ff898c046a42`, B boot `faf970c4866c4b57`. Deployment evidence:
`build/updater/runs/20260917T154452Z-sb68kj2w/result.json`.

Three subsequent real USB `link` commands on A parsed correctly, with core ages
`0/0`, UART `197/0/3`, DMA `03/31/31`, RX remainder changing, and cleanup `0/0`.
Both boards were still responding normally. Evidence:
`build/tests/link-v118-live-20260917T154538Z/`. This verifies the diagnostic
read path, not clipboard acceptance. The helper was not relaunched. The user
has been asked to open B's `screen` port and transcribe its single `link` line
before any coordinated clipboard retry.

The user then confirmed B's direct `screen` response:
`C=0/0 U=197/0/3 D=03/31/31 P=0/0`; R was not transcribed. This shows fresh
core checkpoints, no reported UART receive error, active RX DMA and idle
cleanup at that observation. With B's independent observation path available,
the helper was relaunched on A with exact board ID/port and build `0.118`.
PID 34454 owns `/dev/cu.usbmodem21203`; a one-second sample confirms
`Session.run()`/`SerialTransport.read` after its identity/HELLO handshake.
Evidence: `build/tests/clipboard-app-v118.sample.txt`.

The user was asked for one controlled retry: copy `hello` on A, prepare
blank TextEdit on B, start B's `link watch`, focus TextEdit and press L3-V once.
If input fails, keep B powered long enough to transcribe the diagnostic line
or report whether the watch stopped. No clipboard should be typed into the
diagnostic terminal.

The user then reported “It worked!” The metadata-only capture at
`build/tests/clipboard-v118-trial-20260917T155400Z/` confirms A's trigger,
admission, grant, helper request/result and payload readiness; B's admission,
payload readiness, release, typing and completion; and A's completion.
Neither board reset: A retained boot `3671ff898c046a42`, B retained
`faf970c4866c4b57`. Both cores on both boards had checkpoint age zero, both
updaters were idle, and peer status was `ok`. Neither history had overwritten
entries, and no retained error/drop or clipboard rejection/cancellation was
reported. The capture includes no clipboard text.

The A helper was briefly stopped to release the serial port for capture, then
restored with the same exact A identity, port and build `0.118`. PID 35710 owns
the port; `build/tests/clipboard-app-v118-restored.sample.txt` confirms
`Session.run()`/`SerialTransport.read` after handshake. No new flash or reset
was performed. This establishes one physical A-to-B success; the v0.117 freeze
cause, long-term reliability, reverse-direction hardware acceptance and the
second Sofle half's separate direct-USB startup check remain unresolved.

## Permanent helper installation

The user requested making the helper permanent. The verified existing bundle
was copied to `/Users/benji/Applications/DeskHop Clipboard.app`; strict deep
signature verification passed and the executable, bundled reader and Info.plist
match the tested source bundle. The build-folder instance quit, and the installed
copy launched without command-line configuration. Its saved A identity, port,
firmware `0.118` and resume preference were preserved. Exactly one helper was
running (PID 37733), owning A's serial port, and the UI reported connected.

The app's native **Launch at login** checkbox was enabled; its state changed to
checked with no pending approval or unavailable-status message. Registration uses
`SMAppService.mainApp`. No custom LaunchAgent, root daemon, firmware change or
clipboard test was needed. Login-session restart itself was not tested. Evidence:
`build/tests/clipboard-permanent-install.json`.

## Source publication — 2026-09-17

The user authorized committing, merging and pushing the completed work after
the successful paste and permanent helper installation. The matching QMK
shortcut is commit `3c463df188` in `benji-york/qmk_firmware`.

Publication preflight confirmed all 713 source entries in the frozen v0.118
manifest still match the tested/deployed files, all 74 recorded deep checks
passed, and the ARM configure/build succeeded. The Swift helper's 16 fixture
groups were rerun successfully; its log is
`build/tests/clipboard-app-publication.log`. The canonical QMK keymap exactly
matches the recorded patch used for its successful RP2040 build and both writes.
Both repositories passed `git diff --check`.

Publication makes no firmware or helper change. It does not broaden physical
acceptance beyond the recorded A-to-B success or resolve the earlier freeze.
