"""Historical startup-starvation witnesses, executed against unguarded C.

Build and run from the repository root (no hardware/network required)::

The maintained test_fw_startup.py driver compiles a known local historical
commit and calls these cases. This helper must NOT be run against the new
guarded source: its assertions deliberately require the old failure behavior.

Only source core 1 is paused, using the existing scheduler boundary. Source
core 0, both receiver cores, wire serialization, DMA RX, watchdogs, production
queues/parser/negotiation/flash/CRC all keep running. The pause models the
*effect* of TinyUSB's blocking 50+450 ms host enumeration; TinyUSB enumeration
itself is not composed into this paired simulator. The 250 us polling quantum,
zero flash durations and injected motion rates are explicit fixture choices,
not measured hardware timing or proof of the physical failure's cause.

The delayed-advertisement case is an INTERVENTION FIXTURE, not implemented
firmware policy: unlike the other cases it does not manually queue the early
production heartbeat before the source core-1 pause. Its first ordinary
scheduled heartbeat executes after that pause. No protocol implementation is
replaced, and no UART drops, corruption, timeout changes or hidden receiver
state changes are injected. Results distinguish transmitted requests from
requests actually accepted into source history.

The quick matrix checks exact mode transitions and committed-page contents.
--full additionally completes one word-fallback and one intervention transfer,
checking the whole image, full page sequence and untouched settings/source.
--output stores compact reproducible observations, not a multi-million-byte
wire trace; the existing simulator trace remains the observation oracle.
"""
import argparse
import ctypes as C
import itertools
import json
import pathlib
import struct

from fixtures import MOUSE, mouse
from simulator import Simulation
from test_fw_batch import (
    BATCH_BEGIN, CAPS_QUEUED, CAPS_REQ, CAPS_RESP, CONFIG, COUNT, DATA, END,
    HistoryEvent, HistoryWindow, OLD_REQ, OLD_RESP, PAGE, PAGE_REQ, RETRY,
    SIZE, SOURCE, TIMING, WORDS_BEGIN, image,
)
from test_uart_integrity import wire_decode

PAUSE_US = 500000
HEARTBEAT = 12
CASES = {
    'before_caps_quiet': ('before', 0),
    'before_caps_motion_100hz': ('before', 10000),
    'before_caps_motion_500hz': ('before', 2000),
    'after_caps_quiet': ('after', 0),
    'after_caps_motion_100hz': ('after', 10000),
    'after_caps_motion_500hz': ('after', 2000),
    'delayed_advertisement_motion_100hz': ('delayed', 10000),
    'delayed_advertisement_motion_500hz': ('delayed', 2000),
}


def history(s):
    """Read the production source ring without adding diagnostic wire traffic."""
    lib = s.nodes[0]
    lib.diagnostic_history_window.argtypes = [C.c_uint]
    lib.diagnostic_history_window.restype = HistoryWindow
    lib.diagnostic_history_read.argtypes = [C.c_uint64, C.POINTER(HistoryEvent)]
    lib.diagnostic_history_read.restype = C.c_bool
    window = lib.diagnostic_history_window(64)
    assert window.overwritten == 0, 'startup probe flooded source history'
    result = []
    for seq in range(window.first_seq, window.end_seq):
        event = HistoryEvent()
        assert lib.diagnostic_history_read(seq, C.byref(event))
        assert event.seq == seq and event.reserved == 0 and event.time_us <= s.now
        result.append({'seq': event.seq, 'at': event.time_us, 'type': event.type,
                       'phase': event.a, 'mode': event.b, 'value': event.value})
    return result


def source_phases(records):
    return [(row['phase'], row['mode'], row['value']) for row in records
            if row['type'] == SOURCE]


def transmitted(s):
    result = {}
    for event in s.trace:
        if event['kind'] != 'uart_tx':
            continue
        kind, payload = wire_decode(event['data'])
        result.setdefault((event['node'], kind), []).append(
            {'at': event['at'], 'payload': payload.hex()})
    return result


def assert_integrity(s, *, full):
    expected = image(205, 0x31)
    assert s.flash(0) == expected, 'source image changed'
    for role in (0, 1):
        assert s.flash(role, CONFIG, 4096) == b'\xa5' * 4096
        s.expect(role, 'stopped', 0)
    programs = [event for event in s.trace
                if event['kind'] == 'program' and event['node'] == 1]
    assert programs and all(event['b'] == PAGE for event in programs)
    assert [event['a'] for event in programs] == list(range(0, len(programs) * PAGE, PAGE))
    committed = len(programs) * PAGE
    assert s.flash(1, 0, committed) == expected[:committed]
    assert not any(event['kind'] in ('program', 'erase', 'reset') and event['node'] == 0
                   for event in s.trace)
    if full:
        s.expect(1, 'reboot', 1)
        s.expect(1, 'fw_dirty', 0)
        assert committed == SIZE and s.flash(1) == expected
    else:
        s.expect(1, 'reboot', 0)
        assert committed >= PAGE
    return committed


def run_case(case, *, seed=1, order=None, library=None, full=False):
    phase, motion_period = CASES[case]
    with Simulation(seed=seed, library=library, core_order=order) as s:
        s.do(0, 'fw_prepare', 205, 0x31)
        s.do(1, 'fw_prepare', 204, 0x72)
        assert s.flash(0) == image(205, 0x31)
        assert s.flash(1) == image(204, 0x72)
        profiled = hasattr(s.nodes[0], 'firmware_source_word_locked')
        for role in (0, 1):
            s.do(role, 'host', 1, 0)
        s.do(1, 'mount', 1, 0, 2, MOUSE.hex())
        if phase != 'delayed':
            # A genuine production heartbeat is admitted before the modeled
            # USB-host pause, as in the physical early-advertisement lead.
            s.do(0, 'task', 'heartbeat_output_task')
        if phase == 'after':
            while not (any(row['type'] == SOURCE and row['phase'] == CAPS_QUEUED
                           for row in history(s)) if profiled
                       else transmitted(s).get((0, CAPS_RESP))):
                assert s.now < 10000, 'capability request was not serviced'
                s.advance(50)

        pause_start = s.now
        s.do(0, 'pause', 1, PAUSE_US)
        pause_end = pause_start + PAUSE_US
        reports = 0
        # Keep the identical noise active for another 100 ms after resumption;
        # successful intervention does not depend on turning traffic off first.
        until = pause_end + 100000
        while s.now < until:
            if motion_period:
                s.do(1, 'report', 1, 0, mouse(x=1 if reports % 2 == 0 else -1))
                reports += 1
            s.advance(min(motion_period or (until - s.now), until - s.now))

        before_full = history(s)
        phases = source_phases(before_full)
        wire = transmitted(s)
        caps_requests = wire.get((1, CAPS_REQ), [])
        caps_responses = wire.get((0, CAPS_RESP), [])
        page_requests = wire.get((1, PAGE_REQ), [])
        words = wire.get((1, OLD_REQ), [])
        assert len(caps_requests) == 1
        tag = struct.unpack_from('<I', bytes.fromhex(caps_requests[0]['payload']))[0]
        assert tag != 0 and tag % 64 == 0
        rx_during_pause = sum(event['kind'] == 'uart_rx' and event['node'] == 0
                              and pause_start <= event['at'] < pause_end for event in s.trace)
        if motion_period:
            assert reports == (PAUSE_US + 100000) // motion_period
            # With no source core-1 RX dispatch during the pause, >1KiB of
            # independently serialized bytes necessarily wraps its DMA ring.
            assert rx_during_pause > 1024
        else:
            assert rx_during_pause < 1024

        if phase == 'delayed':
            if profiled:
                assert phases == [(CAPS_QUEUED, 0, tag), (BATCH_BEGIN, 1, 0)]
            assert not words and page_requests and caps_responses
            advertisements = wire.get((0, HEARTBEAT), [])
            assert advertisements and all(event['at'] >= pause_end for event in advertisements)
            assert caps_requests[0]['at'] >= pause_end
        elif phase == 'before':
            assert not page_requests and words[0]['at'] < pause_end
            if motion_period:
                assert not caps_responses
                if profiled:
                    assert phases == [(WORDS_BEGIN, 2, 0)]
            else:
                assert caps_responses[0]['at'] >= pause_end
                if profiled:
                    assert phases == [(CAPS_QUEUED, 0, tag), (WORDS_BEGIN, 2, 0)]
        else:
            assert len(caps_responses) == 1 and len(page_requests) == 3
            assert caps_responses[0]['at'] < pause_start + 100000
            assert words and words[0]['at'] < pause_end
            decoded = [struct.unpack('<II', bytes.fromhex(event['payload']))
                       for event in page_requests]
            assert all(address == 0 for _, address in decoded)
            assert all(left[0] < right[0] for left, right in zip(decoded, decoded[1:]))
            assert all(event['at'] < pause_end for event in page_requests)
            if motion_period:
                assert not wire.get((0, DATA)) and not wire.get((0, END))
                if profiled:
                    assert phases == [(CAPS_QUEUED, 0, tag), (WORDS_BEGIN, 2, 0)]
            else:
                assert wire.get((0, DATA)), 'quiet old requests must remain serviceable'
                if profiled:
                    assert phases == [(CAPS_QUEUED, 0, tag), (BATCH_BEGIN, 1, 0),
                                      (RETRY, 1, 0), (WORDS_BEGIN, 3, 0)]
        for row in before_full:
            if row['type'] == SOURCE and row['phase'] in (BATCH_BEGIN, WORDS_BEGIN):
                assert row['at'] >= pause_end
        # No wire fault injections were needed; production ring handling and
        # deadlines, rather than a Python protocol substitute, caused fallback.
        assert not any(event['kind'].startswith('fault_') for event in s.trace)
        committed_before_full = assert_integrity(s, full=False)

        if full:
            while not s.get(1, 'reboot'):
                assert s.now < 90000000, 'full startup-probe transfer did not complete'
                s.advance(1000)
            assert_integrity(s, full=True)
            wire = transmitted(s)
            complete_history = history(s)
            counts = {row['phase']: (row['mode'], row['value'])
                      for row in complete_history if row['type'] == COUNT}
            if phase == 'delayed':
                if profiled:
                    assert counts == {1: (1, SIZE // PAGE), 2: (1, 0), 3: (1, 0)}
                assert len(wire.get((1, PAGE_REQ), [])) == SIZE // PAGE
                assert len(wire.get((0, DATA), [])) == SIZE // 4
                assert len(wire.get((0, END), [])) == SIZE // PAGE
                assert not wire.get((1, OLD_REQ)) and not wire.get((0, OLD_RESP))
            else:
                # Source summaries count accepted requests, not bytes delivered.
                # Late duplicate word-zero requests may be answered as well.
                replies = wire.get((0, OLD_RESP), [])
                addresses = [struct.unpack_from('<I', bytes.fromhex(event['payload']))[0]
                             for event in replies]
                assert set(addresses) == set(range(0, SIZE, 4))
                if profiled:
                    assert counts[2] == (2 if motion_period or phase == 'before' else 3,
                                         len(replies))
        else:
            complete_history = before_full

        names = [(1, CAPS_REQ, 'caps_requests'), (0, CAPS_RESP, 'caps_responses'),
                 (1, PAGE_REQ, 'page_requests'), (1, OLD_REQ, 'word_requests'),
                 (0, OLD_RESP, 'word_responses')]
        observed_wire = {name: {'count': len(wire.get((node, kind), [])),
                                'first': wire.get((node, kind), [])[:6],
                                'last': wire.get((node, kind), [])[-1:]}
                         for node, kind, name in names}
        return {'case': case, 'seed': seed, 'core_order': order, 'quantum_us': s.quantum,
                'library_sha256_by_role': s.library_sha256_by_role,
                'pause_start_us': pause_start, 'pause_end_us': pause_end,
                'source_rx_bytes_during_pause': rx_during_pause,
                'motion_reports': reports, 'committed_before_full': committed_before_full,
                'full_image_verified': full, 'virtual_end_us': s.now,
                'source_profile_available': profiled,
                'wire': observed_wire,
                'source_history': [row for row in complete_history
                                   if row['type'] in (SOURCE, TIMING, COUNT)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=pathlib.Path)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--seeds', nargs='+', type=int, default=[1, 2, 3])
    parser.add_argument('--all-orders', action='store_true')
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    cases = args.case or list(CASES)
    orders = list(itertools.permutations(range(4))) if args.all_orders else [None]
    results = []
    for seed in args.seeds:
        for order in orders:
            for case in cases:
                full = args.full and case in ('after_caps_motion_500hz',
                                              'delayed_advertisement_motion_500hz')
                result = run_case(case, seed=seed, order=order, library=args.library, full=full)
                results.append(result)
                print('PASS', case, f'seed={seed}', f'order={order}',
                      f'virtual_us={result["virtual_end_us"]}',
                      f'rx_during_pause={result["source_rx_bytes_during_pause"]}',
                      f'full={full}', flush=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({'schema': 1, 'results': results}, indent=2) + '\n')


if __name__ == '__main__':
    main()
