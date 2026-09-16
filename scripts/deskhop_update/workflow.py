"""One bounded, journaled upgrade. All device I/O is injected for unit tests."""
from contextlib import nullcontext
import hashlib
from pathlib import Path
import re
import struct
import zlib

from .console import parse_status, parse_verify, validate_help, validate_history
from .platform import DeploymentError, require, write_json


def encoded(build):
    require(isinstance(build, str) and re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', build), 'Invalid firmware build.')
    major, minor = map(int, build.split('.'))
    require(minor < 1000 and major * 1000 + minor + 100 <= 65535, 'Firmware build out of range.')
    return major * 1000 + minor + 100


def validate_profile(profile):
    require(isinstance(profile, dict) and profile.get('target') in ('A', 'B'), 'Profile target must be A or B.')
    require(set(profile.get('uids', {})) == {'A', 'B'}, 'Profile needs both physical flash UIDs.')
    require(all(isinstance(uid, str) and re.fullmatch(r'[0-9A-F]{16}', uid)
                for uid in profile['uids'].values()), 'UIDs must be 16 uppercase hexadecimal digits.')
    require(profile['uids']['A'] != profile['uids']['B'], 'A and B must have different UIDs.')
    require(isinstance(profile.get('port'), str) and re.fullmatch(r'/dev/cu\.[^/]+', profile['port']),
            'Profile needs one explicit macOS callout port.')
    return profile


def check_status(status, profile, candidate=None):
    require(status['peer'] == 'ok' and set(status['boards']) == {'A', 'B'}, 'Both Pico identities must be freshly available.')
    require(status['local'] == profile['target'], 'The selected target must be USB-connected to this Mac; remote bootloader entry is not an upload tunnel.')
    require(status['verification'] == 'available', 'Firmware lacks fresh image verification.')
    for role, board in status['boards'].items():
        require(board['board_id'] == profile['uids'][role], f'{role}: unexpected physical identity.')
        require(len(board['cores']) == 2 and all(c['checkpoints'] is not None and c['age_ms'] is not None and c['age_ms'] <= 500
                    for c in board['cores']), f'{role}: core progress unavailable or stale.')
        if candidate:
            require(board['build'] == candidate['build'] and board['image_crc_at_boot'] == candidate['boot_crc'],
                    f'{role}: executing build/boot checksum differs from candidate.')


def progressing(first, second):
    for role in ('A', 'B'):
        a, b = first['boards'][role], second['boards'][role]
        if a['boot_session'] != b['boot_session'] or b['uptime_ms'] <= a['uptime_ms']:
            return False
        for ac, bc in zip(a['cores'], b['cores']):
            if ac['checkpoints'] is None or bc['checkpoints'] is None:
                return False
            if not 0 < ((bc['checkpoints'] - ac['checkpoints']) & 0xffffffff) < 0x80000000:
                return False
    return True


def backup_version(image):
    require(len(image) == 262144, 'Short firmware backup.')
    magic, version, reserved, crc = struct.unpack_from('<IHHI', image, 258048)
    require(magic == 0xf00d and reserved == 0 and crc == zlib.crc32(image[:258048]),
            'Existing image is invalid; use a separately reviewed recovery procedure.')
    require(version >= 202, 'Older UART firmware needs an explicit two-board migration, not this updater.')
    return version


class Updater:
    def __init__(self, candidate, profile, backend, evidence, *, already_bootloader=False,
                 rollout_timeout=90, progress=print):
        self.candidate, self.profile = candidate, validate_profile(profile)
        self.backend, self.evidence = backend, Path(evidence)
        require(1 <= rollout_timeout <= 300, 'Rollout timeout must be 1..300 seconds.')
        self.already_bootloader, self.rollout_timeout = already_bootloader, rollout_timeout
        self.progress = progress
        self.record = {'schema': 1, 'build': candidate['build'], 'slot_crc': candidate['slot_crc'],
                       'target': profile['target'], 'uids': profile['uids'], 'port': profile['port'],
                       'stage': 'preflight', 'write_started': False, 'reboot_requested': False,
                       'firmware_verified': False, 'input_acceptance': 'pending', 'phases': [], 'statuses': []}

    def journal(self):
        write_json(self.evidence / 'result.json', self.record)

    def stage(self, name):
        self.record['stage'] = name
        self.record['phases'].append({'name': name, 'at_monotonic': self.backend.now()})
        self.journal()
        self.progress(name.replace('_', ' '))

    def status(self, console):
        parsed = parse_status(console.command('status'))
        self.record['statuses'].append(parsed)
        self.journal()
        return parsed

    def candidate_intact(self):
        for ext in ('bin', 'uf2'):
            require(hashlib.sha256(Path(self.candidate[f'{ext}_path']).read_bytes()).hexdigest()
                    == self.candidate[f'{ext}_sha256'], 'Candidate bytes changed; no flash will be attempted.')

    def settled(self, console, before):
        deadline = self.backend.now() + self.rollout_timeout
        prior = None
        while self.backend.now() < deadline:
            status = self.status(console)
            if status['peer'] == 'ok':
                check_status(status, self.profile)
                if all(board['build'] == self.candidate['build']
                       and board['image_crc_at_boot'] == self.candidate['boot_crc']
                       and board['update']['phase'] == 'idle'
                       for board in status['boards'].values()):
                    if before:
                        require(all(status['boards'][role]['boot_session'] != before['boards'][role]['boot_session']
                                    for role in ('A', 'B')), 'Expected new boot sessions on both upgraded Picos.')
                    if prior and progressing(prior, status):
                        return status
                    prior = status
                else:
                    prior = None
                    peer = status['boards']['B' if self.profile['target'] == 'A' else 'A']
                    self.progress(f"peer {peer['build']}: {peer['update']['phase']} {peer['update']['received']}/{peer['update']['total']} bytes")
            else:
                prior = None
            self.backend.pause(0.5)
        raise DeploymentError('Peer rollout did not settle before the deadline; no flash/reboot is retried.')

    def diagnostics(self, console):
        validate_help(console.command('help'))
        prior = self.status(console)
        check_status(prior, self.profile, self.candidate)
        self.backend.pause(0.25)
        current = self.status(console)
        check_status(current, self.profile, self.candidate)
        require(progressing(prior, current), 'Both cores on both boards must advance.')
        history = validate_history(console.command('history 16'), current['boards'])
        verifications = []
        wrong_crc = f"{int(self.candidate['slot_crc'], 16) ^ 1:08x}"
        for crc in (self.candidate['slot_crc'], wrong_crc, self.candidate['slot_crc']):
            raw = console.command(f"verify {self.candidate['build']} {crc}")
            verification = parse_verify(raw, self.candidate['build'], self.candidate['slot_crc'],
                                        self.candidate['boot_crc'], current['boards'], command_crc=crc,
                                        previous=verifications[-1] if verifications else None)
            verifications.append(verification)
            following = self.status(console)
            check_status(following, self.profile, self.candidate)
            require(progressing(current, following), 'A Pico restarted or stopped making progress during verification.')
            current = following
        final_history = validate_history(console.command('history 64'), current['boards'], previous=history)
        self.record.update(verifications=verifications, histories=[history, final_history], firmware_verified=True)
        self.journal()

    def run(self):
        started = self.backend.now()
        before = None
        boot_console = None
        target = self.profile['target']
        uid = self.profile['uids'][target]
        try:
            self.stage('preflight')
            self.candidate_intact()
            baseline = self.backend.health(boot=self.already_bootloader)
            # Match the successful macOS diagnostic's CDC lifetime: retain it
            # through ROM operations and normal reboot, then close before a
            # fresh application console. This alone did not resolve every ROM
            # timeout; the underlying USB/timing issue remains under study.
            # The already-ROM path must not open an application serial port.
            session = nullcontext() if self.already_bootloader else self.backend.console()
            with session as boot_console:
                if not self.already_bootloader:
                    before = self.status(boot_console)
                    check_status(before, self.profile)
                    require(all(b['update']['phase'] == 'idle' for b in before['boards'].values()),
                            'A firmware update/reboot is already active; do not interrupt it.')
                    if all(b['build'] == self.candidate['build'] and b['image_crc_at_boot'] == self.candidate['boot_crc']
                           for b in before['boards'].values()):
                        self.stage('already_current_verifying')
                        self.diagnostics(boot_console)
                        self.backend.health(baseline=baseline)
                        self.record['already_current'] = True
                        self.stage('complete')
                        return self.record
                    require(all(self.candidate['encoded_version'] > encoded(b['build'])
                                for b in before['boards'].values()),
                            'Candidate must be newer than both Picos; same-version changes/downgrades cannot auto-propagate.')
                    require(encoded(before['boards'][target]['build']) >= 204,
                            'Local firmware lacks serial bootloader entry; use the existing shortcut and --already-bootloader.')
                    validate_help(boot_console.command('help'))
                    self.stage('bootloader_requested')
                    boot_console.bootloader(target)  # Exactly once; retained through all ROM operations.
                    self.backend.wait_bootloader(uid, baseline)
                else:
                    self.backend.identity(uid)
                self.stage('backing_up')
                old = self.backend.save('firmware-before', 0x10000000, 0x10040000, uid)
                settings = self.backend.save('settings-before', 0x101ff000, 0x10200000, uid)
                require(len(settings) == 4096, 'Short settings backup; no flash will be attempted.')
                version = backup_version(old)
                if before:
                    require(version == encoded(before['boards'][target]['build']), 'Backed-up image differs from observed executing version.')
                require(version < self.candidate['encoded_version'], 'Candidate must be newer than the backed-up image.')
                self.record['backup'] = {'version': version, 'firmware_sha256': hashlib.sha256(old).hexdigest(),
                                         'settings_sha256': hashlib.sha256(settings).hexdigest()}
                self.backend.health(boot=True, baseline=baseline)
                self.candidate_intact()
                self.stage('flashing')
                self.record['write_started'] = True
                self.journal()  # Persist before a potentially partially successful hardware command.
                self.backend.load(self.candidate['uf2_path'], uid)
                self.stage('readback')
                image = self.backend.save('firmware-after', 0x10000000, 0x10040000, uid)
                saved = self.backend.save('settings-after', 0x101ff000, 0x10200000, uid)
                require(image == self.candidate['image'], 'Independent firmware readback differs; target left in ROM.')
                require(saved == settings, 'Saved settings changed; target left in ROM for investigation.')
                self.record.update(independent_readback=True, settings_unchanged=True)
                self.backend.health(boot=True, baseline=baseline)
                self.stage('reboot_requested')
                self.record['reboot_requested'] = True
                self.journal()
                self.backend.reboot(uid)
            self.backend.wait_port()
            self.backend.health(baseline=baseline)
            self.stage('waiting_for_peer')
            with self.backend.console(total_timeout=self.rollout_timeout + 30) as console:
                self.settled(console, before)
                self.stage('verifying_both')
                self.diagnostics(console)
            self.backend.health(baseline=baseline)
            self.stage('complete')
            return self.record
        except BaseException as exc:
            self.record['error'] = f'{type(exc).__name__}: {exc}'
            self.record['failed_stage'] = self.record['stage']
            self.record['stage'] = 'failed'
            raise
        finally:
            if boot_console is not None:
                # Cleanup ioctls can report ENXIO for the departed USB device.
                # Console.close still closes the fd and preserves the original
                # failure; retain those diagnostics rather than hiding them.
                self.record['bootloader_console_cleanup_errors'] = list(boot_console.cleanup_errors)
            self.record['elapsed_seconds'] = round(self.backend.now() - started, 6)
            self.journal()

    def verify_only(self):
        """A fresh, read-only check after an interrupted run; never repairs/reboots."""
        started = self.backend.now()
        try:
            self.stage('preflight')
            self.candidate_intact()
            baseline = self.backend.health()
            self.stage('verifying_both')
            with self.backend.console() as console:
                self.diagnostics(console)
            self.backend.health(baseline=baseline)
            self.stage('complete')
            return self.record
        except BaseException as exc:
            self.record['error'] = f'{type(exc).__name__}: {exc}'
            self.record['failed_stage'] = self.record['stage']
            self.record['stage'] = 'failed'
            raise
        finally:
            self.record['elapsed_seconds'] = round(self.backend.now() - started, 6)
            self.journal()
