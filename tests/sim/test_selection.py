"""Desired selection convergence properties; independently encoded wire inputs.

Recovery assumes two upgraded peers, recurring heartbeat polls and eventual
packet delivery. Reset fixtures cover joining a live peer, not arbitrary stale
traffic across unlimited resets. Counter ordering assumes less than 2**31
outstanding selection generations.
"""
import argparse
import pathlib
from fixtures import attach, keyboard
from test_transport import frame


def selection_frame(output, counter, origin, *, joined=True, joining=False, pending=False, kind=32):
    flags = origin | (0x80 if joined else 0) | (0x20 if joining else 0) | (0x40 if pending else 0)
    return frame(kind, bytes([output, flags, 0x53, 0x31]) + counter.to_bytes(4, 'little'))


def inject(s, node, wire):
    s.do(node, 'raw', wire)
    s.do(node, 'task', 'packet_receiver_task')  # Actual UART receiver/parser/handler.


def isolated_start(s):
    # Lose startup traffic so each firmware instance begins without peer state.
    for node in (0, 1):
        s.do(node, 'fault', {'drop': 1000})
    attach(s)


def scenario_selection_loss(s):
    attach(s)
    s.advance(5000)
    s.do(0, 'fault', {'drop': 1})
    s.do(0, 'select', 1)
    s.advance(1200000)
    for node in (0, 1):
        s.expect(node, 'output', 1)


def scenario_selection_link_recovery(s):
    attach(s)
    for node in (0, 1):
        s.do(node, 'fault', {'drop': 1000})
    s.do(0, 'select', 1)
    s.advance(2200000)
    s.expect(0, 'output', 1)
    s.expect(1, 'output', 0)
    for node in (0, 1):
        s.do(node, 'fault', {'drop': 0})
    s.advance(1100000)
    for node in (0, 1):
        s.expect(node, 'output', 1)


def scenario_selection_reordering(s):
    attach(s)
    for node in (0, 1):
        inject(s, node, selection_frame(1, 200, 0))
        s.expect(node, 'output', 1)
        inject(s, node, selection_frame(0, 199, 1, kind=3))
        s.expect(node, 'output', 1)
    s.advance(1200000)
    for node in (0, 1):
        s.expect(node, 'output', 1)


def scenario_selection_concurrent(s):
    attach(s)
    # Same logical generation: B wins the documented origin tie-break even
    # though its value is A. Neither side may depend on packet arrival order.
    s.do(0, 'select', 1)
    s.do(1, 'select', 0)
    s.advance(1200000)
    for node in (0, 1):
        s.expect(node, 'output', 0)
    for node, order in ((0, (0, 1)), (1, (1, 0))):
        for origin in order:
            inject(s, node, selection_frame(1 - origin, 300, origin, kind=3))
        s.expect(node, 'output', 0)


def scenario_selection_duplicate_keeps_keys(s):
    attach(s)
    wire = selection_frame(0, 100, 1)
    inject(s, 0, wire)
    s.do(0, 'report', 1, 0, keyboard(2, 4))
    s.advance(5000)
    s.expect_report(0, 1, keyboard(2, 4))
    count = len(s.reports(0, 1))
    for _ in range(3):
        inject(s, 0, wire)
    inject(s, 0, selection_frame(0, 101, 1))
    s.advance(1200000)
    s.check('usb_count', 0, 1, count)
    s.expect_report(0, 1, keyboard(2, 4))


def scenario_selection_passive_join(s):
    isolated_start(s)
    # Model a newly booted Pico receiving a surviving peer's established state.
    inject(s, 0, selection_frame(1, 100, 1))
    s.expect(0, 'output', 1)
    # A delayed empty startup announcement must not undo that established state.
    inject(s, 0, selection_frame(0, 0, 0, joined=False, kind=3))
    s.expect(0, 'output', 1)


def scenario_selection_prejoin_intent(s):
    isolated_start(s)
    s.do(0, 'select', 1)
    s.expect(0, 'output', 1)
    # The user selected B before learning the live peer's larger generation.
    # Preserve that intent and rebase it once above the established token.
    established = selection_frame(0, 100, 1)
    inject(s, 0, established)
    s.expect(0, 'output', 1)
    inject(s, 1, established)
    for node in (0, 1):
        s.do(node, 'fault', {'drop': 0})
    s.advance(1200000)
    for node in (0, 1):
        s.expect(node, 'output', 1)


def scenario_selection_two_fresh_intents(s):
    isolated_start(s)
    s.do(0, 'select', 1)
    s.do(1, 'select', 0)
    # First contact sees another fresh peer, not an established session. Merge
    # those intents once, without both sides repeatedly rebasing above each other.
    inject(s, 0, selection_frame(0, 1, 1, joined=False, pending=True))
    inject(s, 1, selection_frame(1, 1, 0, joined=False, pending=True))
    for node in (0, 1):
        s.do(node, 'fault', {'drop': 0})
    s.advance(2200000)
    for node in (0, 1):
        s.expect(node, 'output', 0)


def scenario_selection_fresh_asymmetric_join(s):
    isolated_start(s)
    s.do(0, 'select', 1)
    s.do(1, 'select', 0)
    # B sees A first; A's first incoming packet is B's handshake reply. B must
    # identify that reply as JOINING, not as an already established live peer,
    # otherwise A incorrectly rebases its losing concurrent intent above B.
    inject(s, 1, selection_frame(1, 1, 0, joined=False, pending=True))
    s.do(1, 'fault', {'drop': 1})  # Lose B's original selection, deliver its reply.
    s.advance(5000)
    # The queued response is production-encoded, not synthesized by this test.
    s.expect(0, 'output', 0)
    s.do(0, 'fault', {'drop': 0})
    s.advance(2200000)
    for node in (0, 1):
        s.expect(node, 'output', 0)


def scenario_selection_wrap(s):
    isolated_start(s)
    inject(s, 0, selection_frame(1, 0xfffffffe, 1))
    s.expect(0, 'output', 1)
    inject(s, 0, selection_frame(0, 0, 0, joined=False, kind=3))
    s.expect(0, 'output', 1)
    s.do(0, 'select', 0)  # ffffffff
    s.do(0, 'select', 1)  # 00000000
    inject(s, 0, selection_frame(0, 0xffffffff, 1, kind=3))
    s.expect(0, 'output', 1)
    inject(s, 0, selection_frame(0, 1, 1))
    s.expect(0, 'output', 0)


def scenario_selection_legacy_and_invalid(s):
    attach(s)
    inject(s, 0, frame(3, b'\x01'))
    s.expect(0, 'output', 1)
    inject(s, 0, frame(3, b'\x00'))
    s.expect(0, 'output', 0)
    for invalid in (frame(32, b'\x01'),
                    selection_frame(2, 1000, 0),
                    selection_frame(1, 1000, 2),
                    selection_frame(1, 1000, 0, joined=True, pending=True)):
        inject(s, 0, invalid)
        s.expect(0, 'output', 0)


def scenario_selection_queue_retry(s):
    attach(s)
    s.do(0, 'fault', {'drop': 1})
    s.do(0, 'select', 1)
    s.advance(990000)  # Just before the heartbeat scheduled at one second.
    s.do(0, 'pause', 0, 30000)
    s.do(0, 'fill', 1, 256)
    deadline = s.now + 10000
    s.advance(10000)
    # Full-queue periodic sync must return, then retry at the next heartbeat.
    s.check('time_range', deadline, deadline + 1000)
    s.expect(0, 'uart_queue', 256)
    s.expect(1, 'output', 0)
    s.advance(1100000)
    for node in (0, 1):
        s.expect(node, 'output', 1)


def check_legacy_receiver(library, legacy_library):
    """Feed a real newly emitted selection to the previous production receiver.

    The legacy image is supplied by the baseline-build runner. ID32 must be
    ignored, and byte zero of the extended ID3 must retain its old meaning.
    """
    from simulator import Simulation
    with Simulation(library=library) as sim:
        attach(sim)
        sim.do(0, 'select', 1)
        sim.advance(5000)
        emitted = [event['data'] for event in sim.trace
                   if event['kind'] == 'uart_tx' and event['node'] == 0
                   and bytes.fromhex(event['data'])[2] == 3]
        wire = emitted[-1]
    with Simulation(library=legacy_library) as legacy:
        attach(legacy)
        inject(legacy, 1, wire)
        legacy.expect(1, 'output', 1)
        inject(legacy, 1, selection_frame(0, 100, 0))
        legacy.expect(1, 'output', 1)
    print('PASS actual legacy receiver accepts ID3 extension and ignores ID32')


SCENARIOS = {name.removeprefix('scenario_'): value for name, value in list(globals().items())
             if name.startswith('scenario_')}


def main():
    from simulator import Simulation
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', choices=['all', *SCENARIOS], default='all')
    parser.add_argument('--library', type=pathlib.Path)
    parser.add_argument('--legacy-library', type=pathlib.Path)
    args = parser.parse_args()
    for name in SCENARIOS if args.scenario == 'all' else [args.scenario]:
        with Simulation(library=args.library) as sim:
            SCENARIOS[name](sim)
        print('PASS', name)
    if args.legacy_library:
        check_legacy_receiver(args.library, args.legacy_library)


if __name__ == '__main__':
    main()
