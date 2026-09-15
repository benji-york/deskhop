# UART command integrity draft (bug #3)

2026-09-15. Second draft in the ordered fix stack, based exactly on keyboard
draft `b8992dc16370bbb1ce00a238463ec6e8207d0883`, itself descended from deployed
v0.101 `af100bcaba8013633d694bd6de8583c2970f6e4d`. Firmware version **0.101** and
configuration format **10** deliberately remain unchanged. **UNFLASHABLE as a
release** until the stack has a deliberate version/migration decision. No
deployment, host configuration change, or QMK change is part of this draft.

## Defect and boundary

The old frame was `AA 55 type payload[8] XOR(payload)`. Its type was unchecked.
An independent production-simulator witness sends POINTER_SYNC_MSG (26), payload
`003e003e00000000`, then flips raw byte 2 by `0x10`. The actual old receiver
dispatches WIPE_CONFIG_MSG (10): one flash erase, timeout 123 becomes default 300.
This concerns accidental line corruption, not evidence of attacker access.

Every new UART command now goes through one versioned CRC32 frame. No legacy
frame can reach the general dispatcher, no discovery response enables legacy
decoding, and no timeout/garbage triggers downgrade. The keyboard IDs42–48 and
their existing five-fragment, 40-byte challenge/session/focus/CRC32 envelope are
preserved inside this outer transport. Its source-activity, final-release,
synthetic lease and remote lease policies are unchanged.

## UART v1 wire format

Every frame is exactly **32 bytes**:

`7e | 30 nibble symbols | 7f`

Each pair of symbols encodes a binary byte, high nibble first. A symbol is
`0x40 | nibble` (only `40`–`4f` are legal). The decoded body is:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 1 | Protocol version, exactly 1 |
| 1 | 1 | Payload length, exactly 8 |
| 2 | 1 | Command type |
| 3 | 8 | Payload, including zero padding |
| 11 | 4 | IEEE CRC32 of bytes 0–10, little endian |

CRC parameters: reflected polynomial `0xedb88320`, initial state `ffffffff`,
final XOR `ffffffff` (standard CRC32/ISO-HDLC). Version and length are checked
even if a sender computes a correct CRC for unsupported values. Both delimiters
are required; neither is legal in the encoded body. All single-bit wire changes
therefore either violate the format or fail the CRC. Unknown command types
are ignored by the dispatch table.

The symbol alphabet also excludes the legacy `AA55` preamble everywhere,
including in encoded firmware words and CRCs. Thus correctly transmitted new
traffic cannot masquerade as commands to an old receiver. This alphabet costs
32 bytes versus the old 12: at 3,686,400 baud, 8N1, nominal frame serialization
is about 86.8 microseconds (previously 32.6). No electrical timing is claimed.
The 32-byte TX DMA buffer already fits; the 256-entry queue holds internal
13-byte packets instead of 10-byte packets. The WebHID queue remains 12 bytes
per report. Admission validates pointer, payload length and command-width bounds
before copying. Queue admission/backpressure semantics remain unchanged.

The receiver scans only the captured DMA-ring occupancy and dispatches at most
one valid packet per core pass. It retains a partial candidate. On malformed
data it advances one byte, so a complete frame immediately following a truncated
one remains available. The next intact start delimiter restores alignment;
concatenated frames need no idle gap. The DMA ring remains 1024 bytes. A hardware
overrun can lose traffic, including whole frames; this change does not detect
DMA laps or guarantee delivery through a saturated receiver.

CRC is accidental-error detection, **not cryptographic authentication**. An
intentionally constructed valid command still executes. CRC collisions, whole
valid-frame replay, malicious peers and command authorization are not solved.
The keyboard's inner challenge/sequence rules remain stronger than the general
command transport. Other commands retain their existing retry/duplicate policies.

## Compatibility and migration decision

| Peers / event | Behavior |
| --- | --- |
| Both UART-v1 | Input, LEDs, config proxy, diagnostic protocols and firmware pull all use protected frames from startup. Existing capability messages remain application payloads. |
| New / deployed v0.101 or older | Both ignore the other's correctly transmitted UART frames. Local USB behavior continues; cross-board input, status, proxy and firmware propagation are unavailable. No fallback. |
| Rebooted UART-v1 peer | Starts protected immediately; no remembered negotiation is required. Existing keyboard challenges, selection reconciliation and diagnostic request timeouts handle application resynchronization. |
| Truncation, corruption, mixed legacy/new bytes | Invalid candidates are discarded; a later complete v1 frame restores parsing. Legacy packets before or after valid v1 traffic cannot switch modes. |
| Unsupported frame version/length | Rejected, even with a valid CRC. No capability discovery can relax this rule. |

**Initial migration requires deliberately upgrading both boards independently.**
The eventual release must receive a new firmware version above deployed v0.101;
these drafts must not be used for automatic version-based deployment. Saved
configuration layout does not change, so this transport itself requires no
configuration-format bump. Both-board manual programming and acceptance belong
to a later authorized deployment task.

A legacy update-only bootstrap was considered and deliberately omitted. The old
receiver dispatches its unchecked type before it can learn any new capability.
Sending old heartbeats/update responses to drive auto-propagation would continue
exposing that board to the same destructive type confusion. Merely restricting
the new board's legacy receive allowlist cannot repair the old receiver. The
preceding keyboard draft already requires both upgraded boards. After this
one-time migration, future higher-version **v1-speaking** images retain existing
automatic peer pull and image CRC/version verification. A future incompatible
transport change needs another explicit migration decision.

Old application payload variants (selection, diagnostics, firmware sentinel)
can still be interpreted *inside verified v1 frames*. That is payload-level
compatibility and never means accepting old XOR UART frames. Historical simulator
oracles retain old receiver fixtures and explicitly check this transport boundary.

## WebHID configuration

UART and USB no longer share their wire encoder. Vendor report ID6 stays exactly
12 bytes and retains `AA55`, type and eight payload bytes. Its final byte is now
CRC-8/ATM (polynomial `07`, initial/final XOR 0, non-reflected) over all preceding
11 bytes, including preamble/type. The USB callback checks configuration mode,
OUTPUT report type, exact length, preamble, CRC and command allowlist, then
constructs a validated internal packet. Configuration replies use the dedicated
encoder; descriptor report counts remain 12. The generated browser page checks
response ID, length, type, header and CRC before using returned field values.
Use the page supplied with the eventual new firmware; old cached pages have an
incompatible checksum convention. Some old XOR values can coincide with CRC8,
so this is not a promise that every old page request is rejected. There is no
USB protocol negotiation or fallback. USB link-level CRC remains independently
provided by USB. This is also error detection, not authentication.
The generated HTML and FAT image are updated together. CMake explicitly tracks
the image as an assembler dependency so incremental ARM builds cannot retain an
old embedded page after the checksum convention changes.

Proxy envelopes may contain only one allowed configuration command; nested
proxies and input/update tunneling through that configuration envelope are
rejected on both USB and UART ingress. Mutable configuration-value semantics
remain the responsibility of bug #4. Reboot authorization, queue timeout/reboot
policy and power-loss boot safety remain the responsibility of later work; a
correctly encoded reboot/wipe/maintenance command still invokes its handler.

## Validation

Independent Python `zlib.crc32` and wire encoders are used for expected UART
frames; tests do not generate every expectation with the production checksum.
The baseline witness is saved under `build/tests/uart-baseline/`, including the
old compiled library and a replayable input trace. Focused regressions cover the
real encoder, DMA-ring receiver, full-frame bit faults, stream repair, downgrade
rejection, full queues and command traffic. Inherited keyboard and historical
comparisons remain part of the overall validation.

Completed validation (native production models, no hardware acceptance):

- Final `python3 tests/run.py fast`: **38/38 steps passed**, including all
  **97 paired scenarios** and all 12 UART integrity scenarios in the registry.
- `python3 tests/run.py deep`: **46/46 steps passed**, including 20,000 generated
  HID cases, 16 storage seeds, 32 generated paired input seeds, the four inherited
  24-order explorations, **21 storage and 17 paired production mutations**, and
  the nine historical behavior contracts. Both new transport mutations were
  detected at runtime: accepting a regenerated incoming CRC reproduces the
  timeout reset; consuming a rejected whole frame loses immediate recovery.
- `python3 tests/sim/test_uart_integrity.py --baseline build/tests/uart-baseline/node.so`:
  **12/12 scenarios passed**, with the old one-erase/timeout-reset witness first
  confirmed. Includes **2,304 single-bit faults** across nine representative
  frames, 255 production encoder patterns, 31 truncations, all 32 deletion and
  duplication positions, 31 DMA wrap positions, 510 unsupported version/length
  records with correct CRCs, 224 old-XOR-preserving errors, garbage, delayed tails,
  concatenation, legacy rejection before/after protected traffic, full queues,
  exact fault replay, real firmware source reads, and a corrupt-response retry
  that commits an independently compared first page only after recovery.
- Inherited keyboard reliability: **24 scenarios × 3 seeds = 72 passes**, plus
  **24/24** remount core orders. Includes late/duplicate fragments, held-source
  reconnects, synthetic all-up loss, and unchanged activity behavior. The
  synthetic-loss fixture now waits 100 µs for the 87 µs modeled frame to finish.
- Additional queue and corruption scheduling: **50/50 runs** (each scenario's
  default plus all 24 core orders).
- `python3 tests/run.py arm`: **2/2 steps passed** with ARM GCC14. RAM use is
  **220,092 / 262,144 bytes**. The binary's full 65,536-byte embedded disk region
  exactly matches `disk/disk.img`; its HTML matches the generated config page.
- WebHID: independent CRC8 oracles and all **96 single-bit positions**, strict
  report input checks, real configuration response queue, and both TinyUSB
  device/host suites passed. The standalone source/body/page transfer tests do
  not claim one integrated physical USB-to-full-UART-flash deployment.

The full deep registry contains all 12 new UART scenarios. Historical `uart_faults`
now explicitly asserts delivery of the first intact frame after truncation and
compares every remaining old effect; it no longer preserves the old lost-frame
bug. The real old receiver's ID3-extension/unknown-ID32 behavior is still tested
with its explicit historical framing, alongside actual mixed-version rejection.

Results: `build/tests/results-{fast,deep,arm}.json`; focused, baseline, inherited,
replay and scheduling artifacts live under `build/tests/uart-integrity/`,
`build/tests/uart-baseline/`, `build/tests/compatibility-uart/` and
`build/tests/uart-scheduling/`. Unflashed ARM UF2 SHA256:
`95bf9e9c76c7dc68a49e87428cd356fb5cee0de4cfca47205a37432595afba75`.

Limits include native HAL scheduling rather than every ARM instruction/race,
physical UART/DMA/isolator behavior, USB/macOS/keyboard acceptance, CRC collisions,
valid-frame replay and arbitrary interrupted running-slot flash updates. CRC does
not authorize commands. Configuration-value and reboot-policy fixes remain the
next stack tasks.

Focused reruns:

```sh
python3 tests/sim/build.py
python3 tests/sim/test_uart_integrity.py
python3 tests/sim/run.py --scenario uart_queue_switch --interleavings
python3 tests/sim/run.py --scenario uart_faults --interleavings
python3 tests/sim/run.py --scenario keyboard_synthetic_remount_before_sync --interleavings
```

The simulator's replayable `fault` operation supports `xor` with `xor_at`, a list
of `[byte, bit]` pairs in `bit_flips`, `delete_byte`, `duplicate_byte`, and
`delay_byte: {index, us}` in addition to whole-frame delay/drop/truncate/duplicate.
Indices refer to actual wire bytes, including delimiters, version/type symbols
and CRC symbols. Saved traces retain both the original transmission and the
applied fault; generic replay checks preserve the byte scheduling and assertions.
