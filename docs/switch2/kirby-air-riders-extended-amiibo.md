# Kirby Air Riders "Figure Player" extended amiibo format

Status: 🔵 Partial — file-format byte map established from community dumps; console read
protocol and firmware support are **Unknown / not implemented**. Needs a hardware capture.
Last updated: 2026-07-26

## Why this exists

Kirby Air Riders amiibo encode a **rider + machine** ("Figure Player") combination, and a real
Switch 2 detects both. Community dump sets (e.g. "Kirby Air Riders amiibo for
Pixl.js/allmiibo/flashiibo") ship one file per rider per machine — four machines each for Kirby,
Meta Knight, King Dedede, and Bandana Waddle Dee. These files do **not** load as normal amiibo in
PicoSwitch2, and this document explains why and what would be required.

## What the files are

- 2048-byte files; meaningful data through offset `0x3FF` (1024 bytes), zero-padded to 2048.
- A standard NTAG215 amiibo structure is present at the start: static lock `0F E0` at `0x0A`,
  capability container `F1 10 FF EE` at `0x0C`, identity block at `0x54`, tag HMAC region at
  `0x34`, data HMAC at `0x80`, encrypted app-data at `0xA0`–`0x1B4`.
- Identity format byte at `0x5B` is `0x03` (classic amiibo use `0x02`), a first signal of a new
  generation/format.
- UID/BCC check bytes are placeholders (do not satisfy the NTAG cascade formula); see the import
  note below.

## Byte map: what encodes rider vs machine

Derived by diffing all 16 files (4 riders × 4 machines), holding one axis constant
(`tools`-free `python` diff, 2026-07-26):

| Region | Varies by | Meaning (inferred) |
|---|---|---|
| `0x01`–`0x05`, `0x54`–`0x59` | rider | UID + amiibo identity block |
| `0x14`–`0x53` | rider | app-data + tag HMAC |
| `0x60`–`0x247` | rider | keygen salt + encrypted data, **extending past the classic 540-byte (`0x21C`) boundary** |
| `0x248`–`0x3C1` | constant | fixed template / zero |
| `0x3C2`–`0x3FF` | **machine** | Figure-Player machine block: a 17-byte hash/HMAC-like value (`0x3C2`–`0x3D2`), a 5-char ASCII code at `0x3D4` (e.g. `B5T42`, `C6V28`, `B4W17`), a discriminator byte at `0x3DC` (`0x02`/`0x04`), and a 2-byte trailer at `0x3FE` |

Key consequences:

- **The machine is real tag data, not container metadata** — it is a distinct, apparently signed
  block. Two machines can share the ASCII code (`Warp Star` and `Winged Star` both show `B4W17`)
  but differ in the hash block and the `0x3DC` byte, so the machine identity is the binary block,
  not the ASCII string.
- **Rider data extends past `0x21C` (540).** The classic NTAG215 amiibo is 540 bytes (572 with the
  originality signature). Here, rider-specific bytes run to `0x247` and machine data to `0x3FF`, so
  the console-relevant image is **larger than a standard amiibo** — roughly 1024 bytes.
- The standard app-data region (`0xA0`–`0x1B4`) is identical across a rider's four machines, so the
  machine is **not** stored in the classic writable save area; it lives only in the `0x3C2+`
  extended block.

## Why PicoSwitch2 does not support these yet

The virtual amiibo store and NFC virtual-reader are built and hardware-validated for the 540/572
NTAG215 format (`VIRTUAL_AMIIBO_RAW_SIZE = 540`, extended 572; a 600-byte console read buffer
fetched as 70-byte chunks — see [`nfc-implementation.md`](nfc-implementation.md)). An extended
~1024-byte tag needs:

1. **A larger store image** and a matching NFC serve path.
2. **The console's read protocol for the extended format**, which is unknown. The captured Switch 2
   read sequence targets a 540-byte amiibo; whether the console reads more pages (and how) for a
   Figure-Player tag has not been observed.

Neither can be implemented safely from the files alone — the read protocol must be observed on real
hardware first.

## Capture audit (2026-07-26): no genuine amiibo read exists to RE from

A full sweep of every capture in the repo and the owner's Downloads was done to check whether the
extended read protocol could be reverse-engineered from existing data. Method: search each capture
for the amiibo capability container `F1 10 FF EE` (present in the leading pages of *every* amiibo
image) and for the four Kirby rider identity signatures. The search was validated against
`dumps/virtual-amiibo-lifecycle-validation-2026-07-25.jsonl`, which correctly shows `f110ffee` and
the NFC descriptor markers.

Result — **no capture contains a genuine amiibo read of any kind**:

| Capture | Content | Amiibo read? |
|---|---|---|
| `usbpcaptures/genuine_procon_2.pcapng` (11 MB) | Pro2 enumeration/input/motion | `F110FFEE` = 0 |
| `usbpcaptures/picoswitch_2_dongle.pcapng` (10 MB) | dongle traffic | `F110FFEE` = 0 |
| `usbpcaptures/genuine_procon2_headset_*.pcap` | audio | `F110FFEE` = 0 |
| `dumps/BLE CAPTURE/*.jsonl,*.ndjson` | motion/gyro/wake | no amiibo |
| `btle_*_wake_console*.pcapng` (Downloads) | BLE wake | `F110FFEE` = 0 |
| `dumps/virtual-amiibo-*.jsonl` | **PicoSwitch2's own virtual (standard 540) tests** | yes, but standard format only |

The only amiibo-bearing captures are PicoSwitch2 serving the standard 540-byte format we already
support. The owner's two large amiibo collections (932 and 1035 files) are all exactly 540 bytes;
the Kirby Air Riders set is the only extended one, and it is dump **files**, not a read capture.

The `.bin` files reveal the tag *data* layout (mapped above) but not the console *read protocol*
for a larger tag, and they carry a standard NTAG215 capability container (`F1 10 FF EE`, which
signals 540-byte memory) despite holding ~1024 bytes — internally inconsistent, which indicates an
emulator-normalized container rather than a faithful raw dump. So the files alone are not a reliable
RE basis either.

**Conclusion: the extended read protocol cannot be reverse-engineered from existing material.** A
new capture of a genuine read is required.

## Smallest useful next experiment (blocking)

With a **physical Kirby Air Riders amiibo** and the dongle, capture a genuine read on a real
Switch 2 through the UART tracer (see [`uart-trace-tooling.md`](uart-trace-tooling.md)):

1. Present the physical amiibo to the Switch 2 with a source controller in the UART-gated relay.
2. Record the full command/response sequence and the total number of bytes/pages the console reads.
3. Compare against the 540-byte read model: does the console issue additional/read-more commands?
   What is the true tag size and page layout?

Define acceptance in advance: the capture is sufficient if it reveals (a) the total read length and
(b) the command sequence for the extended region. Then the store size, the reader-buffer size, and
the chunking can be extended to match, and each rider+machine file can be served as a distinct tag.

Until that capture exists, PicoSwitch2 imports only the leading standard amiibo portion of these
files (the rider) and flags the machine/extended data as unsupported; it must not claim
rider+machine support it cannot serve.

## Current portal behavior

`coerceAmiiboImport()` takes the leading 540-byte NTAG215 image and repairs the placeholder BCC
bytes, so the **rider** imports and is visible. Files with meaningful data past `0x248` are detected
as extended and the import message says the machine/Figure-Player data is not yet supported. Whether
even the rider-only image is console-valid for this new generation is itself unverified pending the
capture above.

## References

- Community dump set: "Kirby Air Riders amiibo for Pixl.js/allmiibo/flashiibo" (2026-02-07),
  README notes even those emulators "need an update before they work."
- [`amiibo-identity-and-generation.md`](amiibo-identity-and-generation.md),
  [`nfc-implementation.md`](nfc-implementation.md), [`nfc-protocol-inventory.md`](nfc-protocol-inventory.md)
