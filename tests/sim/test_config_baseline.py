#!/usr/bin/env python3
"""Reproduce both configuration API crashes in the accepted v108 source image."""
import io
import json
import subprocess
import tarfile
from build import ROOT, build

PREDECESSOR = '4549ade4bb34084f31825a7cdfd57c20e13f35cd'


def main():
    target = ROOT / 'build/tests/config-baseline'
    source = target / 'source'
    source.mkdir(parents=True, exist_ok=True)
    archive = subprocess.check_output(['git', 'archive', PREDECESSOR, 'src'], cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(archive)) as files:
        files.extractall(source, filter='data')
    binary = build(target / 'config-crashes', source_root=source,
                   executable=ROOT / 'tests/sim/test_config_crashes.c', sanitize=True)
    results = []
    for name, expected in [('border', 'division by zero'), ('identity', 'index 3 out of bounds')]:
        result = subprocess.run([str(binary), name], text=True, capture_output=True)
        (target / f'{name}.log').write_text(result.stdout + result.stderr)
        assert result.returncode != 0, f'{name}: predecessor unexpectedly passed'
        assert expected in result.stderr, f'{name}: expected {expected!r}: {result.stderr}'
        results.append(dict(scenario=name, commit=PREDECESSOR,
                            returncode=result.returncode, diagnostic=expected))
    (target / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Accepted v108: both real API/mouse sanitizer crash witnesses reproduced')


if __name__ == '__main__':
    main()
