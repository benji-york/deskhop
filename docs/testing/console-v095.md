# v0.95 serial-console candidate validation

Validated in the isolated feature worktree on 2026-09-14, starting from
`dfebd6ec24c68f02a274c37125b66205f49462f0` with the console changes uncommitted.
This is the first slice of the [diagnostic console design](../diagnostics.md).
Configuration format remains 10. Pico A deployment and serial checks passed.
The initial input check found a dead trackball on both outputs; unplugging and
reconnecting only the trackball restored operation. Benji reported "Works great
after replug." The re-enumeration cause is unresolved and should be checked on
the next update; the post-replug input smoke check passed.

## Hardware deployment

On the user's `ready` response after Layer 3 A, Pico A was identified by flash
UID `E6654854574C3E30`. Its v0.94 image (194, valid CRC `79af8a82`) and saved
settings were backed up. Official Raspberry Pi picotool v2.3.1 loaded and
verified the frozen v0.95 artifact, then independent readbacks matched all
262,144 firmware bytes and all 4,096 unchanged configuration bytes. Pico A
rebooted on 2026-09-14 at approximately 19:35 UTC.

macOS enumerated `DeskHop Console`, bound its built-in AppleUSBACM driver, and
exposed `/dev/cu.usbmodem21203`. A bounded Python-stdlib smoke check passed:
fragmented help/status, increasing uptime with stable identity, a brief reader
pause, and close/reopen discarding an incomplete command. No software was
installed. A separate `/usr/bin/screen` session successfully ran both commands.
At 77,165 ms uptime it reported the same boot session as the earlier checks:

```text
board=A
board_id=E6654854574C3E30
build=0.95
image_crc_at_boot=237a0b65
boot_session=019cb85a4a3e372a
```

The console CRC remains metadata; the independent picotool readback established
the flashed bytes. The reader pause permits macOS buffering and therefore does
not establish USB-level backpressure. Pico B's version and the user's keyboard,
pointer, switching, and keyboard right-click behavior were not exhaustively
tested. A later serial check showed the same boot session at approximately
165 seconds uptime. The user subsequently confirmed normal operation after
replugging the trackball, as recorded above.
No coordinated reboot was requested during this hardware check.

Deployment results, before/after backups, and serial transcripts are under
`build/flashing`, including `pico-a-v095-result.json` and
`console-v095-smoke-20260914T193525.406124Z.{json,txt}`.

## Completed checks

- The fast tier passed; the final deep tier repeated every fast component and
  completed all 31 recorded steps. Its larger workloads include 20,000 generated
  HID cases, 16 storage seeds, 40 paired scenarios, 32 generated paired sequences,
  and 24 fixed orders of the four modeled cores.
- All 11 storage and 15 paired source mutations were detected by runtime checks.
  All nine v0.92 baseline scenario contracts passed: four exact effect traces,
  five per-host state comparisons with at most 500 microseconds observed drift
  (1 ms permitted). The paired model has no active CDC client; the actual CDC
  stack runs in the separate device harness.
- Real TinyUSB CDC tests pass under ASan/UBSan: normal/config descriptors,
  line coding and DTR, fragmented/coalesced command streams, CR/LF/CRLF,
  backspace/DEL/Ctrl-C, non-ASCII/NUL and overlong-line recovery, immutable boot
  identity while the updater cache changes, and per-call 32-byte RX / 64-byte TX
  limits. A stalled CDC IN transfer still permits HID IN and LED control traffic.
- DTR close, USB unplug, deconfiguration, and rapid bus-reset tests pass. These
  tests first exposed two production defects: cleanup rearming CDC's cleared
  endpoint as EP0 after unmount, and partial commands surviving reset followed
  by enumeration before the next console poll. Both were fixed and their
  regressions retained. Already-submitted IN bytes are explicitly outside the
  promise to discard unsent console output.
- The new scheduler adapter checks named task resolution and core ownership for
  both current 7/8 and baseline 6/8 task tables. Existing isolation, bounded wait,
  exact replay, and failure minimization checks pass.
- ARM configure/build passed. Main RAM usage is 172,192 / 262,144 bytes (65.69%),
  including RAM-resident code, versus 155,348 bytes for v0.94. The map also
  reserves 2 KiB in SCRATCH_X. The compiler reports a 96-byte console-task frame;
  this is not a whole-call-chain or measured hardware stack high-water mark.
- USB-device source coverage was regenerated with the real stack. It is a
  separate denominator, not a whole-firmware coverage claim.

Logs and machine-readable results are retained under `build/tests`, including
`deep-v095.log`, `arm-v095.log`, `coverage-usb-v095.log`, and per-tier JSON files.
The earlier fast run preceded the final rapid-reset fix; the final deep run
includes that fix and repeats the complete fast tier.

## Frozen artifact

The flash candidate is copied separately from the build directory:

- `build/flashing/deskhop-v0.95-console.uf2`
- UF2 SHA-256: `c08d7237d442ef23465b2e7ab1ba7517152c02a19ca6089c801c464e55ff1cf5`
- Binary SHA-256: `b309ee6c34f40bdd80032a2507d55cbc50f9abfd07e8d2c13db745283309f74b`
- Metadata: magic `0xf00d`, encoded version `195`, CRC `0x237a0b65`.

An independent Python check recalculated the CRC over the binary excluding its
last 4 KiB metadata sector. All 1,024 RP2040-family UF2 blocks are unique, cover
the expected 262,144 firmware bytes, and match the binary byte-for-byte. The
image spans `[0x10000000, 0x10040000)`; the saved configuration at
`[0x101ff000, 0x10200000)` is excluded. The artifact manifest is
`build/flashing/v095-candidate.json`.

These checks validate the files, modeled behavior, and the specific local serial
hardware observations above. Perceived input smoothness, broader physical USB
timing behavior, and the peer's deployment still need interactive checks.
