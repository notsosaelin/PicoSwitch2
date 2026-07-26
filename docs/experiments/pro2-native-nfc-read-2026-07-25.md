# Genuine Pro Controller 2 native NFC read through PicoSwitch2

Date: 2026-07-25
Result: **successful — the Switch 2 recognized the physical amiibo**

## Purpose

Determine the actual console-facing and controller-facing NFC transaction while a genuine Switch 2
Pro Controller is paired to PicoSwitch2 and the dongle remains attached to a real Switch 2.

This is primary evidence. The UART USB trace and BLE capture came from the same live transaction;
the console-recognition result was reported directly by the maintainer.

## Diagnostic path

- Personality: Pro Controller 2
- Source: genuine Switch 2 Pro Controller, PID `0x2069`
- UART gate: `nfcmirror on`
- USB trace: NFC/bulk command and response records
- BLE bridge:
  - command writes to value handle `0x0016`, with the genuine 33-byte zero prefix;
  - the bridge subscribed to `0x001E`/CCC `0x001F`;
  - ordinary matching NFC replies were actually observed on the already-active primary response
    value handle `0x001A`;
  - only the transport/envelope fields were translated between USB and BLE.

The bridge remained asynchronous and bounded. The successful transaction recorded 20 submitted
commands, 20 genuine responses, no timeouts, and no rejected responses.

## Confirmed command sequence

The successful read included:

1. `0x01/0x03` — enter/configure scan
2. `0x01/0x05` — status, initially `09 00` with the seven-byte UID
3. `0x01/0x06` — begin read with a zero UID in the operation descriptor
4. `0x01/0x05` — operation status `04 00`
5. nine `0x01/0x15` offset reads
6. `0x01/0x05` — operation status remains `04 00`
7. `0x01/0x04` — stop/leave scan

The genuine input report NFC state advanced `0 → 1 → 2 → 3`.

## Offset-read framing

Contrary to the earlier full-USB-response hypothesis, the Switch itself requested bounded chunks
over USB. Each request data field was a two-byte little-endian offset:

```text
0000 0046 008C 00D2 0118 015E 01A4 01EA 0230
```

Each response data field was:

```text
[last:u8][length:u16le][data]
```

- The first eight chunks had `last=0`, `length=0x0046` (70 bytes).
- The final `0x0230` chunk had `last=1`, `length=0x0028` (40 bytes).
- Total reader buffer: `8 × 70 + 40 = 600` bytes.
- Total USB response size was 81 bytes for a full chunk and 51 bytes for the final chunk, including
  the standard eight-byte command envelope.

The reader buffer layout is:

| Offset | Size | Meaning |
|---:|---:|---|
| `0` | 4 | fixed header beginning `04 00 00 00` |
| `4` | 4 | identity/type fields `01 02 00 07` |
| `8` | 7 | tag UID |
| `15` | 4 | zero/reserved |
| `19` | 32 | originality-signature field |
| `51` | 9 | bytes 10–18 of the preceding `0x06` operation descriptor |
| `60` | 540 | raw NTAG215 image |

This layout is the previous 603 meaningful bytes with the unverified three-byte full-response
prefix removed. There is no 19-byte trailer in the console-requested reader buffer.

## Consequences

- USB and BLE do **not** require different full-versus-chunk read models for this transaction.
- Native Pro Controller 2 physical-tag reads are feasible through an asynchronous command bridge.
- The virtual reader should build the same 600-byte buffer and answer the same offset requests.
- The generic large vendor-IN pump may remain for safety and future commands, but it is not needed
  for the confirmed read path.
- Native writes, removal during a transaction, reconnect during a transaction, and Joy-Con 2 Right
  behavior remain unvalidated and must not be inferred from this read.
