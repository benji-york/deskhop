"""Opt-in, foreground Mac clipboard helper; payloads are never journaled.

No clipboard reads occur until a validated fresh REQUEST reaches this process.
The native reader is a short-lived child; all protocol buffers are bounded.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import resource
import secrets
import signal
import select
import struct
import subprocess
import sys
import time

from deskhop_update.console import Console, parse_status, encoded_build
from .protocol import (MAX_TEXT, FRAME_BYTES, Status, ProtocolError, classify,
                       hello, ping, request, require, responses, wipe)

HEARTBEAT_SECONDS = 0.25
RESPONSE_SECONDS = 2.5  # firmware also enforces 3 seconds from original trigger
NATIVE_SECONDS = 2.0


class PeerRestarted(OSError):
    """Drop the old helper lease before repeating identity validation."""


class PrivateConsole(Console):
    """Reuse strict CDC ownership/cleanup, suppress every transcript/event hook."""
    def __init__(self, port):
        super().__init__(port, timeout=6, total_timeout=15)
        self.partial_at = None

    def _log(self, *_):
        pass

    def _send(self, payload, deadline):
        require(self.fd is not None, 'clipboard port is closed')
        offset = 0
        while offset < len(payload):
            _, ready, _ = select.select([], [self.fd], [], self._remaining(deadline))
            if ready:
                try:
                    written = os.write(self.fd, memoryview(payload)[offset:])
                except (BlockingIOError, InterruptedError):
                    continue
                require(written > 0, 'clipboard write failed')
                offset += written

    def write_frame(self, value, deadline):
        require(len(value) == FRAME_BYTES, 'invalid outgoing clipboard frame')
        self._send(value, deadline)

    def enter(self, helper):
        require(not self.pending, 'unsolicited clipboard entry bytes')
        command = f'clipboard {helper:016x}'
        deadline = time.monotonic() + 2
        self._send((command + '\n').encode('ascii'), deadline)
        echo = self._read_until(b'\r\n', deadline)
        require(echo == (command + '\r\n').encode('ascii'), 'clipboard entry echo differs')
        self.deadline = float('inf')  # foreground lifetime; every I/O still bounded

    def read_frame(self, timeout):
        deadline = time.monotonic() + timeout
        while len(self.pending) < FRAME_BYTES:
            now = time.monotonic()
            require(not self.pending or self.partial_at is None or now - self.partial_at < 1,
                    'incomplete clipboard frame expired')
            if now >= deadline:
                return None
            ready, _, _ = select.select([self.fd], [], [], min(0.05, deadline - now))
            if ready:
                try:
                    chunk = os.read(self.fd, FRAME_BYTES - len(self.pending))
                except (BlockingIOError, InterruptedError):
                    continue
                if chunk:
                    if not self.pending:
                        self.partial_at = now
                    self.pending += chunk
                else:
                    # VMIN=0 may return no data. Bounded polling prevents a spin.
                    time.sleep(min(0.01, max(0, deadline - time.monotonic())))
        result, self.pending = bytearray(self.pending[:FRAME_BYTES]), self.pending[FRAME_BYTES:]
        self.partial_at = time.monotonic() if self.pending else None
        return result


def check_identity(raw, uid, build):
    state = parse_status(raw)
    board = state['boards'][state['local']]
    require(board['board_id'] == uid, 'Local Pico identity differs')
    require(encoded_build(build) >= encoded_build('0.115') and board['build'] == build,
            'Local Pico firmware differs from explicit supported build')
    for peer in state['boards'].values():
        require(peer['build'] == build and peer['update']['phase'] == 'idle',
                'peer firmware differs or maintenance is active')
    boot = int(board['boot_session'], 16)
    require(boot != 0, 'Local Pico boot identity missing')
    return boot


class NativeReader:
    def __init__(self, executable):
        self.executable = str(executable)

    def __call__(self, deadline, tick):
        """Bound output before reading, and service heartbeats while child waits."""
        raw = bytearray(MAX_TEXT + 4)  # header3 +1024 payload + overflow sentinel
        used = 0
        child = None
        try:
            child = subprocess.Popen([self.executable, '--read-current'], stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     close_fds=True)
            os.set_blocking(child.stdout.fileno(), False)
            eof = False
            deadline = min(deadline, time.monotonic() + NATIVE_SECONDS)
            while not eof or child.poll() is None:
                tick()
                if time.monotonic() >= deadline:
                    return Status.UNAVAILABLE, bytearray()
                if eof:
                    time.sleep(0.025)  # stdout may close before a hung child exits
                    continue
                ready, _, _ = select.select([child.stdout], [], [], 0.025)
                if ready:
                    try:
                        count = os.readv(child.stdout.fileno(), [memoryview(raw)[used:]])
                    except (BlockingIOError, InterruptedError):
                        continue
                    if count == 0:
                        eof = True
                    used += count
                    if used == len(raw):
                        return Status.UNAVAILABLE, bytearray()
            if child.returncode != 0 or used < 3:
                return Status.UNAVAILABLE, bytearray()
            status, length = raw[0], struct.unpack_from('<H', raw, 1)[0]
            if status not in Status._value2member_map_ or length > MAX_TEXT or used != length + 3:
                return Status.UNAVAILABLE, bytearray()
            status = Status(status)
            data = bytearray(memoryview(raw)[3:used])
            if (status == Status.OK and classify(data) != Status.OK or
                    status != Status.OK and data):
                wipe(data)
                return Status.UNAVAILABLE, bytearray()
            return status, data
        except (OSError, subprocess.SubprocessError):
            return Status.UNAVAILABLE, bytearray()
        finally:
            if child is not None:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=1)
                child.stdout.close()
            wipe(raw)


class Session:
    def __init__(self, transport, source_boot, helper, reader, clock=time.monotonic):
        self.transport, self.source_boot, self.helper = transport, source_boot, helper
        self.reader, self.clock = reader, clock
        self.target_boot = None
        self.last_nonce = 0
        self.next_ping = 0.0

    def heartbeat(self):
        now = self.clock()
        if now >= self.next_ping:
            value = ping(self.helper)
            try:
                self.transport.write_frame(value, now + 0.5)
            finally:
                wipe(value)
            self.next_ping = self.clock() + HEARTBEAT_SECONDS

    def handle(self, value):
        started = self.clock()
        binding = request(value)
        source_boot, target_boot, helper, nonce, _ = binding
        require((source_boot, helper) == (self.source_boot, self.helper), 'stale clipboard request session')
        if self.target_boot is not None and target_boot != self.target_boot:
            raise PeerRestarted('Pico B session changed')
        require(nonce > self.last_nonce, 'duplicate or reordered clipboard request')
        self.target_boot, self.last_nonce = target_boot, nonce  # consumed even on unavailable/timeout
        data = bytearray()
        stream = None
        try:
            status, data = self.reader(started + RESPONSE_SECONDS, self.heartbeat)
            require(isinstance(data, bytearray), 'clipboard reader must return mutable data')
            if self.clock() >= started + RESPONSE_SECONDS:
                return  # never send a late result
            stream = responses(binding, status, data)
            for outgoing in stream:
                if self.clock() >= started + RESPONSE_SECONDS:
                    return  # no END means firmware cannot type
                self.heartbeat()
                self.transport.write_frame(outgoing, min(started + RESPONSE_SECONDS, self.clock() + 0.5))
        finally:
            if stream is not None:
                stream.close()
            wipe(data)

    def run(self, stop=lambda: False):
        while not stop():
            self.heartbeat()
            value = self.transport.read_frame(0.05)
            if value is not None:
                try:
                    self.handle(value)
                finally:
                    wipe(value)


def connect(port, uid, build, reader, transport_factory=PrivateConsole, stop=lambda: False):
    helper = 0
    while not helper:
        helper = secrets.randbits(64)
    with transport_factory(port) as transport:
        source_boot = check_identity(transport.command('status'), uid, build)
        transport.enter(helper)
        greeting = transport.read_frame(2)
        require(greeting is not None, 'clipboard HELLO missing')
        try:
            hello(greeting, source_boot, helper)
        finally:
            wipe(greeting)
        session = Session(transport, source_boot, helper, reader)
        session.run(stop)


def disable_core_dumps():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--port', required=True, help='explicit /dev/cu.* connected to this Mac')
    result.add_argument('--expected-uid', '--expected-a-uid', dest='expected_uid', required=True, help='local physical flash UID, 16 hex digits')
    result.add_argument('--expected-build', required=True, help='exact installed candidate build, at least 0.115')
    result.add_argument('--reader', type=Path, required=True, help='locally compiled native reader executable')
    result.add_argument('--reconnects', type=int, choices=range(4), default=3,
                        help='bounded attempts after disconnect/timeout; default 3, at most 3')
    return result


def interrupt(signum, frame):
    raise KeyboardInterrupt


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        require(sys.platform == 'darwin', 'clipboard helper requires macOS')
        require(re.fullmatch(r'/dev/cu\.[^/\x00]+', args.port) is not None, 'explicit /dev/cu.* required')
        require(re.fullmatch(r'[0-9a-fA-F]{16}', args.expected_uid) is not None, 'invalid local UID')
        require(args.reader.is_file() and os.access(args.reader, os.X_OK), 'native reader is not executable')
        require(encoded_build(args.expected_build) >= encoded_build('0.115'), 'unsupported firmware build')
        disable_core_dumps()
        signal.signal(signal.SIGTERM, interrupt)
        reader = NativeReader(args.reader.resolve())
        print('Clipboard helper active in foreground. Ctrl-C releases serial for upgrades.', flush=True)
        for attempt in range(args.reconnects + 1):
            try:
                connect(args.port, args.expected_uid.upper(), args.expected_build, reader)
                return 0
            except (OSError, TimeoutError):
                if attempt == args.reconnects:
                    raise
                # A reconnect has a new helper session and repeats UID/build checks.
                print('Clipboard connection interrupted; retrying identity check.', flush=True)
                time.sleep(1)
    except KeyboardInterrupt:
        print('Clipboard helper stopped; serial port released.', flush=True)
        return 130
    except Exception:
        # Never stringify exceptions, subprocess outputs or frame payloads here.
        print('Clipboard helper stopped: device identity, protocol, or connection check failed.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
