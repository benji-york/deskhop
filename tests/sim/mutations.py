#!/usr/bin/env python3
"""Compile broken variants and require specific observable regression failures.

Only temporary copies are changed. Compile/link errors do not count as kills.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
from build import ROOT, build
MUTATIONS=[
 ('stale-remote-click','mouse.c','if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT && !position_changed)',
  'if (false && !CURRENT_BOARD_IS_ACTIVE_OUTPUT && !position_changed)','pointer','host 0 ID 2'),
 ('no-pointer-sync','mouse.c','void sync_pointer_position(device_t *state) {',
  'void sync_pointer_position(device_t *state) { return;','pointer_sync','x[0] on 1'),
 ('drop-critical-release','keyboard.c','state->reboot_requested = true;\n            return false;',
  'state->reboot_requested = false;\n            return false;','critical_queue','reboot[0]'),
 ('jitter-counts-as-activity','tasks.c','queue_mouse_report(report, state);',
  'queue_mouse_report(report, state);\n    record_local_activity(state, BOARD_ROLE);',
  'timed_system_wide_keepawake','direct_valid[0]'),
 ('zoom-no-quiet-period','zoom_tracker.c','now + ZOOM_ASSIST_QUIET_TIME_US',
  'now','zoom_scroll_debt_and_quiet_exit','zoom'),
]
def main():
    with tempfile.TemporaryDirectory(prefix='deskhop-sim-mutants-') as tmp:
        base=pathlib.Path(tmp)
        baseline=build(base/'baseline/node.so')
        for scenario in dict.fromkeys(m[4] for m in MUTATIONS):
            subprocess.run([sys.executable,str(ROOT/'tests/sim/run.py'),'--scenario',scenario,
                '--library',str(baseline)],check=True,capture_output=True,timeout=30)
        for name,filename,old,new,scenario,oracle in MUTATIONS:
            root=base/name;shutil.copytree(ROOT/'src',root/'src')
            shutil.copy2(ROOT/'CMakeLists.txt',root/'CMakeLists.txt')
            file=root/'src'/filename;text=file.read_text()
            assert text.count(old)==1,f'mutation anchor drifted: {name}'
            file.write_text(text.replace(old,new))
            lib=build(root/'node.so',source_root=root)
            result=subprocess.run([sys.executable,str(ROOT/'tests/sim/run.py'),'--scenario',scenario,
                '--library',str(lib),'--artifact-dir',str(ROOT/'build/tests/mutation-traces'/name)],capture_output=True,text=True,timeout=30)
            assert result.returncode!=0,f'SURVIVED {name}'
            assert 'AssertionError' in result.stderr and oracle in result.stderr, result.stderr
            print(f'KILLED {name}: {scenario} observable oracle')
    print(f'paired production mutations: {len(MUTATIONS)}/{len(MUTATIONS)} caught')
if __name__=='__main__':main()
