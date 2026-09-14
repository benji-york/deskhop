#!/usr/bin/env python3
"""Compare valid-input observable traces with the task's original main baseline.

Finite regression evidence, not a proof of equivalence for all firmware inputs.
Requires the named baseline commit locally; never fetches or changes a checkout.
"""
import argparse
import io
import pathlib
import subprocess
import tarfile
import tempfile
from build import ROOT,build
from simulator import Simulation
from run import SCENARIOS,BACKGROUND_FALSE
NAMES=('pointer','pointer_sync','uart_faults','backpressure','f24_releases_held_modifiers',
       'reboot_three_completed_taps','led_focus_and_acknowledgement',
       'zoom_scroll_debt_and_quiet_exit','timed_system_wide_keepawake')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline',default='d42c930');a=p.parse_args()
    archive=subprocess.run(['git','archive',a.baseline,'src','CMakeLists.txt'],cwd=ROOT,capture_output=True,check=True).stdout
    with tempfile.TemporaryDirectory(prefix='deskhop-baseline-') as tmp:
        baseline=pathlib.Path(tmp)
        # Only regular project files under the explicitly archived paths.
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            for member in tar:
                path=pathlib.PurePosixPath(member.name)
                assert not path.is_absolute() and '..' not in path.parts
                if member.isdir():continue
                assert member.isfile()
                dest=baseline/member.name;dest.parent.mkdir(parents=True,exist_ok=True)
                dest.write_bytes(tar.extractfile(member).read())
        old=build(baseline/'old/node.so',source_root=baseline)
        new=build(baseline/'new/node.so')
        for name in NAMES:
            traces=[]
            for lib in (old,new):
                with Simulation(seed=1,library=lib,background=name not in BACKGROUND_FALSE) as sim:
                    SCENARIOS[name](sim);traces.append(sim.trace)
            assert traces[0]==traces[1],f'observable trace changed: {name}'
            print(f'EQUIVALENT valid-input trace: {name} ({len(traces[0])} events)')
    print(f'{len(NAMES)} baseline/current trace comparisons passed; no universal equivalence claim')
if __name__=='__main__':main()
