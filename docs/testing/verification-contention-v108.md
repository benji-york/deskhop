# Verification contention repair, v0.108

Status: implementation and hardware-free validation in progress. Nothing was
flashed or rebooted during this repair. Both physical Picos remain on v0.106;
the old, uninstalled v0.107 candidate is immutable and retained separately.

Branch `codex/upgrade-reliability-v0.108` is based on `a17ef5c`, so it includes
the four upstream fixes documented in
[the v0.107 integration record](upstream-fixes-v107.md). Main remains at
hardware-accepted `1da1af2`. No merge or push is authorized by this repair.

## Observed failure and cause

The separate recovery run `build/updater/runs/20260916T144841Z-8bmnj2nl/`
completed a correct-CRC scan on both boards, then correctly rejected a wrong
expected CRC. On the final repeat, A reported `UNVERIFIED busy` after all
262,144 bytes, while B passed. The journal remains failed; no validation gate
is relaxed or retroactively relabeled.

The scanner already yields on transient BUSY while starting, reading pages and
finishing a scan. Its subsequent freshness checks did not: one unsuccessful
nonblocking lock attempt permanently replaced COMPLETE with BUSY. Those
checks run before local/peer result publication and before console output.
Ordinary activity can hold the firmware-update lock without changing flash.
Thus an otherwise valid scan could become unavailable during presentation.

## Repair contract

- A freshness check makes one nonblocking attempt. BUSY returns pending without
  changing the completed evidence; callers yield rather than emit PASS.
- Recheck again at actual local queue publication, including after backpressure.
- A completed scanner remains owned while awaiting a freshness check. Local
  and peer work are bounded by the original three-second request deadline.
- Console rows and final verdict defer only until the original 3.5-second
  command deadline. Expiry cannot emit an overall PASS.
- Generation changes, metadata changes and an active update remain terminal
  failures. The scan times are not refreshed to hide stale results.
- Remote evidence remains a `scan_snapshot`, not a new freshness lease over
  every later response-packet transmission.

This changes firmware diagnostic availability, not the CRC algorithm, flash
layout, UART frame version, settings format, update transaction or host checks.
The host's correct/wrong/correct sequence still requires every verdict to
match; it does not ignore busy results or silently retry a failed deployment.
The read-only `verify` CLI now also prints its actual diagnostic-only plan,
instead of the generic flash plan that misleadingly mentioned ROM entry/load.

## Validation

Focused production scanner and real TinyUSB console regressions, full test-tier
results, ARM build and frozen artifact identity will be recorded after the
implementation is stable. Physical input acceptance and live verification
remain outstanding until an explicitly authorized deployment succeeds.

## Separate ROM backup timeout

The failed v0.107 flash stopped before any firmware write. That is a different
failure from this firmware verification issue; this repair must not be described
as fixing both. See [the stock-picotool investigation](rom-backup-timeout-investigation.md)
for the evidence and proposed bounded, no-flash experiment. No speculative
transport workaround or custom picotool is installed by this change.
