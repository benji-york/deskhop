"""Confirmed configuration through real USB admission, paired UART and storage.

The independent wire oracle uses literal protocol IDs and does not substitute
the production request engine. Flash is modeled NOR; a successful ACK is not a
claim that this suite executes macOS or physically reboots the microcontroller.
"""
import argparse
import itertools
import struct

from simulator import Simulation
from test_transport import config_frame
from test_fw_startup import historical_library

META, LO, HI, EXEC, ACK, VALUE = range(56, 62)
CAP, SET, BORDERS, QUERY, SAVE, CHECK = range(1, 7)
OK, INVALID, BUSY, CONFLICT, FLASH_MISMATCH, EXPIRED, INCOMPLETE = range(7)
CONFIG = 2 * 1024 * 1024 - 4096


def frames(token, role, op, key=0, value=0):
    return [(META, struct.pack('<IBBBB', token, role, op, key, 0)),
            (LO, struct.pack('<II', token, value & 0xffffffff)),
            (HI, struct.pack('<II', token, value >> 32)),
            (EXEC, struct.pack('<II', token, 0))]


def send(s, origin, packets):
    for kind, payload in packets:
        s.do(origin, 'vendor', config_frame(kind, payload))


def receipts(s, origin, token, after=0):
    result = {}
    for event in s.trace[after:]:
        if event['kind'] != 'usb' or event['node'] != origin or event['b'] != 6:
            continue
        raw = bytes.fromhex(event['data'])
        assert len(raw) == 12 and raw.hex() == config_frame(raw[2], raw[3:11])
        if raw[2] in (ACK, VALUE) and struct.unpack_from('<I', raw, 3)[0] == token:
            result[raw[2]] = raw[3:11]
    return result


def request(s, origin, role, op, key=0, value=0, *, token, expected=OK):
    after = len(s.trace)
    send(s, origin, frames(token, role, op, key, value))
    deadline = s.now + 200000
    while s.now < deadline:
        r = receipts(s, origin, token, after)
        if set(r) == {ACK, VALUE}:
            assert struct.unpack('<IBBBB', r[ACK]) == (token, role, op, key, expected), r
            return struct.unpack('<II', r[VALUE])[1]
        s.advance(1000)
    raise AssertionError(f'no confirmed reply origin={origin} target={role} op={op} token={token}: {receipts(s, origin, token)}')


def start(s, origin):
    for role in (0, 1):
        s.do(role, 'host', 1, 0)
    s.do(origin, 'set', 'config_mode', 0, 1)
    s.advance(5000)


def writes(s, role):
    return [e for e in s.trace if e['node'] == role and e['kind'] in ('erase', 'program')]


def roundtrip(s, origin):
    start(s, origin)
    tokens = itertools.count(100)
    before_images = [s.flash(role, 0, 262144) for role in (0, 1)]
    for role in (0, 1):
        assert request(s, origin, role, CAP, token=next(tokens)) == 1
        request(s, origin, role, SET, 83, 123, token=next(tokens))
        s.expect(role, 'system_timeout', 123)
        checked = request(s, origin, role, CHECK, 83, 123, token=next(tokens))
        assert checked == request(s, origin, role, QUERY, token=next(tokens))
        assert request(s, origin, role, CHECK, 83, 124, token=next(tokens), expected=CONFLICT) == checked
        assert not writes(s, role), 'apply must not erase settings'

    # Both starting intervals are valid, but opposite orders of individual
    # border SETs would be required to reach this same final interval.
    s.do(0, 'set', 'border_top', 0, 100)
    s.do(0, 'set', 'border_bottom', 0, 1000)
    s.do(1, 'set', 'border_top', 0, 10000)
    s.do(1, 'set', 'border_bottom', 0, 20000)
    digests = []
    for role in (0, 1):
        request(s, origin, role, BORDERS, 0, 5000 | (6000 << 32), token=next(tokens))
        s.expect(role, 'border_top', 5000, 0)
        s.expect(role, 'border_bottom', 6000, 0)
        digest = request(s, origin, role, QUERY, token=next(tokens))
        digests.append(digest)
        token = next(tokens)
        assert request(s, origin, role, SAVE, value=digest, token=token) == digest
        assert [e['kind'] for e in writes(s, role)] == ['erase', 'program']
        saved = s.flash(role, CONFIG, 4096)
        assert saved != b'\xff' * 4096
        # Exact request replay and a fresh Save must never repeat a flash erase
        # when the desired bytes are already present and verified.
        request(s, origin, role, SAVE, value=digest, token=token)
        request(s, origin, role, SAVE, value=digest, token=next(tokens))
        assert len(writes(s, role)) == 2 and s.flash(role, CONFIG, 4096) == saved
        assert s.flash(role, 0, 262144) == before_images[role]
        s.expect(role, 'reboot', 0)
        s.expect(role, 'stopped', 0)
    assert digests[0] == digests[1]
    assert s.flash(0, CONFIG, 4096) == s.flash(1, CONFIG, 4096)


def rejection(s, origin):
    start(s, origin)
    tokens = itertools.count(1000)
    for role in (0, 1):
        original = s.get(role, 'system_timeout')
        request(s, origin, role, SET, 83, 1 << 32, token=next(tokens), expected=INVALID)
        request(s, origin, role, SET, 0, 1, token=next(tokens), expected=INVALID)
        request(s, origin, role, CHECK, 0, 1, token=next(tokens), expected=INVALID)
        request(s, origin, role, CHECK, 83, 1 << 32, token=next(tokens), expected=INVALID)
        request(s, origin, role, BORDERS, 0, 300 | (200 << 32), token=next(tokens), expected=INVALID)
        s.expect(role, 'system_timeout', original)
        before = request(s, origin, role, QUERY, token=next(tokens))
        s.do(role, 'set', 'ss_timeout', 0, 4321)
        request(s, origin, role, SAVE, value=before, token=next(tokens), expected=CONFLICT)
        assert not writes(s, role)
        for state in (1, 2):
            s.do(role, 'verify_update_state', state)
            request(s, origin, role, SAVE, value=before, token=next(tokens), expected=BUSY)
            s.do(role, 'verify_update_state', 0)
            assert not writes(s, role)


def incomplete_and_spoofed(s, origin):
    start(s, origin)
    peer = 1 - origin
    initial = s.get(peer, 'system_timeout')
    # A late fragment with a different nonce cannot supply missing request data.
    packets = frames(5000, peer, SET, 83, 999)
    send(s, origin, packets[:2])
    send(s, origin, [(HI, struct.pack('<II', 4999, 0)), packets[-1]])
    s.advance(100000)
    s.expect(peer, 'system_timeout', initial)
    assert not writes(s, peer)
    # A host cannot inject response packets or use the legacy proxy envelope
    # to bypass the new protocol's ingress separation.
    send(s, origin, [(ACK, struct.pack('<IBBBB', 5000, peer, SET, 83, OK)),
                     (VALUE, struct.pack('<II', 5000, 123))])
    send(s, origin, [(23, bytes([META]) + bytes(7))])
    s.advance(10000)
    s.expect(peer, 'system_timeout', initial)
    assert not writes(s, peer)
    # No trusted OK can have resulted from an incomplete request or spoofed ACK.
    r = receipts(s, origin, 5000)
    if ACK in r:
        assert r[ACK][-1] != OK


def response_loss(s, origin):
    start(s, origin)
    peer = 1 - origin
    request(s, origin, peer, SET, 83, 99, token=6000)
    digest = request(s, origin, peer, QUERY, token=6001)
    # Drop the actual peer ACK frames, not the mutation: the caller cannot
    # conclude Save failed simply because it cannot confirm its result.
    s.do(peer, 'fault', {'drop_types': [ACK, VALUE], 'drop_type_count': 100})
    send(s, origin, frames(6002, peer, SAVE, value=digest))
    s.advance(200000)
    assert [e['kind'] for e in writes(s, peer)] == ['erase', 'program']
    assert not receipts(s, origin, 6002)
    s.do(peer, 'fault', {'drop_types': [], 'drop_type_count': 0})
    # Allow the bounded bridge transaction to expire before fresh requests.
    s.advance(3000000)
    assert not receipts(s, origin, 6002), 'bridge must not invent a peer receipt after timeout'
    before = len(writes(s, peer))
    assert request(s, origin, peer, CAP, token=6003) == 1
    digest = request(s, origin, peer, QUERY, token=6004)
    assert request(s, origin, peer, SAVE, value=digest, token=6005) == digest
    assert len(writes(s, peer)) == before, 'retry should verify already saved bytes'


def disconnect_and_fragments(s, origin):
    start(s, origin)
    peer = 1 - origin
    initial = s.get(origin, 'system_timeout')
    # Host disconnection cancels queued USB-origin commands before execution.
    send(s, origin, frames(8000, origin, SET, 83, 17))
    s.do(origin, 'host', 0, 0)
    s.do(origin, 'host', 1, 0)
    s.advance(20000)
    s.expect(origin, 'system_timeout', initial)
    assert not receipts(s, origin, 8000)

    # Swapped low/high arrival is complete, but conflicting duplicate pieces
    # invalidate the entire staged value rather than accepting last-writer-wins.
    packets = frames(8001, origin, SET, 83, 23)
    send(s, origin, [packets[i] for i in (0, 2, 1, 3)])
    s.advance(20000)
    assert receipts(s, origin, 8001)[ACK][-1] == OK
    s.expect(origin, 'system_timeout', 23)
    packets = frames(8002, origin, SET, 83, 99)
    send(s, origin, packets[:2] + [(LO, struct.pack('<II', 8002, 100))] + packets[2:])
    s.advance(20000)
    assert receipts(s, origin, 8002)[ACK][-1] == INVALID
    s.expect(origin, 'system_timeout', 23)

    # Drop one of the actual forwarded frames. Target rejects incomplete
    # assembly; neither its original value nor flash changes.
    initial = s.get(peer, 'system_timeout')
    s.do(origin, 'fault', {'drop_types': [HI], 'drop_type_count': 1})
    request(s, origin, peer, SET, 83, 42, token=8003, expected=INCOMPLETE)
    s.expect(peer, 'system_timeout', initial)
    assert not writes(s, peer)

    # Old staged fragments cannot execute after their bounded assembly window.
    packets = frames(8004, origin, SET, 83, 33)
    send(s, origin, packets[:3])
    s.advance(2100000)
    send(s, origin, packets[3:])
    s.advance(20000)
    s.expect(origin, 'system_timeout', 23)
    assert receipts(s, origin, 8004)[ACK][-1] == EXPIRED
    assert not writes(s, origin)


def reply_backpressure(s, origin):
    start(s, origin)
    peer = 1 - origin
    request(s, origin, peer, SET, 83, 321, token=9000)
    digest = request(s, origin, peer, QUERY, token=9001)
    s.do(peer, 'uart_stall', 1)
    s.do(peer, 'fill', 1, 10000)
    send(s, origin, frames(9002, peer, SAVE, value=digest))
    s.advance(50000)
    assert [e['kind'] for e in writes(s, peer)] == ['erase', 'program']
    assert not receipts(s, origin, 9002)
    s.do(peer, 'uart_stall', 0)
    s.advance(200000)
    r = receipts(s, origin, 9002)
    assert set(r) == {ACK, VALUE} and r[ACK][-1] == OK, r
    assert struct.unpack('<II', r[VALUE])[1] == digest
    assert len(writes(s, peer)) == 2, 'retrying ACK admission must not retry flash'
    for role in (0, 1):
        s.expect(role, 'stopped', 0)


def contradictory_receipt(s, origin):
    start(s, origin)
    peer = 1 - origin
    s.do(peer, 'fault', {'drop_types': [ACK, VALUE], 'drop_type_count': 100})
    send(s, origin, frames(9500, peer, SET, 83, 111))
    s.advance(20000)
    s.expect(peer, 'system_timeout', 111)
    s.do(peer, 'fault', {'drop_types': [], 'drop_type_count': 0})
    # Valid wire CRCs do not make contradictory semantic receipts trustworthy.
    # No stale OK may be combined with a later error's VALUE word.
    for status in (OK, INVALID):
        s.do(peer, 'packet', ACK, struct.pack('<IBBBB', 9500, peer, SET, 83, status).hex(), True)
    s.do(peer, 'packet', VALUE, struct.pack('<II', 9500, 777).hex(), True)
    s.advance(20000)
    assert not receipts(s, origin, 9500)
    assert not writes(s, peer)
    assert request(s, origin, peer, CAP, token=9501) == 1
    # A bridge may replay the exact peer receipt, but cannot invent a result
    # for different contents that happen to reuse the previous nonce.
    send(s, origin, frames(9501, peer, SET, 83, 555))
    after = len(s.trace)
    s.advance(20000)
    assert not receipts(s, origin, 9501, after)
    s.expect(peer, 'system_timeout', 111)


def old_peer(library, current=None):
    # Firmware from the accepted pre-feature commit must neither acknowledge
    # support nor mutate settings on these additive request message IDs.
    with Simulation(library=[current, library]) as s:
        start(s, 0)
        original = s.get(1, 'system_timeout')
        send(s, 0, frames(7000, 1, CAP))
        s.advance(200000)
        assert not receipts(s, 0, 7000)
        s.expect(1, 'system_timeout', original)
        assert not writes(s, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--all-orders', action='store_true')
    parser.add_argument('--library', help='Current production node library, including coverage builds')
    args = parser.parse_args()
    orders = list(itertools.permutations(range(4))) if args.all_orders else [None]
    for order in orders:
        for origin in (0, 1):
            for case in (roundtrip, rejection, incomplete_and_spoofed, response_loss,
                         disconnect_and_fragments, reply_backpressure, contradictory_receipt):
                with Simulation(library=args.library, core_order=order) as sim:
                    case(sim, origin)
                print('PASS', case.__name__, f'USB origin={origin}', f'order={order}', flush=True)
    with historical_library() as old:
        old_peer(old, args.library)
    print('PASS unsupported historical peer: no support claimed or settings changed')


if __name__ == '__main__':
    main()
