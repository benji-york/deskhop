"""Page-burst updater contracts through paired production firmware.

Only fixture images, NOR storage, clocks and peripheral edges are modeled.
Production heartbeat, negotiation, UART queues/DMA/parser, flash commits and
final image validation execute unchanged. Measured virtual timing is a bounded
scheduler/wire model, not an RP2040 or USB benchmark and not proof of physical
reboot. The historical compatibility checks never fetch or change a checkout.
"""
import argparse
import io
import pathlib
import struct
import subprocess
import tarfile
import tempfile
import zlib

from fixtures import attach, keyboard
from simulator import Simulation, ROOT
from test_uart_integrity import wire_decode, wire_frame

CAPS_REQ, CAPS_RESP, PAGE_REQ, DATA, END = 51, 52, 53, 54, 55
OLD_REQ, OLD_RESP = 24, 25
TX, RX, UPGRADE = 'process_uart_tx_task', 'packet_receiver_task', 'firmware_upgrade_task'
SIZE, PAGE, CONFIG = 262144, 256, 2 * 1024 * 1024 - 4096


def image(version, salt):
    raw = bytearray(((i * 73 + i // 127) ^ salt) & 255 for i in range(SIZE))
    struct.pack_into('<IHHI', raw, SIZE - 4096, 0xf00d, version, 0, zlib.crc32(raw[:-4096]))
    return bytes(raw)


def prepare(s, source_version=205, target_version=204):
    s.do(0, 'fw_prepare', source_version, 0x31)
    s.do(1, 'fw_prepare', target_version, 0x72)
    assert s.flash(0) == image(source_version, 0x31)
    assert s.flash(1) == image(target_version, 0x72)


def frames(s, node, kind):
    return [(event, wire_decode(event['data'])[1]) for event in s.trace
            if event['kind'] == 'uart_tx' and event['node'] == node
            and wire_decode(event['data'])[0] == kind]


def pump(s, rounds=1):
    for _ in range(rounds):
        for node in (0, 1): s.do(node, 'task', TX)
        s.advance(100)
        for node in (0, 1):
            s.do(node, 'task', RX)
            s.do(node, 'task', UPGRADE)


def until(s, predicate, timeout=500000, *, background=False):
    end = s.now + timeout
    while not predicate():
        assert s.now < end, f'updater condition timed out at {s.now} us'
        if background: s.advance(min(20000, end - s.now))
        else: pump(s)


def begin_manual(s):
    prepare(s)
    s.do(0, 'task', 'heartbeat_output_task')
    until(s, lambda: bool(frames(s, 0, DATA)))
    assert frames(s, 1, CAPS_REQ) and frames(s, 0, CAPS_RESP)
    assert not frames(s, 1, OLD_REQ)


def assert_page_zero(s):
    assert s.flash(1, 0, PAGE) == image(205, 0x31)[:PAGE]
    programs = [event for event in s.trace if event['node'] == 1 and event['kind'] == 'program']
    assert sum(event['a'] == 0 for event in programs) == 1
    assert s.flash(1, CONFIG, 4096) == b'\xa5' * 4096


def scenario_batch_full_image(s):
    prepare(s)
    attach(s)
    s.do(0, 'select', 1)
    until(s, lambda: bool(frames(s, 0, DATA)), background=True)
    # A full source FIFO must not be populated with a page burst: real input is
    # still delivered promptly while a firmware stream is actively progressing.
    for report in (keyboard(0, 4), keyboard(), keyboard(2, 5), keyboard()):
        started = s.now
        s.do(0, 'report', 1, 0, report)
        until(s, lambda: bool(s.reports(1, 1)) and s.reports(1, 1)[-1]['data'] == report,
              timeout=10000, background=True)
        assert s.now - started <= 10000
    until(s, lambda: bool(s.get(1, 'reboot')), timeout=30000000, background=True)
    assert s.flash(1) == image(205, 0x31), 'full programmed image differs from independent fixture'
    assert s.flash(0) == image(205, 0x31), 'source slot was modified'
    for role in (0, 1): assert s.flash(role, CONFIG, 4096) == b'\xa5' * 4096
    assert len(frames(s, 1, PAGE_REQ)) == SIZE // PAGE
    assert len(frames(s, 0, DATA)) == SIZE // 4
    assert len(frames(s, 0, END)) == SIZE // PAGE
    assert not frames(s, 1, OLD_REQ)
    assert not frames(s, 0, OLD_RESP)
    programs = [event for event in s.trace if event['node'] == 1 and event['kind'] == 'program']
    assert [event['a'] for event in programs] == list(range(0, SIZE, PAGE))
    assert all(event['b'] == PAGE for event in programs)
    assert s.get(1, 'fw_dirty') == 0
    # This model schedules a core pass every 250 us. Bound the modeled result
    # without claiming the same duration on silicon or weakening verification.
    assert s.now < 25000000


def scenario_batch_lost_data_retry(s):
    begin_manual(s)
    first_tag = struct.unpack_from('<I', frames(s, 1, PAGE_REQ)[0][1])[0]
    s.do(0, 'fault', {'drop_types': [DATA], 'drop_type_count': 1})
    until(s, lambda: len(frames(s, 1, PAGE_REQ)) >= 2, timeout=200000)
    retry_tag = struct.unpack_from('<I', frames(s, 1, PAGE_REQ)[1][1])[0]
    assert retry_tag != first_tag
    assert s.get(1, 'fw_address') == 0 and s.get(1, 'fw_dirty') == 0
    # Late, internally valid data and END from the abandoned attempt cannot
    # populate the retry bitmap or complete/commit its page.
    before = s.flash(1, 0, PAGE)
    for kind, payload in ((DATA, struct.pack('<II', first_tag, 0xdeadbeef)),
                          (END, struct.pack('<II', first_tag, 0))):
        s.do(1, 'raw', wire_frame(kind, payload)); s.do(1, 'task', RX)
    assert s.flash(1, 0, PAGE) == before
    until(s, lambda: s.get(1, 'fw_address') >= PAGE)
    # Address progression precedes the scheduled commit in the existing task.
    pump(s, 4)
    assert_page_zero(s)
    assert not frames(s, 1, OLD_REQ)


def scenario_batch_crc_conflict_retry(s):
    begin_manual(s)
    data = frames(s, 0, DATA)[0][1]
    pump(s, 2)  # Deliver the real first word before its conflicting duplicate.
    tag_index, value = struct.unpack('<II', data)
    for _ in range(3):
        s.do(1, 'raw', wire_frame(DATA, struct.pack('<II', tag_index, value ^ 1)))
        s.do(1, 'task', RX)
    assert s.get(1, 'fw_dirty') == 0
    until(s, lambda: len(frames(s, 1, PAGE_REQ)) >= 2, timeout=200000)
    until(s, lambda: s.get(1, 'fw_address') >= PAGE)
    pump(s, 4)
    assert_page_zero(s)


def scenario_batch_reordered_end(s):
    begin_manual(s)
    tag = struct.unpack_from('<I', frames(s, 1, PAGE_REQ)[0][1])[0]
    page = image(205, 0x31)[:PAGE]
    # A genuine page checksum may arrive before its data without making a
    # partial page writable. Suppress the real END to require that early copy.
    s.do(0, 'fault', {'drop_types': [END], 'drop_type_count': 1})
    s.do(1, 'raw', wire_frame(END, struct.pack('<II', tag, zlib.crc32(page))))
    s.do(1, 'task', RX)
    assert s.get(1, 'fw_dirty') == 0
    first = frames(s, 0, DATA)[0][1]
    for _ in range(3):
        s.do(1, 'raw', wire_frame(DATA, first)); s.do(1, 'task', RX)
    assert s.get(1, 'fw_address') == 0
    until(s, lambda: s.get(1, 'fw_address') >= PAGE)
    pump(s, 4)
    assert_page_zero(s)
    requests = frames(s, 1, PAGE_REQ)
    assert sum(struct.unpack_from('<I', payload, 4)[0] == 0 for _, payload in requests) == 1


def scenario_batch_end_loss_fallback(s):
    prepare(s)
    s.do(0, 'fault', {'drop_types': [END], 'drop_type_count': 100})
    s.do(0, 'task', 'heartbeat_output_task')
    until(s, lambda: bool(frames(s, 1, OLD_REQ)), timeout=600000)
    requests = frames(s, 1, PAGE_REQ)
    assert 1 <= len(requests) <= 4, len(requests)
    assert struct.unpack_from('<I', frames(s, 1, OLD_REQ)[0][1])[0] == 0
    assert s.get(1, 'fw_dirty') == 0
    until(s, lambda: s.get(1, 'fw_address') >= PAGE)
    pump(s, 4)
    assert_page_zero(s)


def scenario_batch_negotiation_fallback(s):
    prepare(s)
    s.do(0, 'fault', {'drop_types': [CAPS_RESP], 'drop_type_count': 1})
    s.do(0, 'task', 'heartbeat_output_task')
    until(s, lambda: bool(frames(s, 1, OLD_REQ)), timeout=200000)
    assert len(frames(s, 1, CAPS_REQ)) == 1
    assert not frames(s, 1, PAGE_REQ)
    late = frames(s, 0, CAPS_RESP)[0][1]
    s.do(1, 'raw', wire_frame(CAPS_RESP, late)); s.do(1, 'task', RX)
    assert s.get(1, 'fw_dirty') == 0
    until(s, lambda: s.get(1, 'fw_address') >= PAGE)
    pump(s, 4)
    assert_page_zero(s)
    assert not frames(s, 1, PAGE_REQ)


def scenario_batch_wrong_page_crc(s):
    begin_manual(s)
    tag = struct.unpack_from('<I', frames(s, 1, PAGE_REQ)[0][1])[0]
    s.do(0, 'fault', {'drop_types': [END], 'drop_type_count': 1})
    # All DATA frames retain correct UART CRCs and bytes; the independent page
    # checksum is wrong, so a complete bitmap still must not authorize a write.
    checksum = zlib.crc32(image(205, 0x31)[:PAGE]) ^ 1
    s.do(1, 'raw', wire_frame(END, struct.pack('<II', tag, checksum)))
    s.do(1, 'task', RX)
    until(s, lambda: len(frames(s, 0, DATA)) >= 64)
    pump(s, 4)
    assert s.get(1, 'fw_address') == 0 and s.get(1, 'fw_dirty') == 0
    assert not any(event['kind'] == 'program' for event in s.trace)
    until(s, lambda: len(frames(s, 1, PAGE_REQ)) >= 2, timeout=200000)
    until(s, lambda: s.get(1, 'fw_address') >= PAGE)
    pump(s, 4)
    assert_page_zero(s)


def scenario_batch_changed_source(s):
    begin_manual(s)
    old_tag = struct.unpack_from('<I', frames(s, 1, PAGE_REQ)[0][1])[0]
    s.do(0, 'fw_prepare', 206, 0x55)
    checksum = zlib.crc32(image(206, 0x55)[:-4096])
    # This is the ordinary protected heartbeat bytes, delivered through the
    # actual UART receiver so source pinning executes unchanged.
    s.do(1, 'raw', wire_frame(12, struct.pack('<HHI', 206, 0xd485, checksum)))
    s.do(1, 'task', RX)
    for kind, data in ((DATA, struct.pack('<II', old_tag, 0xdeadbeef)),
                       (END, struct.pack('<II', old_tag, 0))):
        s.do(1, 'raw', wire_frame(kind, data)); s.do(1, 'task', RX)
    assert s.get(1, 'fw_address') == 0 and s.get(1, 'fw_dirty') == 0
    until(s, lambda: s.get(1, 'fw_address') >= PAGE, timeout=300000)
    pump(s, 4)
    assert s.flash(1, 0, PAGE) == image(206, 0x55)[:PAGE]
    tags = [struct.unpack_from('<I', data)[0] for _, data in frames(s, 1, PAGE_REQ)]
    assert len(tags) >= 2 and len(tags) == len(set(tags))
    assert not frames(s, 1, OLD_REQ)


def scenario_batch_source_priority_and_guard(s):
    begin_manual(s)
    prior = len(frames(s, 0, DATA))
    # Ordinary protected pointer traffic must leave before the next bulk word.
    s.do(0, 'packet', 26, '003e003e00000000', True)
    pump(s)
    assert len(frames(s, 0, DATA)) == prior
    assert frames(s, 0, 26)
    # The maintenance lease treats serving as active even though source.fw is
    # otherwise idle. A serial reset cannot strand the receiver mid-page.
    s.do(0, 'maintenance_request', 0, 1234)
    s.expect(0, 'maintenance_start', 2)
    # An independently admitted local update prevents any subsequent source
    # data. This fixture tests the service guard, not MSC admission itself.
    s.do(0, 'verify_update_state', 1)
    prior = len(frames(s, 0, DATA))
    pump(s, 20)
    assert len(frames(s, 0, DATA)) == prior
    assert not any(event['kind'] == 'program' for event in s.trace)


def scenario_batch_active_source_expired_lease(s):
    begin_manual(s)
    s.do(0, 'uart_stall', 1)
    s.advance(3000001)
    assert s.now - s.get(0, 'maintenance_source_last') >= 3000000
    s.do(0, 'maintenance_request', 0, 5678)
    s.expect(0, 'maintenance_start', 2)
    s.expect(0, 'maintenance_reserved', 0)
    s.expect(0, 'stopped', 0)
    # The config button's independently pending local request has the same
    # guard. Use real config-frame validation and deferred core0 processing.
    from test_transport import config_frame
    s.do(0, 'set', 'config_mode', 0, 1)
    s.do(0, 'vendor', config_frame(4))
    s.do(0, 'task', TX)
    s.expect(0, 'bootloader_local_pending', 0)
    s.expect(0, 'stopped', 0)
    assert not any(event['kind'] == 'reset' for event in s.trace)


def scenario_batch_reject_unnegotiated(s):
    prepare(s)
    payloads = [(PAGE_REQ, struct.pack('<II', 128, 0)),
                (CAPS_REQ, struct.pack('<IHH', 64, 204, 1)),
                (CAPS_REQ, struct.pack('<IHH', 64, 205, 2))]
    for kind, payload in payloads:
        s.do(0, 'raw', wire_frame(kind, payload)); s.do(0, 'task', RX)
        pump(s, 2)
    assert not frames(s, 0, CAPS_RESP) and not frames(s, 0, DATA)
    s.do(0, 'raw', wire_frame(CAPS_REQ, struct.pack('<IHH', 64, 205, 1)))
    s.do(0, 'task', RX)
    pump(s, 2)
    assert len(frames(s, 0, CAPS_RESP)) == 1
    for tag, address in [(128, 1), (128, SIZE), (64, 0), (0, 0)]:
        s.do(0, 'raw', wire_frame(PAGE_REQ, struct.pack('<II', tag, address)))
        s.do(0, 'task', RX); pump(s, 2)
    assert not frames(s, 0, DATA) and not frames(s, 0, END)
    for kind in (DATA, END):
        s.do(1, 'raw', wire_frame(kind, struct.pack('<II', 128, 0)))
        s.do(1, 'task', RX)
    assert not any(event['kind'] in ('program', 'erase', 'reset') for event in s.trace)
    assert s.flash(1) == image(204, 0x72)


def scenario_batch_flash_blackout(s):
    # 50 ms erase + 1 ms page program are explicit stress assumptions, not a
    # measurement of this board's flash chip. UART DMA/peer loops stay live.
    s.flash_erase_us = 50000
    s.flash_program_us = 1000
    prepare(s)
    until(s, lambda: bool(s.get(1, 'reboot')), timeout=30000000, background=True)
    assert s.flash(1) == image(205, 0x31)
    assert s.flash(1, CONFIG, 4096) == b'\xa5' * 4096
    requests = frames(s, 1, PAGE_REQ)
    assert len(requests) == SIZE // PAGE, 'flash blackout forced duplicate page requests'
    assert not frames(s, 1, OLD_REQ), 'flash blackout forced legacy fallback'
    assert s.get(0, 'stopped') == 0 and s.get(1, 'fw_dirty') == 0
    programs = [event for event in s.trace if event['node'] == 1 and event['kind'] == 'program']
    assert len(programs) == SIZE // PAGE
    # Every request after page zero must be sent after the previous page's
    # program call completed; source DATA cannot run ahead into the DMA ring.
    for (_, payload), event in zip(requests[1:], programs):
        assert struct.unpack_from('<I', payload, 4)[0] == event['a'] + PAGE
    for (request, _), event in zip(requests[1:], programs):
        assert request['at'] >= event['at'] + s.flash_program_us
    assert s.now < 25000000


def scenario_batch_core_schedule(s):
    # Run the real four-core scheduler through loss, retry and sector/page
    # commits. The deep tier repeats this under all 24 fixed priority orders.
    s.flash_erase_us = 50000
    s.flash_program_us = 1000
    prepare(s)
    s.do(0, 'fault', {'drop_types': [DATA], 'drop_type_count': 1})
    until(s, lambda: len([event for event in s.trace
                          if event['node'] == 1 and event['kind'] == 'program']) >= 3,
          timeout=600000, background=True)
    assert s.flash(1, 0, 3 * PAGE) == image(205, 0x31)[:3 * PAGE]
    assert s.flash(1, CONFIG, 4096) == b'\xa5' * 4096
    requests = frames(s, 1, PAGE_REQ)
    first, retry = (struct.unpack('<II', payload) for _, payload in requests[:2])
    assert first[1] == retry[1] == 0 and first[0] != retry[0]
    assert not frames(s, 1, OLD_REQ)


SCENARIOS = {
    'batch_full_image': scenario_batch_full_image,
    'batch_lost_data_retry': scenario_batch_lost_data_retry,
    'batch_crc_conflict_retry': scenario_batch_crc_conflict_retry,
    'batch_reordered_end': scenario_batch_reordered_end,
    'batch_end_loss_fallback': scenario_batch_end_loss_fallback,
    'batch_negotiation_fallback': scenario_batch_negotiation_fallback,
    'batch_wrong_page_crc': scenario_batch_wrong_page_crc,
    'batch_changed_source': scenario_batch_changed_source,
    'batch_source_priority_and_guard': scenario_batch_source_priority_and_guard,
    'batch_active_source_expired_lease': scenario_batch_active_source_expired_lease,
    'batch_reject_unnegotiated': scenario_batch_reject_unnegotiated,
    'batch_flash_blackout': scenario_batch_flash_blackout,
    'batch_core_schedule': scenario_batch_core_schedule,
}
BACKGROUND_FALSE = set(SCENARIOS) - {'batch_full_image', 'batch_flash_blackout', 'batch_core_schedule'}


def mixed_versions(baseline='2509929', library=None):
    """Compile exact historical production C; never approximate old negotiation."""
    from build import build
    archive = subprocess.run(['git', 'archive', baseline, 'src', 'CMakeLists.txt'],
                             cwd=ROOT, capture_output=True, check=True).stdout
    with tempfile.TemporaryDirectory(prefix='deskhop-batch-baseline-') as temporary:
        directory = pathlib.Path(temporary)
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            for member in tar:
                path = pathlib.PurePosixPath(member.name)
                assert not path.is_absolute() and '..' not in path.parts
                if member.isdir(): continue
                assert member.isfile()
                target = directory / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
        old = build(directory / 'baseline/node.so', source_root=directory)
        new = pathlib.Path(library or ROOT / 'build/tests/sim/node.so')
        for labels, libraries in [('new source / v104 receiver', [new, old]),
                                  ('v104 source / new receiver', [old, new])]:
            with Simulation(library=libraries) as sim:
                source_version = 205 if libraries[0] == new else 204
                prepare(sim, source_version, source_version - 1)
                until(sim, lambda: bool(sim.get(1, 'reboot')), timeout=60000000, background=True)
                assert sim.flash(1) == image(source_version, 0x31)
                assert sim.flash(1, CONFIG, 4096) == b'\xa5' * 4096
                assert not frames(sim, 1, PAGE_REQ)
                assert len(frames(sim, 1, OLD_REQ)) == SIZE // 4
                assert len(frames(sim, 0, OLD_RESP)) == SIZE // 4
                assert len(frames(sim, 1, CAPS_REQ)) == int(libraries[1] == new)
                assert not frames(sim, 0, CAPS_RESP)
                print(f'PASS full mixed-version transfer: {labels}; virtual_us={sim.now}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mixed-versions', action='store_true', required=True)
    parser.add_argument('--baseline', default='2509929')
    parser.add_argument('--library', type=pathlib.Path)
    args = parser.parse_args()
    mixed_versions(args.baseline, args.library)
