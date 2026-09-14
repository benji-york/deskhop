#!/usr/bin/env python3
"""Build the production-code node. No SDK download or ARM compiler needed."""
import argparse, os, pathlib, platform, subprocess, re
ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCES = ['defaults', 'constants', 'protocol', 'hid_parser', 'hid_report', 'keyboard',
           'mouse', 'reboot_hotkey', 'screensaver_policy', 'zoom_tracker', 'zoom',
           'tasks', 'handlers', 'led', 'uart', 'usb', 'usb_descriptors', 'utils',
           'fw_update', 'config_migration']
def build(output, source_root=ROOT, coverage=False, executable=None, sanitize=False):
    output = pathlib.Path(output).resolve(); output.parent.mkdir(parents=True, exist_ok=True)
    main = (source_root/'src/main.c').read_text()
    tables = re.findall(r'static task_t tasks_core[01]\[\] = \{(.*?)\n    \};', main, re.S)
    pattern = r'\.exec = &([a-z_]+),\s*\.frequency = ([_A-Z0-9()]+)'
    per_core = [re.findall(pattern, table) for table in tables]
    assert [len(items) for items in per_core] == [6, 8], 'production core task grouping changed; update the native adapter'
    entries = [entry for items in per_core for entry in items]
    assert len(entries) == 14, 'production task table changed; update simulator core mapping'
    (output.parent/'task_tables.inc').write_text('static task_t sim_tasks[] = {\n'+''.join('{.exec=&%s,.frequency=%s},\n' % e for e in entries)+'};\n')
    flags = ['-std=c11', '-g', '-O1', '-fPIC', '-fno-common', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-pointer-to-int-cast',
             '-Wno-incompatible-function-pointer-types', '-Wno-deprecated-non-prototype']
    compiler = os.environ.get('CC', 'cc')
    version = subprocess.run([compiler, '--version'], capture_output=True, text=True, check=True).stdout.lower()
    if 'clang' not in version:
        flags.remove('-Wno-incompatible-function-pointer-types')
        flags.remove('-Wno-deprecated-non-prototype')
    if coverage: flags += ['-fprofile-instr-generate', '-fcoverage-mapping']
    if platform.system() != 'Darwin': flags += ['-Wl,-Bsymbolic', '-Wl,-z,defs']
    if sanitize: flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    cmd = [os.environ.get('CC', 'cc'), *flags, *([] if executable else ['-shared']),
           '-I'+str(output.parent), '-I'+str(ROOT/'tests/sim/include'), '-I'+str(source_root/'src/include'),
           '-I'+str(ROOT/'pico-sdk/src/common/pico_util/include'),
           '-I'+str(ROOT/'pico-sdk/lib/tinyusb/src'),
           str(ROOT/'tests/sim/node.c'), *([str(executable)] if executable else []), *[str(source_root/f'src/{s}.c') for s in SOURCES],
           str(ROOT/'pico-sdk/src/common/pico_util/queue.c'), '-lm', '-o', str(output)]
    subprocess.run(cmd, check=True)
    return output
if __name__ == '__main__':
    p=argparse.ArgumentParser(); p.add_argument('--output',default=str(ROOT/'build/tests/sim/node.so')); p.add_argument('--coverage',action='store_true')
    a=p.parse_args(); print(build(a.output, coverage=a.coverage))
