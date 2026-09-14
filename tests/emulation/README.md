# RP2040 emulator feasibility probe

This optional experiment executes two ARMv6-M instructions and probes the
RP2040 multicore launch interface in **rp2040js 1.3.4**, the npm latest version
on 2026-09-14 (published 2026-09-09). It does not run DeskHop or validate USB.
It also connects two independently instantiated RP2040 models via UART0 callbacks
and a shared virtual clock. Each runs four real ARM instructions to write one
byte to UART0_DR and read the other model's byte back into r2: A receives `0x42`,
B receives `0x41`. The normal test tiers are offline and do not depend on this package.

Reproduce using a Node.js runtime:

```sh
mkdir -p /tmp/deskhop-rp2040js-probe
curl -L --fail https://registry.npmjs.org/rp2040js/-/rp2040js-1.3.4.tgz -o /tmp/deskhop-rp2040js-probe/package.tgz
# Verify the registry's pinned SHA-512 digest before extracting:
python3 - <<'PY'
import base64, hashlib
from pathlib import Path
p = Path('/tmp/deskhop-rp2040js-probe/package.tgz')
assert base64.b64encode(hashlib.sha512(p.read_bytes()).digest()).decode() == '3Y+WpXT1F0iDv2oqZQHy1aAl7FcVLRaX6kRx5HU8O3fwoVKtTT3I9zw/yVDfYF8DaqfbkM5dwwzyGJcE6Lh4/g=='
PY
tar -xzf /tmp/deskhop-rp2040js-probe/package.tgz -C /tmp/deskhop-rp2040js-probe
node tests/emulation/probe_rp2040js.cjs /tmp/deskhop-rp2040js-probe/package
```

Recorded output is [rp2040js-1.3.4-probe.json](rp2040js-1.3.4-probe.json).
The emulator executes the arithmetic correctly (`r0=43`) but instantiates only
one CPU. `CPUID` is always zero. Reads of FIFO_ST and FIFO_RD return
`0xffffffff`; the FIFO write/read log invalid SIO addresses, so they are not a
second core handshake. USB/PIO objects existing does not establish USB host,
PIO-USB, endpoint, or timing fidelity. The probe exits successfully only when
these documented limitations are reproduced; after a dependency upgrade a
changed result deliberately prompts architectural reassessment.

The UART connection is possible and was executed. It does not give each
emulated Pico its missing second core. Its byte arrival timing is a deliberately
chosen ideal 8N1 link at 3,686,400 baud plus 1,000 ns propagation; those are harness
parameters, not electrical measurements or validated UART/isolator timing. The
emulator emits UART writes via an immediate callback, so this external link
model adds the delay. No USB peripherals or PIO USB bus are attached by the probe.

Neither QEMU nor Renode was installed in the task environment. Their assessment
in [the architecture decision](../../docs/testing/architecture.md) is based on
current primary documentation/source, not on a claimed firmware boot attempt.
