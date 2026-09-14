#!/usr/bin/env python3
"""LLVM source coverage for six independent native test layers.

Each layer has its own denominator, profile, text/JSON report and source HTML.
Percentages across layers MUST NOT be summed or called whole-firmware coverage.
Python/JavaScript tests, specification models, ARM startup and physical devices
are outside these native C source reports. Requires Clang and LLVM tools.
"""
from __future__ import annotations
import argparse
import html
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ('paired', 'storage', 'hid', 'policies', 'usb_device', 'usb_host')
POLICIES = ('zoom_tracker', 'fw_update', 'screensaver_policy', 'reboot_hotkey', 'config_migration')
METRICS = ('lines', 'branches', 'functions', 'regions')


def llvm_tool(name):
    tool = shutil.which(name)
    if tool:
        return tool
    if shutil.which('xcrun'):
        result = subprocess.run(['xcrun', '--find', name], capture_output=True, text=True, check=True)
        return result.stdout.strip()
    raise SystemExit(f'{name} is required; install LLVM or add its bin directory to PATH')


def module(name, relative_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative_path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def run(command, environment=None):
    subprocess.run(list(map(str, command)), cwd=ROOT, env=environment, check=True)


def compile_native(binary, test, sources, extra=()):
    flags = ['-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fprofile-instr-generate', '-fcoverage-mapping',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    run([os.environ['CC'], *flags, *extra, '-I' + str(ROOT / 'src/include'),
         ROOT / test, *sources, '-o', binary])
    return binary


def build_and_run(name, output, hid_iterations):
    """Return persistent object files and only the production sources to report."""
    output.mkdir(parents=True, exist_ok=True)
    for raw in output.glob('*.profraw'):
        raw.unlink()
    environment = dict(os.environ, LLVM_PROFILE_FILE=str(output / '%p-%m.profraw'))
    if name == 'paired':
        sim = module('coverage_sim_build', 'tests/sim/build.py')
        library = sim.build(output / 'node.so', coverage=True)
        native = sim.build(output / 'native-boundaries', coverage=True,
                           executable=ROOT / 'tests/sim/test_native_boundaries.c')
        run([sys.executable, ROOT / 'tests/sim/run.py', '--library', library, '--known-gaps'], environment)
        run([native], environment)
        sources = [ROOT / f'src/{source}.c' for source in sim.SOURCES]
        sources += [ROOT / 'pico-sdk/src/common/pico_util/queue.c']
        return [library, native], sources
    if name == 'storage':
        storage = module('coverage_storage_build', 'tests/storage/run.py')
        binary = storage.build(output, coverage=True)
        run([binary, '1'], environment)
        return [binary], [ROOT / 'src' / source for source in storage.SOURCES]
    if name == 'hid':
        sources = [ROOT / f'src/{source}.c' for source in
                   ('hid_parser', 'hid_report', 'keyboard', 'usb', 'reboot_hotkey')]
        flags = ['-Wno-unused-parameter', '-Wno-sign-compare',
                 '-I' + str(ROOT / 'tests/hid_stubs'),
                 '-I' + str(ROOT / 'pico-sdk/lib/tinyusb/src')]
        regression = compile_native(output / 'hid-regression', 'tests/test_hid_regressions.c', sources, flags)
        properties = compile_native(output / 'hid-properties', 'tests/hid_fuzz.c', sources, flags)
        run([regression], environment)
        run([properties, '--seed', '0x484944', '--iterations', hid_iterations], environment)
        return [regression, properties], sources
    if name == 'policies':
        sources = [ROOT / f'src/{policy}.c' for policy in POLICIES]
        objects = []
        for policy, source in zip(POLICIES, sources):
            binary = compile_native(output / policy, f'tests/test_{policy}.c', [source])
            run([binary], environment)
            objects.append(binary)
        contract = compile_native(output / 'policy-contract', 'tests/model/policy_contract.c',
                                  [ROOT / 'src/fw_update.c'])
        run([contract], environment)
        return [*objects, contract], sources
    stack = module('coverage_' + name + '_build',
                   'tests/usb_stack/run.py' if name == 'usb_device' else 'tests/usb_host/run.py')
    binary = stack.build(output, coverage=True)
    run([binary], environment)
    sources = [Path(source) for source in stack.SOURCES if 'tests' not in Path(source).relative_to(ROOT).parts]
    return [binary], sources


def group_summary(files):
    result = {'mapped_source_files': len(files)}
    for metric in METRICS:
        count = sum(file['summary'][metric]['count'] for file in files)
        covered = sum(file['summary'][metric]['covered'] for file in files)
        result[metric] = {'covered': covered, 'count': count,
                          'percent': 100 * covered / count if count else None}
    return result


def export_layer(name, output, objects, sources, tools):
    raw = sorted(output.glob('*.profraw'))
    if not raw:
        raise RuntimeError(f'{name}: no LLVM execution profiles were emitted')
    profile = output / 'combined.profdata'
    run([tools['profdata'], 'merge', '-sparse', *raw, '-o', profile])
    # Every executable participates: mapping present only in a native executable
    # must not be omitted merely because a shared library was listed first.
    args = [str(objects[0]), '-instr-profile=' + str(profile)]
    for obj in objects[1:]:
        args += ['-object', str(obj)]
    args += list(map(str, sources))
    report = subprocess.run([tools['cov'], 'report', *args], capture_output=True, text=True, check=True).stdout
    (output / 'report.txt').write_text(report)
    exported = subprocess.run([tools['cov'], 'export', '-summary-only', *args],
                              capture_output=True, text=True, check=True).stdout
    data = json.loads(exported)['data'][0]
    (output / 'llvm-summary.json').write_text(exported)
    run([tools['cov'], 'show', '-format=html', '-output-dir=' + str(output / 'html'), *args])
    allowed = {str(source.resolve()) for source in sources}
    files = data['files']
    unexpected = {file['filename'] for file in files} - allowed
    if unexpected:
        raise RuntimeError(f'{name}: LLVM exported unexpected denominator files: {unexpected}')
    app_files = [file for file in files if Path(file['filename']).parent == ROOT / 'src']
    sdk_files = [file for file in files if file not in app_files]
    summary = {'layer': name,
               'denominator': 'LLVM mapped regions in the explicitly listed production C files for this layer only',
               'objects': [str(obj.relative_to(output)) for obj in objects],
               'requested_sources': [str(source.relative_to(ROOT)) for source in sources],
               'mapped_sources': [str(Path(file['filename']).relative_to(ROOT)) for file in files],
               'groups': {'application': group_summary(app_files)}}
    if sdk_files:
        summary['groups']['sdk_stack'] = group_summary(sdk_files)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def metric_text(metric):
    if metric['percent'] is None:
        return 'n/a (0 mapped)'
    return f"{metric['percent']:.2f}% ({metric['covered']}/{metric['count']})"


def publish_index(output, summaries, tools):
    disclaimer = ('Independent layer denominators; percentages overlap and cannot be summed. '
                  'Only listed production C files with LLVM mapping count. Tests, HAL doubles, headers, '
                  'Python/JavaScript, formal specifications, ARM startup and physical hardware are excluded. '
                  'Constants-only files may have no mapped executable lines. This is not whole-firmware coverage.')
    rows = []
    for summary in summaries:
        for group, metrics in summary['groups'].items():
            rows.append((summary['layer'], group, metrics))
    lines = [disclaimer, '', 'Layer / source group | Lines | Branches | Functions']
    for name, group, metrics in rows:
        lines.append(f'{name} / {group} | {metric_text(metrics["lines"])} | '
                     f'{metric_text(metrics["branches"])} | {metric_text(metrics["functions"])}')
    report = '\n'.join(lines) + '\n'
    (output / 'report.txt').write_text(report)
    index = {'schema': 1, 'disclaimer': disclaimer, 'tools': tools, 'layers': summaries}
    (output / 'summary.json').write_text(json.dumps(index, indent=2) + '\n')
    body = ['<!doctype html><meta charset="utf-8"><title>DeskHop native source coverage</title>',
            '<style>body{font:16px system-ui;max-width:1200px;margin:40px auto;padding:0 20px}'
            'table{border-collapse:collapse;width:100%}td,th{border:1px solid #bbb;padding:9px;text-align:left}'
            'p{line-height:1.5}</style><h1>DeskHop native source coverage by layer</h1>',
            '<p>' + html.escape(disclaimer) + '</p>',
            '<table><tr><th>Layer / source group</th><th>Lines</th><th>Branches</th><th>Functions</th></tr>']
    for name, group, metrics in rows:
        body.append(f'<tr><td><a href="../{name}/html/index.html">{name}</a> / {group}</td>' +
                    ''.join('<td>' + html.escape(metric_text(metrics[m])) + '</td>'
                            for m in ('lines', 'branches', 'functions')) + '</tr>')
    body.append('</table><p>Per-layer directories retain binaries, raw profiles, merged profiles, '
                'LLVM JSON, source HTML and exact requested/mapped file lists.</p>')
    (output / 'html').mkdir(exist_ok=True)
    (output / 'html/index.html').write_text('\n'.join(body))
    print('\n' + report)
    print('HTML source coverage:', output / 'html/index.html')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layer', choices=LAYERS, action='append', help='repeat to select layers; default: all six')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tests/coverage')
    parser.add_argument('--hid-iterations', type=int, default=2000)
    args = parser.parse_args()
    if args.hid_iterations < 1:
        parser.error('--hid-iterations must be positive')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ['CC'] = os.environ.get('CC', 'clang')
    tools = {'cov': llvm_tool('llvm-cov'), 'profdata': llvm_tool('llvm-profdata'), 'compiler': os.environ['CC']}
    summaries = []
    for layer in dict.fromkeys(args.layer or LAYERS):
        print('\nCoverage layer:', layer, flush=True)
        objects, sources = build_and_run(layer, output / layer, args.hid_iterations)
        summaries.append(export_layer(layer, output / layer, objects, sources, tools))
    publish_index(output, summaries, tools)


if __name__ == '__main__':
    main()
