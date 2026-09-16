#!/usr/bin/env python3
"""Prepare, preview, flash, or verify a DeskHop pair; no hardware is the default."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from deskhop_update.artifact import fingerprint_sources, freeze, load_candidate
from deskhop_update.platform import MacBackend, deployment_lock, require, write_json
from deskhop_update.workflow import Updater, VERIFICATION_MODES, validate_profile

ROOT = Path(__file__).resolve().parents[1]
LATEST = ROOT / 'build/updater/latest.json'


def manifest_path(path):
    path = Path(path).resolve()
    document = json.loads(path.read_text())
    if isinstance(document, dict) and document.get('kind') == 'deskhop-candidate-pointer':
        return (path.parent / document['manifest']).resolve()
    return path


def recipe(args):
    return {'schema': 1, 'tier': args.tier, 'build_dir': str(args.build_dir.resolve()),
            'toolchain_dir': str(args.toolchain_dir.resolve()) if args.toolchain_dir else None,
            'python': sys.executable, 'jobs': args.jobs,
            'cmake_options': ['-DDH_CONSOLE=ON', '-DDH_DEBUG=OFF', '-DDH_DEBUG_CDC_FLASH=OFF']}


def run_check(name, argv, directory, env, timeout):
    print(f'{name} …', flush=True)
    started = time.monotonic()
    log = directory / (name.lower().replace(' ', '-') + '.log')
    record = {'name': name, 'command': list(map(str, argv)), 'log': str(log)}
    try:
        with log.open('xb') as output:
            completed = subprocess.run(record['command'], cwd=ROOT, env=env, stdout=output,
                                       stderr=subprocess.STDOUT, timeout=timeout)
        record['returncode'] = completed.returncode
        require(completed.returncode == 0, f'{name} failed: {log}')
        print(f'{name} passed ({time.monotonic() - started:.1f}s)', flush=True)
        return record
    except BaseException as exc:
        record['error'] = f'{type(exc).__name__}: {exc}; log: {log}'
        raise
    finally:
        record['seconds'] = round(time.monotonic() - started, 3)
        write_json(log.with_suffix('.json'), record)


def prepare(args):
    require(args.jobs > 0, 'Build jobs must be positive.')
    for field in ('build_dir', 'release_dir', 'latest', 'toolchain_dir'):
        if getattr(args, field) is not None:
            setattr(args, field, getattr(args, field).resolve())
    args.latest.parent.mkdir(parents=True, exist_ok=True)
    with deployment_lock(ROOT / 'build/updater/prepare'):
        before = fingerprint_sources(ROOT)
        requested = recipe(args)
        if args.latest.exists() and not args.force:
            # Invalid previous evidence is not a cache hit. Never flash it.
            try:
                prior = load_candidate(manifest_path(args.latest))
                proof = prior['validation_record']
                old_recipe = dict(proof.get('recipe', {}))
                sufficient = old_recipe.get('tier') == args.tier or old_recipe.get('tier') == 'deep'
                old_recipe['tier'] = args.tier
                if prior['source_files'] == before and sufficient and old_recipe == requested:
                    print(f"Reusing validated v{prior['build']}: {prior['manifest_path']}")
                    return prior['manifest_path']
            except (ValueError, OSError, KeyError) as exc:
                print(f'Previous candidate cannot be reused: {exc}', file=sys.stderr)
        directory = Path(tempfile.mkdtemp(prefix='prepare-', dir=ROOT / 'build/updater/prepare'))
        env = dict(os.environ)
        if args.toolchain_dir:
            require(args.toolchain_dir.is_dir(), 'TOOLCHAIN_DIR must be an existing directory.')
            env['PATH'] = str(args.toolchain_dir.resolve()) + os.pathsep + env.get('PATH', '')
        commands = [run_check('Tests', [sys.executable, 'tests/run.py', args.tier], directory, env, 1800),
                    run_check('ARM configure', ['cmake', '-S', str(ROOT), '-B', str(args.build_dir),
                              *requested['cmake_options']], directory, env, 120),
                    run_check('ARM build', ['cmake', '--build', str(args.build_dir), '--parallel', str(args.jobs)],
                              directory, env, 600)]
        require(fingerprint_sources(ROOT) == before, 'Sources changed during validation; prepare again after edits finish.')
        artifacts = {ext: hashlib.sha256((args.build_dir / f'deskhop.{ext}').read_bytes()).hexdigest()
                     for ext in ('bin', 'uf2')}
        proof = {'commands': commands, 'source_before': before, 'artifacts': artifacts, 'recipe': requested,
                 'configured_build': configured_build(args.build_dir, directory, env)}
        manifest = freeze(ROOT, args.build_dir, args.release_dir, validation=proof, source_before=before)
        write_json(args.latest, {'kind': 'deskhop-candidate-pointer',
                                'manifest': os.path.relpath(manifest, args.latest.parent)})
        print(f'Prepared: {manifest}')
        return manifest


def configured_build(build_dir, directory, env):
    """Retain actual configuration and tool versions, not only configure argv."""
    cache = (build_dir / 'CMakeCache.txt').read_text()
    values = {}
    for key in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_MAKE_PROGRAM', 'CMAKE_COMMAND'):
        matches = re.findall(r'^' + key + r':[^=]+=(.+)$', cache, re.MULTILINE)
        require(len(matches) == 1, f'Cannot identify configured {key}.')
        command = matches[0]
        result = run_check(key, [command, '--version'], directory, env, 10)
        log = Path(result['log'])
        require(log.stat().st_size <= 32768, 'Unexpected tool-version output size.')
        values[key] = {'path': command, 'version': log.read_text(), 'check': result}
    return {'cmake_cache': cache, 'cmake_cache_sha256': hashlib.sha256(cache.encode()).hexdigest(), 'tools': values}


def read_profile(args):
    profile = json.loads(args.profile.read_text())
    for field in ('port', 'target'):
        if getattr(args, field):
            profile[field] = getattr(args, field)
    return validate_profile(profile)


def describe(candidate, profile, already_bootloader=False, *, verify_only=False,
             verification_mode='normal'):
    target = profile['target']
    print(f"Candidate: v{candidate['build']} | CRC32 {candidate['slot_crc']} | BIN SHA256 {candidate['bin_sha256']}")
    print(f"Manifest: {candidate['manifest_path']}")
    print(f"USB target: {target} ({profile['uids'][target]}), console {profile['port']}")
    print(f'Verification mode: {verification_mode}')
    scans = ('correct/wrong/correct CRC scans on both boards' if verification_mode == 'thorough'
             else 'one fresh correct-CRC scan on both boards')
    if verify_only:
        print(f'Plan: read-only media/identity checks → {scans} + core/history verification.')
        print('No bootloader entry, firmware/settings write, or reboot will be requested.')
        return
    readback = ('extra ROM firmware readback + settings readback' if verification_mode == 'thorough'
                else 'settings readback')
    print('Plan: media/identity checks → ' + ('inspect existing disk-free ROM' if already_bootloader else 'serial bootloader entry')
          + f' → firmware/settings backups → picotool load -v → {readback} → normal reboot\n'
          + f'      → bounded peer propagation → {scans} + core/history verification.')
    print('Already-current images are verified without rewriting. Same-version replacements/downgrades are refused.')


def stage_images(candidate, directory):
    """Private per-run copies sever dependence on mutable build/candidate paths."""
    candidate = dict(candidate)
    for ext in ('bin', 'uf2'):
        data = candidate[f'{ext}_path'].read_bytes()
        require(hashlib.sha256(data).hexdigest() == candidate[f'{ext}_sha256'], 'Candidate changed after validation.')
        path = directory / f'candidate.{ext}'
        with path.open('xb') as output:
            output.write(data)
        path.chmod(0o444)
        candidate[f'{ext}_path'] = path
    return candidate


def operate(args):
    candidate = load_candidate(manifest_path(args.manifest))
    profile = read_profile(args)
    describe(candidate, profile, args.already_bootloader, verify_only=args.command == 'verify',
             verification_mode=args.verification_mode)
    if args.command == 'plan':
        print('Preview only: no USB inspection, serial port, or picotool was opened.')
        return
    require(not (args.command == 'verify' and args.already_bootloader), 'Verify needs running firmware, not ROM.')
    # One lock for this checkout, independent of per-run evidence locations.
    with deployment_lock(ROOT / 'build/updater/deployment'):
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
        prefix = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-')
        evidence = Path(tempfile.mkdtemp(prefix=prefix, dir=args.evidence_dir)).resolve()
        print(f'Evidence: {evidence}', flush=True)
        write_json(evidence / 'invocation.json', {'operation': args.command, 'profile': profile,
                   'manifest': str(candidate['manifest_path']), 'already_bootloader': args.already_bootloader,
                   'verification_mode': args.verification_mode})
        (evidence / 'candidate-manifest.json').write_bytes(candidate['manifest_path'].read_bytes())
        candidate = stage_images(candidate, evidence)
        backend = MacBackend(evidence, profile['port'], args.picotool)
        updater = Updater(candidate, profile, backend, evidence, already_bootloader=args.already_bootloader,
                          rollout_timeout=args.rollout_timeout, verification_mode=args.verification_mode)
        if args.command == 'flash':
            updater.run()
        else:
            updater.verify_only()
        print('Both firmware images and sampled core progress verified. Please check typing, buttons and switching.')
        print(f'Result: {evidence / "result.json"}')


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest='command', required=True)
    prepare_parser = commands.add_parser('prepare', help='test, build, validate and freeze; no USB access')
    prepare_parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/arm-validation')
    prepare_parser.add_argument('--release-dir', type=Path, default=ROOT / 'build/releases')
    prepare_parser.add_argument('--latest', type=Path, default=LATEST)
    prepare_parser.add_argument('--tier', choices=('fast', 'deep'), default='fast')
    prepare_parser.add_argument('--jobs', type=int, default=4)
    prepare_parser.add_argument('--toolchain-dir', type=Path)
    prepare_parser.add_argument('--force', action='store_true', help='rerun tests/build instead of reusing an exact candidate')
    for command, help_text in [('plan', 'print candidate and intended actions; no hardware access'),
                               ('flash', 'perform one verified upgrade (writes hardware)'),
                               ('verify', 'read-only diagnostics of both running Picos; no flash or reboot')]:
        child = commands.add_parser(command, help=help_text)
        child.add_argument('--manifest', type=Path, default=LATEST)
        child.add_argument('--profile', type=Path, default=ROOT / 'config/updater.json')
        child.add_argument('--port', help='explicit /dev/cu.*; never selected by wildcard')
        child.add_argument('--target', choices=('A', 'B'), help='physical Pico connected to this Mac')
        child.add_argument('--picotool', default='picotool')
        child.add_argument('--evidence-dir', type=Path, default=ROOT / 'build/updater/runs')
        child.add_argument('--rollout-timeout', type=float, default=90)
        child.add_argument('--verification-mode', choices=VERIFICATION_MODES, default='normal',
                           help='normal: one fresh both-board CRC check; thorough: negative/repeat checks and extra flash readback during upgrades')
        child.add_argument('--already-bootloader', action='store_true', help='explicit entry for a target already in disk-free ROM')
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        prepare(args) if args.command == 'prepare' else operate(args)
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f'Stopped: {exc}\nNo automatic flash or reboot retry was attempted.', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('Interrupted. Inspect the run journal and actual hardware state before retrying.', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
