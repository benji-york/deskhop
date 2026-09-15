"""Application behavior scenarios driven through independent USB fixtures.

All clocks and devices are simulated. The manual-clock cases still run the
production task scheduler; they omit background polling to check exact timer
boundaries and minutes of inactivity without a wall-clock wait.
"""
import struct

from fixtures import KEYBOARD, attach, keyboard, mouse
from test_transport import decode_frame


# USB HID usages/modifier bits are fixture values, independent of C headers.
CTRL_SHIFT = 0x21
COMMAND = 0x08
Q, G, Z, F24 = 0x14, 0x0A, 0x1D, 0x73
KEYBOARD_ID, ABS_MOUSE_ID, REL_MOUSE_ID = 1, 2, 5
KBD_TASK, MOUSE_TASK = 'process_kbd_queue_task', 'process_mouse_queue_task'
TX_TASK, RX_TASK = 'process_uart_tx_task', 'packet_receiver_task'
BLINK_TASK, LED_TASK = 'led_blinking_task', 'led_sync_task'
SAVER_TASK, HEARTBEAT_TASK, ZOOM_TASK = 'screensaver_task', 'heartbeat_output_task', 'zoom_assist_task'


def host_mouse(*, buttons=0, x=0, y=0, wheel=0, pan=0, relative=False):
    """Device-report fixture: buttons, little-endian 16-bit X/Y, wheel/pan/pad."""
    return struct.pack('<BhhbbB', buttons, x, y, wheel, pan, int(relative)).hex()


def key(s, modifiers=0, *keys):
    s.do(0, 'report', 1, 0, keyboard(modifiers, *keys))


def pointer(s, node=1, **values):
    s.do(node, 'report', 1, 0 if node else 1, mouse(**values))


def pump(s, us=5000):
    """Drain real TX/RX and USB queues without running timed policy tasks."""
    end = s.now + us
    while s.now < end:
        for node in (0, 1):
            for task in (TX_TASK, RX_TASK, KBD_TASK, MOUSE_TASK):
                s.do(node, 'task', task)
        s.advance(min(250, end - s.now))


def at(s, timestamp):
    assert timestamp >= s.now, (timestamp, s.now)
    s.advance(timestamp - s.now)


def maintenance_boot(s, target):
    attach(s)
    # The maintenance target is independent of which Mac currently has focus.
    survivor = 1 - target
    s.do(0, 'select', survivor)
    s.advance(5000)
    last_kick = s.get(survivor, 'last_kick')
    # Actual Layer 3 A/B report: both shifts, F12, and the selected letter.
    key(s, 0x22, 0x45, 0x04 + target)
    key(s)
    s.advance(10000)
    s.expect(target, 'stopped', 1)
    s.expect(survivor, 'stopped', 0)
    s.check('reset_count', target, 1, 1, 1)
    if target == 1:
        # The command retains its meaning inside the protected UART envelope.
        requests = [decode_frame(bytes.fromhex(x['data'])) for x in s.trace
                    if x['kind'] == 'uart_tx' and x['node'] == 0
                    and decode_frame(bytes.fromhex(x['data']))[0] == 4]
        assert requests == [(4, bytes([1] + [0] * 7))]
    # Pass one full watchdog deadline; the board left running still services
    # its cores after the target has left the firmware and UART link.
    s.advance(510000)
    s.expect(survivor, 'stopped', 0)
    s.check('range', survivor, 'last_kick', 0, last_kick + 1, s.now)


def scenario_maintenance_a_picoboot_only(s):
    maintenance_boot(s, 0)


def scenario_maintenance_b_picoboot_only(s):
    maintenance_boot(s, 1)


def scenario_reboot_three_completed_taps(s):
    attach(s)
    # Put a genuine held modifier on each host, then focus the second host.
    key(s, 0x02)
    s.advance(3000)
    s.expect_report(0, KEYBOARD_ID, keyboard(0x02))
    key(s, 0x02, F24)
    s.advance(5000)
    s.expect_report(0, KEYBOARD_ID, keyboard())
    key(s, 0x02)
    s.advance(3000)
    s.expect_report(1, KEYBOARD_ID, keyboard(0x02))

    for tap in range(3):
        key(s, CTRL_SHIFT, Q)
        # Auto-repeat/duplicate reports while Q remains down are not taps.
        for _ in range(3):
            s.advance(1000)
            key(s, CTRL_SHIFT, Q)
            s.expect(0, 'reboot', 0)
            s.expect(1, 'reboot', 0)
        key(s, CTRL_SHIFT)
        s.expect(0, 'reboot', int(tap == 2))
        s.expect(0, 'blinks', 5)
        s.advance(5000)
        s.expect(1, 'reboot', int(tap == 2))
        if tap < 2:
            s.expect_report(1, KEYBOARD_ID, keyboard(CTRL_SHIFT))

    for node in (0, 1):
        s.expect_report(node, KEYBOARD_ID, keyboard())
        s.expect(node, 'modifiers', 0)
        s.expect(node, 'stopped', 0)
    s.check('key_absent', Q)
    # Both modeled watchdogs expire after giving queued all-up reports time.
    s.advance(600000)
    for node in (0, 1):
        s.expect(node, 'stopped', 1)


def scenario_reboot_rejects_extra_modifiers(s):
    attach(s)
    for extra in (0x02, 0x04, 0x08, 0x10, 0x40, 0x80):
        for _ in range(3):
            key(s, CTRL_SHIFT | extra, Q)
            s.advance(2000)
            s.expect_report(0, KEYBOARD_ID, keyboard(CTRL_SHIFT | extra, Q))
            key(s)
            s.advance(2000)
            for node in (0, 1):
                s.expect(node, 'reboot', 0)
    # Two valid taps followed by an inexact chord cancel accumulated progress.
    for _ in range(2):
        key(s, CTRL_SHIFT, Q)
        key(s)
        s.advance(2000)
    key(s, CTRL_SHIFT | COMMAND, Q)
    key(s)
    s.advance(2000)
    for _ in range(2):
        key(s, CTRL_SHIFT, Q)
        key(s)
        s.advance(2000)
        s.expect(0, 'reboot', 0)
        s.expect(1, 'reboot', 0)
    s.expect_report(0, KEYBOARD_ID, keyboard())


def scenario_f24_releases_held_modifiers(s):
    attach(s)
    output = 0
    for modifiers in (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xFF):
        key(s, modifiers)
        s.advance(3000)
        s.expect_report(output, KEYBOARD_ID, keyboard(modifiers))
        key(s, modifiers, F24)
        output ^= 1
        s.advance(5000)
        for node in (0, 1):
            s.expect(node, 'output', output)
            s.expect(node, 'modifiers', 0)
            s.expect_report(node, KEYBOARD_ID, keyboard())
        key(s)
        s.advance(2000)
    s.check('key_absent', F24)


def scenario_led_focus_and_acknowledgement(s):
    attach(s)
    s.do(1, 'mount', 2, 0, 1, KEYBOARD.hex())
    pump(s)
    # Device attachment acknowledges itself; complete that sequence first.
    for _ in range(5):
        s.advance(80000)
        for node in (0, 1):
            s.do(node, 'task', BLINK_TASK)
    for node in (0, 1):
        s.do(node, 'set', 'led_indicator', 0, 0)
    s.do(0, 'led', 3)  # Host A asks for Num Lock + Caps Lock.
    s.do(1, 'led', 4)  # Host B asks for Scroll Lock.
    pump(s)
    for node in (0, 1):
        s.do(node, 'task', LED_TASK)
        s.expect(node, 'led', 3, node)
    key(s, 0, F24)
    pump(s)
    for node in (0, 1):
        s.expect(node, 'output', 1)
        s.expect(node, 'led', 4, node)
        s.do(node, 'set', 'led_indicator', 0, 1)
    s.advance(34000)
    for node in (0, 1):
        s.do(node, 'task', LED_TASK)
        s.expect(node, 'led', 6, node)  # Scroll Lock + output-B Caps indicator.

    key(s, CTRL_SHIFT, G)
    start = s.now
    s.expect(0, 'blinks', 5)
    s.advance(79999)
    s.expect(0, 'blinks', 5)
    s.expect(0, 'led', 6)
    # Acknowledgement uses five 80 ms transitions; periodic sync must not
    # replace those intentional states with the host/focus LEDs midway.
    for transition, expected in enumerate((7, 0, 7, 0, 6), 1):
        at(s, start + transition * 80000)
        s.do(0, 'task', BLINK_TASK)
        s.expect(0, 'blinks', 5 - transition)
        s.expect(0, 'led', expected)
        s.do(0, 'task', LED_TASK)
        s.expect(0, 'led', expected)
    key(s)
    key(s, 0, F24)
    pump(s)
    for node in (0, 1):
        s.expect(node, 'output', 0)
        s.expect(node, 'led', 1, node)  # Preserve Num Lock; clear focus indicator.


def expect_zoom(s, *, active, debt, overscroll=0, pending=0, direction=1):
    for node in (0, 1):
        for field, value in (('zoom', active), ('zoom_debt', debt),
                             ('zoom_overscroll', overscroll), ('zoom_pending', pending),
                             ('zoom_direction', direction)):
            s.expect(node, field, value)


def scenario_zoom_scroll_debt_and_quiet_exit(s):
    attach(s)
    pointer(s, wheel=2)
    pump(s)
    expect_zoom(s, active=0, debt=0, direction=0)
    s.expect_report(0, ABS_MOUSE_ID, host_mouse(x=16000, y=16000, wheel=2))
    key(s, COMMAND)
    pump(s)
    s.expect(1, 'peer_modifiers', COMMAND)
    pointer(s, wheel=4)
    pump(s)
    expect_zoom(s, active=1, debt=4)
    s.expect_report(0, REL_MOUSE_ID, host_mouse(wheel=4, relative=True))

    pointer(s, wheel=-3)
    pump(s)
    expect_zoom(s, active=1, debt=1)
    pointer(s, wheel=-1)
    pump(s)
    expect_zoom(s, active=1, debt=0)
    s.advance(300000)
    s.do(0, 'task', ZOOM_TASK)
    expect_zoom(s, active=1, debt=0)  # Repaying debt alone is not an exit.
    pointer(s, wheel=-5)
    pump(s)
    expect_zoom(s, active=1, debt=0, overscroll=5)
    pointer(s, wheel=-1)
    pump(s)
    expect_zoom(s, active=1, debt=0, overscroll=6, pending=1)
    first_deadline = s.get(0, 'zoom_deadline')
    s.expect(0, 'zoom_deadline', first_deadline)

    # A further qualifying wheel event restarts the quiet timer.
    at(s, first_deadline - 50000)
    pointer(s, wheel=-1)
    pump(s)
    expect_zoom(s, active=1, debt=0, overscroll=7, pending=1)
    deadline = s.get(0, 'zoom_deadline')
    s.check('range', 0, 'zoom_deadline', 0, first_deadline+1, 2**63-1)
    at(s, first_deadline)
    s.do(0, 'task', ZOOM_TASK)
    expect_zoom(s, active=1, debt=0, overscroll=7, pending=1)
    at(s, deadline - 1)
    s.expect(0, 'zoom', 1)
    at(s, deadline)
    s.do(0, 'task', ZOOM_TASK)
    s.expect(0, 'zoom', 0)
    pump(s)
    expect_zoom(s, active=0, debt=0)
    for node in (0, 1):
        s.expect(node, 'gaming', 0)
    key(s)
    pump(s)
    pointer(s, node=0, x=1)
    pump(s)
    s.expect_report(0, ABS_MOUSE_ID, host_mouse(x=16001, y=16000))


def scenario_zoom_and_gaming_are_independent(s):
    attach(s)
    key(s, CTRL_SHIFT, G)
    key(s)
    pump(s)
    for node in (0, 1):
        s.expect(node, 'gaming', 1)
    key(s, COMMAND)
    pump(s)
    # Learn the other natural-scrolling direction on this fresh paired image.
    pointer(s, wheel=-2)
    pump(s)
    expect_zoom(s, active=1, debt=2, direction=-1)
    key(s, CTRL_SHIFT, Z)
    key(s)
    pump(s)
    expect_zoom(s, active=0, debt=0, direction=0)
    for node in (0, 1):
        s.expect(node, 'gaming', 1)
    pointer(s, node=0, x=3)
    pump(s)
    s.expect_report(0, REL_MOUSE_ID, host_mouse(x=3, relative=True))

    key(s, COMMAND)
    pump(s)
    pointer(s, wheel=2)
    pump(s)
    expect_zoom(s, active=1, debt=2)
    key(s, CTRL_SHIFT, G)
    key(s)
    pump(s)
    for node in (0, 1):
        s.expect(node, 'gaming', 0)
    expect_zoom(s, active=1, debt=2)
    pointer(s, node=0, x=2)
    pump(s)
    s.expect_report(0, REL_MOUSE_ID, host_mouse(x=2, relative=True))


def configure_keepawake(s, *, inactive_only=False, timeout=30):
    for node in (0, 1):
        s.do(node, 'set', 'ss_mode', node, 2)
        s.do(node, 'set', 'ss_idle', node, 2000000)
        s.do(node, 'set', 'ss_max', node, 0)
        s.do(node, 'set', 'ss_inactive', node, int(inactive_only))
        s.do(node, 'set', 'ss_timeout', 0, timeout)


def saver_pulse(s, node, *, y=None):
    s.do(node, 'task', SAVER_TASK)
    s.expect(node, 'mouse_queue', int(y is not None))
    if y is not None:
        s.do(node, 'task', MOUSE_TASK)
        s.expect(node, 'mouse_queue', 0)
        s.expect_report(node, REL_MOUSE_ID, host_mouse(y=y, relative=True))


def heartbeat(s):
    for node in (0, 1):
        s.do(node, 'task', HEARTBEAT_TASK)
    pump(s)


def scenario_timed_system_wide_keepawake(s):
    attach(s)
    pump(s)
    configure_keepawake(s)
    at(s, 10000000)
    for node in (0, 1):
        saver_pulse(s, node, y=-2)
        s.expect(node, 'direct_valid', 0)
        s.expect(node, 'peer_valid', 0)

    # Real physical input on both Picos targets the same active computer.
    at(s, 12000000)
    key(s, 0, 4)
    key(s)
    pump(s)
    at(s, 14000000)
    pointer(s, node=1, x=1)
    pump(s)
    s.expect(0, 'direct', 12000000)
    s.expect(1, 'direct', 14000000)
    s.expect(0, 'direct_valid', 1)
    s.expect(1, 'direct_valid', 1)
    heartbeat(s)
    for node in (0, 1):
        s.expect(node, 'peer_valid', 1)
    # Source timestamps remain unchanged through repeated synchronization and
    # synthetic output. Age packets may round by <1 second, never echo forward.
    for second, y in ((20, 2), (30, -2), (40, 2)):
        at(s, second * 1000000)
        heartbeat(s)
        for node in (0, 1):
            saver_pulse(s, node, y=y)
            s.expect(node, 'direct', (12 if node == 0 else 14) * 1000000)
            s.expect(node, 'direct', 0, 1)
            s.expect(node, 'activity', 0, 1)
            s.expect(node, 'peer', 0, 1)
        s.check('range', 0, 'peer', 0, 14000000, 14999999)
        s.check('range', 1, 'peer', 0, 12000000, 12999999)
        s.expect(0, 'peer', s.get(0, 'peer'))
        s.expect(1, 'peer', s.get(1, 'peer'))

    at(s, 50000000)
    heartbeat(s)
    for node in (0, 1):
        saver_pulse(s, node)  # Entire system idle for more than 30 seconds.
        s.expect(node, 'direct', (12 if node == 0 else 14) * 1000000)

    # Focus the other computer, then real input at its own Pico restarts both
    # computers. Selection/all-up itself must not invent physical activity.
    at(s, 51000000)
    s.do(0, 'select', 1)
    pump(s)
    s.expect(0, 'direct', 12000000)
    s.expect(1, 'direct', 14000000)
    at(s, 52000000)
    pointer(s, node=1, x=1)
    pump(s)
    s.expect(1, 'direct', 52000000, 1)
    s.expect(1, 'direct', 14000000)
    heartbeat(s)
    at(s, 55000000)
    for node in (0, 1):
        saver_pulse(s, node, y=-2)


def scenario_keepawake_inactive_only(s):
    attach(s)
    pump(s)
    configure_keepawake(s, inactive_only=True, timeout=60)
    at(s, 10000000)
    saver_pulse(s, 0)
    saver_pulse(s, 1, y=-2)
    key(s, 0, F24)
    key(s)
    pump(s)
    for node in (0, 1):
        s.expect(node, 'output', 1)
    at(s, 20000000)
    saver_pulse(s, 0, y=-2)
    saver_pulse(s, 1)
    # Disable the system-wide cutoff explicitly; the inactive-only gate stays.
    for node in (0, 1):
        s.do(node, 'set', 'ss_timeout', 0, 0)
    at(s, 90000000)
    saver_pulse(s, 0, y=2)
    saver_pulse(s, 1)


SCENARIOS = {
    name.removeprefix('scenario_'): function
    for name, function in list(globals().items())
    if name.startswith('scenario_')
}
BACKGROUND_FALSE = set(SCENARIOS) - {
    'reboot_three_completed_taps', 'reboot_rejects_extra_modifiers',
    'f24_releases_held_modifiers', 'maintenance_a_picoboot_only',
    'maintenance_b_picoboot_only',
}
