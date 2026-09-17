# DeskHop Clipboard for either Mac

A native Swift menu-bar app for **macOS 13 Ventura or later**, on Apple Silicon
and Intel. Run it on either or both Macs to make their current clipboard text
available to the other Mac. A receiving Mac needs no helper: DeskHop types ordinary
keyboard events and leaves its clipboard unchanged. With no connected helper on
the opposite Mac, the shortcut produces no typing. Firmware v0.118 and this
helper have completed one physical A-to-B paste; see the
[deployment record](../../docs/testing/clipboard-deployment.md) for validation
and remaining hardware acceptance.

## Build and package

With Xcode Command Line Tools and Python 3 installed, from the repository root:

```sh
make helper-app
```

This builds both architectures, runs offline Swift tests, builds the bundled
Swift clipboard reader, draws the app icon, signs both executables and the app,
checks the signature/bundle/architectures, tests only the reader's `--fixture`
mode, and creates:

* `build/clipboard-app/DeskHop Clipboard.app`
* `build/clipboard-app/DeskHop-Clipboard-0.1.0.zip`, including `INSTALL.txt`

To build and install in your user's Applications folder:

```sh
make install-helper-app
```

This also builds first, then installs `~/Applications/DeskHop Clipboard.app`.
It verifies the staged app before replacing an existing copy. A running helper
quits normally to release its serial port, then restarts from the installed copy.
If it cannot quit, installation stops without replacing it. A stopped helper
stays stopped; open it from Applications when ready. Saved connection settings
and the existing Launch at login choice are preserved. Installation does not
enable login startup for a new user or flash DeskHop firmware.

Choose another destination with `make install-helper-app HELPER_INSTALL_DIR=/Applications`
(the directory must be writable). Python can be selected with `PYTHON=python3`.
The underlying build command remains `python3 scripts/build_clipboard_app.py`.

Python is a build tool only. The shipped app and reader are Swift executables;
there is no Python, Zig, shell script, third-party framework or downloaded
package dependency in the app. The build never installs or starts the app,
opens a serial device, reads a clipboard or registers a login item.

The default build uses an **ad-hoc local-development signature**, not a Developer
ID signature or notarization. For distribution, rebuild with an explicitly
selected Developer ID Application identity, then notarize and staple through
Apple's supported tooling before making a new ZIP. Signing alone is not
notarization. No certificate or Apple account is selected automatically:

```sh
make helper-app HELPER_SIGN_IDENTITY='Developer ID Application: YOUR IDENTITY'
```

Downloaded unnotarized apps may be rejected by Gatekeeper. Do not disable
Gatekeeper or strip quarantine. Local reviewed-source builds and a properly
signed/notarized release have different acceptance paths. See Apple's
[Developer ID guide](https://developer.apple.com/developer-id/) and
[notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).

## Use and startup

After firmware deployment, run `make install-helper-app`, or drag the packaged
app into `/Applications` or your user's `~/Applications`, then open it on each
Mac that should supply text. The first launch is paused
and displays settings. Enter the verified local `/dev/cu.*` serial port, physical
16-hex-digit board ID and exact installed firmware version (0.115 or later).
Choose **Connect / Resume**. Port discovery does not silently select a device.
The connection opens only that port exclusively, checks the strict diagnostic
identity/build and a fresh binary session before listening for requests.

Copy on either Mac with a connected helper, select the other Mac in DeskHop,
and press/release the proposed Sofle
**Layer 3 V / bare F23** shortcut. Nothing is read until a fresh request arrives.
The entire supported value must be 1–1024 bytes; US ANSI printable ASCII, LF and
TAB only. Unsupported/oversized values reject completely. Caps must be off for
correct case. Firmware v0.117 uses Caps-off as the default when the host omits LED
reports; a reported Caps-on state still blocks typing. Unreported Caps-on state
cannot be corrected. Older v0.115–116 firmware requires a fresh local LED report
and may refuse the shortcut when a host/remapper omits it. The ordinary US input
source, composition and remapper restrictions in the
[firmware workflow](../../docs/clipboard-keyboard.md) still apply.

The menu and settings expose connection status, **Pause & release serial port**,
**Connect / Resume**, **Launch at login**, and Quit. Pause closes the exclusive
serial descriptor before reporting “serial port released”; Quit waits for the
same cleanup. Pause before diagnostics, configuration or the updater, then
resume with the new exact build afterward. A disconnected transport gets at most
three reconnect attempts, one second apart, using the same port and fresh
sessions. Identity/protocol errors stop immediately. Restarted destination sessions require
a new identity handshake. No old clipboard response is retried.

**Launch at login** changes only through an explicit user action and uses
[`SMAppService.mainApp`](https://developer.apple.com/documentation/servicemanagement/smappservice/mainapp),
Apple's macOS 13+ user-session login-item API. The app reads its current status
on launch/menu open; it does not silently re-register a disabled item. A pending
approval is shown with an **Open Login Items** action. Enablement is refused from
a build folder or installer: move the app to Applications first. No root daemon,
handwritten LaunchAgent, privileged helper or privileged operation is used.

An explicit Connect choice is remembered for later app launches; an explicit
Pause is also remembered, so login startup does not undo Pause. Quit releases
the port but retains the chosen resume preference for the next launch. Uncheck
Launch at login before removing the app. Only port, board ID, expected version
and the resume preference are stored in UserDefaults, never clipboard text.

## Data handling

The app retains text in one bounded, uniquely owned mutable Swift-core buffer;
`memset_s` clears its full capacity after use and on failure. Outgoing frames
are separately owned 64-byte buffers, cleared after each send. UI state and
errors contain fixed operational messages, not payloads. No content is displayed,
logged, written to a temporary file or kept as history.

A fresh validated request launches the bundled short-lived Swift/AppKit reader
once. The result pipe is bounded to 1028 bytes including an overflow sentinel;
the native read has a two-second budget. The response deadline is 2.5 seconds
from receiving REQUEST, with 250 ms heartbeats while the child runs. The source has a three-second response deadline from admitting the trigger;
the destination has a separate three-second assembly deadline from admission.
A destination-originated PULL also expires after three seconds if no grant arrives. Failure never produces END or a
partial application “paste.” A response is typed only after the destination's full validation
and actual shortcut release, at about 100 characters/sec.

AppKit can materialize a larger immutable NSString before the size check; the
accepted/transmitted limit is 1024 bytes, not a bound on OS-owned clipboard
storage. Native mutable buffers are cleared on normal exit. A killed child
relies on OS reclamation. Both processes disable core dumps, and child stderr is
discarded. OS/runtime copies, pipe/serial buffers, swap and OS diagnostics cannot
be securely erased by this app. The wire's CRC/session checks are not
cryptographic authentication against a malicious source Mac.

## Offline checks and visual preview

```sh
python3 scripts/build_clipboard_app.py --test-only
```

This runs a framework-free Swift test executable, so full Xcode/XCTest is not
needed. It exercises production framing, identity parsing, actual POSIX I/O over
Unix socket pairs, native process bounds with explicit fixtures, session freshness,
cancellation, Pause/Resume cleanup, bounded reconnect, and fake login services.
No test opens `/dev/cu.*`, invokes `--read-current` or calls ServiceManagement
registration. The older Python helper is a development protocol reference and
additional fixture oracle, not a runtime component of the shipping app.

For a visual review of the real native menu/settings without side effects:

```sh
open -n 'build/clipboard-app/DeskHop Clipboard.app' --args --ui-fixture
```

Fixture mode uses in-memory settings, a fake login service and simulated
connection state. It creates no connection controller or pasteboard reader.
Connect/Pause and Launch at login can be exercised without devices, clipboard
reads, preferences writes or login-item registration. Quit closes the preview.
Actual login registration, Gatekeeper on a distribution build, macOS 13/Intel
execution and physical serial/clipboard acceptance remain separate manual tests.
