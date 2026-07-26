# Virtual Amiibo write, eject, re-present, and UART export validation

Date: 2026-07-25
Result: **hardware-confirmed on a real Switch 2**

## Goal

Validate the intended separation between the browser-selected mutable image and the tag currently
presented to the console:

1. a selected Virtual Amiibo is presented when the console starts a scan;
2. console writes update that selected image;
3. successful completion logically removes the tag;
4. a later scan presents the same updated image again;
5. the console-written image can be exported over UART while USB remains attached to the Switch.

## Setup

- Real Switch 2
- Pico 2 W attached to the console over USB
- Independent CP210x UART adapter on `COM11`
- Feature-gated Virtual Amiibo runtime
- 540-byte selected image with UID `04 A4 47 F2 6F 40 80`
- Matching collection source:
  `Super Smash Bros/[SSB] 33 - Charizard.bin`
- NFC-only UART trace

Before the interaction, `amiibo status` reported:

- loaded: true
- dirty: false
- persisted: true
- size: 540
- generation: 4

## Hardware result

The maintainer reported that the complete game interaction worked perfectly. The retained trace
contains all decisive lifecycle boundaries despite the 128-record ring overwriting its earliest
212 records:

- `seq 220`: `0x08` write commit, acknowledged at `seq 221`;
- `seq 223`: completion status `05 00`;
- `seq 224`: Stop;
- `seq 227`: logical absence `07 41`;
- `seq 232` and `238`: later scan commands;
- `seq 235` and `241`: the same UID is freshly present as `09 00`;
- `seq 242`: a new zero-UID `0x06` read begins;
- `seq 246..263`: all nine 600-byte-buffer read chunks are returned;
- `seq 316..327`: a later complete six-chunk, 454-byte write staging transaction;
- `seq 328`: second retained `0x08` commit;
- `seq 331`: completion `05 00`;
- `seq 332`: Stop;
- `seq 335`: logical absence `07 41`.

No one-second present/Stop loop followed the final removal. This confirms the removal event ends
the old loop while a later explicit scan can re-present the still-selected image.

Primary trace:
[`dumps/virtual-amiibo-lifecycle-validation-2026-07-25.jsonl`](../../dumps/virtual-amiibo-lifecycle-validation-2026-07-25.jsonl)

## RAM mutation and UART export

After the interaction, live UART status reported:

- loaded: true
- dirty: true
- persisted: false
- size: 540
- generation: 7

`read_uart_diag.ps1 -Command 'amiibo dump'` then:

1. pulled one generation in bounded 64-byte requests;
2. received exactly 540 bytes;
3. validated the NTAG215 UID/BCC;
4. wrote
   [`dumps/virtual-amiibo-after-write-2026-07-25.bin`](../../dumps/virtual-amiibo-after-write-2026-07-25.bin);
5. acknowledged the download only after the file was safely written.

Post-acknowledgement status retained the same loaded generation and changed dirty from true to
false. It remained `persisted:false`, as intended: console writes do not perform live flash
operations.

The UID uniquely matched one file in the maintainer's 1,035-file collection. Comparison against
that original proves a real mutation:

| Image | SHA-256 |
|---|---|
| Original Charizard | `0A4AE1AD5BBA333FD4D6C530A21691E58AB6A079489823C961DFB54FB0D4345E` |
| UART-exported image | `AC42AA8B69C4E0099ED6D03E69E0DB399243EE0413579A21E172B2BAA2500252` |

There are 426 changed bytes in three bounded ranges: `17..18`, `20..51`, and `128..519`. UID and
manufacturer bytes remain unchanged, as do bytes `520..539` outside the allowed console-write
area.

## Confirmed behavior

- Multi-packet `0x14` RX reassembly no longer crashes the console.
- Complete staging, atomic commit, and `05 00` completion work on hardware.
- Committed Stop produces logical tag removal and `07 41`.
- The next scan re-presents the same selected, updated image.
- The updated image is readable again by the console in the same powered session.
- UART can export the live image without moving the console-facing USB connection.
- Dirty acknowledgement occurs only after a validated PC file write.

## Still open

- Reload the exported file after a power cycle and confirm game-visible save state.
- Hardware-test explicit config-mode flash persistence and journal recovery for the modified image.
- Add manual portal Eject/Present controls.
- Capture and implement native physical-tag writes.
