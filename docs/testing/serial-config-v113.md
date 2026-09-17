# Serial configuration-mode entry: v0.113

Benji requested and authorized a separate serial command to enter configuration
mode without the attached keyboard, then explicitly authorized flashing it.
This extends `codex/confirmed-config-saves` after the successful v0.112
deployment. v0.113 is now the user-accepted device release, including both
confirmed two-Pico saves and serial configuration-mode entry.

## Contract

`config` (exactly, no arguments) targets the USB-connected Pico only. It does not
change `bootloader A|B`, add a remote UART command, upload firmware or save
settings. Existing config-mode scratch words and watchdog reboot are used,
without the keyboard shortcut's exit-toggle behavior. An already-configured
device returns `already_active`, so an explicit retry cannot toggle back out.

The existing maintenance reservation, update/source-lease checks, bounded USB
reply-completion fence and UART drain guard also apply to config entry. A
disconnect or timeout before authorization cancels it. Safety is rechecked
before changing scratch words. The accepted reply identifies the local role and
`action=enter_configuration_mode_after_reply`; it is not enumeration proof.
The page's existing Exit action returns to normal mode.

## Validation plan and limits

- Real TinyUSB CDC tests: exact command grammar, help capacity, local roles,
  idempotence, rejection, completion fence, backpressure and disconnect/reset.
- Paired production maintenance tests: watchdog config words, no USB ROM entry,
  no peer reboot, update/maintenance guards, cancellation and bounded timeouts.
- Full hardware-free release validation and ARM build before the frozen image.
- Guarded normal upgrade, unchanged settings check, automatic peer propagation
  and fresh full-image CRC on both Picos.
- Actual serial entry and actual Chrome WebHID Save receipts from both Picos.

Hardware-free tests do not prove USB enumeration on this Mac or user-visible
input routing. Browser save verification does not substitute for physical
keyboard/trackball checks on both Macs. Evidence below will distinguish these.

Implementation, validation and deployment are complete. Physical serial entry,
idempotence and return to normal mode passed as detailed below. Benji accepted
physical input and the browser Save check, satisfying the explicitly authorized
commit, merge and push gate.

## Targeted validation completed

The real TinyUSB virtual-controller suite passes with ASan/UBSan, including
existing bootloader tests and the new config-command grammar, both local roles,
final-newline completion fence, pipelined input, HID progress, stale replies,
timeouts, DTR loss, bus reset and already-active checks. All 67 production paired
serial-maintenance scenarios pass: 33 existing bootloader scenarios and 34 new
configuration-entry scenarios. These also cover UART/DMA stalls, updater guards,
watchdog scratch words, focus-state release and absence of peer/ROM entry.

The host updater's strict help validator now recognizes the exact additional
command list. Existing older lists remain supported; unknown/reordered/duplicate
commands and misleading config descriptions remain errors. Its read-only command
method still refuses `config`. All 143 updater unit tests pass. A separate
read-only review found no blocking safety or buffer-size issue.

## Frozen release and deployment

Full deep-tier validation passed in 427.151 seconds, followed by the ARM build
and source-fingerprint check. Preparation evidence is
`build/updater/prepare/prepare-br0x46o9/`. The main RAM linker region uses
241,592 bytes (92.16% of 256 KiB), before dynamic queue allocations.

Frozen manifest: `build/releases/deskhop-v0.113-ozo3oc6a/manifest.json`.
Full-slot CRC `f530270f`, metadata CRC `0808564c`, BIN SHA-256
`1d45964bfe955ce82d5e3dc52b25ba4539021f6be2ea1ee425b3070acba8c5fd`.
The dirty-source snapshot, not base HEAD alone, identifies this release.
Configuration layout 10 and UART framing 1 are unchanged.

The guarded normal upgrade completed in 23.625701 seconds without a retry or
physical power cycle. Stock picotool verified the upload, A's 4096-byte saved
settings were unchanged, B automatically propagated, and fresh 262144-byte
firmware scans on both Picos passed `f530270f`. Both cores progressed. Evidence:
`build/updater/runs/20260916T224415Z-g53_2lg6/`. Saved-settings SHA-256 before/after:
`aa816af793193a7ce487d391b49bf45d514dde09bd77d28255eb782e82a85464`.

## Actual serial-mode verification

An ignored, one-shot verification helper under
`build/flashing/verify-serial-config-v113.py` pinned both known board UIDs and
builds before each action. It did not save settings or write firmware.

1. On normal console `/dev/cu.usbmodem21203`, `config` returned exact local A
   acceptance. The connection was retained through the reply/reboot. macOS then
   exposed A (`E6654854574C3E30`) as VID `2e8a`, PID `107c`, config console
   `/dev/cu.usbmodem21205`, and mounted `/Volumes/DESKHOP`. Evidence:
   `build/flashing/v113-enter/`.
2. `/Volumes/DESKHOP/CONFIG.HTM` and current `webconfig/config.htm` both hashed to
   `f59294a75f55d4fe29827d55a9b246ae4f5583aef9bc8d11e8a7d6ba176cdf94`.
   This verifies the mounted artifact, not browser execution.
3. Repeating `config` on the observed config console returned
   `board=A result=already_active reason=no_reboot`. A's boot session remained
   `7f6e029be83fadfe`; B remained `8a23efe628540b31`. Evidence:
   `build/flashing/v113-idempotence/`.
4. `diskutil unmount /Volumes/DESKHOP` succeeded. Through the exact UID-matched
   vendor HID collection, the helper sent only the existing configuration Exit
   report (`06aa5513000000000000000097`). Evidence:
   `build/flashing/v113-exit/`. The disk disappeared, normal PID `c000` returned,
   and `/dev/cu.usbmodem21203` reappeared.
5. Final read-only status confirmed both v0.113 identities, idle updaters and
   active core checkpoints. A's new normal-mode boot session was
   `22dcb5da865b8623`; B's boot session was still `8a23efe628540b31`, confirming
   that this local entry/exit test did not reboot the peer.

Expected old-handle cleanup errors after A re-enumerated are retained in the
enter/exit evidence. They did not prevent independently observed new USB modes.
Only A's physical serial config entry was exercised; both roles were covered by
the hardware-free tests. Browser Save acceptance comes from Benji's separate
report below, not from these machine checks.

## Final acceptance follow-up

Benji answered the typing, right-click, switching, focus arrows and zoom assist
checklist with **"Input works; I can test Save now"**. Benji also explicitly
authorized **"Yes, merge and push when verified"**. The final response to the
browser Save acceptance question was **"Save looks good."** This completes
the user acceptance gate for publishing v0.113.

A second guarded serial entry mounted the configuration disk for this check;
evidence is `build/flashing/v113-final-save-entry/`. The requested browser test
was Connect, then Save without editing settings. Benji's successful report is
user-observed acceptance; no browser screenshot or raw Save receipts were
captured by the agent. This does not claim a real-device edited-value,
power-loss or partial-failure test. Those modeled cases are covered separately
by the automated suite.

At the subsequent read-only check, the disk was absent and normal console
`/dev/cu.usbmodem21203` was present. Both known board UIDs reported v0.113, idle
updaters and live core checkpoints. A's boot session was `e206c1f41c295617`; B's
session remained `8a23efe628540b31`. This independently confirms return to
normal operation. Supplementary acceptance evidence is
`build/updater/runs/20260916T224415Z-g53_2lg6/user-acceptance.json`; the original
upgrade result remains unchanged.

## Browser-test limitation

The browser-control tool rejected opening the local configuration HTML and
explicitly disallowed indirect workarounds. No Finder or alternate-browser
bypass was attempted. Benji agreed to defer the browser Save check and authorized
flashing plus serial configuration-mode verification while away. The deferred
browser check was subsequently accepted by Benji as recorded above. Serial
tests must not be recorded as agent-observed browser-page acceptance.
