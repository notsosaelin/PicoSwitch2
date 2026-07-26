# Switch 2 controller NFC / amiibo protocol inventory

> Current evidence map. The detailed 2026-07-12 packet-mining report is preserved at
> [`nfc-protocol-inventory-through-2026-07-12.archived.md`](../archive/nfc-protocol-inventory-through-2026-07-12.archived.md).
>
> Overall status: **a genuine Pro Controller 2 physical-tag read through the UART-gated relay and a
> Virtual Amiibo read using the same 600-byte/70-byte-chunk model are both hardware-confirmed.
> A conservative transactional Virtual Amiibo write also reaches complete `0x14`, `0x08`, and accepted
> `05 00` on a real Switch 2 without crashing. Logical removal, next-scan re-presentation,
> same-session updated readback, validated UART export, automatic dual-bank persistence,
> power-cycle recovery, reversible Save 1/Save 2 selection, offline library use, and backup restore
> are hardware/browser-confirmed. Native physical writes remain open.**

## Evidence rules

- Direct Switch 2/UART/BLE captures and hardware tests in this repository are primary.
- Public source code based on private captures is a structured hypothesis until reproduced here.
- Switch 1 and Switch 2 NFC envelopes are different. Similar operation names do not make them
  byte-compatible.
- Do not infer NFC hardware or command support from a controller name.

## Confirmed in this repository

The genuine Pro Controller 2 Windows USB capture
`usbpcaptures/genuine_procon_2.pcapng` contains:

| Exchange | Request | Response |
|---|---|---|
| `0x01/0x0C` | `01 91 00 0c 00 00 00 00` | `01 01 00 0c 00 f8 00 00 61 12 50 10` |
| `0x01/0x01` | `01 91 00 01 00 00 00 00` | `01 04 00 01 00 f8 00 00` |

This proves the implemented `0x0C` data response and the `dir=0x04` bare-ack shape. The capture is a
PC session without an amiibo interaction; it proves no tag read/write behavior.

The current console-facing firmware receives top-level command `0x01` over its USB vendor
interface. The Bluetooth host has an UART-gated genuine-reader bridge that writes extended commands
to `0x0016`, subscribes to `0x001E`, and also matches NFC replies arriving through the ordinary
`0x001A` response path.

## Confirmed native read

On 2026-07-25, a genuine Pro Controller 2 read a physical amiibo through PicoSwitch2 and the Switch
recognized it. See
[`pro2-native-nfc-read-2026-07-25.md`](../experiments/pro2-native-nfc-read-2026-07-25.md).

The console used `0x03 → 0x05 → 0x06 → 0x05 → 0x15... → 0x05 → 0x04`. The reader state advanced
`0 → 1 → 2 → 3`. Status was `09 00` before the operation and `04 00` after `0x06`.

The USB host requested nine offset-addressed `0x15` chunks at offsets `0x0000` through `0x0230`.
Responses used `[last:u8][length:u16le][data]`: eight 70-byte chunks and one 40-byte final chunk,
for an exact 600-byte reader buffer. The underlying layout is 60 bytes of reader/UID/signature/
operation metadata followed by the raw 540-byte NTAG215 image.

The same framing is independently confirmed on the emulated side: the Switch recognized an
uploaded Virtual Amiibo served through the feature-gated runtime. See
[`virtual-amiibo-read-validation-2026-07-25.md`](../experiments/virtual-amiibo-read-validation-2026-07-25.md).

## Reconstructed virtual write

The failed Smash/native-write attempt could not provide a primary write capture because the
physical presentation device changed UID. Existing evidence was sufficient for a guarded
Virtual Amiibo test implementation:

- a UID-bearing `0x06` selects write mode only when its seven-byte UID exactly matches the active
  image;
- `0x15` exposes a 64-byte write-preparation buffer using the confirmed bounded chunk envelope;
- `0x14` uses `[offset:u16le][declared:u16le][data]` to fill exactly 454 bytes;
- `0x08` applies the staged page records atomically and changes status to `05 00`;
- the report NFC field advances as an event counter modulo eight.

All malformed, incomplete, conflicting, stale-generation, protected-page, and mismatched-UID
transactions leave the active image unchanged. Successful writes update RAM and dirty state only;
they never program flash in the live console path. The evidence and remaining hardware gate are
recorded in
[`virtual-amiibo-write-evidence-2026-07-25.md`](../experiments/virtual-amiibo-write-evidence-2026-07-25.md).

The first write test also established that `0x14` is a multi-packet USB OUT command in this
firmware path: its 88 total bytes arrived across the old 64-byte read boundary. Dispatching the
first fragment caused error `2168-0002`; bounded envelope-length reassembly is now host- and
hardware-confirmed. The repeated trace captured complete 88-byte requests, the `0x08` commit, and
the `05 00` completion response without a crash. It also exposed the next lifecycle requirement:
after Stop, the console repeatedly rescanned while the retained image remained presented. The
runtime now retains the dirty image but emits logical tag removal after a committed Stop, then
re-presents the same updated selection on the next `0x03` scan. A subsequent test confirmed that
entire lifecycle plus UART export; see
[`virtual-amiibo-lifecycle-validation-2026-07-25.md`](../experiments/virtual-amiibo-lifecycle-validation-2026-07-25.md)
and
[`virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md`](../experiments/virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md).

## Strong external Switch 2 evidence

At commit `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92`,
`ndeadly/switch2_controller_research` documents:

- command `0x01` as NFC;
- subcommands `0x03`, `0x04`, `0x05`, `0x06`, `0x08`, `0x0C`, `0x14`, and `0x15`;
- USB and BLE transport bytes;
- BLE `0x15` offset reads with bounded responses;
- the extended command response value handle `0x001E` and CCC `0x001F`;
- the NFC state byte in native controller input.

At commit `5f0a815615b8b7f11ff8c1d1f994ba51dc7da0c7`,
`Dycool/NS-PC-Control` implements a USB virtual-tag flow reportedly derived from private captures:

- raw 540-byte and extended 572-byte tag files;
- 32-byte originality signature;
- 61-byte status response;
- 622-byte read payload / 630-byte USB response;
- 454-byte write staging;
- offset-addressed `0x14` chunks and `0x08` commit;
- short state delays and read/write hold windows.

The implementation is internally consistent and much more complete than the older audit, but its
capture files are not public in that repository. These details are strong starting values, not
hardware-confirmed PicoSwitch2 facts.

## Corrected transport model

The earlier “USB full 622-byte response versus BLE offset chunks” distinction was a hypothesis
derived from an external implementation. Primary hardware capture disproved it for the successful
native read: the Switch sent the same two-byte offsets over USB and accepted bounded responses.
The old model remains in the archived audit for provenance, but active code must use the confirmed
600-byte buffer and 70-byte chunk framing.

## Switch 1 evidence

Switch 1 controllers use report `0x11` MCU requests, report `0x31` extended input, and subcommands
`0x21`/`0x22`. This is a different transport from Switch 2 command `0x01`.

At commit `9d0cc455aebd07930b557840b47cb26df9eb4a1f`, the MIT-licensed `jc_toolkit`
contains a physical Joy-Con/Pro Controller reader sequence that:

- selects report `0x31`;
- enables the MCU and selects NFC mode;
- polls for a tag and extracts its UID;
- reads NTAG213/215/216 page ranges and assembles multi-report data.

That is sufficient evidence for a future physical-reader **read** implementation. This audit found
no equivalent physical-tag write path in `jc_toolkit`. Historical JoyControl/Poohl work describes
the console-facing virtual write protocol, not a validated host driving a real Joy-Con to write a
physical tag. Full Switch 1 native read/write translation therefore remains open.

## Unresolved questions requiring primary capture

1. Why matching ordinary NFC replies are delivered on `0x001A` even while `0x001E` is subscribed,
   and whether any operation uses `0x001E` exclusively.
2. Exact NFC event timing during failure. The `0..7` wrap is modelled as a modulo-eight event
   counter; write completion at about 750 ms and the post-Stop removal/next-scan lifecycle are now
   hardware-confirmed.
3. Whether Pro Controller 2 and Joy-Con 2 Right use byte-identical NFC transactions.
4. Whether a 540-byte upload with its missing signature field represented as zero is accepted on current
   Switch 2 firmware.
5. Physical-controller write behavior for Switch 1 and Switch 2 readers.
6. Behavior when the source controller disconnects or the tag is removed mid-transaction.

## Next capture

The next available hardware test should exercise a game-owned write against the stable Virtual
Amiibo slot and download/read back the mutated image. A future native capture should still write
and read back the same physical tag while exporting console USB commands, BLE writes, both
response handles, and native input NFC state on one time base. Native reader support remains
read-only and diagnostic until that capture and the normal regression checklist pass.

The 2026-07-25 Smash attempt did not reach a write because the presentation device changed UID
between scans. See
[`smash-native-nfc-write-attempt-2026-07-25.md`](../experiments/smash-native-nfc-write-attempt-2026-07-25.md).
