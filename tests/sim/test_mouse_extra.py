"""Independent mixed-source, output handoff and wire-negotiation regressions."""
from fixtures import attach, keyboard, mouse, MOUSE
from test_behaviors import pump
from test_transport import frame, config_frame, out_mouse


def scenario_mouse_output_switch_held(s):
    attach(s)
    for old_output in (0, 1):
        for relative in (0, 1):
            s.do(0, 'select', old_output)
            s.advance(5000)
            for node in (0, 1):
                s.do(node, 'set', 'gaming', 0, relative)
                s.do(node, 'set', 'x', 0, 16000)
                s.do(node, 'set', 'y', 0, 16000)
            report_id = 5 if relative else 2
            coord = 0 if relative else 16000
            # Different devices share bit 0; B also holds bit 1.
            s.do(0, 'report', 1, 1, mouse(buttons=1))
            s.advance(5000)
            s.do(1, 'report', 1, 0, mouse(buttons=3))
            s.advance(5000)
            s.expect_report(old_output, report_id, out_mouse(3, coord, coord, mode=relative))

            # Exercise the actual keyboard hotkey, including its remote merge.
            s.do(0, 'report', 1, 0, keyboard(0, 0x73))
            s.advance(10000)
            s.do(0, 'report', 1, 0, keyboard())
            new_output = 1 - old_output
            for node in (0, 1):
                s.expect(node, 'output', new_output)
            # Each HID mouse interface has its own host button latch.
            s.expect_report(old_output, 2, out_mouse(0, 16000, 16000))
            s.expect_report(old_output, 5, out_mouse(0, 0, 0, mode=1))

            # Physical holds survive the all-up sent to the previous host.
            s.do(0, 'report', 1, 1, mouse(buttons=1, x=4, wheel=1, pan=-1))
            s.advance(5000)
            x = 4 if relative else 16004
            s.expect_report(new_output, report_id,
                            out_mouse(3, x, coord, wheel=1, pan=-1, mode=relative))
            before = [s.get(n, 'direct', new_output) for n in (0, 1)]
            s.do(1, 'unmount', 1, 0)
            s.advance(5000)
            x = 0 if relative else 16004
            s.expect_report(new_output, report_id, out_mouse(1, x, coord, mode=relative))
            for node in (0, 1):
                s.expect(node, 'direct', before[node], new_output)
            s.do(0, 'report', 1, 1, mouse())
            s.advance(5000)
            s.expect_report(new_output, report_id, out_mouse(0, x, coord, mode=relative))
            s.expect_report(old_output, 2, out_mouse(0, 16000, 16000))
            s.expect_report(old_output, 5, out_mouse(0, 0, 0, mode=1))
            s.do(1, 'mount', 1, 0, 2, MOUSE.hex())
            s.advance(5000)


def scenario_mouse_asymmetric_capability(s):
    attach(s)
    # Only B has sent its heartbeat. A knows B supports explicit messages, but
    # B has not yet learned A's capabilities. This needs no corrupted packets.
    s.do(1, 'task', 'heartbeat_output_task')
    pump(s)
    s.do(0, 'report', 1, 1, mouse(buttons=1))
    # B constructs its ordinary report before A's newly queued mirror arrives.
    s.do(1, 'report', 1, 0, mouse(buttons=2, x=3))
    pump(s)
    s.expect_report(0, 2, out_mouse(3, 16003, 16000))
    s.expect(0, 'buttons', 3)
    # The release is position-neutral; an earlier queued pointer mirror may
    # legitimately leave B's local cursor cache behind A during this startup.
    s.do(1, 'report', 1, 0, mouse())
    pump(s)
    s.expect_report(0, 2, out_mouse(1, 16003, 16000))
    s.do(0, 'report', 1, 1, mouse())
    pump(s)
    s.expect_report(0, 2, out_mouse(0, 16003, 16000))


def scenario_mouse_switch_inflight_source(s):
    attach(s)
    for old_output in (0, 1):
        for motion in (0, 3):
            s.do(0, 'select', old_output)
            s.advance(5000)
            for node in (0, 1):
                s.do(node, 'set', 'x', 0, 16000)
                s.do(node, 'set', 'y', 0, 16000)
            old_itf = 1 if old_output == 0 else 0
            new_output = 1 - old_output
            new_itf = 1 if new_output == 0 else 0
            s.do(old_output, 'report', 1, old_itf, mouse(buttons=1))
            s.advance(5000)
            s.do(old_output, 'select', new_output)
            # The peer has not received selection yet and legitimately routes
            # this source input using the previous output. It must not re-latch
            # the old host after that host's switch-critical all-up report.
            s.do(new_output, 'report', 1, new_itf, mouse(buttons=2, x=motion))
            s.advance(10000)
            s.expect_report(old_output, 2, out_mouse(0, 16000, 16000))
            s.expect_report(old_output, 5, out_mouse(0, 0, 0, mode=1))
            # Fresh input after convergence still merges both physical holds.
            s.do(new_output, 'report', 1, new_itf, mouse(buttons=2, x=4))
            s.advance(5000)
            # Position may follow the accepted cursor mirror during handoff;
            # only the independently owned button mask is relevant here.
            s.check('usb_bytes', new_output, 2, -1, 0, '03')
            s.do(new_output, 'report', 1, new_itf, mouse())
            s.advance(5000)
            s.do(old_output, 'report', 1, old_itf, mouse())
            s.advance(5000)
            s.check('usb_bytes', new_output, 2, -1, 0, '00')
            s.expect_report(old_output, 2, out_mouse(0, 16000, 16000))


def scenario_mouse_switch_full_queue(s):
    attach(s)
    s.do(0, 'report', 1, 1, mouse(buttons=1))
    s.advance(5000)
    s.expect_report(0, 2, out_mouse(1, 16000, 16000))
    s.do(0, 'endpoint', 0, 1, 0)
    s.do(0, 'fill', 2, 512)
    started = s.now
    s.do(0, 'select', 1)
    # A permanently blocked all-up admission requests recovery within the
    # existing 100 ms critical-input bound instead of silently succeeding.
    s.expect(0, 'reboot', 1)
    s.expect(0, 'mouse_queue', 512)
    s.check('time_range', started + 100000, started + 100999)


def scenario_mouse_synthetic_desktop_preserves_sources(s):
    attach(s)
    # Enable an extra macOS desktop on A through the real configuration API.
    for node in (0, 1):
        s.do(node, 'set', 'config_mode', 0, 1)
        s.do(node, 'vendor', config_frame(21, b'\x0b' + (2).to_bytes(4, 'little')))
        s.do(node, 'set', 'config_mode', 0, 0)
    s.do(0, 'report', 1, 1, mouse(buttons=1))
    s.advance(5000)
    s.do(1, 'report', 1, 0, mouse(buttons=2))
    s.advance(5000)
    for node in (0, 1):
        s.do(node, 'set', 'x', 0, 32766)
    before = len(s.reports(0, 5))
    s.do(1, 'report', 1, 0, mouse(buttons=2, x=4))
    s.advance(10000)
    # macOS desktop switching deliberately uses button-free relative nudges.
    # Merging physical holds into these exact synthetic reports sticks the
    # independent relative HID interface down after the desktop transition.
    s.check('usb_count', 0, 5, before + 5)
    for index in range(before, before + 5):
        s.check('usb_bytes', 0, 5, index, 0, out_mouse(0, 10, 0, mode=1))
    s.expect_report(0, 2, out_mouse(3, 32767, 16383))
    # A subsequent real report still includes both source-owned holds.
    s.do(1, 'report', 1, 0, mouse(buttons=2, x=5))
    s.advance(5000)
    s.expect_report(0, 2, out_mouse(3, 5, 16000))
    s.do(1, 'unmount', 1, 0)
    s.advance(5000)
    s.expect_report(0, 2, out_mouse(1, 5, 16000))
    s.do(0, 'report', 1, 1, mouse())
    s.advance(5000)
    s.expect_report(0, 2, out_mouse(0, 5, 16000))


def scenario_mouse_incomplete_fragments_keep_holds(s):
    attach(s)
    descriptor = bytes.fromhex(
        '05 01 09 02 a1 01 '
        '85 07 05 09 19 01 29 08 15 00 25 01 75 01 95 08 81 02 '
        '85 09 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 '
        '05 0c 0a 38 02 75 08 95 01 81 06 c0')
    for output in (0, 1):
        s.do(0, 'select', output)
        s.advance(5000)
        s.do(0, 'mount', 2, 0, 0, descriptor.hex())
        for node in (0, 1):
            s.do(node, 'set', 'x', 0, 16000)
            s.do(node, 'set', 'y', 0, 16000)
        s.do(1, 'report', 1, 0, mouse(buttons=1))
        s.advance(5000)
        s.do(0, 'report', 2, 0, '0702')
        s.advance(5000)
        s.expect_report(output, 2, out_mouse(3, 16000, 16000))
        before = s.get(0, 'direct', output)
        count = len(s.reports(output, 2))
        for fragment in ('07', '09', '0901', '09010000', '0b00', ''):
            s.do(0, 'report', 2, 0, fragment)
            s.advance(1000)
        s.check('usb_count', output, 2, count)
        s.expect(0, 'direct', before, output)
        s.do(0, 'report', 2, 0, '090300ff02')
        s.advance(5000)
        s.expect_report(output, 2, out_mouse(3, 16003, 16000, wheel=-1, pan=2))
        s.do(0, 'unmount', 2, 0)
        s.advance(5000)
        s.expect_report(output, 2, out_mouse(1, 16003, 16000))
        s.do(1, 'report', 1, 0, mouse())
        s.advance(5000)
        s.expect_report(output, 2, out_mouse(0, 16003, 16000))


SCENARIOS = {
    'mouse_output_switch_held': scenario_mouse_output_switch_held,
    'mouse_asymmetric_capability': scenario_mouse_asymmetric_capability,
    'mouse_switch_inflight_source': scenario_mouse_switch_inflight_source,
    'mouse_switch_full_queue': scenario_mouse_switch_full_queue,
    'mouse_synthetic_desktop_preserves_sources': scenario_mouse_synthetic_desktop_preserves_sources,
    'mouse_incomplete_fragments_keep_holds': scenario_mouse_incomplete_fragments_keep_holds,
}
BACKGROUND_FALSE = {'mouse_asymmetric_capability'}
