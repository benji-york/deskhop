"""Explicit macOS USB/PICOBOOT boundary. Never downloads tools or escalates."""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from .console import Console


class DeploymentError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise DeploymentError(message)


def write_json(path, value):
    """Replace only this run's journal, preserving completed evidence files."""
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    with temporary.open('w') as output:
        json.dump(value, output, indent=2)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


@contextmanager
def deployment_lock(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / '.update.lock').open('a+') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise DeploymentError('Another updater owns this deployment directory.') from exc
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def media_state(text):
    headers = [line for line in text.splitlines() if '+-o ' in line]
    require(headers and all(' active,' in line and 'busy 0 ' in line and 'inactive' not in line
                            for line in headers), 'Mac media clients are not all active and nonbusy.')
    identifiers = [re.findall(r'\bid (0x[0-9a-f]+)', line) for line in headers]
    require(all(len(ids) == 1 for ids in identifiers), 'Cannot identify every Mac media client.')
    flattened = [ids[0] for ids in identifiers]
    require(len(set(flattened)) == len(flattened), 'Duplicate Mac media client identity.')
    return sorted(flattened)


def boot_state(text, required):
    headers = [line for line in text.splitlines() if '+-o RP2 Boot' in line]
    if not required:
        require(not headers, 'An RP2 Boot device already exists; use the explicit already-bootloader mode.')
        return
    require(len(headers) == 1 and ' active,' in headers[0] and 'busy 0 ' in headers[0],
            'Expected exactly one active, nonbusy RP2 Boot device.')
    interfaces = [line for line in text.splitlines() if '+-o ' in line and '<class IOUSBHostInterface,' in line]
    require(len(interfaces) == 1 and ' active,' in interfaces[0] and 'busy 0 ' in interfaces[0],
            'Expected exactly one active, nonbusy PICOBOOT interface.')
    require(re.findall(r'"bInterfaceClass" = (\d+)', text) == ['255'],
            'Refusing non-disk-free ROM mode: PICOBOOT must be the sole USB interface.')


def usb_pin(text):
    """Identify one disk-free ROM connection, not the nonunique ROM serial."""
    boot_state(text, True)
    # Only device properties count; children repeat location/VID/PID fields.
    root = text.split('\n  +-o', 1)[0]
    ids = re.findall(r'^\+-o RP2 Boot@[^\n]*\bid (0x[0-9a-f]+),', root, re.M)
    require(len(ids) == 1, 'Cannot pin the sole ROM registry identity.')
    pin = {'registry_id': ids[0]}
    for key in ('sessionID', 'locationID', 'USB Address', 'idVendor', 'idProduct'):
        values = re.findall(r'^\s*\|?\s*"' + re.escape(key) + r'" = (\d+)\s*$', root, re.M)
        require(len(values) == 1, f'Ambiguous/missing USB property: {key}.')
        pin[key] = int(values[0])
    require(pin['idVendor'] == 0x2e8a and pin['idProduct'] == 3, 'Unexpected ROM VID/PID.')
    require(pin['sessionID'] > 0 and pin['locationID'] > 0
            and 1 <= pin['USB Address'] <= 127, 'Invalid USB connection identity.')
    return pin


def usb_selector(identity_output, pin):
    """Bind the UID check's actual libusb selection to the observed connection."""
    opened = set(re.findall(r'\[libusb_open\]\s+open (\d+)\.(\d+)\s*$', identity_output, re.M))
    require(len(opened) == 1, 'UID check did not open exactly one USB address.')
    bus, address = map(int, next(iter(opened)))
    require(1 <= bus <= 255 and address == pin['USB Address'],
            'libusb selector does not match the pinned ROM device.')
    return ['--bus', str(bus), '--address', str(address)]


class MacBackend:
    def __init__(self, evidence, port, picotool='picotool'):
        require(sys.platform == 'darwin', 'The USB/media safety backend currently supports macOS only.')
        require(re.fullmatch(r'/dev/cu\.[^/]+', port) is not None, 'Specify one explicit /dev/cu.* port.')
        self.evidence, self.port = Path(evidence), port
        self.picotool = shutil.which(picotool)
        require(self.picotool, 'picotool not found. Set PICOTOOL or --picotool to its executable path.')
        self.commands, self.events = [], []
        self.serial_number = 0
        self.rom_session = None
        self._identity_attempted = False

    def run(self, name, argv, timeout=20):
        start = time.monotonic()
        entry = {'name': name, 'command': list(argv), 'started_monotonic': start}
        env = None
        if argv[0] == self.picotool:
            # Stock libusb diagnostic setting; matches the successful ROM read.
            # Scope it to picotool children, never mutate/log the parent env.
            # Timing benefits remain a workaround hypothesis, not a guarantee.
            env = dict(os.environ, LIBUSB_DEBUG='4')
            entry['environment_overrides'] = {'LIBUSB_DEBUG': '4'}
        self.commands.append(entry)
        write_json(self.evidence / 'commands.json', self.commands)
        path = self.evidence / f'{len(self.commands):03d}-{name}.log'
        try:
            # Stream to a file: tool output cannot grow an in-memory capture.
            with path.open('xb') as output:
                completed = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                           timeout=timeout, env=env)
            entry['returncode'] = completed.returncode
            require(completed.returncode == 0, f'{name} failed; inspect {path}. No automatic retry.')
            require(path.stat().st_size <= 4 * 1024 * 1024, f'Excessive output from {name}; inspect {path}.')
            return path.read_text()
        except BaseException as exc:
            entry['error'] = f'{type(exc).__name__}: {exc}'
            raise
        finally:
            entry['seconds'] = round(time.monotonic() - start, 6)
            write_json(self.evidence / 'commands.json', self.commands)

    def health(self, boot=False, baseline=None):
        media = self.run('media', ['ioreg', '-p', 'IOService', '-r', '-c', 'IOMediaBSDClient', '-l', '-w', '0'], 5)
        usb = self.run('usb', ['ioreg', '-p', 'IOService', '-r', '-n', 'RP2 Boot', '-l', '-w', '0'], 5)
        state = media_state(media)
        boot_state(usb, boot)
        if baseline is not None:
            require(state == baseline, 'Mac media clients changed during deployment; stop and inspect.')
        return state

    def wait_bootloader(self, uid, baseline, timeout=8):
        deadline = time.monotonic() + timeout
        while True:
            usb = self.run('wait-usb', ['ioreg', '-p', 'IOService', '-r', '-n', 'RP2 Boot', '-l', '-w', '0'], 5)
            if '+-o RP2 Boot' in usb:
                self.health(boot=True, baseline=baseline)
                self.identity(uid)
                return
            require(time.monotonic() < deadline, 'Bootloader enumeration unconfirmed; inspect the target before retrying.')
            time.sleep(0.1)

    def identity(self, uid):
        try:
            require(not self._identity_attempted, 'ROM identity is checked once; no automatic re-identification.')
            self._identity_attempted = True
            require(re.fullmatch(r'[0-9A-F]{16}', uid) is not None, 'Invalid physical flash UID.')
            before = self.observe_pin()
            output = self.run('identity', [self.picotool, 'info', '-a', '--ser', uid])
            require(re.findall(r'flash id:\s+0x([0-9A-Fa-f]+)\b', output) == [uid],
                    'PICOBOOT flash UID differs or is ambiguous.')
            require(re.findall(r'^\s*name:\s+(\S+)', output, re.M) == ['deskhop'],
                    'Selected flash does not identify as DeskHop.')
            selector = usb_selector(output, before)
            require(self.observe_pin() == before, 'ROM device changed during UID check.')
            session = {'uid': uid, 'pin': before, 'selector': selector}
            write_json(self.evidence / 'rom-session.json', session)
            self.rom_session = session
        except BaseException:
            self.rom_session = None
            raise

    def observe_pin(self):
        return usb_pin(self.run('pin-usb',
            ['ioreg', '-p', 'IOService', '-r', '-n', 'RP2 Boot', '-l', '-w', '0'], 5))

    def guard_session(self, uid):
        require(self.rom_session is not None, 'No valid pinned ROM session; stop and inspect.')
        require(uid == self.rom_session['uid'], 'Requested UID differs from the pinned ROM session.')
        require(self.observe_pin() == self.rom_session['pin'],
                'ROM connection changed; selector expired. Stop and inspect.')
        return list(self.rom_session['selector'])

    @contextmanager
    def rom_operation(self, uid, *, reboot=False):
        # These observations detect enumeration changes; they are not an atomic
        # USB handle. Keep cables/other USB tools untouched during deployment.
        try:
            selector = self.guard_session(uid)
            yield selector
            if not reboot:
                self.guard_session(uid)
        except BaseException:
            self.rom_session = None
            raise
        finally:
            if reboot:
                self.rom_session = None  # Even an uncertain reboot expires it.

    def save(self, label, start, end, uid):
        with self.rom_operation(uid) as selector:
            path = self.evidence / (label + '.bin')
            require(not path.exists(), 'Never overwrite a previous backup/readback.')
            self.run(label, [self.picotool, 'save', '-r', hex(start), hex(end), str(path), *selector])
            data = path.read_bytes()
            require(len(data) == end - start, f'{label}: short flash read.')
        return data

    def load(self, uf2, uid):
        with self.rom_operation(uid) as selector:
            self.run('load-verify', [self.picotool, 'load', '-v', str(uf2), *selector], 30)

    def reboot(self, uid):
        # Normal application only. Never use reboot -u (enables ROM storage).
        with self.rom_operation(uid, reboot=True) as selector:
            self.run('reboot-application', [self.picotool, 'reboot', '-a', *selector], 10)

    def wait_port(self, timeout=8):
        deadline = time.monotonic() + timeout
        while not Path(self.port).exists():
            require(time.monotonic() < deadline, f'{self.port} did not return; no other port will be selected automatically.')
            time.sleep(0.1)

    def console(self, total_timeout=120):
        self.serial_number += 1
        path = self.evidence / f'serial-{self.serial_number}.jsonl'

        def event(value):
            with path.open('a') as output:
                output.write(json.dumps(value) + '\n')

        return Console(self.port, total_timeout=total_timeout, transcript=event)

    @staticmethod
    def pause(seconds):
        time.sleep(seconds)

    @staticmethod
    def now():
        return time.monotonic()
