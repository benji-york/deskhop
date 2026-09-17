#!/usr/bin/env python3
"""Fixture-only paired firmware clipboard tests; never reads an OS clipboard.

The helper boundary calls the exported production API with fixed bytes. Real
HID input, UART framing, scheduler, queues, core locks and B USB reports execute
in the two independent production images. This is a standalone runner because
helper API calls are not part of the generic simulator's JSON replay language.
"""
import argparse
import ctypes as C
from pathlib import Path
import zlib

from simulator import Simulation
from fixtures import KEYBOARD, keyboard, mouse
from test_keyboard_reliability import setup, focus, key, healthy
from test_transport import decode_frame

F23, Z = 0x72, 0x1d
CLIPBOARD_MSG = 62
SESSION = 0x445348434c495031


class Request(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in
                ('boot_source', 'boot_target', 'helper_session', 'nonce', 'focus')]


class HistoryEvent(C.Structure):
    _fields_ = [('seq', C.c_uint64), ('time_us', C.c_uint64), ('value', C.c_uint32),
                ('type', C.c_uint8), ('a', C.c_uint8), ('b', C.c_uint8), ('reserved', C.c_uint8)]


class HistoryWindow(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ('first_seq', 'end_seq', 'oldest_seq', 'overwritten')] + [('count', C.c_uint)]


def diagnostics(s, node):
    import re
    header = (Path(__file__).resolve().parents[2] / 'src/include/clipboard_diagnostics.h').read_text()
    phases, reasons = header.split('#define CLIPBOARD_DIAGNOSTIC_REASONS(X)')
    pattern = r'X\(\w+, "(\w+)"\)'
    phases, reasons = re.findall(pattern, phases), re.findall(pattern, reasons)
    lib = s.nodes[node]
    lib.diagnostic_history_window.argtypes = [C.c_uint]
    lib.diagnostic_history_window.restype = HistoryWindow
    lib.diagnostic_history_read.argtypes = [C.c_uint64, C.POINTER(HistoryEvent)]
    lib.diagnostic_history_read.restype = C.c_bool
    window = lib.diagnostic_history_window(64)
    assert window.overwritten == 0, 'sparse clipboard diagnostics overflowed history'
    result = []
    for seq in range(window.first_seq, window.end_seq):
        row = HistoryEvent()
        assert lib.diagnostic_history_read(seq, C.byref(row))
        if row.type == 19:
            assert row.value == 0 and row.reserved == 0, 'clipboard-derived value entered history'
            result.append((phases[row.a], reasons[row.b]))
    return result


def diagnostic_rejection(s, reason):
    source = prepare(s)
    if reason == 'caps_on': s.do(1, 'led', 2)
    if reason == 'helper_missing': source.clipboard_helper_close(s.now)
    for node in (0, 1): s.do(node, 'history_clear')
    key(s, 0, 2 if reason == 'non_bare' else 0, F23)
    advance(s, 30000)
    first = [diagnostics(s, node) for node in (0, 1)]
    for _ in range(10): key(s, 0, 2 if reason == 'non_bare' else 0, F23)
    advance(s, 100000)
    assert [diagnostics(s, node) for node in (0, 1)] == first, 'held F23 floods history'
    assert ('trigger', 'none') in first[0]
    assert ('rejected', reason) in first[0] + first[1], first
    assert not source.clipboard_helper_poll(C.byref(Request()))
    key(s, 0)


def diagnostic_completion(s, size):
    source = prepare(s)
    for node in (0, 1): s.do(node, 'history_clear')
    req = request(s, source, release=True)
    assert reply(s, source, req, b'z' * size)
    advance(s, 11000000 if size == 1024 else 150000)
    source_log, target_log = diagnostics(s, 0), diagnostics(s, 1)
    assert source_log == [(phase, 'none') for phase in (
        'trigger', 'admitted', 'release', 'grant', 'helper_request',
        'helper_result', 'payload_ready', 'done')], source_log
    assert target_log == [(phase, 'none') for phase in (
        'offer', 'admitted', 'release', 'payload_ready', 'typing', 'done')], target_log
    assert len(typed_z(s)) == size


def diagnostic_pull_missing(s, target):
    setup(s); focus(s, target); api(s)
    s.do(target, 'led', 0)
    for node in (0, 1): s.do(node, 'history_clear')
    key(s, target, 0, F23); key(s, target)
    s.advance(50000)
    assert ('trigger', 'none') in diagnostics(s, target)
    assert diagnostics(s, 1 - target) == [('pull', 'none'), ('rejected', 'helper_missing')]
    assert not s.nodes[1 - target].clipboard_helper_poll(C.byref(Request()))


def diagnostic_incomplete(s):
    source = prepare(s)
    # Two keyboard report IDs cannot prove complete release ownership. The
    # decoded F23 is observable, but must remain inadmissible and unconsumed.
    descriptor = b''.join(KEYBOARD[:6] + bytes((0x85, report_id)) + KEYBOARD[6:]
                          for report_id in (1, 2))
    s.do(0, 'mount', 3, 0, 0, descriptor.hex())
    s.do(0, 'history_clear')
    payload = '01' + keyboard(0, F23)
    for _ in range(10): s.do(0, 'report', 3, 0, payload)
    advance(s, 30000)
    assert diagnostics(s, 0) == [('trigger', 'none'), ('rejected', 'incomplete_report')]
    assert not source.clipboard_helper_poll(C.byref(Request()))


def diagnostic_timeout(s):
    source = prepare(s)
    request(s, source, release=True)
    advance(s, 3100000)
    logs = diagnostics(s, 0) + diagnostics(s, 1)
    assert ('cancelled', 'deadline') in logs, logs
    assert_no_text(s)


def diagnostic_negative_helper(s, status):
    source = prepare(s)
    req = request(s, source, release=True)
    assert source.clipboard_helper_reply_begin(C.byref(req), status, 0, 0, s.now)
    advance(s, 30000)
    reason = ('empty', 'non_text', 'oversize', 'unsupported', 'unavailable')[status - 1]
    assert ('helper_result', reason) in diagnostics(s, 0)
    assert ('cancelled', 'peer_cancelled') in diagnostics(s, 1)
    assert_no_text(s)


def api(s):
    signatures = {
        'clipboard_helper_open': ([C.c_uint64, C.c_uint64], C.c_bool),
        'clipboard_helper_close': ([C.c_uint64], None),
        'clipboard_helper_keepalive': ([C.c_uint64], None),
        'clipboard_helper_poll': ([C.POINTER(Request)], C.c_bool),
        'clipboard_helper_reply_begin': ([C.POINTER(Request), C.c_uint8, C.c_uint16,
                                           C.c_uint32, C.c_uint64], C.c_bool),
        'clipboard_helper_reply_chunk': ([C.c_uint16, C.c_void_p, C.c_size_t,
                                          C.c_uint64], C.c_bool),
        'clipboard_helper_reply_commit': ([C.c_uint64], C.c_bool),
        'clipboard_init': ([C.c_uint64], None),
        'tud_hid_set_report_cb': ([C.c_uint8, C.c_uint8, C.c_int, C.c_void_p,
                                    C.c_uint16], None),
        'tud_mount_cb': ([], None),
    }
    for node in s.nodes:
        for name, (args, result) in signatures.items():
            function = getattr(node, name)
            function.argtypes, function.restype = args, result
    return s.nodes[0]


def prepare(s, *, led_known=True):
    setup(s)
    focus(s, 1)
    source = api(s)
    if led_known:
        # A zero-initialized cache is not evidence of the host's Caps state.
        # Exercise the real local keyboard OUTPUT SET_REPORT callback.
        s.do(1, 'led', 0)
    assert source.clipboard_helper_open(SESSION, s.now)
    return source


def advance(s, duration, keepalive=True):
    while duration:
        part = min(duration, 200000)
        if keepalive:
            s.nodes[0].clipboard_helper_keepalive(s.now)
        s.advance(part)
        duration -= part


def request(s, source, release=False):
    key(s, 0, 0, F23)
    if release:
        key(s, 0)
    advance(s, 30000)
    result = Request()
    assert source.clipboard_helper_poll(C.byref(result)), 'explicit F23 made no request'
    assert result.boot_source == 1 and result.boot_target == 2
    assert result.helper_session == SESSION and result.nonce
    duplicate = Request()
    assert not source.clipboard_helper_poll(C.byref(duplicate)), 'one trigger requested twice'
    return result


def reply(s, source, request, text=b'zzzz', *, commit=True, crc=None):
    checksum = zlib.crc32(text) if crc is None else crc
    assert source.clipboard_helper_reply_begin(C.byref(request), 0, len(text), checksum, s.now)
    for offset in range(0, len(text), 37):
        chunk = text[offset:offset + 37]
        buffer = C.create_string_buffer(chunk)
        assert source.clipboard_helper_reply_chunk(offset, buffer, len(chunk), s.now)
    result = source.clipboard_helper_reply_commit(s.now) if commit else True
    return result


def downs(s, node=1, since=0):
    return [event for event in s.reports(node, 1)
            if event['at'] >= since and any(bytes.fromhex(event['data']))]


def typed_z(s, since=0):
    return [event for event in downs(s, since=since)
            if Z in bytes.fromhex(event['data'])[2:]]


def assert_no_text(s, since=0):
    assert not typed_z(s, since), 'cancelled, incomplete or rejected response typed text'


def rejected_request(s, source):
    since = s.now
    key(s, 0, 0, F23)
    key(s, 0)
    advance(s, 30000)
    empty = Request()
    assert not source.clipboard_helper_poll(C.byref(empty)), 'reported Caps-on state admitted a request'
    assert_no_text(s, since)
    # A waits for a grant when B rejects admission. Real input cancels that
    # attempt so each following negative exercises a fresh trigger.
    key(s, 0, 0, 0x04)
    key(s, 0)
    advance(s, 20000)


def led_initial_unknown(s):
    source = prepare(s, led_known=False)
    # A host/remapper may never send LED output reports. Start with Caps off;
    # an opposite-host Caps report must not change this destination's default.
    s.do(0, 'led', 2)
    advance(s, 10000)
    req = request(s, source, release=True)
    assert reply(s, source, req)
    advance(s, 150000)
    assert len(typed_z(s)) == 4
    assert ('rejected', 'led_unknown') not in diagnostics(s, 1)
    s.do(1, 'led', 2)
    rejected_request(s, source)
    # Neither a peer report nor a focus switch clears locally reported Caps.
    s.do(0, 'led', 0)
    rejected_request(s, source)
    s.do(1, 'led', 0)
    req = request(s, source, release=True)
    assert reply(s, source, req)
    advance(s, 150000)
    assert len(typed_z(s)) == 8


def led_invalid_reports(s):
    source = prepare(s)
    s.do(1, 'led', 2)
    buf = C.create_string_buffer(b'\x00\x00')
    for instance, report_id, kind, length, data in (
            (1, 1, 2, 1, buf), (2, 1, 2, 1, buf), (0, 2, 2, 1, buf),
            (0, 1, 1, 1, buf), (0, 1, 3, 1, buf),
            (0, 1, 2, 0, buf), (0, 1, 2, 2, buf), (0, 1, 2, 1, None)):
        s.nodes[1].tud_hid_set_report_cb(instance, report_id, kind, data, length)
        rejected_request(s, source)
    s.do(1, 'led', 0)
    req = request(s, source, release=True)
    assert reply(s, source, req)
    advance(s, 150000)
    assert len(typed_z(s)) == 4


def host_led_reconnect(s, *, reset_only=False):
    source = prepare(s)
    s.do(1, 'led', 2)
    if reset_only:
        # A bus reset may re-enumerate without an intervening unmount callback.
        s.nodes[1].tud_mount_cb()
    else:
        s.do(1, 'host', 0, 0)
        advance(s, 10000)
        s.do(1, 'host', 1, 0)
    advance(s, 20000)
    # A previous USB session's Caps-on cache cannot override this session's
    # startup default when the new host sends no LED output report.
    req = request(s, source, release=True)
    assert reply(s, source, req)
    advance(s, 150000)
    assert len(typed_z(s)) == 4


def led_reconnect_unknown(s):
    host_led_reconnect(s)


def led_bus_reset_unknown(s):
    host_led_reconnect(s, reset_only=True)


def led_focus_retains_known(s):
    source = prepare(s)
    focus(s, 0)
    focus(s, 1)
    req = request(s, source, release=True)
    assert reply(s, source, req)
    advance(s, 150000)
    assert len(typed_z(s)) == 4


def trigger_release(s):
    source = prepare(s)
    req = request(s, source)
    assert reply(s, source, req)
    advance(s, 250000)
    assert_no_text(s)
    # Consumed state was all-up throughout; repeat down must not imply release.
    key(s, 0, 0, F23)
    advance(s, 20000)
    assert_no_text(s)
    key(s, 0)
    advance(s, 100000)
    assert len(typed_z(s)) == 4
    s.expect_report(1, 1, keyboard())
    assert not downs(s, 0), 'clipboard text reached personal host A'
    s.check('key_absent', F23)
    healthy(s)


def admission_and_source_ownership(s):
    source = prepare(s)
    empty = Request()
    focus(s, 0)
    key(s, 0, 0, F23)
    key(s, 0)
    advance(s, 20000)
    assert not source.clipboard_helper_poll(C.byref(empty)), 'A-focus shortcut read clipboard'
    focus(s, 1)
    s.do(1, 'led', 2)
    advance(s, 10000)
    key(s, 0, 0, F23)
    key(s, 0)
    advance(s, 30000)
    assert not source.clipboard_helper_poll(C.byref(empty)), 'Caps Lock admitted typing'
    s.do(1, 'led', 0)
    advance(s, 10000)
    s.do(0, 'mount', 3, 0, 1, KEYBOARD.hex())
    # A consumed hotkey on another real interface has zero normal stored
    # state. Its raw modifiers still block a clipboard trigger.
    s.do(0, 'report', 3, 0, keyboard(0x21, 0x0a))
    key(s, 0, 0, F23)
    key(s, 0)
    advance(s, 30000)
    assert not source.clipboard_helper_poll(C.byref(empty)), 'consumed physical hold was ignored'
    s.do(0, 'report', 3, 0, keyboard())
    req = request(s, source)
    assert reply(s, source, req)
    s.do(0, 'report', 3, 0, keyboard())
    advance(s, 100000)
    assert_no_text(s)
    key(s, 0)
    advance(s, 100000)
    assert len(typed_z(s)) == 4
    s.expect_report(1, 1, keyboard())


def sparse_trigger_slot(s):
    source = prepare(s)
    s.do(0, 'report', 1, 0, keyboard(0, 0, 0, F23))
    advance(s, 30000)
    req = Request()
    assert source.clipboard_helper_poll(C.byref(req)), 'sole F23 usage depended on array position'
    assert reply(s, source, req)
    advance(s, 20000)
    assert_no_text(s)
    key(s, 0)
    advance(s, 100000)
    assert len(typed_z(s)) == 4


def second_trigger_cancels(s):
    source = prepare(s)
    req = request(s, source)
    assert reply(s, source, req)
    advance(s, 20000)
    s.do(0, 'mount', 3, 0, 1, KEYBOARD.hex())
    s.do(0, 'report', 3, 0, keyboard(0, F23))
    s.do(0, 'report', 3, 0, keyboard())
    advance(s, 100000)
    assert_no_text(s)
    key(s, 0)
    advance(s, 100000)
    assert_no_text(s)
    empty = Request()
    assert not source.clipboard_helper_poll(C.byref(empty))


def late_reply_and_new_nonce(s):
    source = prepare(s)
    old = request(s, source, release=True)
    advance(s, 3100000)
    assert not source.clipboard_helper_reply_begin(C.byref(old), 0, 4, zlib.crc32(b'zzzz'), s.now)
    assert_no_text(s)
    new = request(s, source)
    assert new.nonce != old.nonce
    # Every correlation field is checked independently before body admission.
    for field in ('boot_source', 'boot_target', 'helper_session', 'nonce', 'focus'):
        stale = Request.from_buffer_copy(bytes(new))
        setattr(stale, field, getattr(stale, field) ^ 0x100)
        assert not source.clipboard_helper_reply_begin(C.byref(stale), 0, 4,
                                                       zlib.crc32(b'zzzz'), s.now)
        advance(s, 20000)
        key(s, 0)
        advance(s, 10000)
        new = request(s, source)
    key(s, 0)
    assert reply(s, source, new)
    advance(s, 150000)
    assert len(typed_z(s)) == 4


def payload_limits(s):
    source = prepare(s)
    for status, length in ((1, 0), (2, 0), (3, 0), (4, 0), (5, 0),
                           (3, 1025), (0, 0), (0, 1025)):
        req = request(s, source, release=True)
        valid_negative = status != 0 and length == 0
        assert bool(source.clipboard_helper_reply_begin(C.byref(req), status, length, 0,
                                                        s.now)) == valid_negative
        advance(s, 20000)
        assert_no_text(s)
    for text in (b'z\x00z', b'z\rz', b'z\x7fz', b'z\xc3\xa9z'):
        req = request(s, source, release=True)
        assert not reply(s, source, req, text)
        advance(s, 20000)
        assert_no_text(s)
    req = request(s, source, release=True)
    assert not reply(s, source, req, b'zzzz', crc=0)
    advance(s, 20000)
    assert_no_text(s)


def incomplete_duplicate_chunks(s):
    source = prepare(s)
    for mode in ('missing', 'duplicate', 'reordered', 'overshoot'):
        req = request(s, source, release=True)
        assert source.clipboard_helper_reply_begin(C.byref(req), 0, 4, zlib.crc32(b'zzzz'), s.now)
        data = C.create_string_buffer(b'zz')
        assert source.clipboard_helper_reply_chunk(0, data, 2, s.now)
        if mode == 'missing':
            assert not source.clipboard_helper_reply_commit(s.now)
        else:
            offset = {'duplicate': 0, 'reordered': 3, 'overshoot': 2}[mode]
            size = 3 if mode == 'overshoot' else 2
            assert not source.clipboard_helper_reply_chunk(offset, data, size, s.now)
        advance(s, 20000)
        assert_no_text(s)


def maximum_pacing_and_backpressure(s):
    source = prepare(s)
    req = request(s, source, release=True)
    text = (b'aA1!\n\t-' * 147)[:1024]
    assert reply(s, source, req, text)
    s.do(1, 'endpoint', 0, 1, 0)
    advance(s, 300000)
    assert not downs(s), 'USB-stalled host accepted keyboard data'
    s.do(1, 'endpoint', 0, 0, 0)
    advance(s, 11000000)
    reports = downs(s)
    expected = {'a': (0, 4), 'A': (2, 4), '1': (0, 30), '!': (2, 30),
                '\n': (0, 40), '\t': (0, 43), '-': (0, 45)}
    actual = [(bytes.fromhex(event['data'])[0], bytes.fromhex(event['data'])[2])
              for event in reports]
    assert actual == [expected[chr(byte)] for byte in text], 'characters were lost/reordered'
    intervals = [right['at'] - left['at'] for left, right in zip(reports, reports[1:])]
    assert min(intervals) >= 10000, 'synthetic down/up pacing violated'
    assert reports[-1]['at'] - reports[0]['at'] <= 10500000
    s.expect_report(1, 1, keyboard())
    assert not downs(s, 0)
    healthy(s)
    print(f'  1024 bytes: {reports[-1]["at"] - reports[0]["at"]} us, '
          f'minimum down-to-down interval {min(intervals)} us')


def ordinary_uart_priority(s):
    source = prepare(s)
    req = request(s, source, release=True)
    s.do(0, 'uart_stall', 1)
    s.do(0, 'fill', 1, 256)
    assert reply(s, source, req)
    advance(s, 50000)
    assert_no_text(s)
    before = s.now
    s.do(0, 'uart_stall', 0)
    advance(s, 250000)
    stream = [decode_frame(bytes.fromhex(event['data']))[0] for event in s.trace
              if event['node'] == 0 and event['kind'] == 'uart_tx' and event['at'] >= before]
    assert len(stream) > 256 and all(kind != CLIPBOARD_MSG for kind in stream[:256]), \
        'clipboard frames overtook queued ordinary traffic'
    assert len(typed_z(s)) == 4
    healthy(s)


def endpoint_timeout_never_resumes(s):
    source = prepare(s)
    req = request(s, source, release=True)
    s.do(1, 'endpoint', 0, 1, 0)
    assert reply(s, source, req)
    advance(s, 16000000)
    assert_no_text(s)
    s.do(1, 'endpoint', 0, 0, 0)
    advance(s, 200000)
    assert_no_text(s)
    healthy(s)


def cancelled_release_respects_new_focus(s):
    source = prepare(s)
    req = request(s, source, release=True)
    assert reply(s, source, req, b'z' * 100)
    for _ in range(400):
        if typed_z(s): break
        advance(s, 500)
    assert typed_z(s), 'test did not accept a synthetic key-down'
    s.do(1, 'endpoint', 0, 1, 0)
    s.do(0, 'select', 0)
    advance(s, 10000)
    key(s, 1, 1, 0x07)
    advance(s, 20000)
    s.expect_report(0, 1, keyboard(1, 0x07))
    resumed = s.now
    s.do(1, 'endpoint', 0, 0, 0)
    advance(s, 30000)
    assert not downs(s, 1, resumed), 'synthetic release leaked real keys to inactive B'
    s.expect_report(1, 1, keyboard())
    s.expect_report(0, 1, keyboard(1, 0x07))


def cancel_action(s, source, kind):
    if kind == 'key_a': key(s, 0, 1, 0x07)
    elif kind == 'key_b': key(s, 1, 1, 0x07)
    elif kind == 'mouse_a': s.do(0, 'report', 1, 1, mouse(buttons=2))
    elif kind == 'mouse_b': s.do(1, 'report', 1, 0, mouse(buttons=1))
    elif kind == 'focus': s.do(0, 'select', 0)
    elif kind == 'unplug_keyboard': s.do(0, 'unmount', 1, 0)
    elif kind == 'unplug_a': s.do(0, 'host', 0, 0)
    elif kind == 'unplug_b': s.do(1, 'host', 0, 0)
    elif kind == 'caps': s.do(1, 'led', 2)
    elif kind == 'suspend': s.do(1, 'host', 1, 1)
    elif kind == 'config': s.do(1, 'set', 'config_mode', 0, 1)
    elif kind == 'maintenance': s.do(1, 'maintenance_request', 1, 0x434c4950)
    elif kind == 'update': s.do(1, 'verify_update_state', 1)
    elif kind == 'reboot': s.do(1, 'verify_update_state', 3)
    elif kind == 'helper_close': source.clipboard_helper_close(s.now)
    elif kind == 'helper_restart': assert source.clipboard_helper_open(SESSION + 1, s.now)
    elif kind == 'short_keyboard': s.do(0, 'report', 1, 0, '000000')
    else: raise AssertionError(kind)


def cancellation(s, kind, during=False):
    source = prepare(s)
    req = request(s, source, release=during)
    text = b'z' * 100
    assert reply(s, source, req, text)
    advance(s, 50000)
    if during: assert typed_z(s), 'test never began typing'
    else: assert_no_text(s)
    cancel_action(s, source, kind)
    cancelled = s.now
    # Source-side cancellation must cross the actual UART link. No lingering
    # character downs are allowed after its bounded propagation window.
    advance(s, 50000)
    assert_no_text(s, cancelled + 10000)
    if kind in ('key_a', 'key_b'):
        s.expect_report(1, 1, keyboard(1, 0x07))
    if kind == 'suspend': s.do(1, 'host', 1, 0)
    if kind == 'unplug_b': s.do(1, 'host', 1, 0)
    if kind == 'unplug_a': s.do(0, 'host', 1, 0)
    key(s, 0)
    advance(s, 200000)
    assert_no_text(s, cancelled + 10000)
    assert not source.clipboard_helper_reply_begin(C.byref(req), 0, len(text), zlib.crc32(text), s.now)
    if kind == 'key_b':
        s.expect_report(1, 1, keyboard(1, 0x07))
    elif kind not in ('reboot', 'focus', 'unplug_keyboard'):
        s.expect_report(1, 1, keyboard())


def expiry(s):
    source = prepare(s)
    req = request(s, source, release=True)
    assert reply(s, source, req, b'z' * 1024)
    advance(s, 3100000, keepalive=False)
    stopped_at = s.now
    advance(s, 200000, keepalive=False)
    assert_no_text(s, stopped_at)
    s.expect_report(1, 1, keyboard())


def wire_fault(s, fault):
    source = prepare(s)
    req = request(s, source, release=True)
    # Corrupt only clipboard data after the request/grant handshake. Ordinary
    # input remains live; fault fixture bytes never originate on a real host.
    s.do(0, 'fault', fault)
    assert reply(s, source, req, b'zzzz')
    advance(s, 3300000)
    assert_no_text(s)


def clipboard_frames(s, source=0, since=0):
    return [(event['data'], decode_frame(bytes.fromhex(event['data']))[1])
            for event in s.trace
            if event['kind'] == 'uart_tx' and event['node'] == source and event['at'] >= since
            and decode_frame(bytes.fromhex(event['data']))[0] == CLIPBOARD_MSG]


def inject_frames(s, target, frames):
    for wire in frames:
        s.do(target, 'raw', wire)
        s.do(target, 'task', 'packet_receiver_task')


def old_uart_replay(s):
    source = prepare(s)
    req = request(s, source, release=True)
    assert reply(s, source, req)
    advance(s, 150000)
    assert len(typed_z(s)) == 4
    old = clipboard_frames(s)
    trigger = [wire for wire, payload in old if payload[0] == 1]
    before = len(clipboard_frames(s, 1))
    inject_frames(s, 1, trigger)
    advance(s, 20000)
    new_grants = [payload for _, payload in clipboard_frames(s, 1)[before:]
                  if payload[0] == 2]
    assert not new_grants, 'completed trigger replay reacquired B pending slot'
    next_req = request(s, source, release=True)
    before = s.now
    inject_frames(s, 1, [wire for wire, payload in old if payload[0] != 1])
    advance(s, 200000)
    assert_no_text(s, before)
    # A stale reply must not become a cached payload for this newer nonce.
    assert next_req.nonce != req.nonce


def wire_reordered(s):
    source = prepare(s)
    req = request(s, source, release=True)
    s.do(0, 'fault', {'drop_types': [CLIPBOARD_MSG], 'drop_type_count': 1000})
    before = s.now
    assert reply(s, source, req)
    advance(s, 100000)
    begin = [wire for wire, payload in clipboard_frames(s, since=before) if payload[0] == 3]
    assert len(begin) == 9
    inject_frames(s, 1, [begin[0], begin[2], begin[1], *begin[3:]])
    # Follow with the originally valid remaining stream. Once cancelled it may
    # neither finish an incomplete response nor revive the previous operation.
    rest = [wire for wire, payload in clipboard_frames(s, since=before) if payload[0] != 3]
    inject_frames(s, 1, rest)
    advance(s, 200000)
    assert_no_text(s)


def symmetric(s, target, trigger, mode):
    setup(s); focus(s, target); api(s)
    source = 1 - target
    if mode != 'no_led': s.do(target, 'led', 0)
    local, remote = s.nodes[target], s.nodes[source]
    assert local.clipboard_helper_open(SESSION + target + 10, s.now)
    if mode != 'missing':
        assert remote.clipboard_helper_open(SESSION + source, s.now)
    if mode == 'paused': remote.clipboard_helper_close(s.now)
    def step(us):
        while us:
            n = min(us, 100000)
            if mode not in ('missing', 'paused', 'source_close'):
                remote.clipboard_helper_keepalive(s.now)
            s.advance(n); us -= n
    key(s, trigger, 0, F23)
    if mode not in ('held',): key(s, trigger)
    step(50000)
    req = Request()
    if mode in ('missing', 'paused'):
        assert not remote.clipboard_helper_poll(C.byref(req))
        step(3500000)
        assert not downs(s, target)
        # Starting a helper after the request expired cannot resurrect it.
        assert remote.clipboard_helper_open(SESSION + source, s.now)
        step(100000)
        assert not remote.clipboard_helper_poll(C.byref(req))
        return
    assert remote.clipboard_helper_poll(C.byref(req)), (target, trigger, mode)
    assert (req.boot_source, req.boot_target) == (source + 1, target + 1)
    assert req.helper_session == SESSION + source
    assert not local.clipboard_helper_poll(C.byref(Request())), 'read destination clipboard'
    if mode in ('local_close', 'local_restart'):
        local.clipboard_helper_close(s.now)
        if mode == 'local_restart': assert local.clipboard_helper_open(SESSION + 20, s.now)
    payload = b'z' * (1024 if mode == 'maximum' else 4)
    assert reply(s, remote, req, payload)
    if mode == 'source_close': remote.clipboard_helper_close(s.now)
    if mode == 'caps': s.do(target, 'led', 2)
    if mode == 'focus': focus(s, source)
    step(100000 if mode != 'maximum' else 11000000)
    if mode in ('source_close', 'caps', 'focus'):
        assert not downs(s, target); return
    if mode == 'held':
        assert not downs(s, target)
        key(s, trigger); step(100000)
    typed = [e for e in downs(s, target) if Z in bytes.fromhex(e['data'])[2:]]
    assert len(typed) == len(payload), (target, trigger, mode, len(typed))
    assert not downs(s, source), 'typed into clipboard source'
    s.check('key_absent', F23); healthy(s)
    # Reverse focus using the same live helper sessions; destination-only helper
    # use must not consume/reset the opposite direction's nonce high-water mark.
    if mode == 'both':
        focus(s, source); s.do(source, 'led', 0)
        key(s, trigger, 0, F23); key(s, trigger); step(50000)
        reverse = Request(); assert local.clipboard_helper_poll(C.byref(reverse))
        assert reverse.boot_source == target + 1 and reverse.boot_target == source + 1
        assert reply(s, local, reverse, b'zz'); step(100000)
        assert len([e for e in downs(s, source) if Z in bytes.fromhex(e['data'])[2:]]) == 2


def cancelled_local_release(s, target):
    setup(s); focus(s, target); api(s); s.do(target, 'led', 0)
    source = s.nodes[1 - target]
    assert source.clipboard_helper_open(SESSION, s.now)
    key(s, target, 0, F23); s.advance(50000)
    first = Request(); assert source.clipboard_helper_poll(C.byref(first))
    assert source.clipboard_helper_reply_begin(C.byref(first), 1, 0, 0, s.now)
    s.advance(50000); key(s, target); s.advance(50000)
    key(s, 1 - target, 0, F23); s.advance(50000)
    second = Request(); assert source.clipboard_helper_poll(C.byref(second))
    assert reply(s, source, second); s.advance(100000)
    assert not downs(s, target), 'old cancelled local release released a new remote trigger'
    key(s, 1 - target); s.advance(100000)
    assert len(downs(s, target)) == 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=1)
    options = parser.parse_args()
    cases = [(function.__name__, function) for function in
             (led_initial_unknown, led_invalid_reports, led_reconnect_unknown,
              led_bus_reset_unknown, led_focus_retains_known,
              trigger_release, admission_and_source_ownership, sparse_trigger_slot, second_trigger_cancels,
              late_reply_and_new_nonce, payload_limits, incomplete_duplicate_chunks,
              maximum_pacing_and_backpressure, ordinary_uart_priority,
              endpoint_timeout_never_resumes, cancelled_release_respects_new_focus,
              expiry, old_uart_replay, wire_reordered)]
    for kind in ('key_a', 'key_b', 'mouse_a', 'mouse_b', 'focus', 'unplug_keyboard',
                 'unplug_a', 'unplug_b', 'caps', 'suspend', 'config', 'maintenance',
                 'update', 'reboot', 'helper_close', 'helper_restart', 'short_keyboard'):
        for during in (False, True):
            cases.append((f'cancel_{kind}_{"during" if during else "held"}',
                          lambda s, kind=kind, during=during: cancellation(s, kind, during)))
    for name, fault in (('wire_duplicate', {'duplicate': True}),
                        ('wire_truncated', {'truncate': 12}),
                        ('wire_dropped', {'drop_types': [CLIPBOARD_MSG], 'drop_type_count': 1})):
        cases.append((name, lambda s, fault=fault: wire_fault(s, fault)))
    for target in (0, 1):
        for trigger in (0, 1):
            for mode in ('both', 'no_led', 'missing', 'paused', 'held', 'local_close', 'local_restart', 'source_close', 'caps', 'focus', 'maximum'):
                cases.append((f'symmetric_{target}_{trigger}_{mode}',
                    lambda s, target=target, trigger=trigger, mode=mode: symmetric(s, target, trigger, mode)))
    for target in (0, 1):
        cases.append((f'cancelled_local_release_{target}', lambda s, target=target: cancelled_local_release(s, target)))
    for reason in ('caps_on', 'helper_missing', 'non_bare'):
        cases.append((f'diagnostic_{reason}', lambda s, reason=reason: diagnostic_rejection(s, reason)))
    for size in (4, 1024):
        cases.append((f'diagnostic_completion_{size}', lambda s, size=size: diagnostic_completion(s, size)))
    for status in range(1, 6):
        cases.append((f'diagnostic_helper_{status}', lambda s, status=status: diagnostic_negative_helper(s, status)))
    cases.append(('diagnostic_timeout', diagnostic_timeout))
    cases.append(('diagnostic_incomplete', diagnostic_incomplete))
    for target in (0, 1):
        cases.append((f'diagnostic_pull_missing_{target}', lambda s, target=target: diagnostic_pull_missing(s, target)))
    for name, test in cases:
        with Simulation(options.seed, options.library) as simulation:
            test(simulation)
        print(f'PASS clipboard_{name}')
    print(f'{len(cases)} paired clipboard scenarios passed')


if __name__ == '__main__':
    main()
