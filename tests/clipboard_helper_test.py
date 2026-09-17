"""Hardware-free fixtures only. Never calls NSPasteboard or opens a serial port."""
from contextlib import contextmanager
from pathlib import Path
import io
import os
import struct
import subprocess
import sys
import time
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests/updater'))
from clipboard import protocol as p, helper as h
from test_console import status, IDS, SESSIONS

BINDING = (int(SESSIONS['A'], 16), int(SESSIONS['B'], 16), 33, 1, 55)


def request(binding=BINDING):
    value = p.frame(p.REQUEST)
    struct.pack_into('<QQQQQ', value, 8, *binding)
    return value


class FakeTransport:
    def __init__(self):
        self.sent = []
        self.fail_at = None

    def write_frame(self, value, deadline):
        if len(self.sent) == self.fail_at:
            raise OSError('fixture disconnect')
        self.sent.append(bytearray(value))


class ProtocolTests(unittest.TestCase):
    def test_supported_exact_charset(self):
        self.assertEqual(p.classify(bytes(range(32, 127)) + b'\n\t'), p.Status.OK)
        for c in range(256):
            with self.subTest(c=c):
                expected = p.Status.OK if c in (9, 10) or 32 <= c <= 126 else p.Status.UNSUPPORTED
                self.assertEqual(p.classify(bytes([c])), expected)

    def test_limits_and_negative_statuses(self):
        self.assertEqual(p.classify(None), p.Status.NON_TEXT)
        self.assertEqual(p.classify(b''), p.Status.EMPTY)
        self.assertEqual(p.classify(b'a'*1024), p.Status.OK)
        self.assertEqual(p.classify(b'a'*1025), p.Status.OVERSIZE)
        self.assertEqual(p.classify('é'.encode()), p.Status.UNSUPPORTED)
        for status_value in list(p.Status)[1:]:
            packets = [bytes(v) for v in p.responses(BINDING, status_value, bytearray())]
            self.assertEqual(len(packets), 1)
            self.assertEqual(packets[0][48], status_value)
            self.assertFalse(any(packets[0][49:]))

    def test_complete1024_bytes_frames_crc_offsets(self):
        data = bytearray(b'X'*1024)
        packets = [bytes(v) for v in p.responses(BINDING, p.Status.OK, data)]
        self.assertEqual(len(packets), 28)
        self.assertEqual(struct.unpack_from('<QQQQQ', packets[0], 8), BINDING)
        self.assertEqual(struct.unpack_from('<BHI', packets[0], 48), (0, 1024, zlib.crc32(data)))
        actual = bytearray()
        for index, value in enumerate(packets[1:-1]):
            self.assertEqual(len(value), 64)
            self.assertEqual(value[:8], b'DHC2\x03\0\0\0')
            nonce, offset, count = struct.unpack_from('<QHB', value, 8)
            self.assertEqual((nonce, offset), (1, index*40))
            self.assertTrue(1 <= count <= 40)
            actual.extend(value[19:19+count])
            self.assertFalse(any(value[19+count:]))
        self.assertEqual(actual, data)
        self.assertEqual(packets[-1][4], p.END)
        self.assertEqual(struct.unpack_from('<Q', packets[-1], 8)[0], 1)
        self.assertFalse(any(packets[-1][16:]))

    def test_response_generator_wipes_completed_and_cancelled_frames(self):
        stream = p.responses(BINDING, p.Status.OK, bytearray(b'abc'))
        begin = next(stream)
        data = next(stream)
        self.assertFalse(any(begin))
        stream.close()
        self.assertFalse(any(data))

    def test_invalid_success_and_negative_payload_rejected(self):
        for status_value, data in [(p.Status.OK, b''), (p.Status.OK, b'\r'),
                                   (p.Status.OK, b'x'*1025), (p.Status.EMPTY, b'a')]:
            with self.assertRaises(p.ProtocolError):
                list(p.responses(BINDING, status_value, data))

    def test_strict_request_headers_and_padding(self):
        self.assertEqual(p.request(request()), BINDING)
        for position in (0, 4, 5, 6, 7, 48, 63):
            value = request()
            value[position] ^= 128
            with self.subTest(position=position), self.assertRaises(p.ProtocolError):
                p.request(value)
        for length in (0, 63, 65, 1024):
            with self.assertRaises(p.ProtocolError):
                p.request(bytearray(length))

    def test_hello_correlated_and_canonical(self):
        value = p.frame(p.HELLO)
        struct.pack_into('<QQ', value, 8, BINDING[0], BINDING[2])
        p.hello(value, BINDING[0], BINDING[2])
        for boot, helper in ((1, BINDING[2]), (BINDING[0], 1)):
            with self.assertRaises(p.ProtocolError):
                p.hello(value, boot, helper)
        value[-1] = 1
        with self.assertRaises(p.ProtocolError):
            p.hello(value, BINDING[0], BINDING[2])


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.transport = FakeTransport()
        self.reads = []
        self.data = bytearray(b'Fixture text\n')
        def reader(deadline, tick):
            self.reads.append(deadline)
            tick()
            return p.Status.OK, self.data
        self.session = h.Session(self.transport, BINDING[0], BINDING[2], reader)

    def test_once_current_per_fresh_request_no_cache(self):
        self.session.handle(request())
        self.assertEqual(len(self.reads), 1)
        self.assertFalse(any(self.data))
        for nonce in (1, 0):
            with self.assertRaises(p.ProtocolError):
                self.session.handle(request((*BINDING[:3], nonce, BINDING[4])))
        self.assertEqual(len(self.reads), 1)
        self.data = bytearray(b'New current fixture')
        self.session.handle(request((*BINDING[:3], 2, BINDING[4])))
        self.assertEqual(len(self.reads), 2)
        self.assertFalse(any(self.data))

    def test_stale_binding_and_reboot_never_read(self):
        for field in (0, 2):
            binding = list(BINDING)
            binding[field] += 1
            with self.assertRaises(p.ProtocolError):
                self.session.handle(request(binding))
        self.assertFalse(self.reads)
        self.session.handle(request())
        with self.assertRaises(h.PeerRestarted):
            self.session.handle(request((BINDING[0], BINDING[1]+1, BINDING[2], 2, BINDING[4])))
        self.assertEqual(len(self.reads), 1)

    def test_timeout_discards_and_wipes_without_begin(self):
        clock = [0]
        def reader(deadline, tick):
            clock[0] = h.RESPONSE_SECONDS
            return p.Status.OK, self.data
        session = h.Session(self.transport, BINDING[0], BINDING[2], reader, lambda: clock[0])
        session.handle(request())
        self.assertFalse(self.transport.sent)
        self.assertFalse(any(self.data))

    def test_disconnected_mid_transfer_no_end_and_wipe(self):
        self.transport.fail_at = 2  # PING and BEGIN succeed; DATA fails
        with self.assertRaises(OSError):
            self.session.handle(request())
        self.assertFalse(any(self.data))
        self.assertNotIn(p.END, [v[4] for v in self.transport.sent])

    def test_negative_result_never_sends_payload_or_end(self):
        for status_value in list(p.Status)[1:]:
            transport = FakeTransport()
            session = h.Session(transport, BINDING[0], BINDING[2],
                                lambda *_: (status_value, bytearray()))
            session.handle(request())
            self.assertEqual([v[4] for v in transport.sent], [p.PING, p.BEGIN])

    def test_heartbeat_every250ms_not_every_poll(self):
        now = [0]
        session = h.Session(self.transport, 1, 2, None, lambda: now[0])
        for at in (0, .05, .24, .25, .49, .5):
            now[0] = at
            session.heartbeat()
        self.assertEqual(len(self.transport.sent), 3)
        for value in self.transport.sent:
            self.assertEqual(value[4], p.PING)
            self.assertEqual(struct.unpack_from('<Q', value, 8)[0], 2)
            self.assertFalse(any(value[16:]))

    def test_run_wipes_request_on_failure(self):
        value = request((1, *BINDING[1:]))
        self.transport.read_frame = lambda timeout: value
        with self.assertRaises(p.ProtocolError):
            self.session.run()
        self.assertFalse(any(value))


class IdentityAndTransportTests(unittest.TestCase):
    def test_exact_uid_role_build_boot_and_optional_peer(self):
        self.assertEqual(h.check_identity(status(build='0.115'), IDS['A'], '0.115'), BINDING[0])
        self.assertNotEqual(h.check_identity(status(build='0.115', local='B'), IDS['B'], '0.115'), 0)
        self.assertEqual(h.check_identity(status(build='0.115', peer='timeout_or_unsupported'),
                                         IDS['A'], '0.115'), BINDING[0])
        for raw, uid, build in [(status(build='0.115', local='B'), IDS['A'], '0.115'),
                                (status(build='0.115'), IDS['B'], '0.115'),
                                (status(build='0.113'), IDS['A'], '0.113'),
                                (status(build='0.114'), IDS['A'], '0.114'),
                                (status(build='0.115'), IDS['A'], '0.116'),
                                (status(build='0.115').replace(SESSIONS['A'].encode(), b'0'*16), IDS['A'], '0.115'),
                                (status(build='0.115').replace(b'build=0.115', b'build=0.116', 1), IDS['A'], '0.115')]:
            with self.assertRaises(ValueError):
                h.check_identity(raw, uid, build)

    def test_private_console_never_keeps_transcript(self):
        console = h.PrivateConsole('/dev/cu.fixture')
        output = io.StringIO()
        console.transcript = output
        console._log('TX', b'sensitive fixture')
        console._log('RX', b'sensitive fixture')
        self.assertFalse(console.events)
        self.assertEqual(output.getvalue(), '')

    def test_partial_writes_do_not_drop_characters(self):
        console = h.PrivateConsole('/dev/cu.fixture')
        console.fd = 88
        written = bytearray()
        def write(fd, value):
            count = min(7, len(value))
            written.extend(value[:count])
            return count
        with patch.object(h.select, 'select', return_value=([], [88], [])), patch.object(h.os, 'write', write):
            console.write_frame(request(), time.monotonic()+1)
        self.assertEqual(written, request())

    def test_fragmented_reads_bounded_to64(self):
        console = h.PrivateConsole('/dev/cu.fixture')
        console.fd = 88
        source = bytearray(request())
        asked = []
        def read(fd, maximum):
            asked.append(maximum)
            value = bytes(source[:min(maximum, 3)])
            del source[:len(value)]
            return value
        with patch.object(h.select, 'select', return_value=([88], [], [])), patch.object(h.os, 'read', read):
            self.assertEqual(console.read_frame(.5), request())
        self.assertFalse(source)
        self.assertTrue(all(1 <= n <= 64 for n in asked))
        self.assertFalse(console.pending)

    def test_incomplete_frame_deadline(self):
        console = h.PrivateConsole('/dev/cu.fixture')
        console.pending = b'DH'
        console.partial_at = time.monotonic() - 2
        with self.assertRaises(p.ProtocolError):
            console.read_frame(.05)

    def test_mode_entry_exact_echo_without_prompt(self):
        console = h.PrivateConsole('/dev/cu.fixture')
        console._send = lambda *_: None
        console._read_until = lambda *_: b'clipboard 0000000000000021\r\n'
        console.enter(33)
        console._read_until = lambda *_: b'ERROR clipboard unavailable\r\n'
        with self.assertRaises(p.ProtocolError):
            console.enter(33)


class ConnectAndLifecycleTests(unittest.TestCase):
    class Transport(FakeTransport):
        def __init__(self, port):
            super().__init__()
            self.port, self.closed, self.helper = port, False, None
            self.raw_status = status(build='0.115')
            self.bad_hello = False
        def __enter__(self):
            return self
        def __exit__(self, *_):
            self.closed = True
        def command(self, command):
            assert command == 'status'
            return self.raw_status
        def enter(self, helper):
            self.helper = helper
        def read_frame(self, timeout):
            value = p.frame(p.HELLO)
            struct.pack_into('<QQ', value, 8, BINDING[0], self.helper + int(self.bad_hello))
            return value

    def test_fresh_helper_session_every_reconnect_no_implicit_read(self):
        sessions = []
        reads = []
        def factory(port):
            transport = self.Transport(port)
            sessions.append(transport)
            return transport
        with patch.object(h.secrets, 'randbits', side_effect=[0, 101, 102]):
            for _ in range(2):
                h.connect('/dev/cu.fixture', IDS['A'], '0.115', lambda *_: reads.append(True),
                          factory, stop=lambda: True)
        self.assertEqual([session.helper for session in sessions], [101, 102])
        self.assertTrue(all(session.closed for session in sessions))
        self.assertFalse(reads)

    def test_identity_and_hello_failure_close_before_read(self):
        for bad in ('identity', 'hello'):
            transport = self.Transport('/dev/cu.fixture')
            transport.bad_hello = bad == 'hello'
            if bad == 'identity':
                transport.raw_status = status(build='0.115', local='B')
            with self.assertRaises(p.ProtocolError):
                h.connect(transport.port, IDS['A'], '0.115', lambda *_: self.fail('clipboard read'),
                          lambda _: transport, stop=lambda: True)
            self.assertTrue(transport.closed)
            if bad == 'identity':
                self.assertIsNone(transport.helper)

    def test_close_drops_dtr_and_releases_even_after_disconnect(self):
        console = h.PrivateConsole('/dev/cu.fixture')
        console.fd = 88
        console.original_attrs = ['fixture']
        actions = []
        def dtr(enabled):
            actions.append(('dtr', enabled))
            raise OSError('fixture removed')
        console._dtr = dtr
        with patch('deskhop_update.console.termios.tcsetattr', side_effect=lambda *_: actions.append(('restore', True))), \
             patch('deskhop_update.console.fcntl.ioctl', side_effect=lambda *_: actions.append(('exclusive', False))), \
             patch('deskhop_update.console.os.close', side_effect=lambda _: actions.append(('closed', True))):
            console.close()
        self.assertEqual(actions[0], ('dtr', False))
        self.assertEqual(actions[-1], ('closed', True))
        self.assertIn(('restore', True), actions)
        self.assertIsNone(console.fd)

    def test_reconnect_count_is_bounded_and_errors_are_sanitized(self):
        args = ['--port', '/dev/cu.fixture', '--expected-a-uid', IDS['A'], '--expected-build',
                '0.115', '--reader', sys.executable, '--reconnects', '3']
        output = io.StringIO()
        for failure, expected_calls in ((OSError('PRIVATE FIXTURE'), 4),
                                        (p.ProtocolError('PRIVATE FIXTURE'), 1)):
            with patch.object(h.sys, 'platform', 'darwin'), \
                 patch.object(h, 'disable_core_dumps'), patch.object(h.signal, 'signal'), \
                 patch.object(h.time, 'sleep'), patch.object(h, 'connect', side_effect=failure) as connect, \
                 patch.object(h.sys, 'stdout', output), patch.object(h.sys, 'stderr', output):
                self.assertEqual(h.main(args), 1)
            self.assertEqual(connect.call_count, expected_calls)
        self.assertNotIn('PRIVATE FIXTURE', output.getvalue())

    def test_ctrl_c_and_sigterm_release_connection(self):
        with self.assertRaises(KeyboardInterrupt):
            h.interrupt(15, None)
        transport = self.Transport('/dev/cu.fixture')
        with patch.object(h.Session, 'run', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            h.connect(transport.port, IDS['A'], '0.115', None, lambda _: transport)
        self.assertTrue(transport.closed)


class NativeBoundaryTests(unittest.TestCase):
    def read_fixture(self, script, *, seconds=.5):
        real_popen = subprocess.Popen
        def fixture_process(*_, **kwargs):
            return real_popen([sys.executable, '-c', script], **kwargs)
        ticks = []
        with patch.object(h.subprocess, 'Popen', fixture_process):
            status_value, data = h.NativeReader('/never/a/clipboard/reader')(
                time.monotonic()+seconds, lambda: ticks.append(True))
        self.assertTrue(ticks)
        return status_value, data

    def test_valid_native_result(self):
        self.assertEqual(self.read_fixture("import os; os.write(1, b'\\0\\3\\0abc')"),
                         (p.Status.OK, bytearray(b'abc')))

    def test_native_maximum(self):
        status_value, data = self.read_fixture("import os; os.write(1, b'\\0\\0\\4'+b'x'*1024)")
        self.assertEqual((status_value, len(data)), (p.Status.OK, 1024))
        p.wipe(data)

    def test_negative_results(self):
        for code in range(1, 6):
            self.assertEqual(self.read_fixture(f'import os; os.write(1, bytes([{code},0,0]))'),
                             (p.Status(code), bytearray()))

    def test_malformed_native_results_fail_closed(self):
        for output in (b'', b'\0', b'\0\0\0', b'\0\2\0a', b'\0\1\0\r',
                       b'\6\0\0', b'\1\1\0a', b'\0\1\4'+b'a'*1025,
                       b'\0\1\0aTRAILING'):
            self.assertEqual(self.read_fixture(f'import os; os.write(1, {output!r})'),
                             (p.Status.UNAVAILABLE, bytearray()))

    def test_no_unbounded_child_output_or_hung_reader(self):
        started = time.monotonic()
        self.assertEqual(self.read_fixture('import os; os.write(1,b"x"*1000000)'),
                         (p.Status.UNAVAILABLE, bytearray()))
        self.assertEqual(self.read_fixture('import time; time.sleep(60)', seconds=.1),
                         (p.Status.UNAVAILABLE, bytearray()))
        self.assertEqual(self.read_fixture('import os,time; os.close(1); time.sleep(60)', seconds=.1),
                         (p.Status.UNAVAILABLE, bytearray()))
        self.assertLess(time.monotonic()-started, 2)

    def test_child_failure_and_missing_reader(self):
        self.assertEqual(self.read_fixture('raise SystemExit(1)'), (p.Status.UNAVAILABLE, bytearray()))
        self.assertEqual(h.NativeReader('/fixture/not/existing')(time.monotonic()+1, lambda: None),
                         (p.Status.UNAVAILABLE, bytearray()))

    @unittest.skipUnless(sys.platform == 'darwin' and os.environ.get('DESKHOP_TEST_NATIVE_READER'),
                         'native fixture binary not requested')
    def test_swift_fixture_only(self):
        reader = os.environ['DESKHOP_TEST_NATIVE_READER']
        for value in (b'', b'Hello\n\tWorld!', bytes(range(32,127)), b'a'*1024,
                      b'a'*1025, b'\r', b'\0', 'é'.encode()):
            result = subprocess.run([reader, '--fixture'], input=value, capture_output=True, check=True)
            self.assertEqual(result.stderr, b'')
            expected = p.classify(value)
            length = len(value) if expected == p.Status.OK else 0
            self.assertEqual(result.stdout, struct.pack('<BH', expected, length)+(value if length else b''))


if __name__ == '__main__':
    unittest.main()
