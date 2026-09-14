#!/usr/bin/env python3
"""Finite explicit-state model of one Pico's two-core update exclusion protocol.

This is a specification model, NOT execution/verification of C or RP2040 memory
ordering. See README.md for the abstraction map and assumptions. BFS gives a
shortest counterexample in this transition system, and emits replayable JSON.
"""
from __future__ import annotations
import argparse
from collections import deque
from dataclasses import asdict, dataclass, replace
import json
from pathlib import Path
import sys

# Phase: 0 idle, 1 guard, 2 prepare, 3 acquire flash, 4 access flash,
#        5 release flash, 6 finish, 7 release firmware-state lock.
OPS = (('drop', 'config', 'read', 'reboot', 'verify'),
       ('pull', 'read', 'reboot', 'verify'))
WRITERS = ('drop', 'pull', 'config')
MUTATIONS = ('state_lock', 'flash_lock', 'reboot_guard', 'checksum_pin', 'config_guard')

@dataclass(frozen=True)
class State:
    pcs: tuple[int, int] = (0, 0)
    ops: tuple[str, str] = ('', '')
    state_lock: int = -1
    flash_mask: int = 0
    source: str = ''
    # Two representative pages: 0=old, 1=pinned image, 2=changed source image.
    pages: tuple[int, int] = (0, 0)
    progress: int = 0
    dirty: bool = False
    peer_changed: bool = False
    reboot: bool = False
    recovery: bool = False
    powered_off: bool = False
    bad: str = ''


def core(s: State, i: int, **changes) -> State:
    for key in ('pcs', 'ops'):
        if key in changes:
            vals = list(getattr(s, key)); vals[i] = changes[key]; changes[key] = tuple(vals)
    return replace(s, **changes)


def transitions(s: State, mutation: str = '', power_loss: bool = False):
    if s.powered_off or s.bad:
        return
    if power_loss and s.dirty:
        # Valid first-page boot2 plus incomplete body is NOT rejected by ROM CRC.
        mixed = len(set(s.pages)) > 1
        yield 'power_loss', replace(s, pcs=(0, 0), ops=('', ''), state_lock=-1,
                                   flash_mask=0, source='', powered_off=True,
                                   bad='power_cut_can_boot_partial_image' if mixed else '')
    if not s.peer_changed and s.source == 'pull':
        yield 'peer_changes_image', replace(s, peer_changed=True)
    if s.reboot or s.recovery:
        # Once reset is armed, existing operations may drain; no new operations.
        pass
    for i in range(2):
        pc, op = s.pcs[i], s.ops[i]
        prefix = f'core{i}'
        if pc == 0:
            if s.reboot or s.recovery:
                continue
            for new_op in OPS[i]:
                if new_op != 'read' and s.state_lock != -1 and mutation != 'state_lock':
                    continue
                lock = s.state_lock if new_op == 'read' or mutation == 'state_lock' else i
                yield f'{prefix}:begin:{new_op}', core(s, i, pcs=1, ops=new_op, state_lock=lock)
        elif pc == 1:
            allowed = not s.reboot and not s.recovery
            if op == 'drop':
                allowed &= s.source != 'drop' or s.progress < 2
            elif op == 'pull':
                allowed &= s.source in ('', 'pull') and s.progress < 2
            elif op == 'config':
                allowed &= (not s.source or mutation == 'config_guard')
            elif op == 'reboot':
                allowed &= ((not s.source and not s.dirty) or mutation == 'reboot_guard')
            elif op == 'verify':
                allowed &= bool(s.source) and s.progress == 2
            yield f'{prefix}:guard:{"accept" if allowed else "reject"}', core(s, i, pcs=2 if allowed else 7)
        elif pc == 2:
            if op in ('drop', 'pull'):
                # UF2 may take over a pull BETWEEN locked page transactions.
                other = 1 - i
                in_other_transaction = s.ops[other] in ('drop', 'pull') and 3 <= s.pcs[other] <= 6
                stale = (op == 'pull' and s.source == 'drop') or (op == 'drop' and in_other_transaction)
                progress = 0 if op == 'drop' and s.source != 'drop' else s.progress
                yield f'{prefix}:claim:{op}', core(s, i, pcs=3, source=op, progress=progress,
                    bad='two_sources_claim_shared_staging' if stale else s.bad)
            elif op in ('read', 'config'):
                yield f'{prefix}:prepare:{op}', core(s, i, pcs=3,
                    bad='config_uses_update_page_buffer' if op == 'config' and s.source else s.bad)
            else:
                yield f'{prefix}:prepare:{op}', core(s, i, pcs=6)
        elif pc == 3:
            if not s.flash_mask or mutation == 'flash_lock':
                yield f'{prefix}:flash_acquire', core(s, i, pcs=4, flash_mask=s.flash_mask | (1 << i))
        elif pc == 4:
            others = s.flash_mask & ~(1 << i)
            conflict = bool(others) and (op in WRITERS or any(
                s.ops[j] in WRITERS for j in range(2) if others & (1 << j)))
            bad = 'flash_read_write_overlap' if conflict else s.bad
            if op in ('drop', 'pull'):
                pages = list(s.pages)
                if s.progress >= 2:
                    bad = 'staging_progress_overrun'
                else:
                    pages[s.progress] = 2 if op == 'pull' and s.peer_changed else 1
                yield f'{prefix}:program_page', core(s, i, pcs=5, pages=tuple(pages),
                    progress=min(2, s.progress + 1), dirty=True, bad=bad)
            else:
                yield f'{prefix}:flash_{op}', core(s, i, pcs=5, bad=bad)
        elif pc == 5:
            yield f'{prefix}:flash_release', core(s, i, pcs=6, flash_mask=s.flash_mask & ~(1 << i))
        elif pc == 6:
            if op == 'reboot':
                unsafe = s.dirty or bool(s.source)
                yield f'{prefix}:arm_reset', core(s, i, pcs=7, reboot=True,
                    bad='reset_armed_during_update' if unsafe else s.bad)
            elif op == 'verify':
                # Independent oracle: correct bytes must equal pinned image.
                pinned = s.pages == (1, 1)
                accepted = pinned or (mutation == 'checksum_pin' and s.pages == (2, 2))
                yield f'{prefix}:validate:{"accept" if accepted else "recovery"}', core(
                    s, i, pcs=7, source='', dirty=not accepted, reboot=accepted,
                    recovery=not accepted,
                    bad='unpinned_image_accepted' if accepted and not pinned else s.bad)
            else:
                yield f'{prefix}:finish', core(s, i, pcs=7)
        elif pc == 7:
            lock = -1 if s.state_lock == i else s.state_lock
            yield f'{prefix}:unlock', core(s, i, pcs=0, ops='', state_lock=lock)


def trace_for(state, parents):
    actions = []
    while parents[state] is not None:
        parent, action = parents[state]
        actions.append(action); state = parent
    return actions[::-1]


def search(depth: int, mutation='', power_loss=False):
    initial = State()
    parents = {initial: None}
    queue = deque([(initial, 0)])
    edges = 0
    truncated = 0
    while queue:
        state, level = queue.popleft()
        if state.bad:
            return {'status': 'counterexample', 'property': state.bad,
                    'states': len(parents), 'edges': edges, 'depth': level,
                    'mutation': mutation, 'power_loss': power_loss,
                    'actions': trace_for(state, parents), 'final': asdict(state)}
        if level >= depth:
            truncated += 1
            continue
        for action, successor in transitions(state, mutation, power_loss):
            edges += 1
            if successor not in parents:
                parents[successor] = (state, action)
                queue.append((successor, level + 1))
    return {'status': 'safe_within_bound' if truncated else 'finite_model_exhausted',
            'states': len(parents), 'edges': edges, 'depth': depth,
            'frontier_states': truncated, 'mutation': mutation, 'power_loss': power_loss}


def replay(report):
    state = State()
    for index, action in enumerate(report['actions']):
        choices = dict(transitions(state, report.get('mutation', ''), report.get('power_loss', False)))
        if action not in choices:
            raise ValueError(f'Cannot replay action {index}: {action}')
        state = choices[action]
    if state.bad != report['property']:
        raise ValueError(f'Expected {report["property"]}, got {state.bad!r}')
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--depth', type=int, default=64)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--power-loss', action='store_true', help='expose documented uncontrolled power-cut gap')
    parser.add_argument('--output', type=Path, help='directory for JSON counterexample traces')
    parser.add_argument('--replay', type=Path)
    args = parser.parse_args()
    if args.replay:
        report = json.loads(args.replay.read_text()); replay(report)
        print(f'Replayed {report["property"]}: {len(report["actions"])} transitions'); return 0
    cases = [('', False)]
    if args.mutations:
        cases += [(m, False) for m in MUTATIONS]
    if args.power_loss:
        cases += [('', True)]
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
    failed = False
    for mutation, power_loss in cases:
        report = search(args.depth, mutation, power_loss)
        expected_failure = bool(mutation or power_loss)
        found = report['status'] == 'counterexample'
        failed |= found != expected_failure
        name = mutation or ('power_loss_gap' if power_loss else 'baseline')
        if found:
            replay(report)
        if args.output:
            (args.output / f'{name}.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items() if k not in ('actions', 'final')}))
    return int(failed)

if __name__ == '__main__':
    sys.exit(main())
