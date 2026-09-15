"""Peer history through real ownership queues, UART dispatcher, and both cores."""
import struct
import zlib

from fixtures import attach, keyboard, mouse
from test_transport import frame, out_mouse
from test_peer_status import request as status_request, result as status_result


def seed_history(s, node, count=80):
    s.do(node, 'history_clear')
    for index in range(count):
        s.do(node, 'history_record', 2, node, 1 - node, node * 1000 + index)
    return s.now


def request(s, node, token, count=16, accepted=1):
    s.do(node, 'history_request', token, count)
    s.expect(node, 'history_request_accepted', accepted)


def result(s, node, token, outcome=0):
    s.do(node, 'history_poll')
    s.expect(node, 'history_poll_ready', 1)
    s.expect(node, 'history_borrowed', 1)
    s.expect(node, 'history_token', token)
    s.expect(node, 'history_outcome', outcome)
    if outcome == 0:
        s.expect(node, 'history_role', 1 - node)
        s.expect(node, 'history_boot_session', 2 - node)
    s.do(node, 'history_poll')
    s.expect(node, 'history_poll_ready', 0)  # Notice delivered once, result still borrowed.
    s.expect(node, 'history_borrowed', 1)


def release(s, node):
    s.do(node, 'history_release')
    s.expect(node, 'history_borrowed', 0)


def expect_seed(s, node, count, time_us, total=80):
    peer = 1 - node
    s.expect(node, 'history_count', count)
    s.expect(node, 'history_first_seq', total - count + 1)
    s.expect(node, 'history_end_seq', total + 1)
    s.expect(node, 'history_oldest_seq', max(1, total - 63))
    s.expect(node, 'history_overwritten', max(0, total - 64))
    s.expect(node, 'history_gap_mask', 0)
    for index in range(count):
        s.expect(node, 'history_event_seq', total - count + 1 + index, index)
        s.expect(node, 'history_event_time_us', time_us, index)
        s.expect(node, 'history_event_type', 2, index)
        s.expect(node, 'history_event_a', peer, index)
        s.expect(node, 'history_event_b', node, index)
        s.expect(node, 'history_event_value', peer * 1000 + total - count + index, index)
        s.expect(node, 'history_event_reserved', 0, index)


def check_wire(s, client, token, limit):
    """Independent wire assembler, not a call back into the C decoder."""
    expected_size = 64 + 24 * limit + 4
    data = bytearray(expected_size)
    indices = set()
    requests = []
    for event in s.trace:
        if event['kind'] != 'uart_tx':
            continue
        raw = bytes.fromhex(event['data'])
        if int.from_bytes(raw[3:7], 'little') != token:
            continue
        if event['node'] == client and raw[2] == 38:
            requests.append(raw)
        if event['node'] == 1 - client and raw[2] == 39:
            index = int.from_bytes(raw[7:9], 'little')
            assert index not in indices and index < expected_size // 2
            indices.add(index)
            data[2 * index:2 * index + 2] = raw[9:11]
    assert requests == [bytes.fromhex(frame(38, struct.pack('<IBB2x', token, 2, limit)))]
    assert indices == set(range(expected_size // 2))
    assert data[:4] == bytes([2, 1 - client, limit, 24])
    assert data[4:8] == bytes(4)
    assert int.from_bytes(data[8:16], 'little') == 2 - client
    assert int.from_bytes(data[-4:], 'little') == zlib.crc32(data[:-4])
    for index in range(limit):
        seq, timestamp, value, kind, a, b, reserved = struct.unpack_from('<QQIBBBB', data, 64 + 24 * index)
        assert seq == 81 - limit + index
        assert kind == 2 and a == 1 - client and b == client and reserved == 0
        assert value == (1 - client) * 1000 + 80 - limit + index
        assert timestamp == s.get(client, 'history_event_time_us', index)


def pacing(s):
    for node in (0, 1):
        s.check('diagnostic_pacing', node, 1000)
        s.expect(node, 'stopped', 0)


def scenario_peer_history_roundtrip(s):
    attach(s)
    seeded = [seed_history(s, node) for node in (0, 1)]
    started = s.now
    request(s, 0, 1001, 16)
    request(s, 1, 1002, 64)
    status_request(s, 0, 2001)
    status_request(s, 1, 2002)
    s.do(0, 'report', 1, 0, keyboard(0, 4))
    s.do(1, 'report', 1, 0, mouse(x=7, y=-3))
    s.advance(150000)
    s.expect_report(0, 1, keyboard(0, 4))
    s.expect_report(0, 2, out_mouse(0, 16007, 15997))
    for node in (0, 1):
        status_result(s, node, 2001 + node)
    s.advance(2150000)
    for node, limit in ((0, 16), (1, 64)):
        result(s, node, 1001 + node)
        expect_seed(s, node, limit, seeded[1 - node])
        s.expect(node, 'history_requested_us', started)
        s.check('range', node, 'history_sampled_us', 0, started, started + 10000)
        s.check('range', node, 'history_first_response_us', 0, started + 1, started + 500000)
        check_wire(s, node, 1001 + node, limit)
        release(s, node)
    pacing(s)


def scenario_peer_history_borrowed(s):
    attach(s)
    seeded = [seed_history(s, node) for node in (0, 1)]
    request(s, 0, 3001)
    s.advance(900000)
    result(s, 0, 3001)
    expect_seed(s, 0, 16, seeded[1])
    request(s, 0, 3002, accepted=0)
    # A's result remains borrowed while it serves an independent request from B.
    request(s, 1, 3003)
    s.do(1, 'history_record', 2, 1, 0, 9999)
    s.advance(900000)
    result(s, 1, 3003)
    expect_seed(s, 1, 16, seeded[0])
    expect_seed(s, 0, 16, seeded[1])  # Neither serving nor new B events mutate it.
    release(s, 1)
    release(s, 0)
    request(s, 0, 3004)
    s.advance(900000)
    result(s, 0, 3004)
    s.expect(0, 'history_end_seq', 82)
    s.expect(0, 'history_event_seq', 81, 15)
    s.expect(0, 'history_event_value', 9999, 15)
    release(s, 0)
    pacing(s)


def scenario_peer_history_stale(s):
    attach(s)
    seed_history(s, 1)
    request(s, 0, 4001)
    s.advance(900000)
    # No poll: equivalent to abandoning the old terminal's completed command.
    request(s, 0, 4002)
    s.advance(900000)
    result(s, 0, 4002)
    release(s, 0)
    request(s, 0, 4003)
    s.advance(10000)
    s.do(0, 'raw', frame(39, struct.pack('<IH2x', 4001, 65535)))
    s.advance(890000)
    result(s, 0, 4003)  # A malformed late reply cannot poison a new query.
    release(s, 0)
    request(s, 0, 4004)
    s.advance(10000)
    # A new terminal can queue while the old request is still in flight. Its
    # result keeps the old token and must be released before the next finishes.
    request(s, 0, 4005)
    s.advance(900000)
    result(s, 0, 4004)
    release(s, 0)
    s.advance(900000)
    result(s, 0, 4005)
    release(s, 0)
    pacing(s)


def scenario_peer_history_capture_gap(s):
    attach(s)
    seeded = seed_history(s, 0)
    request(s, 1, 4501)
    s.advance(10000)
    # Capture copies one row per task tick. Overwrite the original window
    # after capture begins, so unread rows must be represented as gaps.
    for index in range(80):
        s.do(0, 'history_record', 2, 0, 1, 5000 + index)
    s.advance(900000)
    result(s, 1, 4501)
    s.expect(1, 'history_first_seq', 65)
    s.expect(1, 'history_end_seq', 81)
    s.expect(1, 'history_oldest_seq', 17)
    s.expect(1, 'history_overwritten', 16)
    s.expect(1, 'history_count', 16)
    gaps = s.get(1, 'history_gap_mask')
    assert 0 < gaps < 65535  # Some original records survived the bounded copy.
    first_gap = next(index for index in range(16) if gaps & (1 << index))
    s.expect(1, 'history_gap_mask', 65535 ^ ((1 << first_gap) - 1))
    for index in range(16):
        if gaps & (1 << index):
            for field in ('seq', 'time_us', 'value', 'type', 'a', 'b', 'reserved'):
                s.expect(1, 'history_event_' + field, 0, index)
        else:
            s.expect(1, 'history_event_seq', 65 + index, index)
            s.expect(1, 'history_event_time_us', seeded, index)
            s.expect(1, 'history_event_value', 64 + index, index)
    release(s, 1)
    pacing(s)


def scenario_peer_history_disconnected(s):
    attach(s)
    seed_history(s, 1)
    for token, count in ((0, 16), (5000, 0), (5000, 65)):
        request(s, 0, token, count, accepted=0)
    s.do(0, 'fault', {'drop': 100000})
    request(s, 0, 5001)
    s.advance(2999000)
    s.do(0, 'history_poll')
    s.expect(0, 'history_poll_ready', 0)
    s.do(0, 'report', 1, 0, keyboard(0, 5))
    s.advance(4000)
    result(s, 0, 5001, 1)
    s.expect_report(0, 1, keyboard(0, 5))
    release(s, 0)
    s.do(0, 'fault', {'drop': 0})
    request(s, 0, 5002)
    s.advance(1000000)
    result(s, 0, 5002)
    release(s, 0)
    pacing(s)


def scenario_peer_history_uart_stalled(s):
    attach(s)
    seed_history(s, 1)
    s.do(0, 'uart_stall', 1)
    s.do(0, 'fill', 1, 256)
    request(s, 0, 6001, 64)
    status_request(s, 0, 6101)
    s.do(0, 'report', 1, 0, keyboard(0, 6))
    s.do(0, 'report', 1, 1, mouse(x=4))
    s.advance(502000)
    status_result(s, 0, 6101, 1)
    s.expect_report(0, 1, keyboard(0, 6))
    s.expect_report(0, 2, out_mouse(0, 16004, 16000))
    s.advance(2497000)
    s.do(0, 'history_poll')
    s.expect(0, 'history_poll_ready', 0)
    s.advance(4000)
    result(s, 0, 6001, 1)
    s.expect(0, 'uart_queue', 256)
    s.check('diagnostic_pacing', 0, 1000)
    attempts = [e for e in s.trace if e['kind'] == 'diagnostic_enqueue' and e['node'] == 0]
    assert len(attempts) > 100 and all(e['b'] == 0 for e in attempts)
    release(s, 0)
    s.do(0, 'uart_stall', 0)
    s.advance(100000)
    request(s, 0, 6002)
    s.advance(1000000)
    result(s, 0, 6002)
    release(s, 0)
    pacing(s)


def scenario_peer_history_malformed(s):
    attach(s)
    s.do(0, 'fault', {'drop': 100000})
    request(s, 0, 7001)
    s.advance(10000)
    s.do(0, 'raw', frame(39, struct.pack('<IH2x', 7001, 65535)))
    s.advance(4000)
    result(s, 0, 7001, 2)
    release(s, 0)
    s.do(0, 'fault', {'drop': 0})
    request(s, 0, 7002)
    s.advance(1000000)
    result(s, 0, 7002)
    release(s, 0)
    pacing(s)


SCENARIOS = {
    'peer_history_roundtrip': scenario_peer_history_roundtrip,
    'peer_history_borrowed': scenario_peer_history_borrowed,
    'peer_history_stale': scenario_peer_history_stale,
    'peer_history_capture_gap': scenario_peer_history_capture_gap,
    'peer_history_disconnected': scenario_peer_history_disconnected,
    'peer_history_uart_stalled': scenario_peer_history_uart_stalled,
    'peer_history_malformed': scenario_peer_history_malformed,
}
