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


class MacBackend:
    def __init__(self, evidence, port, picotool='picotool'):
        require(sys.platform == 'darwin', 'The USB/media safety backend currently supports macOS only.')
        require(re.fullmatch(r'/dev/cu\.[^/]+', port) is not None, 'Specify one explicit /dev/cu.* port.')
        self.evidence, self.port = Path(evidence), port
        self.picotool = shutil.which(picotool)
        require(self.picotool, 'picotool not found. Set PICOTOOL or --picotool to its executable path.')
        self.commands, self.events = [], []
        self.serial_number = 0

    def run(self, name, argv, timeout=20):
        start = time.monotonic()
        entry = {'name': name, 'command': list(argv), 'started_monotonic': start}
        self.commands.append(entry)
        write_json(self.evidence / 'commands.json', self.commands)
        path = self.evidence / f'{len(self.commands):03d}-{name}.log'
        try:
            # Stream to a file: tool output cannot grow an in-memory capture.
            with path.open('xb') as output:
                completed = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
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
        output = self.run('identity', [self.picotool, 'info', '-a', '--ser', uid])
        require(re.search(r'flash id:\s+0x' + re.escape(uid) + r'\b', output), 'PICOBOOT flash UID differs.')
        require(re.search(r'name:\s+deskhop\b', output), 'Selected flash does not identify as DeskHop.')

    def save(self, label, start, end, uid):
        path = self.evidence / (label + '.bin')
        require(not path.exists(), 'Never overwrite a previous backup/readback.')
        self.run(label, [self.picotool, 'save', '-r', hex(start), hex(end), str(path), '--ser', uid])
        data = path.read_bytes()
        require(len(data) == end - start, f'{label}: short flash read.')
        return data

    def load(self, uf2, uid):
        self.run('load-verify', [self.picotool, 'load', '-v', str(uf2), '--ser', uid], 30)

    def reboot(self, uid):
        # Normal application only. Never use reboot -u (enables ROM storage).
        self.run('reboot-application', [self.picotool, 'reboot', '-a', '--ser', uid], 10)

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
