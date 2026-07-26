# Virtual Amiibo first write attempt: console crash and USB RX fix

Date: 2026-07-25
Initial result: **Switch 2 error 2168-0002**
Root cause: **88-byte `0x14` command dispatched through a 64-byte receive buffer**
Fix status: **hardware-confirmed; repeated write did not crash**

## Test

- Real Switch 2
- Pico 2 W Virtual Amiibo build
- Stable browser-loaded Virtual Amiibo image
- NFC-only UART trace armed
- Game-owned amiibo interaction

The console crashed with error `2168-0002`.

The trace was stopped immediately with 88 retained records, zero overwrite, and NFC-only filtering.
During the pull, the Switch power/reboot transition reset the Pico. The pull script timed out and
the volatile ring was lost, so no JSONL trace survived. The temporary empty dump marker was
discarded rather than preserving it as a misleading zero-record test.

## Root cause

The console-facing vendor OUT path used:

```c
uint8_t cmd[64];
uint32_t n = tud_vendor_read(cmd, sizeof(cmd));
ns2_dispatch(cmd, n);
```

A normal `0x14` write-buffer command contains:

- eight-byte command envelope;
- four-byte offset/declared-length prefix;
- normally 76 staging bytes;
- **88 bytes total**.

The USB endpoint therefore carries it as 64 bytes plus 24 bytes. Firmware incorrectly dispatched
the first 64 bytes immediately. The NFC runtime saw only 56 bytes after the command envelope even
though the `0x14` prefix declared 76 bytes, rejected it, and treated the remaining 24-byte USB
fragment as an unrelated command. All previously validated NFC reads avoided this bug because
their largest requests were much smaller than 64 bytes.

This is a transport-framing defect independent of the 454-byte write codec.

## Fix

Added `ns2_vendor_rx`, a bounded stream reassembler that:

- reads the big-endian payload length from command-envelope bytes 4–5;
- waits for exactly `8 + payload_length` bytes before dispatch;
- accepts arbitrary fragmentation, including the observed 64+24 split;
- can dispatch multiple complete commands from one TinyUSB read;
- rejects and drains oversized commands without misframing the following command;
- resets on USB mount so a partial command cannot survive re-enumeration.

The NFC mirror command/frame capacity was raised from 64 to 128 bytes so future native-write
diagnostics can carry the same complete `0x14` command.

`read_uart_diag.ps1 -OutputPath` now appends each validated trace record immediately. If another
power/reboot transition interrupts a dump, the PC retains a valid partial JSONL prefix instead of
losing every record while waiting to write the completed list.

## Automated validation

- Exact 88-byte `0x14` reconstruction from 64+24 bytes.
- Seven-byte arbitrary fragmentation.
- Two back-to-back commands in one feed.
- Oversize discard and recovery.
- Existing NFC mirror tests after the 128-byte capacity change.
- All 46 compiled host-test executables pass.
- Motion/PDU and all eight magnetometer-probe tests pass.
- Pico W and Pico 2 W release builds succeed.

## Hardware gate

The rebuilt Pico 2 W repeated the same game-owned write with **no console crash**. The retained
NFC-only trace in
[`dumps/virtual-amiibo-write-retest-2026-07-25.jsonl`](../../dumps/virtual-amiibo-write-retest-2026-07-25.jsonl)
confirms:

1. full reassembled `0x14` commands are 88 bytes;
2. retained chunks at offsets `0x0098`, `0x00E4`, `0x0130`, and `0x017C` were acknowledged;
3. `0x08` commit was acknowledged;
4. 750 ms later `0x05` returned accepted completion `05 00`;
5. `0x04` Stop completed without a crash.

The 128-record ring overwrote the two earliest write chunks, but the commit cannot succeed without
complete 454-byte staging coverage, and the four retained chunks preserve the corrected 88-byte
transport framing directly.

## Newly exposed lifecycle issue

After Stop, the console repeated scan → present `09 00` → Stop about once per second. The write and
transport had completed; the virtual reader was immediately presenting the still-loaded image
again while the game waited for physical removal.

The next build separates storage from presentation. A successful commit followed by Stop:

- retains the mutated RAM image and dirty state for download;
- emits one immediate modulo-eight removal edge;
- reports absent `07 41` until a new scan begins;
- does not auto-eject after an ordinary read or aborted write;
- presents the same selected, mutated image again on the next `0x03` scan.

All 46 host tests, both board builds, motion/PDU checks, and eight magnetometer-probe tests pass.
The follow-up hardware test confirmed auto-eject, next-scan re-presentation, same-session updated
readback, and a validated dirty UART export. See
[`virtual-amiibo-lifecycle-validation-2026-07-25.md`](virtual-amiibo-lifecycle-validation-2026-07-25.md).
