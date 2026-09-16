"""Real one-second firmware-advertisement guard and historical witnesses.

Run from the repository root; no hardware or network is used::

    python3 tests/sim/build.py
    python3 tests/sim/test_fw_startup.py --seeds 1 2 3
    python3 tests/sim/test_fw_startup.py --full --seeds 1

The current production heartbeat task, rather than an intervention fixture,
must suppress only firmware metadata before one second. A 500 ms source-core-1
pause models the scheduling effect of the real TinyUSB host attach delay;
core 0, both receiver cores, DMA, UART, queues and watchdogs remain running.
Actual HID mouse reports provide bounded 100/500 Hz traffic. TinyUSB itself is
not composed into this paired scheduler; its real blocking-delay contract is
checked separately by the host-stack harness. Virtual timings use a declared
250 us polling quantum and zero flash durations, not measured hardware costs.

The known local v0.109 commit below supplies the exact pre-guard production
code, including the same batch protocol as deployed v0.110. It is compiled
from git archive without checkout changes, fetching or Python substitutes.
Its source lacks v0.110's profile hooks, so historical assertions observe
actual transmitted caps/page/word frames and programmed data instead. Both
the historical failure and current fix must pass their distinct contracts.
--full adds complete historical/current/mixed-receiver transfers with exact
image, settings, page sequence and production CRC/reboot assertions.
"""
import argparse
from contextlib import contextmanager
import io
import itertools
import json
import pathlib
import subprocess
import tarfile
import tempfile

from build import build
from fixtures import MOUSE, mouse
from fw_startup_baseline import history, source_phases, transmitted, run_case as old_case
from simulator import ROOT, Simulation
from test_fw_batch import (
    BATCH_BEGIN, CAPS_QUEUED, CAPS_REQ, CAPS_RESP, CONFIG, COUNT, DATA, END,
    OLD_REQ, OLD_RESP, PAGE, PAGE_REQ, SIZE, SOURCE, TIMING, image,
)

BASELINE = '6bf9528030bcb3f69cedd665add55d94a7713f82'
GUARD_US, PAUSE_US, HEARTBEAT = 1000000, 500000, 12
HISTORICAL = ('before_caps_quiet', 'before_caps_motion_100hz',
              'after_caps_quiet', 'after_caps_motion_100hz', 'after_caps_motion_500hz')
# source role, actual motion period, simulated host-attach pause, old receiver
CASES = {
    'guard_quiet': (0, 0, True, False),
    'guard_motion_100hz': (0, 10000, True, False),
    'guard_motion_500hz': (0, 2000, True, False),
    'guard_reverse_motion_500hz': (1, 2000, True, False),
    'guard_no_peripheral': (0, 0, False, False),
    'guard_old_receiver_motion_500hz': (0, 2000, True, True),
    'guard_reverse_old_receiver_motion_500hz': (1, 2000, True, True),
}


@contextmanager
def historical_library():
    archive = subprocess.run(['git', 'archive', BASELINE, 'src', 'CMakeLists.txt'],
                             cwd=ROOT, capture_output=True, check=True).stdout
    with tempfile.TemporaryDirectory(prefix='deskhop-startup-baseline-') as temporary:
        directory = pathlib.Path(temporary)
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            for member in source:
                path = pathlib.PurePosixPath(member.name)
                assert not path.is_absolute() and '..' not in path.parts
                if member.isdir():
                    continue
                assert member.isfile()
                target = directory / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.extractfile(member).read())
        yield build(directory / 'compiled/node.so', source_root=directory)


def source_history(s, source):
    # Reuse the existing layout-checked production-ring reader for either role;
    # it reads s.nodes[0] only. Do not alter the real simulation's node list.
    class View:
        nodes = [s.nodes[source]]
        now = s.now
    return history(View())


def integrity(s, source, *, full):
    receiver = 1 - source
    expected = image(205, 0x31)
    assert s.flash(source) == expected
    for role in (0, 1):
        assert s.flash(role, CONFIG, 4096) == b'\xa5' * 4096
        s.expect(role, 'stopped', 0)
    programs = [event for event in s.trace
                if event['kind'] == 'program' and event['node'] == receiver]
    assert programs and all(event['b'] == PAGE for event in programs)
    assert [event['a'] for event in programs] == list(range(0, len(programs) * PAGE, PAGE))
    committed = len(programs) * PAGE
    assert s.flash(receiver, 0, committed) == expected[:committed]
    assert not any(event['kind'] in ('erase', 'program', 'reset') and event['node'] == source
                   for event in s.trace)
    if full:
        s.expect(receiver, 'reboot', 1)
        s.expect(receiver, 'fw_dirty', 0)
        assert committed == SIZE and s.flash(receiver) == expected
    else:
        assert committed >= 3 * PAGE
        s.expect(receiver, 'reboot', 0)
    return committed


def guarded_case(case, *, old, library=None, seed=1, order=None, full=False):
    source, period, pause, old_receiver = CASES[case]
    receiver = 1 - source
    current = library or ROOT / 'build/tests/sim/node.so'
    libraries = [current, current]
    if old_receiver:
        libraries[receiver] = old
    with Simulation(seed=seed, library=libraries, core_order=order) as s:
        s.do(source, 'fw_prepare', 205, 0x31)
        s.do(receiver, 'fw_prepare', 204, 0x72)
        assert s.flash(source) == image(205, 0x31)
        assert s.flash(receiver) == image(204, 0x72)
        for role in (0, 1):
            s.do(role, 'host', 1, 0)
        if period:
            s.do(receiver, 'mount', 1, 0, 2, MOUSE.hex())
        s.do(source, 'select', source)
        # Execute the genuine initial task passes: the guard, not a fixture
        # suppression, must stop their otherwise-immediate metadata advert.
        s.advance(20000)
        assert not transmitted(s).get((source, HEARTBEAT))
        pause_start, pause_end = s.now, s.now
        if pause:
            pause_end += PAUSE_US
            s.do(source, 'pause', 1, PAUSE_US)
        reports = 0
        while s.now < 1200000:
            if period:
                s.do(receiver, 'report', 1, 0, mouse(x=1 if reports % 2 == 0 else -1))
                reports += 1
            s.advance(min(period or 1000, 1200000 - s.now))
        wire = transmitted(s)
        advertisements = wire.get((source, HEARTBEAT), [])
        assert advertisements and GUARD_US <= advertisements[0]['at'] < GUARD_US + 10000
        assert all(event['at'] >= GUARD_US for event in advertisements)
        caps = wire.get((receiver, CAPS_REQ), [])
        assert len(caps) == 1 and caps[0]['at'] >= GUARD_US
        assert len(wire.get((source, CAPS_RESP), [])) == 1
        assert wire.get((receiver, PAGE_REQ)) and wire.get((source, DATA))
        assert not wire.get((receiver, OLD_REQ)) and not wire.get((source, OLD_RESP))
        phases = source_phases(source_history(s, source))
        assert len(phases) == 2 and phases[0][:2] == (CAPS_QUEUED, 0)
        assert phases[1] == (BATCH_BEGIN, 1, 0)
        rx_in_pause = sum(event['kind'] == 'uart_rx' and event['node'] == source
                          and pause_start <= event['at'] < pause_end for event in s.trace)
        if period and pause:
            assert rx_in_pause > 1024, 'noise must still exercise the pre-transfer RX ring wrap'
        committed = integrity(s, source, full=False)
        if full:
            while not s.get(receiver, 'reboot'):
                assert s.now < 30000000, 'guarded full-image transfer stalled'
                s.advance(1000)
            integrity(s, source, full=True)
            wire = transmitted(s)
            assert len(wire.get((receiver, PAGE_REQ), [])) == SIZE // PAGE
            assert len(wire.get((source, DATA), [])) == SIZE // 4
            assert len(wire.get((source, END), [])) == SIZE // PAGE
            assert not wire.get((receiver, OLD_REQ)) and not wire.get((source, OLD_RESP))
            counts = {row['phase']: (row['mode'], row['value'])
                      for row in source_history(s, source) if row['type'] == COUNT}
            assert counts == {1: (1, SIZE // PAGE), 2: (1, 0), 3: (1, 0)}
        assert not any(event['kind'].startswith('fault_') for event in s.trace)
        return {'case': case, 'seed': seed, 'core_order': order,
                'source': source, 'old_receiver': old_receiver,
                'library_sha256_by_role': s.library_sha256_by_role,
                'quantum_us': s.quantum, 'pause_start_us': pause_start,
                'pause_end_us': pause_end, 'source_rx_bytes_during_pause': rx_in_pause,
                'first_metadata_advert_us': advertisements[0]['at'],
                'motion_reports': reports, 'committed_before_full': committed,
                'full_image_verified': full, 'virtual_end_us': s.now,
                'page_requests': len(wire.get((receiver, PAGE_REQ), [])),
                'word_requests': len(wire.get((receiver, OLD_REQ), [])),
                'source_history': [row for row in source_history(s, source)
                                   if row['type'] in (SOURCE, TIMING, COUNT)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=pathlib.Path)
    parser.add_argument('--seeds', nargs='+', type=int, default=[1, 2, 3])
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--all-orders', action='store_true')
    parser.add_argument('--order', nargs=4, type=int, action='append')
    args = parser.parse_args()
    orders = list(itertools.permutations(range(4))) if args.all_orders else args.order or [None]
    assert all(order is None or sorted(order) == list(range(4)) for order in orders)
    results = []
    with historical_library() as old:
        for seed in args.seeds:
            for order in orders:
                for case in HISTORICAL:
                    full = args.full and case == 'after_caps_motion_500hz'
                    result = old_case(case, seed=seed, order=order, library=old, full=full)
                    result.update(historical_commit=BASELINE, case='baseline_' + case)
                    results.append(result)
                    print('PASS', result['case'], f'seed={seed}', f'order={order}',
                          f'full={full}', flush=True)
                for case in CASES:
                    full = args.full and case in ('guard_motion_500hz',
                                                  'guard_reverse_motion_500hz',
                                                  'guard_old_receiver_motion_500hz')
                    result = guarded_case(case, old=old, library=args.library,
                                          seed=seed, order=order, full=full)
                    results.append(result)
                    print('PASS', case, f'seed={seed}', f'order={order}',
                          f'full={full}', flush=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({'schema': 2, 'results': results}, indent=2) + '\n')


if __name__ == '__main__':
    main()
