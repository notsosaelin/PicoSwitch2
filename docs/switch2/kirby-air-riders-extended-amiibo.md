# Kirby Air Riders "Figure Player" extended amiibo format

Status: 🔵 Format **identified** (NTAG I2C Plus 2K, "figure v3") from xSke/pixl.js; portal imports
and stores the full tag. Adapter/console **serving is not implemented** (firmware work). Strong
evidence tier — corroborated by a mature open-source amiibo emulator, not yet hardware-validated in
PicoSwitch2.
Last updated: 2026-07-26

## TL;DR (resolved by pixl.js figure-v3 support)

Kirby Air Riders amiibo are **NTAG I2C Plus 2K** tags (2048 bytes), a different chip from the
NTAG215 (540 bytes) all prior amiibo use. This is confirmed by the `preliminary figure v3 support`
work in [xSke/pixl.js](https://github.com/xSke/pixl.js) (an amiibo emulator). It explains every
anomaly: the 2048-byte size, the "invalid" BCC (this chip presents the UID differently), and the
data past the classic 540-byte boundary. The console detects the tag type via `GET_VERSION`, then
reads the 2 KB across two sectors using NTAG I2C sector-select and the SRAM pass-through mechanism.

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

## Format, from pixl.js figure-v3 support (Strong evidence)

Commits `ebaa3e4` (preliminary figure v3 support), `6c36caa` (uid gen), and `3c199ec` (v3 read on
Switch 1) in xSke/pixl.js define the format:

**Chip / identity**
- Tag type `NTAG_I2C_PLUS_2K`, 2048 bytes; the whole file is the tag memory (contiguous NFC pages,
  including the SRAM pass-through pages).
- `GET_VERSION` (0x60) response distinguishes it: `00 04 04 05 02 02 15 03` (NTAG I2C 2K) vs NTAG215
  `00 04 04 02 01 00 11 03`. Byte [6]=`0x15` is the 2 KB storage code.
- **UID has no BCC interleave.** `data[0..6]` = 7-byte UID, `data[7]=0x00`, `data[8]=0x44`
  (internal). The NTAG215 rule (BCC0 at index 3, BCC1 at index 8) does **not** apply — which is why
  a BCC check "fails" on these files. Our Kirby dumps match exactly (`04 b4 43 8a db 1f 90 00 44`).

**Read protocol the console uses** (RF/tag side, spoken by a controller's NFC reader)
- `SECTOR_SELECT` (0xC2) + sector byte, then `READ` (0x30)/`FAST_READ` (0x3A) address
  `full_page = sector*256 + page`. 2 KB spans sectors 0 and 1.
- SRAM pass-through: the Switch 2 `FAST_READ`s the NS_REG at page `0xED` and polls until
  `SRAM_RF_READY` (bit `0x08` of byte [2]) is set, then reads SRAM pages `0xF0`–`0xFF`;
  `FAST_WRITE` (0xA6) writes SRAM.
- `PWD_AUTH` (0x1B), `READ_SIG` (0x3C) as usual.

**Crypto layout shift for v3** (amiitool `tag_to_internal`, `tag_v3` branch): the data-HMAC block is
at tag `0x0C0` (not `0x080`) and the encrypted section at tag `0x0E0` (not `0x0A0`); tag HMAC
(`0x34`), identity (`0x54`), and UID mirror (`0x000`) are unchanged. This `0x40` shift is why rider
data in our diff ran to `~0x247` instead of ending at `0x208`.

## Portal support (done) and firmware support (remaining)

**Portal (implemented):** the library detects the 2048-byte NTAG I2C 2K format, stores the **full**
tag (no truncation, no BCC recompute — the UID is 7 contiguous bytes), parses the UID accordingly,
and keys each entry by content so a rider's four machine variants are four distinct, filename-labeled
entries (Kirby & Warp Star, Kirby & Tank Star, …). Activate is gated with a clear "serving in
progress" message because the firmware cannot yet present a 2 KB tag.

**Firmware (remaining):** the virtual amiibo store and NFC virtual-reader are built for the 540/572
NTAG215 format (`VIRTUAL_AMIIBO_RAW_SIZE = 540`; a 600-byte console read buffer in 70-byte chunks —
see [`nfc-implementation.md`](nfc-implementation.md)). Serving a v3 tag needs:

1. A **2048-byte store image** with a tag-type field, and the config upload path extended to accept
   it (currently validates 540/572).
2. The **controller→console** NFC framing for a 2 KB tag. pixl.js gives the *tag-RF* side (what the
   tag returns to a reader); PicoSwitch2 plays the *reader/controller* side (what the controller
   reports to the console over vendor bulk). The console must learn the tag type (so it issues the
   sector-select/SRAM sequence) and read all 2 KB. The exact vendor-bulk framing for the larger tag
   is the one piece pixl.js does not directly give.

Crucially, PicoSwitch2 can **self-capture** this: implement a preliminary v3 serve path (report the
NTAG I2C 2K type and full 2 KB image), present it on a real Switch 2, and read the UART protocol
trace of what the console actually requests. That closes the loop without needing a separate
physical-amiibo capture — the earlier "capture audit" below documented that no such capture exists,
but with the format now known, an implement-and-trace pass is the practical path.

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

## Next step: implement-and-trace the firmware serve path

The format is known, so the remaining work is firmware, and PicoSwitch2 can RE the last piece
itself:

1. Extend the virtual amiibo store to hold a 2048-byte image with a tag-type field, and accept it in
   the config upload path.
2. Serve it: report the NTAG I2C 2K tag type in the NFC status the console reads, and return the
   full 2 KB. Start from the assumption that the console-facing vendor-bulk framing is the existing
   read model scaled to the larger buffer.
3. Present a v3 tag on a real Switch 2 and read the UART trace of what the console actually requests
   (does it enlarge the read buffer? change the status/type handshake?). Iterate the serve path
   until the console builds the Figure Player.

Acceptance: the console reads all 2 KB and shows the correct rider+machine in Kirby Air Riders.
Preserve the trace under `dumps/` and update this doc with the confirmed controller-side framing.

## Current portal behavior

The library detects the 2048-byte NTAG I2C 2K format and stores the **whole** tag (no truncation,
no BCC recompute; UID parsed as 7 contiguous bytes). Each entry is content-keyed, so a rider's four
machine files become four distinct entries labeled by filename (e.g. "Kirby & Warp Star"). Activate
shows a clear "serving in progress" message rather than attempting a 540/572 upload the firmware
would reject. Import/library are correct and ready; only the firmware serve path remains.

## References

- Community dump set: "Kirby Air Riders amiibo for Pixl.js/allmiibo/flashiibo" (2026-02-07),
  README notes even those emulators "need an update before they work."
- [`amiibo-identity-and-generation.md`](amiibo-identity-and-generation.md),
  [`nfc-implementation.md`](nfc-implementation.md), [`nfc-protocol-inventory.md`](nfc-protocol-inventory.md)
