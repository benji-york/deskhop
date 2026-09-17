"""Disk-free serial maintenance through production paired UART and ROM boundary.

The console/USB completion fence is tested separately with real TinyUSB. Here
the same API calls drive the production core-0 state machine; wire bytes are
checked against an independent encoder. ROM entry is not USB enumeration.
"""
from functools import partial

from test_transport import config_frame
from test_uart_integrity import wire_frame, wire_decode
from test_webconfig_bootloader import rom_entry

RX = 'packet_receiver_task'
TX = 'process_uart_tx_task'
REQ, ACK = 49, 50
TOKEN = 0x12345678


def payload(target, token=TOKEN, tag=1, status=0):
    return bytes([1, target | status << 1]) + token.to_bytes(4, 'little') + tag.to_bytes(2, 'little')


def inject(s, node, kind, data):
    s.do(node, 'raw', wire_frame(kind, data))
    s.do(node, 'task', RX)


def result(s, node, outcome, target, token=TOKEN):
    s.do(node, 'maintenance_poll')
    s.expect(node, 'maintenance_ready', 1)
    s.expect(node, 'maintenance_outcome', outcome)
    s.expect(node, 'maintenance_target', target)
    s.expect(node, 'maintenance_token', token)


def start(s, node, target, token=TOKEN, expected=0):
    s.do(node, 'maintenance_request', target, token)
    s.expect(node, 'maintenance_start', expected)


def safe(s, node):
    s.expect(node, 'stopped', 0)
    for kind in ('reset', 'erase', 'program'):
        s.check('event_count', node, kind, 0)


def frames(s, node, kind):
    return [e for e in s.trace if e['node'] == node and e['kind'] == 'uart_tx'
            and wire_decode(e['data'])[0] == kind]


def pump(s, rounds=1):
    for _ in range(rounds):
        for node in (0, 1):
            s.do(node, 'task', TX)
        s.advance(100)
        for node in (0, 1):
            s.do(node, 'task', RX)


def local(s, node):
    # Focus deliberately differs from target; physical role must win.
    s.do(node, 'select', 1 - node)
    pump(s, 8)
    start(s, node, node)
    result(s, node, 0, node)
    s.do(node, 'maintenance_complete', TOKEN + 1)
    s.do(node, 'task', TX)
    safe(s, node)
    s.do(node, 'maintenance_complete', TOKEN)
    s.do(node, 'task', TX)
    rom_entry(s, node)
    safe(s, 1 - node)
    assert not frames(s, node, REQ)
    assert all(e['core'] == 0 for e in s.trace if e['kind'] == 'reset')


def remote(s, node, lost=None, duplicate_frames=False):
    peer = 1 - node
    s.do(node, 'select', node)
    pump(s, 8)
    if duplicate_frames:
        s.do(node, 'fault', {'duplicate': True})
        s.do(peer, 'fault', {'duplicate': True})
    if lost == 'request':
        s.do(node, 'fault', {'drop': 1})
    if lost == 'ack':
        s.do(peer, 'fault', {'drop': 1})
    start(s, node, peer)
    pump(s, 3)
    assert frames(s, node, REQ)[0]['data'] == wire_frame(REQ, payload(peer, tag=node + 1))
    if lost:
        s.advance(3000001)
        s.do(node, 'task', TX)
        result(s, node, 6, peer)
    else:
        result(s, node, 1, peer)
    safe(s, node)
    if lost == 'request':
        safe(s, peer)
    else:
        rom_entry(s, peer)
        sent = frames(s, peer, ACK)
        assert len(sent) == 1 and sent[0]['data'] == wire_frame(ACK, payload(peer, tag=node + 1))
        reset = next(e for e in s.trace if e['node'] == peer and e['kind'] == 'reset')
        assert reset['core'] == 0 and reset['at'] >= sent[0]['at'] + sent[0]['b']
    assert not frames(s, node, 4)  # Never fall back to legacy unacknowledged reset.


def local_cancel(s, expire=False, stall=None):
    start(s, 0, 0)
    result(s, 0, 0, 0)
    if stall:
        s.do(0, stall, 1)
        s.do(0, 'maintenance_complete', TOKEN)
        s.do(0, 'task', TX)
        safe(s, 0)
    if expire:
        s.advance(3000001)
        s.do(0, 'task', TX)
        result(s, 0, 7, 0)
    else:
        s.do(0, 'maintenance_cancel', TOKEN)
    if stall:
        s.do(0, stall, 0)
    s.do(0, 'maintenance_complete', TOKEN)
    pump(s, 3)
    s.expect(0, 'maintenance_reserved', 0)
    safe(s, 0)


def source_backpressure(s, full=False, cancel=False):
    s.do(0, 'uart_stall', 1)
    if full:
        s.do(0, 'fill', 1, 256)
    start(s, 0, 1)
    s.do(0, 'task', TX)
    if cancel:
        s.do(0, 'maintenance_cancel', TOKEN)
    else:
        s.advance(250001 if full else 3000001)
        s.do(0, 'task', TX)
        result(s, 0, 5 if full else 6, 1)
    s.do(0, 'uart_stall', 0)
    pump(s, 260 if full else 3)
    assert not frames(s, 0, REQ), 'cancelled or expired maintenance request escaped to UART'
    s.expect(0, 'maintenance_reserved', 0)
    safe(s, 0)
    safe(s, 1)


def target_backpressure(s, full=False, hardware=False):
    stall = 'uart_busy' if hardware else 'uart_stall'
    s.do(1, stall, 1)
    if full:
        s.do(1, 'fill', 1, 256)
    start(s, 0, 1)
    pump(s, 3)
    safe(s, 1)
    if hardware:
        result(s, 0, 1, 1)  # Acceptance is not proof that ROM was reached.
    s.advance(3000001)
    pump(s, 1)
    if not hardware:
        result(s, 0, 6, 1)
    s.do(1, stall, 0)
    pump(s, 260 if full else 3)
    s.expect(1, 'maintenance_reserved', 0)
    safe(s, 0)
    safe(s, 1)
    if not hardware:
        assert not frames(s, 1, ACK)


def updates(s, remote_target=False, dirty=False):
    node = 1 if remote_target else 0
    s.do(node, 'verify_update_state', 2 if dirty else 1)
    start(s, 0, node, expected=0 if remote_target else 2)
    if remote_target:
        pump(s, 3)
        result(s, 0, 3, node)
        assert wire_decode(frames(s, 1, ACK)[0]['data'])[1] == payload(1, status=2)
    safe(s, 0)
    safe(s, 1)


def source_lease(s):
    inject(s, 0, 24, bytes(8))
    s.expect(0, 'maintenance_source_seen', 1)
    start(s, 0, 0, expected=2)
    pump(s, 2)
    s.advance(3000000)
    start(s, 0, 0)
    result(s, 0, 0, 0)
    s.do(0, 'maintenance_complete', TOKEN)
    s.do(0, 'task', TX)
    rom_entry(s, 0)


def malformed(s):
    # Valid outer CRC does not authorize wrong target, zero token or protocol.
    for data in (payload(0), payload(1, token=0), bytes([2]) + payload(1)[1:], payload(1, status=1)):
        inject(s, 1, REQ, data)
        s.do(1, 'task', TX)
        safe(s, 1)
        s.expect(1, 'maintenance_reserved', 0)
    start(s, 0, 1)
    s.do(0, 'task', TX)
    for data in (payload(0), payload(1, token=TOKEN + 1), payload(1, tag=2), payload(1, status=4)):
        inject(s, 0, ACK, data)
        s.do(0, 'task', TX)
        s.do(0, 'maintenance_poll')
        s.expect(0, 'maintenance_ready', 0)
    s.do(0, 'maintenance_cancel', TOKEN)
    safe(s, 0)


def duplicate(s):
    remote(s, 0, duplicate_frames=True)
    assert len([e for e in s.trace if e['kind'] == 'uart_tx'
                and wire_decode(e['data'])[0] in (REQ, ACK)]) == 2


def simultaneous(s):
    start(s, 0, 1)
    start(s, 1, 0)
    pump(s, 4)
    result(s, 0, 2, 1)
    result(s, 1, 2, 0)
    for node in (0, 1):
        safe(s, node)
        s.expect(node, 'maintenance_reserved', 0)


def stale_ingress(s):
    inject(s, 1, REQ, payload(1))
    s.advance(250001)
    pump(s, 3)
    assert not frames(s, 1, ACK)
    safe(s, 1)


def stale_ack(s, new_session=False):
    # A response delayed through expiry or a new console transaction cannot
    # complete its successor, even with a valid transport checksum.
    start(s, 0, 1)
    s.do(0, 'task', TX)
    inject(s, 0, ACK, payload(1))
    s.advance(3000001)
    s.do(0, 'task', TX)
    result(s, 0, 6, 1)
    if new_session:
        s.do(0, 'maintenance_session', 2)
        next_token = TOKEN
    else:
        next_token = TOKEN + 1
    start(s, 0, 1, token=next_token)
    s.do(0, 'task', TX)
    inject(s, 0, ACK, payload(1))
    s.do(0, 'task', TX)
    s.do(0, 'maintenance_poll')
    s.expect(0, 'maintenance_ready', 0)
    s.do(0, 'maintenance_cancel', next_token)
    safe(s, 0)


def pending_reboot(s, remote_target=False):
    node = int(remote_target)
    inject(s, node, 19, bytes(8))
    s.expect(node, 'reboot', 1)
    start(s, 0, node, expected=0 if remote_target else 3)
    if remote_target:
        pump(s, 3)
        result(s, 0, 4, node)
    safe(s, 0)
    safe(s, 1)


def late_update(s, remote_target=False):
    # Simulate a legacy path changing state despite the admission reservation.
    # Recheck must still prevent ROM entry over the now-dirty image.
    if remote_target:
        start(s, 0, 1)
        s.do(0, 'task', TX)
        s.advance(100)
        s.do(1, 'task', RX)
        s.do(1, 'task', TX)
        s.do(1, 'verify_update_state', 2)
        s.advance(100)
        s.do(1, 'task', TX)
        safe(s, 1)
    else:
        start(s, 0, 0)
        result(s, 0, 0, 0)
        s.do(0, 'maintenance_complete', TOKEN)
        s.do(0, 'verify_update_state', 2)
        s.do(0, 'task', TX)
        result(s, 0, 7, 0)
        safe(s, 0)


def reservation(s):
    start(s, 0, 0)
    result(s, 0, 0, 0)
    # Actual legacy reboot, ROM and byte-serving paths cannot steal reservation.
    for kind in (4, 19, 24):
        inject(s, 0, kind, bytes(8))
    safe(s, 0)
    s.expect(0, 'reboot', 0)
    s.expect(0, 'maintenance_source_seen', 0)
    s.expect(0, 'uart_queue', 0)
    s.do(0, 'maintenance_cancel', TOKEN)


def webhid_boundary(s):
    for node in (0, 1):
        s.do(node, 'set', 'config_mode', 0, 1)
        for kind in (REQ, ACK):
            for command, data in ((kind, payload(node)), (23, bytes([kind]) + payload(node)[:7])):
                s.do(node, 'vendor', config_frame(command, data))
                pump(s, 2)
                safe(s, node)
                s.expect(node, 'maintenance_reserved', 0)


def config_start(s, node, token=TOKEN, expected=0):
    s.do(node, 'maintenance_config', token)
    s.expect(node, 'maintenance_start', expected)


def config_unchanged(s, node):
    safe(s, node)
    s.expect(node, 'reboot', 0)
    s.expect(node, 'watchdog_scratch', 0, index=5)
    s.expect(node, 'watchdog_scratch', 0, index=6)
    assert not frames(s, node, REQ), 'local config request escaped to peer'


def config_local(s, node):
    # A local command must ignore active output and never put either side in ROM.
    s.do(node, 'select', 1 - node)
    pump(s, 8)
    generation = s.get(node, 'kbd_host_generation')
    config_start(s, node)
    s.expect(node, 'maintenance_reserved', 1)
    result(s, node, 0, node)
    s.do(node, 'maintenance_complete', TOKEN + 1)
    s.do(node, 'task', TX)
    config_unchanged(s, node)
    s.do(node, 'maintenance_complete', TOKEN)
    s.do(node, 'task', TX)
    safe(s, node)
    s.expect(node, 'reboot', 1)
    s.expect(node, 'watchdog_scratch', 0xdeadf00f, index=5)
    s.expect(node, 'watchdog_scratch', 0x00c0ffee, index=6)
    s.expect(node, 'kbd_host_generation', generation + 1)
    s.expect(node, 'maintenance_reserved', 0)
    config_unchanged(s, 1 - node)
    assert not frames(s, node, REQ)
    # A second task pass cannot schedule a second transition/release.
    s.do(node, 'task', TX)
    s.expect(node, 'kbd_host_generation', generation + 1)


def config_already(s, node):
    s.do(node, 'set', 'config_mode', 0, 1)
    generation = s.get(node, 'kbd_host_generation')
    config_start(s, node, expected=5)
    s.do(node, 'maintenance_poll')
    s.expect(node, 'maintenance_ready', 0)
    s.do(node, 'maintenance_complete', TOKEN)
    pump(s, 3)
    s.expect(node, 'config_mode', 1)
    s.expect(node, 'maintenance_reserved', 0)
    s.expect(node, 'kbd_host_generation', generation)
    config_unchanged(s, node)


def config_cancel(s, node, expire=False, stall=None):
    config_start(s, node)
    result(s, node, 0, node)
    if stall:
        s.do(node, stall, 1)
        s.do(node, 'maintenance_complete', TOKEN)
        s.do(node, 'task', TX)
        config_unchanged(s, node)
    if expire:
        s.advance(3000001)
        s.do(node, 'task', TX)
        result(s, node, 7, node)
    else:
        # Includes cancellation after USB completion but before the wire drains.
        s.do(node, 'maintenance_cancel', TOKEN)
    if stall:
        s.do(node, stall, 0)
    s.do(node, 'maintenance_complete', TOKEN)
    pump(s, 3)
    s.expect(node, 'maintenance_reserved', 0)
    config_unchanged(s, node)
    config_unchanged(s, 1 - node)


def config_rejected(s, node, guard):
    if guard in ('updating', 'dirty', 'reboot'):
        s.do(node, 'verify_update_state', {'updating': 1, 'dirty': 2, 'reboot': 3}[guard])
        expected = 3 if guard == 'reboot' else 2
    elif guard == 'source':
        inject(s, node, 24, bytes(8))
        expected = 2
    elif guard in ('bootloader_peer_pending', 'bootloader_local_pending'):
        s.do(node, 'set', guard, 0, 1)
        expected = 1
    elif guard == 'busy':
        start(s, node, node)
        expected = 1
    else:
        expected = 4
    config_start(s, node, token=0 if guard == 'zero_token' else TOKEN + 1,
                 expected=expected)
    s.expect(node, 'watchdog_scratch', 0, index=5)
    s.expect(node, 'watchdog_scratch', 0, index=6)
    safe(s, node)
    if guard == 'busy':
        s.do(node, 'maintenance_cancel', TOKEN)
    s.expect(node, 'maintenance_reserved', 0)


def config_late_update(s, node):
    config_start(s, node)
    result(s, node, 0, node)
    s.do(node, 'maintenance_complete', TOKEN)
    s.do(node, 'verify_update_state', 2)
    s.do(node, 'task', TX)
    result(s, node, 7, node)
    s.expect(node, 'maintenance_reserved', 0)
    config_unchanged(s, node)


SCENARIOS = {
    **{f'serial_bootloader_local_{n}': partial(local, node=n) for n in (0, 1)},
    **{f'serial_bootloader_remote_{n}': partial(remote, node=n) for n in (0, 1)},
    **{f'serial_bootloader_lost_{k}': partial(remote, node=0, lost=k) for k in ('request', 'ack')},
    'serial_bootloader_cancel': local_cancel,
    'serial_bootloader_reply_timeout': partial(local_cancel, expire=True),
    **{f'serial_bootloader_local_{stall}_timeout': partial(local_cancel, expire=True, stall=stall)
       for stall in ('uart_stall', 'uart_busy')},
    'serial_bootloader_source_full': partial(source_backpressure, full=True),
    'serial_bootloader_source_stall': source_backpressure,
    'serial_bootloader_source_cancel': partial(source_backpressure, cancel=True),
    'serial_bootloader_target_full': partial(target_backpressure, full=True),
    'serial_bootloader_target_stall': target_backpressure,
    'serial_bootloader_acceptance_not_boot': partial(target_backpressure, hardware=True),
    **{f'serial_bootloader_update_{remote_target}_{dirty}': partial(updates, remote_target=remote_target, dirty=dirty)
       for remote_target in (False, True) for dirty in (False, True)},
    'serial_bootloader_source_lease': source_lease,
    'serial_bootloader_malformed': malformed,
    'serial_bootloader_duplicate': duplicate,
    'serial_bootloader_simultaneous': simultaneous,
    'serial_bootloader_stale_ingress': stale_ingress,
    'serial_bootloader_stale_ack': stale_ack,
    'serial_bootloader_old_session_ack': partial(stale_ack, new_session=True),
    **{f'serial_bootloader_reboot_pending_{r}': partial(pending_reboot, remote_target=r)
       for r in (False, True)},
    **{f'serial_bootloader_late_update_{r}': partial(late_update, remote_target=r)
       for r in (False, True)},
    'serial_bootloader_reservation': reservation,
    'serial_bootloader_webhid_boundary': webhid_boundary,
    **{f'serial_config_local_{n}': partial(config_local, node=n) for n in (0, 1)},
    **{f'serial_config_already_{n}': partial(config_already, node=n) for n in (0, 1)},
    **{f'serial_config_late_update_{n}': partial(config_late_update, node=n) for n in (0, 1)},
    **{f'serial_config_cancel_{n}_{expire}_{stall}':
       partial(config_cancel, node=n, expire=expire, stall=stall)
       for n in (0, 1) for expire in (False, True) for stall in (None, 'uart_stall', 'uart_busy')},
    **{f'serial_config_rejected_{n}_{guard}': partial(config_rejected, node=n, guard=guard)
       for n in (0, 1) for guard in ('updating', 'dirty', 'reboot', 'source', 'busy',
                                    'bootloader_peer_pending', 'bootloader_local_pending', 'zero_token')},
}
BACKGROUND_FALSE = set(SCENARIOS)
