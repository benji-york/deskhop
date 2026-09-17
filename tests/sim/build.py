#!/usr/bin/env python3
"""Build the production-code node. No SDK download or ARM compiler needed."""
import argparse, os, pathlib, platform, subprocess, re
ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCES = ['defaults', 'constants', 'protocol', 'hid_parser', 'hid_report', 'keyboard',
           'mouse', 'reboot_hotkey', 'screensaver_policy', 'zoom_tracker', 'zoom',
           'tasks', 'handlers', 'led', 'uart', 'usb', 'usb_descriptors', 'utils',
           'fw_update', 'config_migration', 'selection']
OPTIONAL_SOURCES = ['keyboard_sync', 'peer_status', 'diagnostic_peer', 'history', 'diagnostic_history',
                    'peer_history', 'diagnostic_peer_history', 'diagnostic_runtime', 'peer_observation',
                    'verification', 'peer_verify', 'diagnostic_verify', 'diagnostic_transport', 'maintenance',
                    'fw_batch', 'firmware_batch', 'config_confirm', 'clipboard', 'clipboard_state']

def extract_tasks(main):
    """Keep table order and core ownership from the supplied production image."""
    tables = re.findall(r'static\s+task_t\s+tasks_core([01])\s*\[\s*\]\s*=\s*\{(.*?)^\s*\};',
                        main, re.S | re.M)
    assert len(tables) == 2 and {core for core, _ in tables} == {'0', '1'}, \
        'expected one production task table for each core'
    pattern = (r'\[(\d+)\]\s*=\s*\{\s*\.exec\s*=\s*&([a-zA-Z_]\w*)\s*,'
               r'\s*\.frequency\s*=\s*([_A-Z0-9()]+)\s*\}')
    per_core = []
    for core, table in sorted(tables):
        entries = re.findall(pattern, table)
        assert entries and len(entries) == len(re.findall(r'\.exec\s*=', table)), \
            f'unrecognized production task initializer on core {core}'
        entries.sort(key=lambda entry: int(entry[0]))
        assert [int(entry[0]) for entry in entries] == list(range(len(entries))), \
            f'noncontiguous production task indices on core {core}'
        per_core.append([(name, frequency) for _, name, frequency in entries])
    names = [name for entries in per_core for name, _ in entries]
    assert len(names) == len(set(names)), 'task names must be unique for named test scheduling'
    return per_core


def build(output, source_root=ROOT, coverage=False, executable=None, sanitize=False):
    output = pathlib.Path(output).resolve(); output.parent.mkdir(parents=True, exist_ok=True)
    main = (source_root/'src/main.c').read_text()
    per_core = extract_tasks(main)
    entries = [entry for items in per_core for entry in items]
    (output.parent/'task_tables.inc').write_text(
        'enum { SIM_CORE0_TASK_COUNT = %d, SIM_CORE1_TASK_COUNT = %d };\n' % tuple(map(len, per_core))
        + 'static task_t sim_tasks[] = {\n'
        + ''.join('{.exec=&%s,.frequency=%s},\n' % e for e in entries) + '};\n'
        + 'static const char *const sim_task_names[] = {\n'
        + ''.join('"%s",\n' % name for name, _ in entries) + '};\n')
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
    # Historical baseline images predate the diagnostic bridge. Compile the
    # real queue/protocol code only when the selected source tree owns it.
    if (source_root/'src/diagnostic_verify.c').exists():
        flags += ['-DSIM_HAS_DIAGNOSTIC_VERIFY=1']
        if 'bool diagnostic_verify_recheck_local' in (source_root/'src/include/diagnostic_verify.h').read_text():
            flags += ['-DSIM_HAS_VERIFY_RECHECK_PENDING=1']
    if (source_root/'src/diagnostic_runtime.c').exists():
        flags += ['-DSIM_HAS_DIAGNOSTIC_RUNTIME=1']
    if (source_root/'src/diagnostic_peer.c').exists():
        flags += ['-DSIM_HAS_DIAGNOSTIC_PEER=1']
    if (source_root/'src/diagnostic_history.c').exists():
        flags += ['-DSIM_HAS_DIAGNOSTIC_HISTORY=1']
    if (source_root/'src/diagnostic_peer_history.c').exists():
        flags += ['-DSIM_HAS_DIAGNOSTIC_PEER_HISTORY=1']
    if (source_root/'src/keyboard_sync.c').exists():
        flags += ['-DSIM_HAS_KEYBOARD_SYNC=1']
    if 'config_bootloader_local_pending' in (source_root/'src/include/structs.h').read_text():
        flags += ['-DSIM_HAS_CONFIG_BOOTLOADER=1']
    if (source_root/'src/maintenance.c').exists():
        flags += ['-DSIM_HAS_MAINTENANCE=1']
        if 'maintenance_request_config(' in (source_root/'src/include/maintenance.h').read_text():
            flags += ['-DSIM_HAS_MAINTENANCE_CONFIG=1']
    if (source_root/'src/clipboard.c').exists():
        flags += ['-DSIM_HAS_CLIPBOARD=1']
    if (source_root/'src/include/firmware_batch.h').exists():
        flags += ['-DSIM_HAS_FW_BATCH=1']
    if (source_root/'src/include/config_confirm.h').exists():
        flags += ['-DSIM_HAS_CONFIG_CONFIRM=1']
    cmd = [os.environ.get('CC', 'cc'), *flags, *([] if executable else ['-shared']),
           '-I'+str(output.parent), '-I'+str(ROOT/'tests/sim/include'), '-I'+str(source_root/'src/include'),
           '-I'+str(ROOT/'pico-sdk/src/common/pico_util/include'),
           '-I'+str(ROOT/'pico-sdk/lib/tinyusb/src'),
           str(ROOT/'tests/sim/node.c'), *([str(executable)] if executable else []),
           *[str(source_root/f'src/{s}.c') for s in SOURCES
             if s != 'selection' or (source_root/f'src/{s}.c').exists()],
           *[str(source_root/f'src/{s}.c') for s in OPTIONAL_SOURCES
             if (source_root/f'src/{s}.c').exists()],
           str(ROOT/'pico-sdk/src/common/pico_util/queue.c'), '-lm', '-o', str(output)]
    subprocess.run(cmd, check=True)
    return output
if __name__ == '__main__':
    p=argparse.ArgumentParser(); p.add_argument('--output',default=str(ROOT/'build/tests/sim/node.so')); p.add_argument('--coverage',action='store_true')
    a=p.parse_args(); print(build(a.output, coverage=a.coverage))
