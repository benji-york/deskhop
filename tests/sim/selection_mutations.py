#!/usr/bin/env python3
"""Require runtime regression detection of selection reconciliation mutations."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
from build import ROOT, build

MUTATIONS = (
    ('no-periodic-sync', 'handlers.c', 'void sync_output_selection(device_t *state) {',
     'void sync_output_selection(device_t *state) { return;', 'selection_loss'),
    ('arrival-order-wins', 'selection.c', 'if (!token_newer(incoming, state))',
     'if (false && !token_newer(incoming, state))', 'selection_reordering'),
    ('reverse-concurrent-tie', 'selection.c', 'incoming->origin > current->origin',
     'incoming->origin < current->origin', 'selection_concurrent'),
    ('discard-prejoin-intent', 'selection.c', 'if (state->pending_local) {',
     'if (false && state->pending_local) {', 'selection_prejoin_intent'),
    ('premature-joined-handshake', 'selection.c', 'state->joined = incoming->joining;',
     'state->joined = true;', 'selection_fresh_asymmetric_join'),
    ('boot-default-overwrites-wrap', 'selection.c', 'if (!incoming->joined && !incoming->joining && !incoming->pending_local)',
     'if (false && !incoming->joined && !incoming->joining && !incoming->pending_local)', 'selection_wrap'),
    ('repeat-all-up', 'handlers.c', 'if (changed || legacy) {',
     'if (accepted || changed || legacy) {', 'selection_duplicate_keeps_keys'),
)

# Shared runner format: source, replacement and the specific observable oracle.
SOURCE_MUTATIONS = [(*mutation, 'predicate usb_count' if mutation[0] == 'repeat-all-up'
                    else 'output[0]') for mutation in MUTATIONS]


def execute(library, scenario):
    return subprocess.run([sys.executable, ROOT/'tests/sim/test_selection.py',
                           '--library', library, '--scenario', scenario],
                          capture_output=True, text=True, timeout=30)


def main():
    with tempfile.TemporaryDirectory(prefix='deskhop-selection-mutants-') as directory:
        base = pathlib.Path(directory)
        baseline = build(base/'baseline/node.so')
        for scenario in dict.fromkeys(mutation[4] for mutation in MUTATIONS):
            result = execute(baseline, scenario)
            assert result.returncode == 0, result.stdout + result.stderr
        for name, filename, old, new, scenario in MUTATIONS:
            root = base/name
            shutil.copytree(ROOT/'src', root/'src')
            shutil.copy2(ROOT/'CMakeLists.txt', root/'CMakeLists.txt')
            source = root/'src'/filename
            code = source.read_text()
            assert code.count(old) == 1, name + ': mutation anchor changed'
            source.write_text(code.replace(old, new))
            binary = build(root/'node.so', source_root=root)
            result = execute(binary, scenario)
            assert result.returncode != 0, 'SURVIVED ' + name
            assert 'AssertionError' in result.stderr, result.stdout + result.stderr
            assert ('expected' in result.stderr or 'predicate usb_count' in result.stderr), result.stderr
            print('KILLED', name, 'with', scenario)
    print(f'selection production mutations: {len(MUTATIONS)}/{len(MUTATIONS)} caught by observable regressions')


if __name__ == '__main__':
    main()
