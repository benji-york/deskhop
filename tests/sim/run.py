#!/usr/bin/env python3
"""Run paired production firmware scenarios, replay and minimize failures."""
import argparse
import itertools
import json
import pathlib
import sys
from simulator import Simulation, ROOT
from test_transport import SCENARIOS as TRANSPORT, KNOWN_GAPS, scenario_generated
from test_behaviors import SCENARIOS as BEHAVIORS, BACKGROUND_FALSE as BEHAVIOR_BACKGROUND_FALSE
from test_mouse_buttons import SCENARIOS as MOUSE_BUTTONS
from test_mouse_extra import SCENARIOS as MOUSE_EXTRA, BACKGROUND_FALSE as MOUSE_BACKGROUND_FALSE
from test_selection import SCENARIOS as SELECTION
from test_peer_status import SCENARIOS as PEER_STATUS
from test_peer_history import SCENARIOS as PEER_HISTORY
from test_verification import SCENARIOS as VERIFICATION
from test_keyboard_reliability import SCENARIOS as KEYBOARD_RELIABILITY
BACKGROUND_FALSE = BEHAVIOR_BACKGROUND_FALSE | MOUSE_BACKGROUND_FALSE
SCENARIOS={**TRANSPORT,**BEHAVIORS,**MOUSE_BUTTONS,**MOUSE_EXTRA,**SELECTION,**PEER_STATUS,**PEER_HISTORY,**VERIFICATION,**KEYBOARD_RELIABILITY,'generated':scenario_generated}

def run_case(name,seed,library=None,artifact_dir=None,core_order=None,expected_gap=False):
    fn=KNOWN_GAPS[name] if expected_gap else SCENARIOS[name]
    with Simulation(seed,library,background=name not in BACKGROUND_FALSE,core_order=core_order) as s:
        try:fn(s)
        except Exception as exc:
            if artifact_dir:
                path=pathlib.Path(artifact_dir)/f'{name}-{seed}.json';s.save(path,str(exc))
                # Save the scenario too: all assertion-level replay steps remain
                # available; scenario replay also retains auxiliary Python asserts.
                data=json.loads(path.read_text());data['scenario']=name;path.write_text(json.dumps(data,indent=2)+'\n')
            if expected_gap:
                # The desired observable property is the final declarative oracle;
                # any earlier model error is a test failure, not an expected gap.
                if not isinstance(exc,AssertionError) or s.steps[-1]['op'] not in ('expect','expect_report'):raise
                print(f'KNOWN GAP {name}: {exc}');return
            raise
        if expected_gap:raise AssertionError(f'Unexpected pass: {name}; update the gap and regression inventory')
        print(f'PASS {name} seed={seed} virtual_us={s.now} scheduled={s.schedule_count}')

def replay(data,library=None,steps=None):
    with Simulation(data['seed'],library,quantum=data.get('quantum',250),
                    background=data.get('background',True),core_order=data.get('core_order')) as s:
        s.replay(data['steps'] if steps is None else steps)
        return s.trace

def minimize(data,library=None):
    """ddmin the input actions while retaining all explicit property assertions.

    Only the same failing assertion is accepted, never loader errors/timeouts.
    The result is 1-minimal by action deletion, not shortest over all values/time.
    """
    original=data['steps'];signature=data.get('failure','')
    if not signature:raise ValueError('minimization needs a failing replay artifact')
    def fails(steps):
        try:replay(data,library,steps)
        except AssertionError as exc:return str(exc)==signature
        return False
    if not fails(original):raise ValueError('recorded failure does not replay with this firmware')
    kept=list(range(len(original)));width=max(1,len(kept)//2)
    while width:
        removed=False
        for start in range(0,len(kept),width):
            chunk=kept[start:start+width]
            candidates=[i for i in chunk if original[i]['op'] not in ('expect','expect_report','check')]
            if not candidates:continue
            candidate=[i for i in kept if i not in candidates]
            if fails([original[i] for i in candidate]):kept=candidate;removed=True;break
        if not removed:width//=2
    result=dict(data,steps=[original[i] for i in kept],trace=[])
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--scenario',choices=['all',*SCENARIOS],default='all')
    p.add_argument('--seed',type=int,default=1);p.add_argument('--seeds',type=int,default=1)
    p.add_argument('--library',type=pathlib.Path);p.add_argument('--artifact-dir',type=pathlib.Path,default=ROOT/'build/tests/failures')
    p.add_argument('--known-gaps',action='store_true');p.add_argument('--interleavings',action='store_true')
    p.add_argument('--replay',type=pathlib.Path);p.add_argument('--minimize',type=pathlib.Path)
    a=p.parse_args()
    if a.seeds<1:p.error('--seeds must be positive')
    if a.replay:replay(json.loads(a.replay.read_text()),a.library);print('Replay passed');return
    if a.minimize:
        result=minimize(json.loads(a.minimize.read_text()),a.library)
        dest=a.minimize.with_suffix('.min.json');dest.write_text(json.dumps(result,indent=2)+'\n');print(dest);return
    names=SCENARIOS if a.scenario=='all' else [a.scenario]
    for seed in range(a.seed,a.seed+a.seeds):
        for name in names:run_case(name,seed,a.library,a.artifact_dir)
    if a.known_gaps:
        for name in KNOWN_GAPS:run_case(name,a.seed,a.library,a.artifact_dir,expected_gap=True)
    if a.interleavings:
        interleaving_scenario = 'backpressure' if a.scenario == 'all' else a.scenario
        for order in itertools.permutations(range(4)):
            run_case(interleaving_scenario,a.seed,a.library,a.artifact_dir,list(order))
        print(f'PASS {interleaving_scenario}: all 24 fixed simultaneous core-priority orders at the modeled boundaries')
if __name__=='__main__':main()
