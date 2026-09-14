#!/usr/bin/env python3
"""Validate the test apparatus against misleading pass/failure behavior."""
import json
from simulator import Simulation, ROOT
from fixtures import attach,keyboard
from run import replay, minimize
from test_transport import gap_button_aggregation

def main():
    with Simulation() as s:
        attach(s)
        # Each image owns globals and static endpoint state independently.
        s.do(0,'set','x',0,42);s.expect(1,'x',16000)
        s.do(0,'pause',1,1200000);s.advance(1100000);s.expect(0,'stopped',1)
        before=s.get(0,'kbd_queue');s.do(0,'report',1,0,keyboard(1,4));s.expect(0,'kbd_queue',before)
    with Simulation() as s:
        attach(s)
        s.do(0,'checkpoint',0,'nonexistent_action',[])
        s.do(0,'report',1,0,keyboard(1,4))
        try:s.advance(5000)
        except AssertionError as e:assert 'AttributeError' in str(e)
        else:raise AssertionError('ctypes swallowed a callback failure')
    with Simulation() as s:
        attach(s);s.do(0,'pause',0,3000000);s.do(0,'fill',1,256)
        try:s.do(0,'select',1)
        except AssertionError as e:assert '2 seconds' in str(e)
        else:raise AssertionError('blocking wait must report its virtual-time bound')
    path=ROOT/'build/tests/replay-contract.json'
    with Simulation(seed=73) as s:
        attach(s);s.do(1,'report',1,0,'0001020000');s.advance(5000)
        s.expect(0,'x',16001);s.save(path)
        before=s.trace
    data=json.loads(path.read_text());assert replay(data)==before
    # The same desired-property failure must survive ddmin and replay. This
    # verifies minimization itself with a real, known failing production path.
    with Simulation() as s:
        try:gap_button_aggregation(s)
        except AssertionError as e:s.save(path,str(e))
        else:raise AssertionError('known gap changed; replace minimizer contract fixture')
    data=json.loads(path.read_text());small=minimize(data)
    assert len(small['steps'])<len(data['steps'])
    try:replay(small)
    except AssertionError as e:assert str(e)==data['failure']
    else:raise AssertionError('minimized failure vanished')
    path.with_suffix('.min.json').write_text(json.dumps(small,indent=2)+'\n')
    print(f'harness: isolated globals/reset, callback failures, bounded waits, exact replay, ddmin {len(data["steps"])} -> {len(small["steps"])} steps passed')
if __name__=='__main__':main()
