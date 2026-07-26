# Virtual Amiibo write reconstruction from existing evidence

Date: 2026-07-25
Result: **RAM-only staging/commit, logical removal, next-scan re-presentation, updated readback, and
UART export hardware-confirmed**

## Constraint

The available physical multi-amiibo device changes UID between presentations, so the attempted
Smash capture never reached a native write. It produced no UID-bearing `0x06`, `0x14`, or `0x08`
transaction. See
[`smash-native-nfc-write-attempt-2026-07-25.md`](smash-native-nfc-write-attempt-2026-07-25.md).

The first Virtual Amiibo write implementation therefore reconstructs only the byte rules that
agree across the existing repository captures, the local Switch 2 command research, and the
capture-derived public implementation identified below. It does not claim that native physical
write behavior is confirmed.

## Evidence used

1. PicoSwitch2's successful genuine and virtual reads confirm the USB envelope, bare-ack
   direction, 61-byte status, 600-byte operation buffer, `[last][length:u16le][data]` chunk
   response, and accepted report-state timing.
2. The local pinned copy of `ndeadly/switch2_controller_research` identifies:
   - `0x06` as read device;
   - `0x08` as write device;
   - `0x14` as write buffer;
   - `0x15` as read buffer;
   - the `0x14` request prefix as `[offset:u16le][length:u16le][data]`.
3. `Dycool/NS-PC-Control` commit `5f0a815615b8b7f11ff8c1d1f994ba51dc7da0c7`
   contains a structured Switch 2 virtual-tag implementation described in its source as derived
   from a private `PC2_Write_Amiibo.pcapng`. The private capture is not available, so this remains
   strong external evidence rather than a primary PicoSwitch2 capture. Its internally consistent
   write model is:
   - zero UID in `0x06` selects read mode;
   - the exact selected seven-byte UID selects write mode;
   - write preparation exposes a 64-byte buffer;
   - six offset-addressed `0x14` chunks fill a 454-byte staging image;
   - `0x08` validates and commits the complete staging image;
   - status becomes `05 00`, with the HID NFC event counter advancing after about 700 ms.
4. The genuine relay capture advanced the report NFC field through `0..7` and wrapped to zero.
   This independently supports treating that field as a modulo-eight event counter rather than
   assigning fixed meanings to values `0`, `1`, `2`, and `3`.

## Implemented transaction

- `0x03`: enter scan and schedule a bounded NFC event transition.
- `0x05`: report ready `09 00`, active `04 00`, committed `05 00`, or error/absent `07 41`.
- `0x06`:
  - all-zero UID builds the hardware-confirmed 600-byte read buffer;
  - exact selected UID builds a 64-byte write-preparation buffer;
  - any other UID fails closed.
- `0x15`: serve either operation buffer with the hardware-confirmed
  `[last][length:u16le][data]` response and a maximum 70-byte data chunk.
- `0x14`: require a four-byte offset/declared-length header, reject zero, truncated,
  out-of-range, or conflicting repeated chunks, and stage at most 454 bytes.
- `0x08`: require complete coverage, `D0 07`, the exact selected UID, a bounded record count,
  writable NTAG pages only, and zero trailing padding. Apply all records atomically or none.

The write changes only the active RAM image. It marks that image dirty through the existing store;
flash persistence remains an explicit config-mode action. The store update uses the generation
captured when `0x06` began, so a newer browser selection cannot be overwritten by a stale console
transaction.

## Automated result

- Codec tests cover the 64-byte write-preparation buffer and chunk framing.
- Runtime tests cover a complete six-chunk write, identical retry, UID mismatch, incomplete
  commit, atomic no-change failures, format-flow promotion, `05 00` status, 700 ms event timing,
  and store-apply failure reporting.
- All 46 compiled host-test executables pass.
- Pico 2 W and Pico W release builds link successfully.

The first hardware write attempt then exposed an independent transport bug: `0x14` is 88 bytes,
but the old USB task dispatched each 64-byte `tud_vendor_read` result as a complete command. The
Switch crashed with error `2168-0002`. A bounded command reassembler now reconstructs the exact
64+24 split before dispatch. The repeated hardware test did not crash and captured complete
88-byte requests, `0x08` commit, and accepted `05 00` completion. It also showed the console
repeatedly rescanning because the retained image never appeared removed; a storage/presentation
split now auto-ejects only after committed Stop and re-presents the same updated selection on the
next scan. A follow-up hardware test confirmed that lifecycle and exported a genuinely mutated,
UID/BCC-valid 540-byte image. All **46** host tests and both board builds pass after these fixes.
See
[`virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md`](virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md).

Current binary measurements:

| Measurement | Pico 2 W | Pico W |
|---|---:|---:|
| firmware `.bin` | 900,568 bytes | 770,388 bytes |
| `.bss` | 178,020 bytes | 110,784 bytes |

There is no periodic NFC work and no clock change.

## Hardware gate

The staging/commit transaction is now hardware-confirmed. Before rebooting the successful build,
enter config mode and verify:

1. the active tag is marked dirty;
2. downloading it returns the console-mutated image;
3. reloading that image preserves the written game state;
4. input, rumble, audio, motion, reconnect, LED, and BOOTSEL remain unchanged.

The transaction and same-session lifecycle are confirmed. Exported-file power-cycle reload and
config-journal recovery remain before describing Virtual Amiibo persistence as complete. Native
Pro Controller/Joy-Con physical writes remain disabled. See
[`virtual-amiibo-lifecycle-validation-2026-07-25.md`](virtual-amiibo-lifecycle-validation-2026-07-25.md).
