"""Fresh full-slot verification through real flash guards, scanner, UART and queues."""
import struct
import zlib
from fixtures import attach, keyboard, mouse
from test_transport import out_mouse
from test_peer_status import request as status_request
from test_peer_history import request as history_request

IMAGE = bytearray(((i * 73 + i // 127) ^ 0x31) & 255 for i in range(262144))
PAYLOAD_CRC = zlib.crc32(IMAGE[:-4096])
struct.pack_into('<IHHI', IMAGE, len(IMAGE)-4096, 0xf00d, 201, 0, PAYLOAD_CRC)
SLOT_CRC = zlib.crc32(IMAGE)
assert SLOT_CRC != PAYLOAD_CRC


def prepare(s):
    attach(s)
    for node in (0, 1):
        s.do(node, 'verify_prepare')


def request(s, node, token):
    s.do(node, 'verify_request', token)
    s.expect(node, 'verify_accepted', 1)


def collect(s, node, token, crc=SLOT_CRC):
    results = {}
    for _ in range(2):
        s.do(node, 'verify_poll')
        s.expect(node, 'verify_ready', 1)
        s.expect(node, 'verify_token', token)
        remote = s.get(node, 'verify_remote')
        assert remote not in results
        s.do(node, 'verify_assess', 201, crc)
        results[remote] = {key: s.get(node, 'verify_' + key) for key in
                          ('transport','scan','crc','bytes','start','end','verdict','reason')}
        if results[remote]['verdict'] == 0:
            s.expect(node, 'verify_bytes', 262144)
            s.expect(node, 'verify_crc', crc)
            s.expect(node, 'verify_metadata_crc', PAYLOAD_CRC)
            s.expect(node, 'verify_minor', 101)
            s.expect(node, 'verify_boot', (1-node if remote else node) + 1)
            assert s.get(node, 'verify_gen_start') == s.get(node, 'verify_gen_end')
            # 1,024 page reads cannot be compressed into fewer1ms ticks.
            assert 1024000 <= s.get(node, 'verify_end') - s.get(node, 'verify_start') < 3000000
            for core in (0, 1):
                assert s.get(node, 'verify_end_ticks', core) > s.get(node, 'verify_start_ticks', core)
                s.check('range', node, 'verify_core_age', core, 0, 2)
    s.do(node, 'verify_poll')
    s.expect(node, 'verify_ready', 0)
    return results


def scenario_verify_roundtrip(s):
    prepare(s)
    request(s, 0, 9001)
    s.advance(500000)
    s.do(0, 'verify_poll')
    s.expect(0, 'verify_ready', 0)  # no partial-image PASS
    s.do(0, 'report', 1, 0, keyboard(0, 4))
    s.do(1, 'report', 1, 0, mouse(x=7, y=-3))
    s.advance(5000)
    s.expect_report(0, 1, keyboard(0, 4))
    s.expect_report(0, 2, out_mouse(0, 16007, 15997))
    s.advance(795000)
    assert all(r['verdict'] == 0 for r in collect(s, 0, 9001).values())
    # Every new query scans again, including a deliberately wrong expectation.
    request(s, 0, 9002)
    s.advance(500000)
    s.do(0, 'verify_poll')
    s.expect(0, 'verify_ready', 0)
    s.advance(800000)
    assert all((r['verdict'],r['reason']) == (1,2) for r in collect(s,0,9002,SLOT_CRC ^ 1).values())
    for node in (0, 1):
        s.expect(node,'stopped',0)
        s.check('diagnostic_pacing',node,1000)


def scenario_verify_concurrent(s):
    prepare(s)
    # Both endpoints request local+remote scans plus status and long history.
    # The single scanner on each board serializes two fresh jobs within3s.
    for node in (0, 1):
        request(s,node,9101+node)
        status_request(s,node,9201+node)
        history_request(s,node,9301+node,64)
    s.do(0,'report',1,0,keyboard(0,5))
    s.advance(100000)
    s.expect_report(0,1,keyboard(0,5))
    for node in (0,1):
        s.do(node,'diagnostic_poll');s.expect(node,'diagnostic_poll_ready',1)
        s.expect(node,'diagnostic_outcome',0)
    s.advance(2400000)
    for node in (0,1):
        assert all(r['verdict']==0 for r in collect(s,node,9101+node).values())
        s.do(node,'history_poll');s.expect(node,'history_poll_ready',1)
        s.expect(node,'history_outcome',0);s.do(node,'history_release')
        s.expect(node,'stopped',0);s.check('diagnostic_pacing',node,1000)


def scenario_verify_mutation(s):
    prepare(s)
    request(s,0,9401)
    s.advance(500000)
    # Rewrite an already-read page identically: the CRC would still match,
    # but the old generation must not certify an uninterrupted scan.
    s.do(1,'verify_mutate',0,255)
    s.advance(800000)
    results=collect(s,0,9401)
    assert results[0]['verdict']==0
    assert (results[1]['verdict'],results[1]['reason'])==(2,5)
    request(s,0,9402)
    s.advance(1300000)
    assert all(r['verdict']==0 for r in collect(s,0,9402).values())
    # Metadata-sector padding is part of this full-slot digest.
    s.do(1,'verify_mutate',262143,0)
    request(s,0,9403)
    s.advance(1300000)
    results=collect(s,0,9403)
    assert results[0]['verdict']==0
    assert (results[1]['verdict'],results[1]['reason'])==(1,2)


def scenario_verify_late_local_result(s):
    prepare(s)
    request(s,0,9501)
    s.advance(1300000)
    s.do(0,'verify_poll');s.expect(0,'verify_ready',1);s.expect(0,'verify_remote',0)
    s.do(0,'verify_assess',201,SLOT_CRC);s.expect(0,'verify_verdict',0)
    s.do(0,'verify_mutate',0,255)
    s.do(0,'verify_recheck');s.do(0,'verify_assess',201,SLOT_CRC)
    s.expect(0,'verify_verdict',2);s.expect(0,'verify_reason',5)
    s.do(0,'verify_poll');s.expect(0,'verify_ready',1);s.expect(0,'verify_remote',1)
    # A later request cannot consume queued results from the old token.
    request(s,0,9502);s.advance(1300000)
    assert all(r['verdict']==0 for r in collect(s,0,9502).values())


def scenario_verify_reopen_mid_scan(s):
    prepare(s)
    abandoned_token, reopened_token = 9801, 9802
    request(s, 0, abandoned_token)
    s.advance(500000)
    s.do(0, 'verify_poll')
    s.expect(0, 'verify_ready', 0)  # both original scans are still in progress
    reopened_at = s.now
    deadline = reopened_at + 3500000  # console's overall presentation deadline
    request(s, 0, reopened_token)  # admitted before either old result is drained

    ignored, fresh = set(), set()
    while s.now < deadline and len(fresh) < 2:
        # Like the reopened console, consume at most one result per poll and
        # discard an abandoned token before interpreting its snapshot as PASS.
        s.advance(min(1000, deadline - s.now))
        s.do(0, 'verify_poll')
        if not s.get(0, 'verify_ready'):
            continue
        token = s.get(0, 'verify_token')
        remote = s.get(0, 'verify_remote')
        assert remote in (0, 1)
        if token == abandoned_token:
            assert remote not in ignored
            s.expect(0, 'verify_transport', 0)
            s.expect(0, 'verify_scan', 0)
            s.expect(0, 'verify_crc', SLOT_CRC)
            s.check('range', 0, 'verify_start', 0, 0, reopened_at - 1)
            ignored.add(remote)
            continue

        s.expect(0, 'verify_token', reopened_token)
        assert remote not in fresh, 'replacement query returned a duplicate board'
        s.check('time_range', reopened_at, deadline - 1)
        s.check('range', 0, 'verify_start', 0, reopened_at, s.now)
        s.check('range', 0, 'verify_end', 0, reopened_at, s.now)
        s.do(0, 'verify_recheck')  # only local snapshots are rechecked
        s.do(0, 'verify_assess', 201, SLOT_CRC)
        for field, value in (('transport', 0), ('scan', 0), ('verdict', 0),
                             ('reason', 0), ('bytes', 262144), ('crc', SLOT_CRC),
                             ('metadata_crc', PAYLOAD_CRC), ('minor', 101),
                             ('boot', remote + 1)):
            s.expect(0, 'verify_' + field, value)
        assert s.get(0, 'verify_gen_start') == s.get(0, 'verify_gen_end')
        assert 1024000 <= s.get(0, 'verify_end') - s.get(0, 'verify_start') < 3000000
        for core in (0, 1):
            assert s.get(0, 'verify_end_ticks', core) > s.get(0, 'verify_start_ticks', core)
            s.check('range', 0, 'verify_core_age', core, 0, 2)
        fresh.add(remote)

    assert ignored == {0, 1}, 'the scenario must encounter both late abandoned results'
    assert fresh == {0, 1}, 'replacement query must complete both fresh scans before its deadline'
    s.do(0, 'verify_poll')
    s.expect(0, 'verify_ready', 0)
    for node in (0, 1):
        s.expect(node, 'stopped', 0)
        s.check('diagnostic_pacing', node, 1000)


def scenario_verify_missing_peer(s):
    prepare(s)
    s.do(0,'fault',{'drop':100000})
    request(s,0,9601)
    s.advance(3002000)
    results=collect(s,0,9601)
    assert results[0]['verdict']==0
    assert (results[1]['verdict'],results[1]['reason'])==(2,7)
    s.do(0,'fault',{'drop':0})
    request(s,0,9602);s.advance(1300000)
    assert all(r['verdict']==0 for r in collect(s,0,9602).values())


def scenario_verify_dirty_image(s):
    prepare(s)
    s.do(1,'verify_update_state',2)
    request(s,0,9701);s.advance(1300000)
    results=collect(s,0,9701)
    assert results[0]['verdict']==0
    assert (results[1]['verdict'],results[1]['reason'])==(2,4)
    s.do(1,'verify_update_state',0)
    request(s,0,9702);s.advance(1300000)
    assert all(r['verdict']==0 for r in collect(s,0,9702).values())


SCENARIOS = {
    'verify_roundtrip':scenario_verify_roundtrip,
    'verify_concurrent':scenario_verify_concurrent,
    'verify_mutation':scenario_verify_mutation,
    'verify_late_local_result':scenario_verify_late_local_result,
    'verify_reopen_mid_scan':scenario_verify_reopen_mid_scan,
    'verify_missing_peer':scenario_verify_missing_peer,
    'verify_dirty_image':scenario_verify_dirty_image,
}
