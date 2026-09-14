#!/usr/bin/env python3
"""Validate the test apparatus against misleading pass/failure behavior."""
import json
from simulator import Simulation, ROOT
from fixtures import attach,keyboard,mouse
from run import replay, minimize

def main():
    with Simulation(background=False) as s:
        # The new core0 entry must not silently shift manual core1 polls onto
        # the wrong task or mark their callbacks as running on the wrong core.
        for name,core in [('process_mouse_queue_task',0),('process_uart_tx_task',0),
                          ('diagnostic_console_task',0),('usb_host_task',1),
                          ('packet_receiver_task',1),('heartbeat_output_task',1)]:
            task=s.task_id(0,name)
            assert s.nodes[0].sim_task_name(task).decode('ascii')==name
            assert s.nodes[0].sim_task_core(task)==core
        s.do(0,'task','diagnostic_console_task')
        assert s.steps[-1]['args']==['diagnostic_console_task']
        assert not s.trace # CDC is disabled at this simulator boundary.
        try:s.task_id(0,s.nodes[0].sim_task_count())
        except ValueError:pass
        else:raise AssertionError('invalid task index reached the C adapter')
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
        s.expect(0,'x',16001)
        s.check('usb_bytes',0,2,-1,0,'00')
        s.check('usb_bytes',0,2,0,0,'00')
        s.save(path)
        before=s.trace
        try:s.check('usb_bytes',0,2,-1,0,'01')
        except AssertionError as e:assert 'usb_bytes' in str(e)
        else:raise AssertionError('report-byte oracle accepted an incorrect button mask')
    data=json.loads(path.read_text());assert replay(data)==before
    # Deliberately prevent delivery, then require it anyway. Preserve a source
    # position precondition so deleting the physical input cannot manufacture
    # the same final failure. This remains useful after known bugs are fixed.
    with Simulation() as s:
        attach(s)
        s.do(1,'fault',{'drop':100000})
        s.do(1,'report',1,0,mouse(x=13));s.advance(5000)
        s.expect(1,'x',16013)
        try:s.expect(0,'x',16013)
        except AssertionError as e:s.save(path,str(e))
        else:raise AssertionError('a disconnected link unexpectedly delivered input')
    data=json.loads(path.read_text());small=minimize(data)
    assert len(small['steps'])<len(data['steps'])
    try:replay(small)
    except AssertionError as e:assert str(e)==data['failure']
    else:raise AssertionError('minimized failure vanished')
    path.with_suffix('.min.json').write_text(json.dumps(small,indent=2)+'\n')
    print(f'harness: named task/core mapping, isolated globals/reset, callback failures, bounded waits, exact replay, ddmin {len(data["steps"])} -> {len(small["steps"])} steps passed')
if __name__=='__main__':main()
