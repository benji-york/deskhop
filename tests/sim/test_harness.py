#!/usr/bin/env python3
"""Validate the test apparatus against misleading pass/failure behavior."""
import json
import contextlib
import io
import itertools
from unittest.mock import patch
import run as runner
from simulator import Simulation, ROOT
from fixtures import attach,keyboard,mouse
from run import replay, minimize
from test_peer_status import scenario_peer_status_roundtrip
from test_peer_history import scenario_peer_history_roundtrip
from test_uart_integrity import scenario_uart_fault_positions

def check_interleaving_cli_selection():
    # Exercise argparse and dispatch, with only expensive firmware execution
    # replaced. A selected scenario must not silently become backpressure.
    for selected, expected in [('peer_status_roundtrip', 'peer_status_roundtrip'),
                               ('peer_history_roundtrip', 'peer_history_roundtrip'),
                               ('all', 'backpressure')]:
        output = io.StringIO()
        with patch('sys.argv', ['run.py', '--scenario', selected, '--interleavings']), \
             patch.object(runner, 'run_case') as execute, contextlib.redirect_stdout(output):
            runner.main()
        ordinary = len(runner.SCENARIOS) if selected == 'all' else 1
        assert execute.call_count == ordinary + 24
        explored = execute.call_args_list[-24:]
        assert all(call.args[0] == expected for call in explored)
        assert {tuple(call.args[4]) for call in explored} == set(itertools.permutations(range(4)))
        assert f'PASS {expected}: all 24' in output.getvalue()

def check_keyboard_startup_ownership():
    # This harness doesn't execute RP2040 multicore launch. Keep its production
    # startup boundary explicit: init before launch; later core0 announcement
    # may invalidate USB output via its locked generation but not mutate sync.
    setup = (ROOT/'src/setup.c').read_text()
    assert setup.index('keyboard_sync_init(boot_session)') < setup.index('multicore_launch_core1(core1_main)')
    handlers = (ROOT/'src/handlers.c').read_text()
    announcement = handlers.split('void announce_initial_output(', 1)[1].split('\n}', 1)[0]
    assert 'keyboard_host_reset(state)' in announcement
    for forbidden in ('keyboard_sync_', 'keyboard_focus_changed(', 'release_all_keys('):
        assert forbidden not in announcement


def main():
    check_keyboard_startup_ownership()
    check_interleaving_cli_selection()
    with Simulation(background=False) as s:
        # The new core0 entry must not silently shift manual core1 polls onto
        # the wrong task or mark their callbacks as running on the wrong core.
        for name,core in [('process_mouse_queue_task',0),('process_uart_tx_task',0),
                          ('diagnostic_console_task',0),('usb_host_task',1),
                          ('packet_receiver_task',1),('heartbeat_output_task',1),
                          ('diagnostic_peer_status_task',1),('keyboard_sync_task',1)]:
            task=s.task_id(0,name)
            assert s.nodes[0].sim_task_name(task).decode('ascii')==name
            assert s.nodes[0].sim_task_core(task)==core
        assert sum(s.nodes[0].sim_task_core(task) == 1
                   for task in range(s.nodes[0].sim_task_count())) == 10
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
    with Simulation(seed=73, background=False) as s:
        scenario_uart_fault_positions(s)
        s.save(path)
        before=s.trace
        assert {'fault_xor','fault_bits','fault_delete_byte','fault_duplicate_byte','fault_delay_byte'} <= {event['kind'] for event in before}
    data=json.loads(path.read_text());assert replay(data)==before
    with Simulation(seed=73) as s:
        scenario_peer_history_roundtrip(s)
        s.save(path)
        before=s.trace
        assert {event['core'] for event in s.trace if event['kind']=='history_request'} == {0}
        assert {event['core'] for event in s.trace if event['kind']=='diagnostic_enqueue'} == {1}
    data=json.loads(path.read_text());assert replay(data)==before
    with Simulation(seed=73) as s:
        scenario_peer_status_roundtrip(s)
        s.save(path)
        before=s.trace
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
    print(f'harness: CLI interleaving selection, named task/core mapping, isolated globals/reset, callback failures, bounded waits, exact replay, ddmin {len(data["steps"])} -> {len(small["steps"])} steps passed')
if __name__=='__main__':main()
