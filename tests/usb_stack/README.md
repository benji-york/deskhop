# Real TinyUSB stack, virtual device controller

```sh
python3 tests/usb_stack/run.py
```

This hardware-free feasibility prototype builds and runs the checked-in TinyUSB
`tusb.c`, `tusb_fifo.c`, `usbd.c`, `usbd_control.c`, `hid_device.c`, and
`msc_device.c`, plus the actual DeskHop `usb_descriptors.c` and `usb.c`. It takes
roughly one second on the development Mac and uses ASan and UBSan. It needs Python
3 and a native C11 compiler (`CC` can select one). All output goes into a temporary
directory. It does not modify or flash the firmware.

The tests use the real production TinyUSB configuration with its device classes,
endpoint buffers, and descriptor definitions. The native override disables the
unused physical host side and selects `OPT_OS_NONE` and a virtual DCD. It retains
Cortex-M0+ packed-safe unaligned accesses on ARM64 hosts via `native_options.h`.
A native `main.h` includes real firmware state/types but substitutes the hardware
include umbrella. Other production USB host functions are compiled but discarded
by the linker; their physical-device behavior is not tested here.

The virtual DCD records endpoint opens, pending transfers, stalls, address changes,
and remote-wakeup requests. Simulated hosts deliver setup packets and completion
interrupt events to the real TinyUSB event queue. They consume IN buffers or fill
OUT buffers only when the real stack submits a transfer. TinyUSB then runs its
actual control setup/data/status handlers, class dispatch, endpoint busy state,
and application callbacks. None of the descriptor responses or enumeration
transitions are reimplemented in the host model.

Verified scenarios include:

- Bus reset, the initial 8-byte device descriptor, full device descriptor,
  `SET_ADDRESS`, configuration header then complete multi-packet configuration,
  `SET_CONFIGURATION`, and configuration zero.
- Both normal identity (`1209:c000`, two HID interfaces) and configuration identity
  (`2e8a:107c`, three HID plus MSC), followed by endpoint opens with independent
  assertions on address, count, and packet size.
- All advertised HID report descriptors, product string, unknown string and
  out-of-range interface stalls, and descriptor bounds/interface walking.
- Actual HID `SET_REPORT` class control path through the production LED callback
  to its cached LED state and modeled peripheral/UART sinks; malformed length
  rejection; buffer-size stall; `SET/GET_IDLE` and `SET/GET_PROTOCOL`.
- Keyboard and relative-mouse report IDs and bytes through real HID IN endpoints;
  a pending transfer blocks the next report until its completion event arrives.
- Remote-wakeup enable, suspend/readiness changes, wake request, resume, unplug,
  and a second enumeration.
- Actual MSC `GET_MAX_LUN` and reset controls, bulk command/status wrappers,
  SCSI INQUIRY, READ CAPACITY(10), two-block READ(10), and malformed-CBW stalls.

MSC backing-store callbacks in this prototype are deliberately modeled. Disk
capacity and returned bytes are fixtures; this is not a TinyUSB-to-real-flash
end-to-end test. `tests/storage` separately exercises the real ramdisk callbacks,
NOR effects, UF2 and updater. Peripheral LED writes and peer UART messages are
observable sinks here, not the dual-node simulator's transport. Those layers must
not be conflated when reporting coverage.

A useful stack-level finding is captured: in this TinyUSB revision, a HID
`GET_REPORT` request with a nonzero report ID returns the ID byte even though
DeskHop's callback returns zero payload bytes. The application comment says zero
requests a stall, which is true for report ID zero; the real class driver prepends
a nonzero ID before testing response length. A callback-only test would miss this
behavior. This suite characterizes it without claiming it causes a host problem.

This DCD is a USB transaction/stack boundary model, not a bit-level USB controller
emulator. It does not establish token/CRC/PID timings, NAK retries on the physical
bus, data toggle correctness, electrical resets, USB PHY behavior, RP2040 DCD
register behavior, PIO host USB, enumeration by an actual macOS host, or Karabiner.
The native OS queue executes deterministically in one thread. It does not test
Pico OS spinlocks or instruction-level interrupt preemption.

To add a control scenario, call `control(bmRequestType, bRequest, wValue, wIndex,
wLength, out_bytes)` and assert the returned bytes in `host_bytes/host_count` or
an application-visible effect. To hold endpoint backpressure, leave its transfer
pending; deliver it later with `complete(endpoint_address, out_bytes, out_length)`.
To add a controller fault, emit a chosen `dcd_event_*` and run `pump()`, then check
both TinyUSB state and endpoint state. Keep virtual-DCD behavior explicit: it must
not silently complete an operation or supply a response on behalf of the stack.
