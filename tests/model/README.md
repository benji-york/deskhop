# What this model proves

Run from the repository root:

```sh
python3 tests/model/check_flash.py --mutations --power-loss --output /tmp/deskhop-model-traces
python3 tests/model/check_flash.py --replay /tmp/deskhop-model-traces/flash_lock.json
cc -std=c11 -Wall -Wextra -Werror -Isrc/include tests/model/policy_contract.c src/fw_update.c -o /tmp/deskhop-model-policy
/tmp/deskhop-model-policy
```

No third-party Python packages or prover are needed. `--depth N` bounds the
breadth-first search. A `safe_within_bound` result only covers paths up to that
bound. A `finite_model_exhausted` result means every reachable state of **this
finite abstraction** was visited. The default depth 64 exhausts the unmutated
model: 3,395 states and 7,832 transitions. Counterexamples are shortest action
sequences within this model, saved as JSON, and replayed automatically. A bound
that misses an expected mutation fails the command rather than counting a
surviving mutation as success.

This is explicit-state model checking of an independently authored
specification. It is **not a proof of the C firmware**, instruction emulation,
verified compilation, a theorem prover, or a weak-memory model. The separate
`policy_contract.c` executes the production `fw_update.c` on 64,128 finite input
cases covering timing boundaries, owner guards, unexpected replies, and clock
translation across 32-bit wrap. It reduces specification drift for that pure
helper; it does not establish refinement of the locking implementation.

## Production abstraction map

| Model action/state | Production obligation |
| --- | --- |
| `begin`, `guard`, `unlock`, `state_lock` | `utils.c` firmware update critical section; calls from `firmware_upgrade_task`, UF2 handler, reboot and config writes |
| `claim:drop`, `claim:pull`, `source` | `ramdisk.c` UF2 ownership; `handlers.c` heartbeat ownership and pinned version/checksum |
| `prepare:config` | `utils.c` `save_config`/`wipe_config` refusing persistence during an active update; shared `page_buffer` |
| `flash_acquire`, `program_page`, `flash_release` | `utils.c` flash critical section enclosing complete erase/program/read operations |
| `pages`, `progress` | two representative pages in the running slot; C transfers 1,024 pages, with independent storage tests covering real size and boundaries |
| `peer_changes_image`, `validate` | transfer checksum and metadata validation in `tasks.c`; a matching numeric version alone cannot accept changed bytes |
| `arm_reset` | `handlers.c` `request_graceful_reboot` checks for active update/dirty image; final successful update arms reset only after validation |
| `power_loss` | adversarial loss of both cores and volatile update state, with persistent partial flash |

Inspect these production functions when changing the model. No source-code
string matching is claimed as proof. The source/staging protocol is abstracted
one page operation at a time, including a host UF2 takeover between peer transactions; the firmware's address framing, UF2 out-of-order
bitmap, UART queues, and error handling must be tested by the production-code
storage simulator.

## Assumptions and exclusions

- One Pico, two cores. The peer is an adversarial source that can change its
  image once. Cross-Pico messages and coordinated acknowledgements are outside
  the model. The same local exclusion obligation applies independently to each
  Pico.
- Sequential consistency and atomic lock acquisition/release. Lock enter/exit
  stand for RP2040 hardware spinlocks plus compiler/memory barriers. This model
  does not validate those assumptions or C data-race freedom.
- A critical section's actor may pause at every modeled phase. A pending reader
  can compete with the writer on the other core. Interrupt/DMA memory accesses
  that bypass these APIs are excluded, so no claim covers those accesses.
- Firmware executes from RAM throughout flash operations; flash-resident code,
  constants, IRQ vectors and DMA descriptors would need separate verification.
- Two representative sequential pages model ownership and completion, not all
  1,024 UF2 block orders or erase-sector behavior. Each page program is atomic
  except the separately reported power cut; actual torn erase/program is covered
  by a peripheral model, not proved here.
- Reset is a controlled request unless `--power-loss` is enabled. Normal guards
  are respected. Invalid completed images enter an abstract ROM recovery state.
- No liveness under an infinitely unfair scheduler or permanently absent peer is
  claimed. The search checks safety; production virtual-time tests must check
  retry/progress deadlines with explicit availability assumptions.

## Deliberate failures and the power-cut gap

| Change | Shortest trace | Invariant violated |
| --- | ---: | --- |
| `state_lock` removed | 6 transitions | both sources claim shared staging after stale guards |
| `flash_lock` removed | 9 | an XIP data read overlaps flash programming |
| `reboot_guard` removed | 12 | controlled reset armed during an update |
| `checksum_pin` removed | 21 | complete but different image accepted from a changing peer |
| `config_guard` removed | 11 | config save uses the shared update buffer |
| uncontrolled `power_loss` | 6 | partially rewritten image may be booted |

The last row is an **expected architectural gap**, not a mutation and not a
newly proved firmware bug. The updater writes the running slot and keeps its
dirty/ownership state in RAM. Once a valid first sector is programmed, a power
cut before later pages finish can leave a valid boot2 CRC followed by incomplete
firmware. RP2040 ROM does not validate DeskHop's full-image CRC. The explicit
recovery path erases the first sector after a detected invalid completion, but
cannot execute after power disappears. Proving recovery under arbitrary power
loss would require a separately designed boot-time validation/commit protocol
or persistent A/B images, plus bootloader and power-cut validation. Tests must
not label the present updater as atomic or universally power-loss safe.
