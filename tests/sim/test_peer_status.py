"""Peer status through both real bridge queues, UART framing, and core tasks."""
import struct
import zlib

from fixtures import attach, keyboard, mouse
from test_transport import frame, out_mouse


def request(s, node, token):
    s.do(node, 'diagnostic_request', token)
    s.expect(node, 'diagnostic_request_accepted', 1)


def result(s, node, token, outcome=0):
    s.do(node, 'diagnostic_poll')
    s.expect(node, 'diagnostic_poll_ready', 1)
    s.expect(node, 'diagnostic_token', token)
    s.expect(node, 'diagnostic_outcome', outcome)
    if outcome == 0:
        role = 1 - node
        s.expect(node, 'diagnostic_role', role)
        s.expect(node, 'diagnostic_major', 0)
        s.expect(node, 'diagnostic_minor', 97)
        s.expect(node, 'diagnostic_boot_session', role + 1)
        s.expect(node, 'diagnostic_crc', 0x12345678 + role)
        for index in range(8):
            s.expect(node, 'diagnostic_board_id', role * 16 + index, index)
    s.do(node, 'diagnostic_poll')
    s.expect(node, 'diagnostic_poll_ready', 0)  # Completion is delivered once.


def diagnostic_frames(s, source, kind):
    return [(event['at'], bytes.fromhex(event['data'])) for event in s.trace
            if event['kind'] == 'uart_tx' and event['node'] == source
            and bytes.fromhex(event['data'])[2] == kind]


def scenario_peer_status_roundtrip(s):
    attach(s)
    # Both boards are clients and servers simultaneously, with CDC disabled at
    # this boundary. The status service must still be present on core 1.
    started = s.now
    for node, token in ((0, 0x10203040), (1, 0x50607080)):
        request(s, node, token)
    s.do(0, 'report', 1, 0, keyboard(0, 4))
    s.do(1, 'report', 1, 0, mouse(x=7, y=-3))
    s.advance(50000)
    s.expect_report(0, 1, keyboard(0, 4))
    s.expect_report(0, 2, out_mouse(0, 16007, 15997))
    for node, token in ((0, 0x10203040), (1, 0x50607080)):
        result(s, node, token)
        s.check('range', node, 'diagnostic_uptime_ms', 0,
                started // 1000, started // 1000 + 3)
        s.expect(node, 'stopped', 0)
        requests = diagnostic_frames(s, node, 36)
        responses = diagnostic_frames(s, 1 - node, 37)
        assert len(requests) == 1 and len(responses) == 13
        assert requests[0][1] == bytes.fromhex(frame(36, struct.pack('<IB3x', token, 1)))
        assert responses[-1][0] < started + 50000
        # Independent wire-format oracle, including CRC across the assembled
        # snapshot. The transport checksum alone cannot certify all 13 chunks.
        data = bytearray(39)
        for _, raw in responses:
            assert raw[:3] == b'\xaa\x55\x25'
            assert int.from_bytes(raw[3:7], 'little') == token
            index = raw[7]
            data[index * 3:index * 3 + 3] = raw[8:11]
        assert data[0:2] == bytes([1, 1 - node])
        assert struct.unpack_from('<HH', data, 2) == (0, 97)
        assert data[6:14] == bytes((1 - node) * 16 + i for i in range(8))
        assert int.from_bytes(data[34:38], 'little') == zlib.crc32(data[:34])
        assert data[38] == 0
    # A human can request status again as soon as the first frame finishes.
    # Respect the server cooldown without manufacturing a peer timeout.
    repeated = s.now
    for node, token in ((0, 0x10203041), (1, 0x50607081)):
        request(s, node, token)
    s.advance(250000)
    for node, token in ((0, 0x10203041), (1, 0x50607081)):
        result(s, node, token)
        s.check('range', node, 'diagnostic_uptime_ms', 0,
                repeated // 1000, (repeated + 250000) // 1000)


def scenario_peer_status_disconnected(s):
    attach(s)
    s.do(0, 'fault', {'drop': 100000})
    request(s, 0, 101)
    s.advance(499000)
    s.do(0, 'diagnostic_poll')
    s.expect(0, 'diagnostic_poll_ready', 0)
    s.do(0, 'report', 1, 0, keyboard(0, 5))
    s.advance(3000)
    result(s, 0, 101, 1)  # No request reached a peer; still bounded.
    s.expect_report(0, 1, keyboard(0, 5))
    s.do(0, 'fault', {'drop': 0})
    request(s, 0, 102)
    s.advance(250000)
    result(s, 0, 102)
    s.advance(200000)
    # A request may arrive while the reverse wire is disconnected.
    s.do(1, 'fault', {'drop': 100000})
    request(s, 0, 103)
    s.advance(502000)
    result(s, 0, 103, 1)
    for node in (0, 1):
        s.expect(node, 'stopped', 0)


def scenario_peer_status_uart_stalled(s):
    attach(s)
    s.do(0, 'uart_stall', 1)
    s.do(0, 'fill', 1, 256)
    request(s, 0, 201)
    s.do(0, 'report', 1, 0, keyboard(0, 6))
    s.do(0, 'report', 1, 1, mouse(x=4))
    s.advance(499000)
    s.expect_report(0, 1, keyboard(0, 6))
    s.expect_report(0, 2, out_mouse(0, 16004, 16000))
    s.do(0, 'diagnostic_poll')
    s.expect(0, 'diagnostic_poll_ready', 0)
    s.advance(3000)
    result(s, 0, 201, 1)
    s.expect(0, 'uart_queue', 256)
    s.expect(0, 'stopped', 0)
    s.do(0, 'uart_stall', 0)
    s.advance(100000)
    request(s, 0, 202)
    s.advance(250000)
    result(s, 0, 202)


def scenario_peer_status_malformed(s):
    attach(s)
    s.do(0, 'fault', {'drop': 100000})
    request(s, 0, 301)
    s.advance(2000)
    # Correct UART envelope, matching token, impossible chunk index.
    s.do(0, 'raw', frame(37, struct.pack('<IB3x', 301, 13)))
    s.advance(3000)
    result(s, 0, 301, 2)
    request(s, 0, 302)
    s.advance(2000)
    # Old response tokens and malformed requests do not poison the current
    # query or stop a following real HID packet in the same receiver loop.
    s.do(0, 'raw', frame(37, struct.pack('<IB3x', 301, 255)))
    s.do(0, 'raw', frame(36, struct.pack('<IB3x', 401, 2)))
    s.do(1, 'report', 1, 0, mouse(x=9))
    s.advance(5000)
    s.expect_report(0, 2, out_mouse(0, 16009, 16000))
    s.do(0, 'diagnostic_poll')
    s.expect(0, 'diagnostic_poll_ready', 0)
    s.advance(495000)
    result(s, 0, 302, 1)
    s.do(0, 'fault', {'drop': 0})
    request(s, 0, 303)
    s.advance(250000)
    result(s, 0, 303)


SCENARIOS = {
    'peer_status_roundtrip': scenario_peer_status_roundtrip,
    'peer_status_disconnected': scenario_peer_status_disconnected,
    'peer_status_uart_stalled': scenario_peer_status_uart_stalled,
    'peer_status_malformed': scenario_peer_status_malformed,
}
