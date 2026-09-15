"""Authoritative keyboard state at the host after transient queue/link failure.

Fixtures use real physical reports and the production UART/USB queues. Recovery
is required within 1.2 seconds after transport/endpoint restoration and draining
this bounded backlog; permanent partitions do not promise key delivery. The
host oracle checks actual USB reports, not the firmware's modifier mirror.
"""
import struct
import zlib

from fixtures import KEYBOARD, attach, keyboard
from test_transport import frame, decode_frame
from test_selection import selection_frame

SHIFT, CTRL = 0x02, 0x01
A, B, C, F24 = 0x04, 0x05, 0x06, 0x73
RECOVERY_US = 1_200_000


def setup(s):
    attach(s)
    s.do(1, 'mount', 2, 0, 1, KEYBOARD.hex())
    s.advance(5000)


def key(s, source, modifiers=0, *keys):
    s.do(source, 'report', 1 if source == 0 else 2, 0,
         keyboard(modifiers, *keys))


def focus(s, target):
    s.do(0, 'select', target)
    s.advance(10000)
    for node in (0, 1):
        s.expect(node, 'output', target)


def healthy(s):
    for node in (0, 1):
        s.expect(node, 'stopped', 0)
        s.expect(node, 'reboot', 0)


def released(s, target):
    s.expect_report(target, 1, keyboard())
    s.expect(target, 'kbd_queue', 0)
    healthy(s)


def scenario_keyboard_local_queue_release(s):
    setup(s)
    for target in (0, 1):
        focus(s, target)
        key(s, target, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.do(target, 'endpoint', 0, 1, 0)
        # Fill with useful transitions, not artificial queue entries. Last
        # accepted state is Shift+B; the next ordinary all-up overflows af100bc.
        for index in range(128):
            key(s, target, SHIFT, A if index % 2 == 0 else B)
        s.expect(target, 'kbd_queue', 128)
        started = s.now
        key(s, target)
        s.check('time_range', started, started + 1000)
        healthy(s)
        s.do(target, 'endpoint', 0, 0, 0)
        s.advance(RECOVERY_US)
        released(s, target)


def scenario_keyboard_uart_queue_release(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.do(source, 'uart_stall', 1)
        s.do(source, 'fill', 1, 256)
        s.expect(source, 'uart_queue', 256)
        started = s.now
        key(s, source)
        s.check('time_range', started, started + 1000)
        s.do(source, 'uart_stall', 0)
        s.advance(RECOVERY_US)
        s.expect(source, 'uart_queue', 0)
        released(s, target)


def scenario_keyboard_wire_release_loss(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        # Drop the whole release exchange, including its modifier mirror. A
        # later ordinary modifier heartbeat alone cannot repair the old bug.
        s.do(source, 'fault', {'drop': 1000})
        key(s, source)
        s.advance(10000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.do(source, 'fault', {'drop': 0})
        s.advance(RECOVERY_US)
        released(s, target)
        s.expect(target, 'peer_modifiers', 0)


def scenario_keyboard_wire_release_corruption(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        # Modifier is unchanged, so the next source packet carries the key
        # transition. Corrupt one payload byte while retaining its checksum.
        s.do(source, 'fault', {'xor': 1})
        key(s, source, SHIFT)
        s.advance(RECOVERY_US)
        s.expect_report(target, 1, keyboard(SHIFT))
        key(s, source)
        s.advance(RECOVERY_US)
        released(s, target)


def scenario_keyboard_resumed_remote_endpoint(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.do(target, 'endpoint', 0, 1, 0)
        for index in range(132):
            key(s, source, SHIFT, A if index % 2 == 0 else B)
            s.advance(2000)  # UART progresses while the host endpoint stalls.
        key(s, source)
        s.advance(10000)
        s.do(target, 'endpoint', 0, 0, 1)
        s.advance(10000)  # Readiness with rejected submissions must retain work.
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.do(target, 'endpoint', 0, 0, 0)
        s.advance(RECOVERY_US)
        released(s, target)


def scenario_keyboard_order_and_snapshot_dedup(s):
    setup(s)
    for source, target in ((0, 0), (0, 1), (1, 0), (1, 1)):
        focus(s, target)
        before = len(s.reports(target, 1))
        transitions = ((SHIFT, A), (SHIFT, B), (SHIFT,), ())
        for report in transitions:
            key(s, source, *report)
            s.advance(3000)
        expected = [keyboard(*report) for report in transitions]
        assert [r['data'] for r in s.reports(target, 1)[before:]] == expected
        count = len(s.reports(target, 1))
        s.advance(RECOVERY_US)
        s.check('usb_count', target, 1, count)
        key(s, source, SHIFT, A)
        s.advance(5000)
        count = len(s.reports(target, 1))
        s.advance(RECOVERY_US)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.check('usb_count', target, 1, count)
        key(s, source)
        s.advance(5000)
        released(s, target)


def scenario_keyboard_sources_release_and_detach(s):
    setup(s)
    # A second physical local source plus the peer must each retain ownership
    # of shared Shift while the other sources release or disconnect.
    s.do(0, 'mount', 3, 0, 1, KEYBOARD.hex())
    for target in (0, 1):
        focus(s, target)
        key(s, 0, SHIFT, A)
        key(s, 1, SHIFT, B)
        s.do(0, 'report', 3, 0, keyboard(CTRL, C))
        s.advance(10000)
        reports = bytes.fromhex(s.reports(target, 1)[-1]['data'])
        assert reports[0] == SHIFT | CTRL and set(reports[2:]) == {0, A, B, C}
        key(s, 0)
        s.advance(5000)
        reports = bytes.fromhex(s.reports(target, 1)[-1]['data'])
        assert reports[0] == SHIFT | CTRL and set(reports[2:]) == {0, B, C}
        s.do(0, 'unmount', 3, 0)
        s.advance(RECOVERY_US)
        s.expect_report(target, 1, keyboard(SHIFT, B))
        key(s, 1)
        s.advance(RECOVERY_US)
        released(s, target)
        s.do(0, 'mount', 3, 0, 1, KEYBOARD.hex())


def scenario_keyboard_host_reconnect_discards_stale_queue(s):
    setup(s)
    for target in (0, 1):
        focus(s, target)
        key(s, target, SHIFT, A)
        s.advance(5000)
        s.do(target, 'endpoint', 0, 1, 0)
        key(s, target, SHIFT, B)
        s.do(target, 'host', 0, 0)
        key(s, target)
        s.advance(10000)
        before = len(s.reports(target, 1))
        s.do(target, 'endpoint', 0, 0, 0)
        s.do(target, 'host', 1, 0)
        s.advance(RECOVERY_US)
        released(s, target)
        for report in s.reports(target, 1)[before:]:
            assert report['data'] == keyboard(), 'stale down replayed after host reconnect'


def scenario_keyboard_delayed_down_after_release(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT)
        s.advance(5000)
        s.do(source, 'fault', {'delay': 100000})
        key(s, source, SHIFT, A)
        s.advance(5000)  # Down has left the TX queue but is still on the wire.
        s.do(source, 'fault', {'delay': 0})
        key(s, source)
        s.advance(20000)
        s.expect_report(target, 1, keyboard())
        before = len(s.reports(target, 1))
        s.advance(RECOVERY_US)
        released(s, target)
        for report in s.reports(target, 1)[before:]:
            assert report['data'] == keyboard(), 'older delayed state restored a released key'


def scenario_keyboard_delayed_down_across_focus(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT)
        s.advance(5000)
        s.do(source, 'fault', {'delay': 100000})
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.do(source, 'fault', {'delay': 0})
        key(s, source, SHIFT, F24)
        s.advance(20000)
        key(s, source)
        s.advance(20000)
        for node in (0, 1):
            s.expect(node, 'output', source)
            s.expect_report(node, 1, keyboard())
        before = [len(s.reports(node, 1)) for node in (0, 1)]
        s.advance(RECOVERY_US)
        for node in (0, 1):
            released(s, node)
            for report in s.reports(node, 1)[before[node]:]:
                assert report['data'] == keyboard(), 'old-focus state replayed after handoff'
    s.check('key_absent', F24)


def scenario_keyboard_consumed_hotkey_never_snapshots(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        s.do(source, 'mount', 3, 0, 1, KEYBOARD.hex())
        key(s, source, 0x21, 0x0A)  # Ctrl+RightShift+G: consumed gaming toggle.
        # A second local physical source must not expose the swallowed chord.
        s.do(source, 'report', 3, 0, keyboard(SHIFT, B))
        s.advance(RECOVERY_US)
        s.expect_report(target, 1, keyboard(SHIFT, B))
        s.check('key_absent', 0x0A)
        key(s, source)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, B))
        s.do(source, 'report', 3, 0, keyboard())
        s.advance(5000)
        released(s, target)


def scenario_keyboard_snapshot_preserves_activity(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        s.do(source, 'fault', {'duplicate': True})
        key(s, source, SHIFT, A)
        s.advance(RECOVERY_US)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        count = len(s.reports(target, 1))
        direct = s.get(source, 'direct', target)
        s.advance(RECOVERY_US)
        s.check('usb_count', target, 1, count)
        # ACTIVITY_MSG reconstructs whole-second ages, so delivery jitter may
        # move this mirror by less than a second without new real input.
        s.check('range', target, 'peer', target, direct, direct + 1010000)
        s.expect(source, 'direct', direct, target)
        key(s, source)
        s.advance(5000)
        released(s, target)


def scenario_keyboard_diagnostic_progress(s):
    setup(s)
    focus(s, 1)
    for node in (0, 1):
        s.do(node, 'diagnostic_request', 0x44530000 + node)
        s.expect(node, 'diagnostic_request_accepted', 1)
    for index in range(20):
        key(s, 0, SHIFT, A if index % 2 else B)
        s.advance(2000)
    key(s, 0)
    s.advance(80000)
    released(s, 1)
    for node in (0, 1):
        s.do(node, 'diagnostic_poll')
        s.expect(node, 'diagnostic_poll_ready', 1)
        s.expect(node, 'diagnostic_token', 0x44530000 + node)
        s.expect(node, 'diagnostic_outcome', 0)


def scenario_keyboard_screenlock_both_hosts(s):
    setup(s)
    for source in (0, 1):
        focus(s, source)
        before = [len(s.reports(node, 1)) for node in (0, 1)]
        key(s, source, 0x10, 0x0F)  # RightCtrl+L locks both Macs.
        s.advance(20000)
        for node in (0, 1):
            reports = [report['data'] for report in s.reports(node, 1)[before[node]:]]
            assert keyboard(0x09, 0x14) in reports, 'host missed its synthetic lock chord'
            s.expect_report(node, 1, keyboard())
        key(s, source)
        s.advance(RECOVERY_US)
        for node in (0, 1):
            released(s, node)


def scenario_keyboard_remote_host_reconnect_discards_backlog(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.do(source, 'uart_stall', 1)
        s.do(source, 'fill', 1, 256)
        key(s, source, SHIFT, B)
        key(s, source)
        s.do(target, 'host', 0, 0)
        s.advance(10000)
        s.do(target, 'host', 1, 0)
        before = len(s.reports(target, 1))
        s.do(source, 'uart_stall', 0)
        s.advance(RECOVERY_US)
        released(s, target)
        for report in s.reports(target, 1)[before:]:
            assert report['data'] == keyboard(), 'pre-disconnect source backlog replayed a key'


def scenario_keyboard_link_lease_preserves_local_source(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, target, CTRL, C)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(CTRL | SHIFT, C, A))
        direct = s.get(source, 'direct', target)
        s.do(source, 'fault', {'drop': 1000})
        s.advance(2200000)
        s.expect_report(target, 1, keyboard(CTRL, C))
        healthy(s)
        # Restoring a still-held physical source intentionally restores its
        # contribution, without disturbing the independently held local keys.
        s.do(source, 'fault', {'drop': 0})
        s.advance(RECOVERY_US)
        s.expect_report(target, 1, keyboard(CTRL | SHIFT, C, A))
        s.expect(source, 'direct', direct, target)
        s.check('range', target, 'peer', target, direct, direct + 1010000)
        key(s, source)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(CTRL, C))
        key(s, target)
        s.advance(5000)
        released(s, target)


def snapshot_frames(nonce, boot, generation, serial, report, output, *, origin=0, bad_crc=False):
    """Independent keyboard envelope oracle, including end-to-end CRC32.

    The five ID43..47 payloads form a 40-byte envelope. UART frame checksums
    remain a separate layer and are intentionally valid in the bad-CRC test.
    """
    body = struct.pack('<QQII', nonce, boot, generation, serial)
    body += bytes.fromhex(report) + bytes([origin, output, 0x4B, 1])
    checksum = zlib.crc32(body) ^ int(bad_crc)
    body += struct.pack('<I', checksum)
    return [frame(43 + index, body[index * 8:index * 8 + 8]) for index in range(5)]


def inject_snapshot(s, target, packets):
    # The production task dispatches one frame per call. Parse each frame
    # before filling more bytes, so this fixture cannot overflow the DMA ring.
    for packet in packets:
        s.do(target, 'raw', packet)
        s.do(target, 'task', 'packet_receiver_task')


def request_nonce(s, target):
    requests = [decode_frame(bytes.fromhex(event['data'])) for event in s.trace
                if event['kind'] == 'uart_tx' and event['node'] == target
                and decode_frame(bytes.fromhex(event['data']))[0] in (42, 48)]
    assert requests, 'owner emitted no keyboard-state challenge'
    return int.from_bytes(requests[-1][1], 'little')


def isolated_protocol(s, target):
    source = 1 - target
    generation = 100 + target
    s.do(source, 'fault', {'drop': 1000})
    for node in (0, 1):
        s.do(node, 'raw', selection_frame(target, generation, 0))
        s.do(node, 'task', 'packet_receiver_task')
    s.advance(105000)
    return generation, request_nonce(s, target)


def scenario_keyboard_snapshot_wire_order_crc_and_context(s):
    setup(s)
    for target in (0, 1):
        generation, nonce = isolated_protocol(s, target)
        boot = 2 - target  # Independent simulator identity: board role + 1.
        down = snapshot_frames(nonce, boot, generation, 100, keyboard(SHIFT, A), target)
        # Duplicate every adjacent fragment, including interior fragments.
        inject_snapshot(s, target, [copy for packet in down for copy in (packet, packet)])
        s.advance(2000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        count = len(s.reports(target, 1))
        # Identical and older reports must neither repeat HID input nor replace
        # the current source contribution with an earlier state.
        inject_snapshot(s, target, down)
        inject_snapshot(s, target, snapshot_frames(nonce, boot, generation, 99, keyboard(), target))
        s.advance(2000)
        s.check('usb_count', target, 1, count)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        up = snapshot_frames(nonce, boot, generation, 101, keyboard(), target)
        inject_snapshot(s, target, up)
        s.advance(2000)
        s.expect_report(target, 1, keyboard())
        count = len(s.reports(target, 1))
        invalid = [
            snapshot_frames(nonce, boot, generation, 102, keyboard(SHIFT, B), target, bad_crc=True),
            snapshot_frames(nonce ^ 1, boot, generation, 102, keyboard(SHIFT, B), target),
            snapshot_frames(nonce, boot, generation + 1, 102, keyboard(SHIFT, B), target),
            snapshot_frames(nonce, boot, generation, 102, keyboard(SHIFT, B), 1 - target),
            snapshot_frames(nonce, boot, generation, 102, keyboard(SHIFT, B), target, origin=1),
            down,
        ]
        for packets in invalid:
            inject_snapshot(s, target, packets)
        s.advance(2000)
        s.check('usb_count', target, 1, count)
        s.expect_report(target, 1, keyboard())
        # A valid successor still works after incomplete/out-of-order packets.
        next_down = snapshot_frames(nonce, boot, generation, 102, keyboard(SHIFT, B), target)
        inject_snapshot(s, target, [next_down[0], next_down[2], next_down[1], *next_down[3:]])
        inject_snapshot(s, target, next_down)
        s.advance(2000)
        s.expect_report(target, 1, keyboard(SHIFT, B))
        inject_snapshot(s, target, snapshot_frames(nonce, boot, generation, 103, keyboard(), target))
        s.advance(2000)
        released(s, target)
        s.do(1 - target, 'fault', {'drop': 0})


def scenario_keyboard_snapshot_new_source_session(s):
    setup(s)
    for target in (0, 1):
        generation, nonce = isolated_protocol(s, target)
        boot = 2 - target
        inject_snapshot(s, target, snapshot_frames(nonce, boot, generation, 100, keyboard(SHIFT, A), target))
        s.advance(2000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        # A restarted source begins its serial again. Accept its empty state
        # and immediately retire the challenge shared with the prior session.
        inject_snapshot(s, target, snapshot_frames(nonce, 999 + target, generation, 1, keyboard(), target))
        s.advance(2000)
        s.expect_report(target, 1, keyboard())
        count = len(s.reports(target, 1))
        inject_snapshot(s, target, snapshot_frames(nonce, boot, generation, 101, keyboard(SHIFT, A), target))
        s.advance(2000)
        s.check('usb_count', target, 1, count)
        fresh = request_nonce(s, target)
        assert fresh != nonce
        inject_snapshot(s, target, snapshot_frames(fresh, 999 + target, generation, 2, keyboard(SHIFT, B), target))
        s.advance(2000)
        s.expect_report(target, 1, keyboard(SHIFT, B))
        inject_snapshot(s, target, snapshot_frames(fresh, 999 + target, generation, 3, keyboard(), target))
        s.advance(2000)
        released(s, target)
        s.do(1 - target, 'fault', {'drop': 0})


def scenario_keyboard_idle_suspend_does_not_wake(s):
    setup(s)
    s.advance(10000)
    before = len(s.trace)
    counts = [len(s.reports(node, 1)) for node in (0, 1)]
    for node in (0, 1):
        s.do(node, 'host', 1, 1)
    s.advance(RECOVERY_US)
    for node in (0, 1):
        s.check('usb_count', node, 1, counts[node])
    assert not any(event['kind'] == 'wake' for event in s.trace[before:]), \
        'idle keyboard reconciliation woke a suspended host'
    healthy(s)


def scenario_keyboard_source_fifo_saturation_release(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(SHIFT, A))
        s.do(source, 'uart_stall', 1)
        s.do(source, 'fill', 1, 256)
        # Fill and overflow the source transition FIFO while no UART envelope
        # can be admitted. Its coalesced tail must still retain final all-up.
        for index in range(132):
            key(s, source, SHIFT, A if index % 2 == 0 else B)
        started = s.now
        key(s, source)
        s.check('time_range', started, started + 1000)
        healthy(s)
        s.do(source, 'uart_stall', 0)
        s.advance(RECOVERY_US)
        released(s, target)


def scenario_keyboard_reconnect_immediate_local_report(s):
    setup(s)
    for source in (0, 1):
        target = 1 - source
        focus(s, target)
        key(s, source, SHIFT, A)
        key(s, target, CTRL, C)
        s.advance(5000)
        s.expect_report(target, 1, keyboard(CTRL | SHIFT, C, A))
        s.do(target, 'pause', 1, 2000)
        s.do(target, 'host', 0, 0)
        s.do(target, 'host', 1, 0)
        # Explicitly execute new local input and USB drain before core 1's
        # next sync poll. The old peer cache is invalid as soon as USB resets.
        key(s, target, CTRL, B)
        s.advance(1000)  # Let the shared HID endpoint become ready; sync stays paused.
        s.do(target, 'task', 'process_kbd_queue_task')
        s.expect_report(target, 1, keyboard(CTRL, B))
        key(s, target)
        key(s, source)
        s.advance(RECOVERY_US)
        released(s, target)


def scenario_keyboard_screenlock_release_wire_loss(s):
    setup(s)
    for source, selected in ((0, 0), (0, 1), (1, 0), (1, 1)):
        target = 1 - source
        focus(s, selected)
        # Manually service source TX so the one missing frame is specifically
        # the production screenlock all-up, after its real down was emitted.
        s.do(source, 'pause', 0, 50000)
        key(s, source, 0x10, 0x0F)
        saw_down = False
        for _ in range(20):
            before = len(s.trace)
            s.do(source, 'task', 'process_uart_tx_task')
            sent = [decode_frame(bytes.fromhex(event['data'])) for event in s.trace[before:]
                    if event['kind'] == 'uart_tx' and event['node'] == source]
            if sent and sent[-1] == (1, bytes.fromhex(keyboard(0x09, 0x14))):
                saw_down = True
                break
            s.advance(50)
        assert saw_down, 'no production remote screenlock down was sent'
        s.do(source, 'fault', {'drop': 1})
        # 32 wire bytes at 3,686,400 baud take 87 us in the 8N1 model.
        # Let that actual DMA transfer finish before asking TX for its all-up.
        s.advance(100)
        before = len(s.trace)
        s.do(source, 'task', 'process_uart_tx_task')
        dropped = [decode_frame(bytes.fromhex(event['data'])) for event in s.trace[before:]
                   if event['kind'] == 'fault_drop' and event['node'] == source]
        assert dropped == [(1, bytes(8))]
        s.advance(80000)
        for node in (0, 1):
            released(s, node)
        key(s, source)
        s.advance(5000)


def scenario_keyboard_synthetic_timeout_restores_physical_state(s):
    setup(s)
    for target, selected in ((0, 0), (0, 1), (1, 0), (1, 1)):
        focus(s, selected)
        key(s, selected, CTRL, C)
        s.advance(5000)
        s.expect_report(selected, 1, keyboard(CTRL, C))
        # An independently encoded legacy synthetic lock packet reaches the
        # actual UART parser; deliberately omit its all-up successor.
        s.do(target, 'raw', frame(1, bytes.fromhex(keyboard(0x09, 0x14))))
        s.do(target, 'task', 'packet_receiver_task')
        s.advance(5000)
        s.expect_report(target, 1, keyboard(0x09, 0x14))
        s.advance(30000)
        expected = keyboard(CTRL, C) if target == selected else keyboard()
        s.expect_report(target, 1, expected)
        key(s, selected)
        s.advance(5000)
        for node in {selected, target}:
            released(s, node)


def synthetic_remount_before_sync(s, target):
    focus(s, 1 - target)
    s.do(target, 'host', 0, 0)
    s.do(target, 'host', 1, 0)
    # New-bus input arrives before core 1 observes the changed generation.
    # Reconciling that generation must retain the newly accepted lock's timer.
    s.do(target, 'raw', frame(1, bytes.fromhex(keyboard(0x09, 0x14))))
    s.do(target, 'task', 'packet_receiver_task')
    s.do(target, 'task', 'keyboard_sync_task')
    s.advance(5000)
    s.expect_report(target, 1, keyboard(0x09, 0x14))
    s.advance(30000)
    released(s, target)


def scenario_keyboard_synthetic_remount_before_sync(s):
    setup(s)
    for target in (0, 1):
        synthetic_remount_before_sync(s, target)


SCENARIOS = {name.removeprefix('scenario_'): value for name, value in list(globals().items())
             if name.startswith('scenario_')}
