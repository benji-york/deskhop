"""Web Config Bootloader reports through real USB, whitelist, UART and handlers.

The HAL records ROM boot entry, not physical USB enumeration. Adjacent browser
requests must survive queue backpressure and drain DMA plus UART before local
entry. Drained transmission is not a peer acknowledgement or hardware test.
"""
import argparse
import ctypes as C
import functools
import json
import os
import pathlib
import shutil
import subprocess

from simulator import ROOT, Simulation
from test_transport import config_frame
from test_uart_integrity import wire_decode, wire_frame

BOOTLOADER = 4
PROXY = 23
RX = 'packet_receiver_task'
TX = 'process_uart_tx_task'


@functools.lru_cache(maxsize=1)
def browser_reports():
    """Capture the production handler; compare with independent CRC8 division.

    The separate JavaScript regression covers delegated DOM clicks and promise
    ordering. Here its actual outgoing bytes become inputs to production C.
    """
    node = (os.environ.get('NODE') or shutil.which('node')
            or str(pathlib.Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node'))
    script = r"""
const fs = require('node:fs'), vm = require('node:vm');
const reports = [];
const context = vm.createContext({
  Uint8Array,
  window: {addEventListener() {}},
  document: {getElementById() { return {addEventListener() {}}; }},
});
vm.runInContext(fs.readFileSync('webconfig/templates/script.js', 'utf8'), context);
context.device = {
  opened: true,
  async sendReport(id, bytes) { reports.push({id, data: Buffer.from(bytes).toString('hex')}); },
};
context.enterBootloaderHandler().then(() => process.stdout.write(JSON.stringify(reports)))
  .catch(error => { console.error(error); process.exitCode = 1; });
"""
    result = subprocess.run([node, '-e', script], cwd=ROOT, check=True,
                            capture_output=True, text=True, timeout=5)
    reports = json.loads(result.stdout)
    expected = [config_frame(PROXY, bytes([BOOTLOADER])), config_frame(BOOTLOADER)]
    assert reports == [{'id': 6, 'data': raw} for raw in expected], reports
    return tuple(report['data'] for report in reports)


def pending(s, node, *, peer=0, local=0):
    s.expect(node, 'bootloader_peer_pending', peer)
    s.expect(node, 'bootloader_local_pending', local)


def no_effect(s, *, update_node=None, update_state=0):
    for node in (0, 1):
        for field in ('stopped', 'reboot', 'uart_queue', 'fw_source'):
            s.expect(node, field, 0)
        s.expect(node, 'fw_dirty', int(node == update_node and update_state == 2))
        s.expect(node, 'fw_upgrading', int(node == update_node and update_state == 1))
        pending(s, node)
        for kind in ('reset', 'uart_tx', 'erase', 'program'):
            s.check('event_count', node, kind, 0)


def rom_entry(s, node):
    s.expect(node, 'stopped', 1)
    s.check('reset_count', node, 1, 1, 1)
    s.check('event_count', node, 'reset', 1)
    for kind in ('erase', 'program'):
        s.check('event_count', node, kind, 0)


def bootloader_frames(s, origin):
    sent = [event for event in s.trace
            if event['kind'] == 'uart_tx' and event['node'] == origin]
    commands = [event for event in sent if wire_decode(event['data'])[0] == BOOTLOADER]
    assert len(commands) == 1, commands
    assert commands[0]['data'] == wire_frame(BOOTLOADER)
    assert wire_decode(commands[0]['data']) == (BOOTLOADER, bytes(8))
    return sent


def reset_after_transmission(s, origin):
    sent = bootloader_frames(s, origin)
    reset = next(event for event in s.trace
                 if event['node'] == origin and event['kind'] == 'reset')
    assert all(reset['at'] >= event['at'] + event['b'] for event in sent), (reset, sent)


def local_bootloader(s, origin):
    _, local = browser_reports()
    peer = 1 - origin
    s.do(origin, 'set', 'config_mode', 0, 1)
    s.do(origin, 'vendor', local)
    s.expect(origin, 'stopped', 0)
    pending(s, origin, local=1)
    s.do(origin, 'task', TX)
    rom_entry(s, origin)
    s.expect(peer, 'stopped', 0)
    s.check('event_count', peer, 'reset', 0)
    for node in (0, 1):
        s.expect(node, 'uart_queue', 0)
        for kind in ('uart_tx', 'erase', 'program'):
            s.check('event_count', node, kind, 0)


def proxy_then_local(s, origin):
    proxy, local = browser_reports()
    peer = 1 - origin
    s.do(origin, 'set', 'config_mode', 0, 1)
    # Only the initiating USB endpoint needs config mode. The remote command
    # travels through the protected UART receiver, not a second USB callback.
    s.expect(peer, 'config_mode', 0)
    s.do(origin, 'vendor', proxy)
    s.expect(origin, 'uart_queue', 0)
    pending(s, origin, peer=1)
    for node in (0, 1):
        s.expect(node, 'stopped', 0)
        s.check('event_count', node, 'reset', 0)
    s.do(origin, 'task', TX)
    pending(s, origin)
    s.advance(100)  # one 32-byte 8N1 frame takes 87 us at the modeled baud rate
    assert len(bootloader_frames(s, origin)) == 1
    s.do(peer, 'task', RX)
    rom_entry(s, peer)
    s.expect(origin, 'uart_queue', 0)
    s.expect(origin, 'stopped', 0)
    s.check('event_count', origin, 'reset', 0)
    # The non-target is still running until the explicit local request.
    # This intentional scheduling gap is not a firmware peer acknowledgement.
    s.advance(1000)
    s.expect(origin, 'stopped', 0)
    s.do(origin, 'vendor', local)
    s.expect(origin, 'stopped', 0)
    pending(s, origin, local=1)
    s.do(origin, 'task', TX)
    rom_entry(s, origin)
    reset_after_transmission(s, origin)


def scenario_bootloader_config_guard(s):
    for origin in (0, 1):
        s.expect(origin, 'config_mode', 0)
        for raw in browser_reports():
            s.do(origin, 'vendor', raw)
            no_effect(s)


def scenario_bootloader_malformed(s):
    for origin in (0, 1):
        s.do(origin, 'set', 'config_mode', 0, 1)
        for raw in browser_reports():
            data = bytes.fromhex(raw)
            # Bad lengths and every one-bit mutation of the actual 12-byte
            # browser report must not become a local or proxied boot request.
            malformed = [data[:-1], data + b'\0']
            for offset in range(len(data)):
                for bit in range(8):
                    changed = bytearray(data)
                    changed[offset] ^= 1 << bit
                    malformed.append(bytes(changed))
            for damaged in malformed:
                s.do(origin, 'vendor', damaged.hex())
                no_effect(s)


def scenario_bootloader_whitelist_boundary(s):
    # The one added maintenance command does not authorize nested proxies,
    # physical input, heartbeat/update discovery or firmware data transport.
    forbidden = (1, 2, 3, 12, PROXY, 24, 25, 26, 34, 42, 43)
    for origin in (0, 1):
        s.do(origin, 'set', 'config_mode', 0, 1)
        validator = s.nodes[origin].validate_packet
        validator.argtypes = [C.c_void_p]
        validator.restype = C.c_bool
        for kind in forbidden:
            for proxy in ((True,) if kind == PROXY else (False, True)):
                command = PROXY if proxy else kind
                payload = bytes([kind, BOOTLOADER]) if proxy else bytes([BOOTLOADER])
                # Inspect the real whitelist too: some unauthorized input
                # packets are intentionally no-ops without attached devices.
                packet = C.create_string_buffer(bytes([command]) + payload.ljust(8, b'\0') + bytes(4))
                assert not validator(packet), (origin, kind, proxy)
                s.do(origin, 'vendor', config_frame(command, payload))
                no_effect(s)


def adjacent_requests(s, origin):
    """Local USB entry must wait until the preceding peer frame drains.

    Awaiting the browser's first sendReport only completes that USB transfer;
    there is no firmware acknowledgement that the peer command was delivered.
    This deliberately models no UART scheduling opportunity between requests.
    """
    proxy, local = browser_reports()
    peer = 1 - origin
    s.do(origin, 'set', 'config_mode', 0, 1)
    s.do(origin, 'vendor', proxy)
    s.do(origin, 'vendor', local)
    pending(s, origin, peer=1, local=1)
    s.expect(origin, 'stopped', 0)
    s.do(origin, 'task', TX)
    s.expect(origin, 'stopped', 0)
    s.expect(origin, 'uart_dma_busy', 1)
    pending(s, origin, local=1)
    s.advance(100)
    # Local service deliberately precedes remote dispatch: a completed UART
    # transmission is sufficient, without inventing a peer acknowledgement.
    s.do(origin, 'task', TX)
    rom_entry(s, origin)
    s.expect(peer, 'stopped', 0)
    s.check('event_count', peer, 'reset', 0)
    s.do(peer, 'task', RX)
    rom_entry(s, peer)
    s.expect(origin, 'uart_queue', 0)
    reset_after_transmission(s, origin)


def full_queue(s, origin, *, also_local=False):
    """A proxy request must not disappear behind a full ordinary UART queue."""
    proxy, local = browser_reports()
    peer = 1 - origin
    s.do(origin, 'set', 'config_mode', 0, 1)
    s.do(origin, 'fill', 1, 256)
    s.expect(origin, 'uart_queue', 256)
    s.do(origin, 'vendor', proxy)
    if also_local:
        s.do(origin, 'vendor', local)
    pending(s, origin, peer=1, local=int(also_local))
    s.do(origin, 'task', TX)
    # Full admission failure retains the command while ordinary TX makes room.
    pending(s, origin, peer=1, local=int(also_local))
    s.expect(origin, 'uart_queue', 255)
    s.expect(origin, 'stopped', 0)
    s.advance(100)
    s.do(peer, 'task', RX)
    # The filler packets are inert type zero. Allow the entire queue plus
    # scheduling margin to drain through the actual serializer and receiver.
    for _ in range(300):
        s.do(origin, 'task', TX)
        s.advance(100)
        s.do(peer, 'task', RX)
    s.expect(origin, 'uart_queue', 0)
    rom_entry(s, peer)
    sent = bootloader_frames(s, origin)
    assert len(sent) == 257, len(sent)
    if also_local:
        rom_entry(s, origin)
        reset_after_transmission(s, origin)
    else:
        s.expect(origin, 'stopped', 0)
        s.check('event_count', origin, 'reset', 0)
        pending(s, origin)


def dma_drain(s, origin):
    _, local = browser_reports()
    s.do(origin, 'set', 'config_mode', 0, 1)
    s.do(origin, 'uart_stall', 1)
    s.expect(origin, 'uart_dma_busy', 1)
    s.expect(origin, 'uart_busy', 0)
    s.do(origin, 'vendor', local)
    for _ in range(3):
        s.do(origin, 'task', TX)
        s.advance(100)
        s.expect(origin, 'stopped', 0)
        pending(s, origin, local=1)
    s.do(origin, 'uart_stall', 0)
    s.expect(origin, 'uart_dma_busy', 0)
    s.do(origin, 'task', TX)
    rom_entry(s, origin)
    s.expect(1 - origin, 'stopped', 0)
    s.check('event_count', origin, 'uart_tx', 0)


def uart_fifo_drain(s, origin):
    proxy, local = browser_reports()
    peer = 1 - origin
    s.do(origin, 'set', 'config_mode', 0, 1)
    s.do(origin, 'uart_busy', 1)
    s.do(origin, 'vendor', proxy)
    s.do(origin, 'vendor', local)
    s.do(origin, 'task', TX)
    s.advance(100)
    s.expect(origin, 'uart_dma_busy', 0)
    s.expect(origin, 'uart_busy', 1)
    s.do(peer, 'task', RX)
    rom_entry(s, peer)
    for _ in range(3):
        s.do(origin, 'task', TX)
        s.advance(100)
        s.expect(origin, 'stopped', 0)
        pending(s, origin, local=1)
    s.do(origin, 'uart_busy', 0)
    s.expect(origin, 'uart_busy', 0)
    s.do(origin, 'task', TX)
    rom_entry(s, origin)
    reset_after_transmission(s, origin)


def scenario_bootloader_update_admission(s):
    for origin in (0, 1):
        s.do(origin, 'set', 'config_mode', 0, 1)
        for state in (1, 2):  # active update, then dirty image without active writer
            s.do(origin, 'verify_update_state', state)
            for raw in browser_reports():
                s.do(origin, 'vendor', raw)
                s.do(origin, 'task', TX)
                no_effect(s, update_node=origin, update_state=state)
            s.do(origin, 'verify_update_state', 0)
            s.advance(100)
            s.do(origin, 'task', TX)
            no_effect(s)  # rejected requests must not become latent maintenance


def scenario_bootloader_update_before_dispatch(s):
    for origin in (0, 1):
        s.do(origin, 'set', 'config_mode', 0, 1)
        for state in (1, 2):
            for raw in browser_reports():
                s.do(origin, 'vendor', raw)
            pending(s, origin, peer=1, local=1)
            s.do(origin, 'verify_update_state', state)
            s.do(origin, 'task', TX)
            no_effect(s, update_node=origin, update_state=state)
            s.do(origin, 'verify_update_state', 0)
            s.advance(100)
            s.do(origin, 'task', TX)
            no_effect(s)


def scenario_bootloader_update_before_reset(s):
    _, local = browser_reports()
    for origin in (0, 1):
        s.do(origin, 'set', 'config_mode', 0, 1)
        for state in (1, 2):
            s.do(origin, 'uart_busy', 1)
            s.do(origin, 'vendor', local)
            s.do(origin, 'task', TX)
            pending(s, origin, local=1)
            s.expect(origin, 'stopped', 0)
            s.do(origin, 'verify_update_state', state)
            s.do(origin, 'uart_busy', 0)
            s.advance(100)
            s.do(origin, 'task', TX)
            no_effect(s, update_node=origin, update_state=state)
            s.do(origin, 'verify_update_state', 0)
            s.advance(100)
            s.do(origin, 'task', TX)
            no_effect(s)


SCENARIOS = {
    'bootloader_local_a': lambda s: local_bootloader(s, 0),
    'bootloader_local_b': lambda s: local_bootloader(s, 1),
    'bootloader_proxy_a_to_b': lambda s: proxy_then_local(s, 0),
    'bootloader_proxy_b_to_a': lambda s: proxy_then_local(s, 1),
    'bootloader_adjacent_a_to_b': lambda s: adjacent_requests(s, 0),
    'bootloader_adjacent_b_to_a': lambda s: adjacent_requests(s, 1),
    'bootloader_full_queue_a_to_b': lambda s: full_queue(s, 0),
    'bootloader_full_queue_b_to_a': lambda s: full_queue(s, 1),
    'bootloader_full_queue_both_from_a': lambda s: full_queue(s, 0, also_local=True),
    'bootloader_full_queue_both_from_b': lambda s: full_queue(s, 1, also_local=True),
    'bootloader_dma_drain_a': lambda s: dma_drain(s, 0),
    'bootloader_dma_drain_b': lambda s: dma_drain(s, 1),
    'bootloader_uart_fifo_drain_a': lambda s: uart_fifo_drain(s, 0),
    'bootloader_uart_fifo_drain_b': lambda s: uart_fifo_drain(s, 1),
    'bootloader_config_guard': scenario_bootloader_config_guard,
    'bootloader_malformed': scenario_bootloader_malformed,
    'bootloader_whitelist_boundary': scenario_bootloader_whitelist_boundary,
    'bootloader_update_admission': scenario_bootloader_update_admission,
    'bootloader_update_before_dispatch': scenario_bootloader_update_before_dispatch,
    'bootloader_update_before_reset': scenario_bootloader_update_before_reset,
}
BACKGROUND_FALSE = set(SCENARIOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=pathlib.Path)
    args = parser.parse_args()
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
