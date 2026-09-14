# macOS storage-service panic after flashing

The user reported a Mac panic after the v0.95 deployment and subsequent
trackball replug recovery. Further firmware work and hardware flashing were
paused for investigation. No v0.96 firmware has been flashed.

## Evidence

The supplied report says:

```text
busy timeout[1], (60s): 'IOMediaBSDClient' (1,1812001) @IOService.cpp:5986
Panicked task: watchdogd
```

macOS 26.5.2 / build 25F84. Decoded report timestamps (UTC):

| Event | 2026-09-14 time |
| --- | --- |
| v0.95 bootloader disconnected after verified flash | 19:35:08 |
| Mac slept | 19:45:28 |
| Mac woke | 19:51:41 |
| First storage-service busy timeout in unified log | 19:52:41 |
| Panic in supplied report | 19:53:41 |

Before v0.95, repeated read-only USB inventories showed an older `RP2 Boot`
object at location `0x02120000` remaining inactive and busy for hours while the
current DeskHop device was active. During the earlier v0.94 deployment,
`diskutil` operations had hung and the bootloader volume failed to mount; direct
official picotool USB access was used instead. That did not prevent the ROM
mass-storage function from appearing to macOS.

Scoped unified logs independently confirm the v0.95 bootloader appearing as
RP2 media/disk5 at 19:32:58, repeated dangling-mountpoint messages, and disk5
removal at 19:35:08. One earlier access attempt was rejected because Google
Chrome already held exclusive access to RP2 Boot. This is an additional USB
ownership observation, not proof that Chrome caused the panic. The logs show
the first IOMediaBSDClient timeout 60 seconds after wake. The retained log is
`build/flashing/panic-storage-20260914.log`.

After the Mac reboot, USB inventory shows the active DeskHop device without the
old RP2 Boot object. The current media-service inventory has no inactive or
busy clients. This is a snapshot, not a guarantee against recurrence.

## Interpretation and limits

Apple's [IOService implementation](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOService.cpp)
prints the deepest busy services and their internal state words when waiting
for the registry to become quiet times out. The tuple in the panic is not a
disk or USB identifier. State 1 indicates an inactive service; Apple's
[IOService header](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOService.h)
documents terminated objects persisting until their clients close.
[IOMediaBSDClient](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOMediaBSDClient.cpp)
belongs to the disk/media path and manages BSD device nodes and termination.

Incomplete bootloader-disk teardown is therefore a credible flashing-related
trigger, especially given the already observed stale RP2 object. The report
does not identify which medium timed out or establish the blocked owner.
It does not prove a new CDC firmware defect, and the old stale state predates
v0.95. Normal v0.95 exposes HID and CDC, without mass storage. `watchdogd` and
the last-started-kext entry alone are not culprit identification. No storage
corruption or link to the trackball enumeration issue is established.

## Before further flashing

Do not repeat the current virtual-disk workflow while investigating. The SDK
supports `reset_usb_boot(activity_mask, 1)`, which disables ROM mass storage
while retaining PICOBOOT for direct picotool operations. Current DeskHop
bootloader hotkeys use mask 0. A narrow firmware change can provide a disk-free
entry path while retaining physical BOOTSEL as the manual recovery fallback.
This requires a separately planned deployment; it is not already present in
v0.95 and does not repair an existing retained Mac service.

`picotool reboot -u` is not a substitute: its RP2040 implementation uses mask 0.
The force options require a compatible firmware reset interface that DeskHop
does not currently provide. These findings come from the
[Raspberry Pi SDK](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/pico_bootrom/include/pico/bootrom.h)
and [picotool implementation](https://github.com/raspberrypi/picotool/blob/master/main.cpp).
No bootloader, serial, sleep, or reboot operation was performed during this
panic investigation; checks were read-only.
