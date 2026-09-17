> **Development reference only.** The shipping helper is now the all-Swift
> [native menu-bar app](../../macos/DeskHopClipboard/README.md), with `.app`
> packaging and explicit Launch at login. The Python CLI below remains a
> protocol/debugging fixture reference and is not bundled or required at runtime.

# Foreground helper on either Mac

This helper is opt-in. It owns the local existing normal-mode CDC serial port while
running. It does not install a service, poll the clipboard, keep clipboard
history, or require a helper on the receiving Mac. Each fresh, validated DeskHop request
starts one native plain-text read. The helper cannot confirm that the active
application on the receiver interpreted the resulting keys as intended.

Build the native reader locally (compilation does not access the clipboard):

```sh
mkdir -p build/clipboard
xcrun swiftc -module-cache-path build/clipboard/module-cache -O \
  scripts/clipboard/native.swift -o build/clipboard/reader
```

After the matching firmware has separately been approved and installed, start
in a terminal on the source Mac, substituting its explicit port and physical flash UID:

```sh
python3 scripts/clipboard_helper.py \
  --port /dev/cu.EXPLICIT_LOCAL_PORT \
  --expected-uid EXPLICIT_16_HEX_LOCAL_UID \
  --expected-build 0.115 \
  --reader build/clipboard/reader
```

The exact expected build is mandatory; builds older than 0.115 are rejected.
The helper checks the console's local board identity and boot session, its firmware
build and idle update state, and the opposite board’s build/update state if present. It then
enters clipboard mode with a fresh random nonzero 64-bit helper session, and
checks the binary HELLO against the preflight boot session. No clipboard read
occurs during startup or reconnect.

Keep this process in the foreground. Press **Ctrl-C before diagnostics,
configuration, or an upgrade**. Ctrl-C and SIGTERM cancel the native reader,
drop DTR, release exclusive ownership, and close the descriptor. Run the normal
updater after it stops. Restart the same command afterward; fresh identity,
build and session checks are required. After a firmware upgrade, set
`--expected-build` to the newly installed build. There is no autostart install,
background process, automatic updater launch, port discovery or firmware write.

Disconnects, I/O timeouts and changed destination boot sessions permit at most three
reconnect attempts for the entire invocation, one second apart. Set
`--reconnects 0` to stop on the first interruption. Identity or protocol failures
stop immediately. A reconnect selects only the original explicit port and
validates its identity again. Stop the helper while running the updater so the
two programs do not compete to reopen a returning serial port.

## Data and timing contract

Only printable US ASCII bytes `0x20`–`0x7e`, LF (`0x0a`) and TAB (`0x09`) are
supported, up to and including 1024 bytes of UTF-8. Empty, non-text, oversized,
unsupported and unavailable clipboard results contain no text. CR/CRLF,
Unicode, NUL, control sequences, rich text without a plain-text flavor and any
other unsupported input are rejected rather than rewritten. LF and TAB become
ordinary Enter and Tab keystrokes on the destination, so the target application may submit a
form or move focus. Correct typing requires the firmware's documented US input
source, Caps Lock, modifier, dead-key and remapper prerequisites on the destination. The destination requires
a fresh host keyboard LED report after mount/reconnect that confirms Caps off.

The native reader retrieves `.string` once and checks the pasteboard generation
before/after; a change during the read returns unavailable. The child has a
two-second budget and returns at most 1027 bytes: a one-byte status, little-endian
16-bit length and at most 1024 payload bytes. A hung or invalid child returns
unavailable and is killed/reaped. No partial result is accepted. The Python
reader allocates 1028 bytes, including one overflow sentinel; extra output
terminates the child. Its serial response has a 2.5-second budget from receipt
of REQUEST. The source requires the complete helper reply within three seconds of
admitting the trigger; the destination requires complete assembly within three seconds of its
admission of that trigger. These are separate local deadlines, not a shared
end-to-end clock. Heartbeats continue every 250 ms while waiting for the child.
A timed-out response never sends END. Clipboard typing depends
on the firmware receiving and validating the complete result and the original
trigger's physical release.

DHC2 frames are exactly 64 bytes: magic `DHC2`, opcode, three zero reserved
bytes, then a 56-byte body. Every unused byte is zero. All integers are
little-endian. Mode entry is `clipboard <16 lowercase hex helper session>`;
the echoed command and CRLF are followed directly by HELLO with no ASCII prompt.
DTR drop exits this mode. Rejection leaves the diagnostic prompt available.

| Opcode | Direction | Body |
| --- | --- | --- |
| 0 HELLO | source → helper | `u64 sourceBoot`, `u64 helper` |
| 1 REQUEST | source → helper | five `u64`: `sourceBoot`, `targetBoot`, `helper`, `nonce`, `focus` |
| 2 BEGIN | helper → source | echoed five IDs, `u8 status`, `u16 length`, `u32 CRC32` |
| 3 DATA | helper → source | `u64 nonce`, `u16 offset`, `u8 count`, up to 40 bytes |
| 4 END | helper → source | `u64 nonce` |
| 5 PING | helper → source | `u64 helper` |

Statuses are 0 success, 1 empty, 2 non-text, 3 oversize, 4 unsupported and
5 unavailable. Non-success sends BEGIN with zero length/CRC and no DATA or END.
Success sends BEGIN, sequential DATA frames, and END. CRC32 is standard reflected
IEEE CRC32, as implemented by Python `zlib.crc32`. Requests must have the current
source boot/helper session, a stable destination boot session, and a strictly increasing nonce.
A reused nonce fails before any native read. There is no retry of old text and
no cached text fallback.

## Memory and privacy limits

The helper creates no content logs, transcripts, temporary payload files or
crash artifacts. It suppresses the inherited console's logging/event hooks,
discards child stderr, reports only fixed operational error messages, and
disables core dumps in both processes before clipboard access. Python mutable
payloads are overwritten during cleanup, including failure, timeout and
cancellation. Native mutable buffers are overwritten on normal completion;
killing a timed-out or cancelled child relies on OS memory reclamation instead.
Each encoded serial frame is overwritten immediately
after sending. Firmware stores the bounded payload only in RAM.

**Public AppKit clipboard retrieval has no byte-limited API.** AppKit may
materialize an oversized immutable string before the native reader can reject
it. The reader checks its UTF-16 and UTF-8 lengths before allocating its bounded
UTF-8 payload; it never forwards an oversized string or streams an unbounded
clipboard through Python or serial. A strict 1 KiB limit on all transient host
allocations is not claimed. Swift/AppKit immutable strings, OS clipboard storage,
kernel pipe/serial buffers, swap and OS-managed diagnostic machinery cannot be
securely erased or fully controlled by this helper. Core dumps and application
content diagnostics are disabled; this is not a secure memory-erasure guarantee.

## Hardware-free validation

Python fixtures do not access a pasteboard or serial device:

```sh
python3 -m unittest discover -s tests -p clipboard_helper_test.py -v
```

To additionally test the compiled Swift boundary, run:

```sh
DESKHOP_TEST_NATIVE_READER="$PWD/build/clipboard/reader" \
  python3 -m unittest discover -s tests -p clipboard_helper_test.py -v
```

The native test uses **only `--fixture`**, which reads at most 1025 fixture bytes
from stdin and never calls `NSPasteboard.general`. It covers exact limits,
negative results and the ASCII/LF/TAB contract. Python tests also exercise all
256 byte values, strict frames and padding, CRC/offsets, native timeout/overflow,
partial serial I/O, stale/replayed requests, disconnects, cleanup, fresh sessions,
bounded reconnects and content-free error reporting. Actual AppKit permissions,
serial handoff to the updater and target application behavior still require
separately authorized physical acceptance.
