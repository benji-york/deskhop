"""Host command orchestration tests. Hardware and compiler boundaries are mocked."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import update_firmware as cli
from deskhop_update.platform import DeploymentError


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='deskhop-cli-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.build = self.root / 'build'
        self.build.mkdir()
        self.manifest = self.root / 'candidate.json'
        self.manifest.write_text('{}')
        self.profile = self.root / 'profile.json'
        self.profile.write_text(json.dumps({'target': 'A', 'port': '/dev/cu.test',
            'uids': {'A': 'E6654854574C3E30', 'B': 'E6654854577F2330'}}))
        self.candidate = {'build': '0.137', 'slot_crc': '12345678', 'bin_sha256': 'a' * 64,
                          'manifest_path': self.manifest}
        self.stdout = contextlib.redirect_stdout(io.StringIO())
        self.stdout.__enter__()
        self.addCleanup(self.stdout.__exit__, None, None, None)

    def prepare_args(self, *extra):
        return cli.parser().parse_args(['prepare', '--build-dir', str(self.build),
            '--release-dir', str(self.root / 'releases'), '--latest', str(self.root / 'latest.json'), *extra])

    def test_plan_never_constructs_backend_or_opens_serial(self):
        with patch.object(cli, 'load_candidate', return_value=self.candidate), \
             patch.object(cli, 'MacBackend', side_effect=AssertionError('hardware boundary reached')):
            result = cli.main(['plan', '--manifest', str(self.manifest), '--profile', str(self.profile)])
        self.assertEqual(result, 0)
        self.assertFalse((self.root / 'runs').exists())

    def test_plan_still_rejects_unknown_target_identity(self):
        self.profile.write_text('{"target":"A","port":"/dev/cu.test","uids":{}}')
        with patch.object(cli, 'load_candidate', return_value=self.candidate), patch.object(cli, 'MacBackend') as backend, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cli.main(['plan', '--manifest', str(self.manifest), '--profile', str(self.profile)]), 1)
            backend.assert_not_called()

    def test_pointer_resolves_relative_to_pointer_not_cwd(self):
        pointer = self.root / 'nested/latest.json'
        pointer.parent.mkdir()
        pointer.write_text('{"kind":"deskhop-candidate-pointer","manifest":"../candidate.json"}')
        self.assertEqual(cli.manifest_path(pointer), self.manifest)

    def test_stage_images_copies_and_detects_changes(self):
        candidate = dict(self.candidate)
        for ext in ('bin', 'uf2'):
            path = self.root / f'original.{ext}'
            path.write_bytes(ext.encode())
            candidate[f'{ext}_path'] = path
            candidate[f'{ext}_sha256'] = hashlib.sha256(ext.encode()).hexdigest()
        directory = self.root / 'evidence'
        directory.mkdir()
        staged = cli.stage_images(candidate, directory)
        self.assertNotEqual(staged['uf2_path'], candidate['uf2_path'])
        self.assertEqual(staged['uf2_path'].read_bytes(), b'uf2')
        candidate['bin_path'].write_bytes(b'changed')
        with self.assertRaisesRegex(DeploymentError, 'changed'):
            cli.stage_images(candidate, self.root)

    def test_prepare_executes_tests_configure_build_and_binds_evidence(self):
        args = self.prepare_args()
        for ext in ('bin', 'uf2'):
            (self.build / f'deskhop.{ext}').write_bytes(ext.encode())
        commands = []

        def check(name, argv, *_args):
            commands.append((name, argv))
            return {'command': list(map(str, argv)), 'returncode': 0}

        with patch.object(cli, 'ROOT', self.root), patch.object(cli, 'fingerprint_sources', return_value={'src/x.c': 'hash'}), \
             patch.object(cli, 'run_check', side_effect=check), patch.object(cli, 'configured_build', return_value={'cmake_cache': 'actual'}), \
             patch.object(cli, 'freeze', return_value=self.manifest) as freeze:
            cli.prepare(args)
        self.assertEqual([name for name, _ in commands], ['Tests', 'ARM configure', 'ARM build'])
        self.assertIn('-DDH_CONSOLE=ON', commands[1][1])
        proof = freeze.call_args.kwargs['validation']
        self.assertEqual(proof['source_before'], {'src/x.c': 'hash'})
        self.assertEqual(proof['recipe']['tier'], 'fast')
        self.assertEqual(len(proof['commands']), 3)
        self.assertEqual(proof['configured_build'], {'cmake_cache': 'actual'})
        self.assertEqual(cli.manifest_path(args.latest), self.manifest)

    def test_prepare_stops_before_freezing_if_sources_change(self):
        args = self.prepare_args()
        with patch.object(cli, 'ROOT', self.root), \
             patch.object(cli, 'fingerprint_sources', side_effect=[{'x': 'old'}, {'x': 'new'}]), \
             patch.object(cli, 'run_check', return_value={'command': ['test'], 'returncode': 0}), \
             patch.object(cli, 'freeze') as freeze:
            with self.assertRaisesRegex(DeploymentError, 'Sources changed'):
                cli.prepare(args)
            freeze.assert_not_called()
        self.assertFalse(args.latest.exists())

    def test_prepare_reuses_only_exact_validated_recipe(self):
        args = self.prepare_args()
        args.latest.write_text('{}')
        prior = {**self.candidate, 'source_files': {'x': 'same'},
                 'validation_record': {'recipe': cli.recipe(args)}}
        with patch.object(cli, 'ROOT', self.root), patch.object(cli, 'fingerprint_sources', return_value={'x': 'same'}), \
             patch.object(cli, 'load_candidate', return_value=prior), patch.object(cli, 'run_check') as check:
            self.assertEqual(cli.prepare(args), self.manifest)
            check.assert_not_called()

    def test_fast_cache_does_not_satisfy_deep_and_force_skips_cache(self):
        for force in (False, True):
            args = self.prepare_args('--tier', 'deep', *(['--force'] if force else []))
            args.latest.write_text('{}')
            old_recipe = {**cli.recipe(args), 'tier': 'fast'}
            prior = {**self.candidate, 'source_files': {'x': 'same'}, 'validation_record': {'recipe': old_recipe}}
            with patch.object(cli, 'ROOT', self.root), patch.object(cli, 'fingerprint_sources', return_value={'x': 'same'}), \
                 patch.object(cli, 'load_candidate', return_value=prior), \
                 patch.object(cli, 'run_check', side_effect=RuntimeError('fresh tests invoked')):
                with self.assertRaisesRegex(RuntimeError, 'fresh tests invoked'):
                    cli.prepare(args)

    def test_failed_check_has_retained_log_and_no_retry(self):
        with patch.object(cli.subprocess, 'run', return_value=subprocess.CompletedProcess(['x'], 2)) as run:
            with self.assertRaisesRegex(DeploymentError, 'Tests failed'):
                cli.run_check('Tests', ['x'], self.root, {}, 2)
            self.assertEqual(run.call_count, 1)
        self.assertEqual(json.loads((self.root / 'tests.json').read_text())['returncode'], 2)

    def test_configured_build_requires_and_records_actual_tools(self):
        keys = ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_MAKE_PROGRAM', 'CMAKE_COMMAND')
        cache = ''.join(f'{key}:STRING=/tools/{key}\n' for key in keys)
        (self.build / 'CMakeCache.txt').write_text(cache)
        log = self.root / 'version.log'
        log.write_text('tool version 1.2.3')
        with patch.object(cli, 'run_check', return_value={'log': str(log), 'returncode': 0}) as check:
            proof = cli.configured_build(self.build, self.root, {})
        self.assertEqual(proof['cmake_cache'], cache)
        self.assertEqual(check.call_count, 4)
        self.assertEqual(proof['tools']['CMAKE_C_COMPILER']['version'], 'tool version 1.2.3')
        (self.build / 'CMakeCache.txt').write_text('')
        with self.assertRaisesRegex(DeploymentError, 'Cannot identify'):
            cli.configured_build(self.build, self.root, {})

    def test_make_defaults_to_help_and_frozen_override_skips_prepare(self):
        repo = Path(__file__).resolve().parents[2]
        default = subprocess.check_output(['make', '-n'], cwd=repo, text=True)
        self.assertNotIn('update_firmware.py flash', default)
        frozen = subprocess.check_output(['make', '-n', 'flash', 'MANIFEST=/tmp/explicit.json'], cwd=repo, text=True)
        self.assertIn('update_firmware.py flash', frozen)
        self.assertNotIn('update_firmware.py prepare', frozen)
        normal = subprocess.check_output(['make', '-n', 'flash'], cwd=repo, text=True)
        self.assertLess(normal.index('update_firmware.py prepare'), normal.index('update_firmware.py flash'))


if __name__ == '__main__':
    unittest.main()
