"""UART v1 integrity and framing oracles, independent of production helpers.

The fixed wire record is 7e, fifteen bytes encoded as 40|high/low nibble,
then 7f. The decoded bytes are version, length, type, eight payload bytes,
and little-endian IEEE CRC32. CRC is accidental-error integrity, not peer
authentication or replay protection; repeated valid ordinary packets remain
valid and command-specific protocols own their replay semantics.
"""
import argparse
import ctypes as C
import json
import random
import struct
import zlib

from simulator import Simulation, ROOT

assert zlib.crc32(b'123456789') == 0xcbf43926


def encode_body(body):
    """Independent symbol packing, also used to build malformed CRC records."""
    return bytes([0x7e, *(symbol for byte in body
                         for symbol in (0x40 | (byte >> 4), 0x40 | (byte & 15))), 0x7f])


def wire_frame(kind, data=b'', *, version=1, length=8):
    data = bytes(data).ljust(8, b'\0')
    assert len(data) == 8
    body = bytes((version, length, kind)) + data
    return encode_body(body + struct.pack('<I', zlib.crc32(body))).hex()


def wire_decode(raw):
    """Validate one complete production output against the independent spec."""
    raw = bytes.fromhex(raw) if isinstance(raw, str) else bytes(raw)
    assert len(raw) == 32 and raw[0] == 0x7e and raw[-1] == 0x7f, raw.hex()
    assert all(0x40 <= byte <= 0x4f for byte in raw[1:-1]), raw.hex()
    body = bytes(((raw[i] & 15) << 4) | (raw[i+1] & 15) for i in range(1, 31, 2))
    assert body[:2] == b'\x01\x08', body.hex()
    assert struct.unpack_from('<I', body, 11)[0] == zlib.crc32(body[:11]), body.hex()
    return body[2], body[3:11]


def legacy_frame(kind, data=b''):
    """The deployed, unchecked-type XOR wire format, never a new fallback."""
    data = bytes(data).ljust(8, b'\0')
    assert len(data) == 8
    checksum = 0
    for byte in data:
        checksum ^= byte
    return (b'\xaa\x55' + bytes([kind]) + data + bytes([checksum])).hex()


RX = 'packet_receiver_task'
TX = 'process_uart_tx_task'
POINTER = bytes.fromhex('003e003e00000000')


def inject(s, raw, *, node=0, passes=None):
    raw = raw.hex() if isinstance(raw, bytes) else raw
    s.do(node, 'raw', raw)
    if passes is None:
        # Production scans at most 32 junk bytes per turn. Include one turn
        # for a retained prefix; explicit one-dispatch order tests stay exact.
        passes = (len(raw) // 2 + 31) // 32 + 1
    for _ in range(passes):
        s.do(node, 'task', RX)


def sentinel(s, node=0):
    s.do(node, 'set', 'x', 0, 11111)
    s.do(node, 'set', 'y', 0, 22222)
    s.do(node, 'set', 'ss_timeout', 0, 123)


def safe(s, node=0):
    s.expect(node, 'system_timeout', 123)
    for field in ('reboot', 'stopped', 'fw_dirty', 'fw_source'):
        s.expect(node, field, 0)
    for kind in ('erase', 'program', 'reset'):
        s.check('event_count', node, kind, 0)


def unchanged(s, node=0):
    s.expect(node, 'x', 11111)
    s.expect(node, 'y', 22222)
    safe(s, node)


def scenario_uart_type_integrity(s):
    sentinel(s)
    # Baseline witness: the original XOR covers only payload; bit4 in the type
    # changes POINTER_SYNC(26) to WIPE_CONFIG(10) while the XOR stays valid.
    old = bytearray.fromhex(legacy_frame(26, POINTER))
    old[2] ^= 0x10
    assert old.hex() == 'aa550a003e003e0000000000'
    inject(s, bytes(old))
    unchanged(s)
    # The same logical type bit is the low bit of its high-nibble symbol.
    protected = bytearray.fromhex(wire_frame(26, POINTER))
    protected[5] ^= 1
    inject(s, bytes(protected))
    unchanged(s)
    inject(s, wire_frame(26, POINTER))
    s.expect(0, 'x', 15872)
    s.expect(0, 'y', 15872)
    safe(s)


def decoder_accepts(s, raw):
    """Real fixed-size production decoder; expectations come from this file."""
    decoder = s.nodes[0].read_raw_packet
    decoder.argtypes = [C.c_void_p, C.c_void_p]
    decoder.restype = C.c_bool
    raw = bytes.fromhex(raw) if isinstance(raw, str) else bytes(raw)
    assert len(raw) == 32
    encoded = C.create_string_buffer(raw)
    packet = C.create_string_buffer(13)  # packed type1 / payload8 / CRC32
    return bool(decoder(encoded, packet))


REPRESENTATIVE = (
    (26, POINTER),                         # input -> destructive type witness
    (34, bytes.fromhex('01013e023e000000')), # physical mouse state
    (6, b'\x02'),                          # keyboard LEDs
    (21, struct.pack('<BI', 83, 123)),       # configuration SET
    (36, struct.pack('<IB3x', 0x10203040, 2)),# diagnostic request
    (24, struct.pack('<II', 256, 0)),        # update word request
    (12, struct.pack('<HHI', 202, 0xd485, 0x12345678)),
    (42, bytes.fromhex('8877665544332211')),# keyboard typed request
    (43, bytes.fromhex('aa55101a00ff7e7f')),# keyboard envelope fragment
)


def scenario_uart_single_bits(s):
    # Every one of the 256 wire-bit positions, for nine representative frames.
    # Actual receiver dispatch safety accompanies the direct decoder assertion;
    # helper-only acceptance would miss framing/path selection regressions.
    for kind, payload in REPRESENTATIVE:
        original = bytes.fromhex(wire_frame(kind, payload))
        assert decoder_accepts(s, original)
        for byte in range(len(original)):
            for bit in range(8):
                sentinel(s)
                changed = bytearray(original)
                changed[byte] ^= 1 << bit
                assert not decoder_accepts(s, changed), (kind, byte, bit)
                inject(s, bytes(changed))
                unchanged(s)
                s.expect(0, 'uart_queue', 0)
                # A good frame directly following the damaged candidate must
                # survive, including corrupted start, end, version and length.
                inject(s, wire_frame(26, POINTER))
                s.expect(0, 'x', 15872)
                s.expect(0, 'y', 15872)
                safe(s)


def scenario_uart_encoder_oracle(s):
    s.do(0, 'fault', {'drop': 100000})
    encoder = s.nodes[0].write_raw_packet
    encoder.argtypes = [C.c_void_p, C.c_void_p]
    encoder.restype = None
    # All type values representable by the sending API, all payload byte
    # values, and arbitrary delimiter/preamble bytes inside decoded payloads.
    for kind in range(1, 256):
        data = bytes((kind + offset) % 256 for offset in range(8))
        before = sum(e['kind'] == 'uart_tx' and e['node'] == 0 for e in s.trace)
        s.do(0, 'packet', kind, data.hex(), True)
        s.do(0, 'task', TX)
        if kind in (49, 50):
            # Maintenance transactions own their dequeue authorization. A raw
            # queued request/ACK has no live token and must never reach DMA.
            s.check('event_count', 0, 'uart_tx', before)
            s.expect(0, 'uart_queue', 0)
            # Keep full 1..255 encoder coverage independently of that policy,
            # using the real packed type1/payload8/CRC32 encoder directly.
            packet = C.create_string_buffer(bytes([kind]) + data + bytes(4), 13)
            encoded = C.create_string_buffer(32)
            encoder(encoded, packet)
            raw = encoded.raw
        else:
            s.check('event_count', 0, 'uart_tx', before + 1)
            event = next(e for e in reversed(s.trace) if e['kind'] == 'uart_tx')
            raw = bytes.fromhex(event['data'])
        assert raw.hex() == wire_frame(kind, data)
        assert wire_decode(raw) == (kind, data)
        assert b'\xaa\x55' not in raw
        s.advance(100)
    # Source bounds reject oversized payloads and non-byte types before memcpy.
    for kind, payload in ((0, b''), (-1, b''), (256, b''), (26, bytes(9))):
        s.do(0, 'packet', kind, payload.hex(), False)
    s.expect(0, 'uart_queue', 0)


def scenario_uart_framing_resync(s):
    original = bytes.fromhex(wire_frame(26, POINTER))
    recovery = bytes.fromhex(wire_frame(26, struct.pack('<hh4x', 23456, 12345)))
    for cut in range(1, len(original)):
        sentinel(s)
        inject(s, original[:cut])
        unchanged(s)
        inject(s, recovery)
        s.expect(0, 'x', 23456)
        s.expect(0, 'y', 12345)
        safe(s)
    # Single missing/duplicated bytes, including both frame delimiters. A
    # duplicate delimiter may leave an intact original record in the stream.
    for position in range(len(original)):
        for duplicate in (False, True):
            sentinel(s)
            changed = (original[:position] + (original[position:position+1] if duplicate else b'')
                       + original[position if duplicate else position+1:])
            inject(s, changed)
            if not duplicate or position not in (0, 31):
                unchanged(s)
            safe(s)
            inject(s, recovery)
            s.expect(0, 'x', 23456)
            s.expect(0, 'y', 12345)
    # Delayed tail after a newer complete frame cannot resurrect its stale
    # prefix. Fragmented valid frames are accepted when all bytes do arrive.
    for cut in (1, 5, 16, 31):
        sentinel(s)
        inject(s, original[:cut])
        inject(s, recovery)
        inject(s, original[cut:])
        s.expect(0, 'x', 23456)
        s.expect(0, 'y', 12345)
        sentinel(s)
        inject(s, original[:cut])
        # The privacy deadline is absolute from the first observed prefix.
        # A valid tail must arrive before its 50 ms expiry.
        s.advance(49999)
        inject(s, original[cut:])
        s.expect(0, 'x', 15872)
        safe(s)
        sentinel(s)
        inject(s, original[:cut])
        s.advance(50000)
        inject(s, original[cut:])
        unchanged(s)
    # Concatenation dispatches one accepted record per task, preserving order.
    sentinel(s)
    inject(s, original + recovery + original, passes=1)
    s.expect(0, 'x', 15872)
    s.do(0, 'task', RX)
    s.expect(0, 'x', 23456)
    s.do(0, 'task', RX)
    s.expect(0, 'x', 15872)
    # Garbage contains every possible byte, fake starts, legacy preambles and
    # long runs. Feed bounded chunks so this tests framing, not ring overwrite.
    sentinel(s)
    garbage = bytes(range(256)) + b'\xaa\x55\x7e' * 50 + b'\x40' * 100
    for start in range(0, len(garbage), 128):
        inject(s, garbage[start:start+128])
        unchanged(s)
    inject(s, recovery)
    s.expect(0, 'x', 23456)
    safe(s)


def scenario_uart_metadata_and_xor_errors(s):
    sentinel(s)
    # Even a freshly calculated CRC does not authorize unsupported framing
    # versions/lengths. Fixed-size receiver never trusts an encoded allocation.
    for value in range(256):
        for field, accepted in (('version', 1), ('length', 8)):
            if value == accepted:
                continue
            raw = wire_frame(26, POINTER, **{field: value})
            assert not decoder_accepts(s, raw), (field, value)
            inject(s, raw)
            unchanged(s)
    # All pairs of payload bytes with the same bit toggled preserve old XOR.
    # Keep the original CRC to prove this former blind spot is now detected.
    body = bytes((1, 8, 26)) + POINTER
    crc = struct.pack('<I', zlib.crc32(body))
    for left in range(8):
        for right in range(left + 1, 8):
            for bit in range(8):
                changed = bytearray(body)
                changed[3+left] ^= 1 << bit
                changed[3+right] ^= 1 << bit
                raw = encode_body(changed + crc)
                assert not decoder_accepts(s, raw)
                inject(s, raw)
                unchanged(s)
    inject(s, wire_frame(26, POINTER))
    s.expect(0, 'x', 15872)
    safe(s)


def scenario_uart_ring_wrap(s):
    # Begin a frame at every position where its 32 bytes wrap the DMA ring.
    # Drain zero padding before injection, keeping occupancy below one lap.
    cursor = 0
    for offset in range(1024 - 31, 1024):
        sentinel(s)
        padding = (offset - cursor) % 1024
        inject(s, bytes(padding))
        unchanged(s)
        inject(s, wire_frame(26, POINTER))
        s.expect(0, 'x', 15872)
        s.expect(0, 'y', 15872)
        safe(s)
        cursor = (offset + 32) % 1024


def scenario_uart_no_downgrade(s):
    sentinel(s)
    # No old command, including old heartbeat/update discovery and proxy
    # control, may re-enable a weak general receiver before or after v1 input.
    for established in (False, True):
        if established:
            inject(s, wire_frame(26, POINTER))
            sentinel(s)
        for kind in range(1, 49):
            inject(s, legacy_frame(kind, b'\x01'))
            unchanged(s)
        inject(s, legacy_frame(12, struct.pack('<HHI', 65535, 0xd485, 0x12345678)))
        unchanged(s)
        # Repeated malformed protected starts cannot select a legacy mode.
        for prefix in (b'\x7e', b'\x7e\x40', b'\x7e' * 32, bytes(32)):
            inject(s, prefix + bytes.fromhex(legacy_frame(10)))
            unchanged(s)
        inject(s, wire_frame(26, POINTER))
        s.expect(0, 'x', 15872)
        safe(s)


def scenario_uart_queue_integrity(s):
    sentinel(s)
    s.do(0, 'uart_stall', 1)
    s.do(0, 'fill', 1, 256)
    s.do(0, 'packet', 26, POINTER.hex(), False)
    s.expect(0, 'uart_queue', 256)
    corrupt = bytearray.fromhex(wire_frame(26, POINTER))
    corrupt[5] ^= 1
    inject(s, bytes(corrupt))
    unchanged(s)
    s.do(0, 'uart_stall', 0)
    for _ in range(256):
        s.do(0, 'task', TX)
        s.advance(100)
        s.do(1, 'task', RX)
    s.expect(0, 'uart_queue', 0)
    s.do(0, 'packet', 26, POINTER.hex(), True)
    s.do(0, 'task', TX)
    s.advance(100)
    s.do(1, 'task', RX)
    s.expect(1, 'x', 15872)
    s.expect(1, 'y', 15872)
    safe(s)


def scenario_uart_fault_positions(s):
    # These use the timed DMA path, not raw injection. Every wire byte can be
    # selected; multiple flips, deletion, duplication and per-byte delay are
    # replayed through the same serialized input actions as other scenarios.
    faults = [{'xor': 1 << (index % 8), 'xor_at': index} for index in range(32)]
    faults += [
        {'bit_flips': [[5, 0], [23, 0]]},
        {'delete_byte': 15}, {'duplicate_byte': 15},
        {'delay_byte': {'index': 5, 'us': 500}},
    ]
    for fault in faults:
        sentinel(s)
        s.do(1, 'fault', fault)
        s.do(1, 'packet', 26, POINTER.hex(), True)
        s.do(1, 'task', TX)
        s.advance(1000)
        s.do(0, 'task', RX)
        unchanged(s)
        s.do(1, 'packet', 26, POINTER.hex(), True)
        s.do(1, 'task', TX)
        s.advance(1000)
        s.do(0, 'task', RX)
        s.expect(0, 'x', 15872)
        safe(s)


def pump_wire(s, duration=1000, *, update=False):
    end = s.now + duration
    while s.now < end:
        for node in (0, 1):
            s.do(node, 'task', TX)
            s.do(node, 'task', RX)
        if update:
            s.do(1, 'task', 'firmware_upgrade_task')
        s.advance(min(100, end - s.now))


def scenario_uart_fault_replay(s):
    scenario_uart_fault_positions(s)
    # Replay serialized fault positions and their state assertions against a
    # fresh copy of this exact production image, including full event order.
    with Simulation(seed=s.seed, library=s.nodes[0]._name, background=False,
                    core_order=s.core_order) as replayed:
        replayed.replay(s.steps)
        assert replayed.trace == s.trace


def source_word(address):
    return bytes(((i * 73 + i // 127) ^ 0x31) & 255 for i in range(address, address + 4))


def scenario_uart_update_words(s):
    # The source fixture has a deterministic, valid production-sized image.
    # Requests and responses cross both real encoders/receivers and handlers.
    s.do(0, 'verify_prepare')
    for address in (0, 4, 256, 262140, 262144):
        before = len([e for e in s.trace if e['kind'] == 'uart_tx' and e['node'] == 0])
        s.do(1, 'packet', 24, struct.pack('<II', address, 0).hex(), True)
        pump_wire(s)
        replies = [e for e in s.trace if e['kind'] == 'uart_tx' and e['node'] == 0][before:]
        expected = bytes(4) if address == 262144 else source_word(address)
        assert len(replies) == 1
        assert wire_decode(replies[0]['data']) == (25, struct.pack('<I', address) + expected)
        s.expect(1, 'fw_address', 0)  # unsolicited valid data has no pending pull
    # Outside-image and misaligned requests must not read arbitrary memory.
    for address in (1, 262148, 0xffffffff):
        before = sum(e['kind'] == 'uart_tx' and e['node'] == 0 for e in s.trace)
        s.do(1, 'packet', 24, struct.pack('<II', address, 0).hex(), True)
        pump_wire(s)
        s.check('event_count', 0, 'uart_tx', before)


def scenario_uart_update_retry(s):
    # This manual-clock fixture tests established word retry deadlines, not the
    # boot-only advertisement grace (covered by the startup scenarios).
    s.advance(max(0, 1000000 - s.now))
    s.do(0, 'verify_prepare')
    s.do(0, 'task', 'heartbeat_output_task')
    pump_wire(s, 2000)
    s.expect(1, 'fw_source', 1)
    s.expect(1, 'fw_address', 0)
    # Exercise the unchanged word retry path by withholding the optional page
    # capability reply. Wait just short of its bounded negotiation timeout so
    # the CRC fault below still targets the first actual word response.
    if hasattr(s.nodes[1], 'firmware_batch_init'):
        s.do(0, 'fault', {'drop_types': [52], 'drop_type_count': 1})
        pump_wire(s, 99000, update=True)
    # Damage a response CRC after a fully protected source discovery. The
    # pending address must survive until the actual production timeout retry.
    s.do(0, 'fault', {'xor_at': 29, 'xor': 1})
    pump_wire(s, 5000, update=True)
    s.expect(1, 'fw_address', 0)
    s.expect(1, 'fw_dirty', 0)
    s.check('event_count', 1, 'erase', 0)
    s.check('event_count', 1, 'program', 0)
    requests = [wire_decode(e['data']) for e in s.trace if e['kind'] == 'uart_tx' and e['node'] == 1]
    assert sum(kind == 24 and data[:4] == bytes(4) for kind, data in requests) == 1
    deadline = s.now + 200000
    while s.get(1, 'fw_address') < 256 and s.now < deadline:
        pump_wire(s, 100, update=True)
    s.expect(1, 'fw_address', 256)
    # sim_task honors the real 4 kHz task deadline; await its next eligible pass.
    s.advance(250)
    s.do(1, 'task', 'firmware_upgrade_task')
    s.expect(1, 'fw_dirty', 1)
    s.expect(1, 'reboot', 0)
    s.expect(1, 'stopped', 0)
    s.check('event_count', 1, 'erase', 1)
    s.check('event_count', 1, 'program', 1)
    requests = [(e['at'], wire_decode(e['data'])) for e in s.trace
                if e['kind'] == 'uart_tx' and e['node'] == 1]
    zeros = [at for at, (kind, data) in requests if kind == 24 and data[:4] == bytes(4)]
    assert len(zeros) == 2 and zeros[1] - zeros[0] >= 100000
    # Compare the actual programmed page independently with the source fixture.
    flash = (C.c_uint8 * (2 * 1024 * 1024)).in_dll(s.nodes[1], 'sim_flash')
    assert bytes(flash[:256]) == b''.join(source_word(i) for i in range(0, 256, 4))


SCENARIOS = {
    'uart_type_integrity': scenario_uart_type_integrity,
    'uart_single_bits': scenario_uart_single_bits,
    'uart_encoder_oracle': scenario_uart_encoder_oracle,
    'uart_framing_resync': scenario_uart_framing_resync,
    'uart_metadata_and_xor_errors': scenario_uart_metadata_and_xor_errors,
    'uart_ring_wrap': scenario_uart_ring_wrap,
    'uart_no_downgrade': scenario_uart_no_downgrade,
    'uart_queue_integrity': scenario_uart_queue_integrity,
    'uart_fault_positions': scenario_uart_fault_positions,
    'uart_fault_replay': scenario_uart_fault_replay,
    'uart_update_words': scenario_uart_update_words,
    'uart_update_retry': scenario_uart_update_retry,
}
BACKGROUND_FALSE = set(SCENARIOS)


def baseline_witness(library):
    with Simulation(library=library, background=False) as s:
        sentinel(s)
        raw = bytearray.fromhex(legacy_frame(26, POINTER))
        raw[2] ^= 0x10
        inject(s, bytes(raw))
        s.expect(0, 'system_timeout', 300)
        s.check('event_count', 0, 'erase', 1)
        s.save(ROOT / 'build/tests/uart-baseline/type26-to10.json')
    print('BASELINE CONFIRMED: type26->10, one erase, timeout123->300')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library')
    parser.add_argument('--baseline', help='Previously built unmodified production simulator library')
    args = parser.parse_args()
    if args.baseline:
        baseline_witness(args.baseline)
    for name, scenario in SCENARIOS.items():
        with Simulation(library=args.library, background=False) as s:
            try:
                scenario(s)
            except Exception as exc:
                s.save(ROOT / f'build/tests/failures/{name}-1.json', str(exc))
                raise
        print('PASS', name)


if __name__ == '__main__':
    main()
