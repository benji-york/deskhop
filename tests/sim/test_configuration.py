"""Configuration edits across two real node libraries and timed protected UART."""
from test_uart_integrity import pump_wire


def config_report(kind, payload):
    body = b'\xaa\x55' + bytes([kind]) + bytes(payload).ljust(8, b'\0')
    assert len(body) == 11
    # Independent polynomial long division over the entire WebHID record.
    remainder = int.from_bytes(body, 'big') << 8
    for bit in range(remainder.bit_length() - 1, 7, -1):
        if remainder & (1 << bit):
            remainder ^= 0x107 << (bit - 8)
    return (body + bytes([remainder])).hex()


def set_value(sim, node, field, value, *, proxy=False):
    payload = bytes([field]) + value.to_bytes(6 if proxy else 7, 'little')
    sim.do(node, 'vendor', config_report(23 if proxy else 21,
                                       bytes([21]) + payload if proxy else payload))
    pump_wire(sim)


def expect_value(sim, node, field, value):
    sim.do(node, 'vendor', config_report(20, bytes([field])))
    sim.advance(2000)
    sim.do(node, 'task', 'process_hid_queue_task')
    sim.expect_report(node, 6, config_report(20, bytes([field]) + value.to_bytes(7, 'little')))


def scenario_configuration_paired_ingress(sim):
    for node in (0, 1):
        sim.do(node, 'host', 1, 0)
        sim.do(node, 'set', 'config_mode', 0, 1)
    set_value(sim, 0, 83, 123)
    set_value(sim, 0, 83, 456, proxy=True)
    sim.expect(0, 'system_timeout', 123)
    sim.expect(1, 'system_timeout', 456)
    for timeout in (7200000000, 281474976710655, 1, 0):
        set_value(sim, 0, 21, timeout, proxy=True)
        expect_value(sim, 1, 21, timeout)
    set_value(sim, 1, 21, 281474976710656)  # Reject the obsolete seventh value byte.
    expect_value(sim, 1, 21, 0)
    for field in (10, 40, 70):
        set_value(sim, 0, field, 3, proxy=True)
        expect_value(sim, 1, field, {10: 0, 40: 1, 70: 10}[field])
    set_value(sim, 0, 14, 16384, proxy=True)
    set_value(sim, 0, 15, 16384, proxy=True)
    expect_value(sim, 1, 14, 16384)
    expect_value(sim, 1, 15, 32767)
    # An invalid SET still travels in an integrity-valid frame and must be
    # rejected semantically by the recipient without changing the sender.
    set_value(sim, 0, 12, 47, proxy=True)
    set_value(sim, 0, 12, 0, proxy=True)
    expect_value(sim, 1, 12, 47)
    set_value(sim, 0, 83, 4294967296, proxy=True)
    sim.expect(0, 'system_timeout', 123)
    sim.expect(1, 'system_timeout', 456)
    for node in (0, 1):
        sim.expect(node, 'reboot', 0)
        for kind in ('erase', 'program', 'reset'):
            sim.check('event_count', node, kind, 0)


SCENARIOS = {'configuration_paired_ingress': scenario_configuration_paired_ingress}
BACKGROUND_FALSE = set(SCENARIOS)
