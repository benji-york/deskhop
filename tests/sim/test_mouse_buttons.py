"""Source-owned mouse buttons: independent USB report and activity oracles."""
from fixtures import attach, mouse, MOUSE
from test_transport import out_mouse


def scenario_mouse_button_aggregation(s):
    attach(s)
    for output in (0, 1):
        s.do(0, 'select', output); s.advance(10000)
        for relative in (0, 1):
            for node in (0, 1): s.do(node, 'set', 'gaming', 0, relative)
            report_id = 5 if relative else 2
            coord = 0 if relative else 16000
            def expect(buttons):
                s.expect_report(output, report_id, out_mouse(buttons, coord, coord, mode=relative))
            s.do(1, 'report', 1, 0, mouse(buttons=1)); s.advance(5000); expect(1)
            s.do(0, 'report', 1, 1, mouse(buttons=2)); s.advance(5000); expect(3)
            # Cover the full source-report path as well as nonmotion packets.
            s.do(1, 'report', 1, 0, mouse(buttons=1, x=3, y=-2, wheel=1)); s.advance(5000)
            s.expect_report(output, report_id, out_mouse(3,
                3 if relative else 16003, -2 if relative else 15998, wheel=1, mode=relative))
            s.do(0, 'report', 1, 1, mouse(buttons=2, x=-3, y=2, wheel=-1)); s.advance(5000)
            s.expect_report(output, report_id, out_mouse(3,
                -3 if relative else 16000, 2 if relative else 16000, wheel=-1, mode=relative))
            s.do(0, 'report', 1, 1, mouse()); s.advance(5000); expect(1)
            # An idle composite interface must neither release the peer's button
            # nor create activity or another host report.
            before = s.get(0, 'direct', output); count = len(s.reports(output, report_id))
            s.do(0, 'report', 1, 1, mouse()); s.advance(5000)
            s.expect(0, 'direct', before, output); s.check('usb_count', output, report_id, count)
            # Both sources hold the same bit. Releasing either must preserve it.
            s.do(0, 'report', 1, 1, mouse(buttons=1)); s.advance(5000); expect(1)
            s.do(1, 'report', 1, 0, mouse()); s.advance(5000); expect(1)
            s.do(0, 'report', 1, 1, mouse()); s.advance(5000); expect(0)


def scenario_mouse_button_sources_and_detach(s):
    attach(s)
    s.do(0, 'mount', 2, 0, 2, MOUSE.hex()); s.advance(5000)
    s.do(1, 'report', 1, 0, mouse(buttons=1)); s.advance(5000)
    s.do(0, 'report', 1, 1, mouse(buttons=2)); s.advance(5000)
    s.do(0, 'report', 2, 0, mouse(buttons=4, x=5)); s.advance(5000)
    s.expect_report(0, 2, out_mouse(7, 16005, 16000))
    before = [s.get(n, 'direct', 0) for n in (0, 1)]
    s.do(0, 'unmount', 1, 1); s.advance(5000)
    s.expect_report(0, 2, out_mouse(5, 16005, 16000))
    s.do(1, 'unmount', 1, 0); s.advance(5000)
    s.expect_report(0, 2, out_mouse(4, 16005, 16000))
    s.do(0, 'unmount', 2, 0); s.advance(5000)
    s.expect_report(0, 2, out_mouse(0, 16005, 16000))
    for n in (0, 1): s.expect(n, 'direct', before[n], 0)
    s.do(0, 'mount', 2, 0, 2, MOUSE.hex()); s.advance(5000)
    count = len(s.reports(0, 2))
    s.do(0, 'report', 2, 0, mouse()); s.advance(5000)
    s.check('usb_count', 0, 2, count)


def scenario_mouse_button_split_reports(s):
    attach(s)
    # One interface sends buttons on ID 7, motion/wheel on ID 9. Motion-only
    # reports must retain this interface's buttons without copying peer state.
    descriptor = bytes.fromhex(
        '05 01 09 02 a1 01 '
        '85 07 05 09 19 01 29 08 15 00 25 01 75 01 95 08 81 02 '
        '85 09 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 '
        '05 0c 0a 38 02 75 08 95 01 81 06 c0')
    s.do(0, 'mount', 2, 0, 0, descriptor.hex()); s.advance(5000)
    s.do(1, 'report', 1, 0, mouse(buttons=1)); s.advance(5000)
    s.do(0, 'report', 2, 0, '0702'); s.advance(5000)
    s.expect_report(0, 2, out_mouse(3, 16000, 16000))
    s.do(0, 'report', 2, 0, '0905000000'); s.advance(5000)
    s.expect_report(0, 2, out_mouse(3, 16005, 16000))
    s.do(0, 'report', 2, 0, '0700'); s.advance(5000)
    s.expect_report(0, 2, out_mouse(1, 16005, 16000))
    s.do(0, 'report', 2, 0, '0905000000'); s.advance(5000)
    s.expect_report(0, 2, out_mouse(1, 16010, 16000))
    s.do(1, 'report', 1, 0, mouse()); s.advance(5000)
    s.expect_report(0, 2, out_mouse(0, 16010, 16000))


SCENARIOS = {
    'mouse_button_aggregation': scenario_mouse_button_aggregation,
    'mouse_button_sources_and_detach': scenario_mouse_button_sources_and_detach,
    'mouse_button_split_reports': scenario_mouse_button_split_reports,
}
