# Real TinyUSB host stack, virtual HID peripherals

```sh
python3 tests/usb_host/run.py
```

This native test compiles the checked-in TinyUSB host core, hub driver, HID host
class, common FIFO and TinyUSB support, together with production DeskHop `usb.c`,
`hid_parser.c`, `hid_report.c`, `keyboard.c`, and `reboot_hotkey.c`. The default run
uses ASan/UBSan and requires Python 3 and a C11 compiler. It takes approximately
one second and leaves build output in a temporary directory. The runner also
exports `build(directory, sanitize=True, coverage=False)` for coverage collection.

The virtual HCD supplies attach/remove interrupts, endpoint opens, setup/data/
status completions, and held interrupt-IN transfers. Its peripheral model answers
standard USB requests using independent, hand-auditable keyboard and mouse
fixtures shared with `tests/sim/fixtures.py`. The tested personalities are a
composite keyboard plus mouse interface (the relevant Sofle/QMK topology) and a
standalone mouse/trackball on output B. These are standard fixture descriptors,
not a captured descriptor dump of the user's exact Sofle or trackball.

The real host stack performs reset/debounce waits against virtual time, reads
initial/full device and configuration descriptors, assigns address/configuration,
opens HID endpoints, negotiates protocol, retrieves report descriptors, and calls
DeskHop's production mount callback. That callback executes the real parser and
arms its first report. Subsequent interrupt completions execute the real HID host
class, production dispatch and keyboard extraction, then rearm through the actual
`tuh_hid_receive_report` path. Enumeration also succeeds when the virtual device
stalls `SET_IDLE`, an explicitly permitted HID behavior handled by the real class
driver.

The suite checks:

- Composite and standalone device enumeration, interface counts, parsed keyboard
  and mouse presence, host mounting, and pending interrupt polls.
- A held Ctrl+A report through actual keyboard extraction and aggregation to a
  modeled host report queue; mouse signed X/Y extraction against the real parsed
  field offsets.
- Zero-length successful and failed transfers, and a truncated keyboard report,
  producing no new keyboard report or activity while polling is rearmed.
- HID `SET_REPORT` control output traversing the real host stack to a modeled
  peripheral's LED byte.
- Detach closing pending polls, clearing connection state, and the actual
  production unmount path emitting an all-up keyboard report and modifier refresh.
- A subsequent standalone trackball attach, report and detach on output B.

Production mouse coordinate transformation, hotkey actions, activity policy,
physical LED behavior, and application output transport are explicit test sinks
here. The dual-node simulator covers those application layers. The keyboard
output queue in this suite is an observable accepting sink, not the SDK queue.
The TinyUSB hub driver is linked because production enables hubs, but this suite
only tests directly attached root devices; downstream hub topology is a gap.

The native configuration imports production buffer/class settings, selects
`OPT_OS_NONE`, disables the unused device side, and uses one virtual root port
zero to stand in for physical PIO root port one. `native_options.h` keeps
Cortex-M0+ packed-safe accesses on the native compiler. There is no physical HCD,
PIO execution, USB PHY, token/data-toggle/CRC model, electrical timing, concurrent
controller execution, or actual target Mac in this test. Interrupt events enter
the real host queue but execute serially. Virtual time tests host enumeration
control flow, not microsecond bus timing.

To extend a peripheral, change the explicit descriptor/response fixture in
`make_configuration` and `hcd_edpt_xfer`; never pre-populate TinyUSB's internal
state or call a production mount callback directly. To deliver a report or an
empty/error completion, call `deliver(endpoint, bytes, size, result)` and assert
both application effects and rearmed endpoint state. Leaving a transfer pending
represents a peripheral with no report available; no synthetic report is supplied
unless the scenario explicitly injects one.
