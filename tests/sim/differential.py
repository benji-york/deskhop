#!/usr/bin/env python3
"""Finite preservation contracts against the original main baseline.

All nine scenarios execute their independent endpoint/state/deadline assertions
on both builds. Four additionally require identical timestamped USB/LED/reset/
wake traces. Five require identical ordered effects on each separate host,
allowing at most one modeled HID poll (1 ms) of timestamp drift per retained
effect. Only repeated identical keyboard states are removed, starting from the
fixtures' empty keyboard state; a release after held keys is always retained.
Mouse reports, wheel/motion events, LED transitions, wake and reset events are
never collapsed. The latter contract excludes global ordering between separate
hosts and the callback's core tag, not report contents or per-host event order.

UART traffic is excluded because selection/source-state reconciliation changes
that protocol deliberately. Same-image replay still compares complete traces;
new bug-fix scenarios independently assert the newly required behavior.
These are stated finite contracts, not universal firmware or timing equivalence.
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
from test_selection import check_legacy_receiver
NAMES=('pointer','pointer_sync','uart_faults','backpressure','f24_releases_held_modifiers',
       'reboot_three_completed_taps','led_focus_and_acknowledgement',
       'zoom_scroll_debt_and_quiet_exit','timed_system_wide_keepawake')
EXACT = {'backpressure', 'led_focus_and_acknowledgement',
         'zoom_scroll_debt_and_quiet_exit', 'timed_system_wide_keepawake'}
MAX_EFFECT_DRIFT_US = 1000


def host_state_effects(trace):
    """Preserve each host's effect order; deduplicate only keyboard state."""
    groups = {0: [], 1: []}
    keyboard = {0: '0000000000000000', 1: '0000000000000000'}
    for event in trace:
        node = event['node']
        if event['kind'] == 'usb' and event['a'] == 0 and event['b'] == 1:
            assert len(event['data']) == 16, 'unexpected keyboard report layout'
            if keyboard[node] == event['data']:
                continue
            keyboard[node] = event['data']
        groups[node].append({key: value for key, value in event.items()
                             if key not in ('node', 'core')})
    return groups


def compare_host_state_effects(name, previous, current):
    previous = host_state_effects(previous)
    current = host_state_effects(current)
    worst_drift = 0
    for node in (0, 1):
        left, right = previous[node], current[node]
        assert len(left) == len(right), f'{name}: host {node} effect count changed: {len(left)} -> {len(right)}'
        for index, (before, after) in enumerate(zip(left, right)):
            assert {k: v for k, v in before.items() if k != 'at'} == {
                k: v for k, v in after.items() if k != 'at'
            }, f'{name}: host {node} effect {index} changed: {before} -> {after}'
            drift = abs(before['at'] - after['at'])
            assert drift <= MAX_EFFECT_DRIFT_US, f'{name}: host {node} effect drift {drift} us exceeds 1 ms'
            worst_drift = max(worst_drift, drift)
    return sum(map(len, previous.values())), worst_drift


def check_projection_contract():
    """The comparison must still detect missing key-up and repeated motion."""
    def usb(report_id, data, at=0):
        return {'at': at, 'node': 0, 'core': 0, 'kind': 'usb', 'a': 0,
                'b': report_id, 'data': data}
    empty = usb(1, '0000000000000000')
    held = usb(1, '0200040000000000')
    motion = usb(5, '0001000000000001')
    assert host_state_effects([empty, empty]) == host_state_effects([])
    assert host_state_effects([held, empty]) != host_state_effects([held])
    assert host_state_effects([motion, motion]) != host_state_effects([motion])


def main():
    check_projection_contract()
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
                    SCENARIOS[name](sim)
                    # Selection/source-state reconciliation intentionally adds
                    # UART traffic. Compare the actual host/device effects;
                    # same-image replay separately checks the complete trace.
                    traces.append([event for event in sim.trace
                                   if event['kind'] in ('usb', 'led', 'reset', 'wake')])
            if name in EXACT:
                if traces[0] != traces[1]:
                    mismatch = next(((left, right) for left, right in zip(*traces)
                                     if left != right), ('length', list(map(len, traces))))
                    raise AssertionError(f'exact effect trace changed: {name}: {mismatch}')
                print(f'EXACT effects: {name} ({len(traces[0])} events)')
            else:
                count, drift = compare_host_state_effects(name, *traces)
                print(f'PRESERVED per-host state effects: {name} ({count} effects; max drift {drift} us)')
        check_legacy_receiver(new, old)
    print(f'{len(NAMES)} independent scenario contracts passed on baseline/current; '
          f'{len(EXACT)} exact effect traces and {len(NAMES)-len(EXACT)} per-host state comparisons passed. '
          'UART changes excluded; no universal equivalence claim.')
if __name__=='__main__':main()
