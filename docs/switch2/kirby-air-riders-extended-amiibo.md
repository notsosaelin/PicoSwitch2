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

## Firmware implementation plan (staged; existing NFC must stay byte-identical)

Two hard realities shape the plan:

- **Layer.** PicoSwitch2 is the *controller/reader* to the console, not the RF *tag*. pixl.js
  implements the tag-RF protocol (READ `0x30`, `SECTOR_SELECT 0xC2`, SRAM). PicoSwitch2 serves the
  console over the controller vendor-bulk NFC protocol (`0x03/0x05/0x06/0x15`, a 600-byte reader
  buffer in 70-byte chunks). The console-facing framing for a 2 KB tag is **not** in pixl.js and is
  the one genuine unknown — it must be observed on hardware.
- **Flash.** Each amiibo journal bank is one 4 KB sector (offsets −3/−5, config at −4, all within
  the 5-sector install-reset region). A 2 KB tag record (~4128 B) needs two sectors per bank, which
  does not fit without relocating the whole persistence map — the validated, install-reset-critical
  area. So v3 must **not** touch that layout in early phases.

**Safety invariant for every phase:** all changes gate on a v3 tag-type flag; the 540/572 NTAG215
store, flash journal, and read path stay byte-identical; all host tests and the `NS2_PRO=OFF` gate
pass at each step.

### Phase 1 — RAM-only v3 slot + present-and-trace (no flash change) — IMPLEMENTED
- Isolated RAM slot in `virtual_amiibo_store.c` (`virtual_amiibo_store_v3_*`), separate from
  `virtual_amiibo_t` and the flash journal, so the 540 store and persistence are untouched. Session-
  scoped; cleared on power cycle.
- Config upload accepts a 2048-byte image: `amiibo begin 2048 <crc>` routes to the v3 slot;
  `chunk`/`commit` follow the active v3 upload. Validated by `ns2_amiibo_v3_valid` + CRC.
- Serve: when a v3 image is loaded, `ns2_v3_serve()` in `switch_pro2.c` handles the console NFC
  commands ahead of the 540 runtime — reports the tag present with the 7-byte UID (`0x05`), begins a
  read (`0x06`), and serves the full 2048-byte image in chunks (`0x15` + little-endian offset) via
  `ns2_virtual_nfc_build_buffer_chunk`; the HID report's NFC event byte (`0x0C`) carries a v3 event
  counter so the console notices the tag. Writes are not handled (trace-only).
- Every console command is already logged by `ns2_dispatch()`'s tracer, and `ns2_v3_serve` traces
  its replies, so a live present captures the exact controller→console read sequence for a 2 KB tag.
- **What the trace will show** (the remaining unknown): whether the console reads only ~600 bytes
  (treating it as NTAG215 and likely rejecting), reads further via `0x15` offsets (revealing a
  larger read length), or issues a new type/sector handshake. Any of recognize / reject / crash is
  useful. Feed the result into Phase 2.

### Phase 1 trace result (2026-07-26) — console stalls before reading

First live UART NFC trace of the v3 present-and-trace stub on a real Switch 2
(`dumps/kirby-v3-serve-trace-2026-07-26.jsonl`, filter `nfc`, 128-record ring, 22
overwritten — i.e. the console kept retrying). The console loops:

```
0x03 scan  -> ACK
0x05 status-> tag present + UID 04 98 8B 22 AB 1F 90   (status byte 0x09 ready)
0x06 begin -> request data D0 07 00 00 00 00 00 00 00 01 03 00 …  (read descriptor:
              D0 07 header + zero UID = READ, same descriptor the 540 path expects)
             ...our stub returned a BARE ACK...
[~3000 ms timeout] 0x04 stop -> 0x03 scan -> repeat, forever
```

**Root cause:** the console never sent `0x15` (chunk read). Comparing to the proven
540 runtime (`ns2_virtual_nfc_runtime.c`), a read is `0x06 begin → status goes
ACTIVE (0x04) + the HID NFC event counter (input report byte `0x0C`) bumps → the
console then pulls the read buffer with `0x15``. The stub answered `0x06` with a
bare ACK and left status at `0x09`/the event counter unchanged, so the console
saw no operation progress, timed out after 3 s, and rescanned.

**Confirmed:** the console reads a v3 tag with the **same** vendor-bulk command set
as a 540 tag (`0x03/0x04/0x05/0x06/0x15`) and the **same** `D0 07`/zero-UID read
descriptor — there is no separate 2 KB handshake at this layer. The size
difference is expected to surface only in how many `0x15` chunks it pulls.

**Fix (build-verified 2026-07-26, hardware test pending):** `ns2_v3_serve` now
drives that state machine — validates the `D0 07`/zero-UID descriptor on `0x06`,
builds a `60-byte prefix + 2048-byte image` read buffer (mirroring the 540
layout, UID at prefix +8, console operation metadata at +51), goes ACTIVE, bumps
the event counter, and serves the buffer over `0x15` in ≤70-byte chunks. The
32-byte originality signature (prefix +0x13) is still zero — **the next trace
question** is whether the console reads all ~2108 buffer bytes and then accepts,
or validates the signature/crypto and rejects (error 2115-0088).

### Phase 2 trace result (2026-07-26) — console reads the tag, then rejects it

Second live trace after the state-machine fix
(`dumps/kirby-v3-serve-trace-2b-2026-07-26.jsonl`, 54 records, **0 overwritten** —
a clean, complete read, no more retry storm). The console advanced all the way:

```
0x03 scan -> 0x05 status (tag present, UID 04 B4 43 8A DB 1F 90)
0x06 begin (D0 07 read descriptor) -> ACTIVE
15x 0x15 chunk reads at offsets 0, 70, 140 … 980 (step 70) = 1050 buffer bytes
   = our 60-byte prefix + ~990 image bytes (through image ~0x3DE)
0x05 status -> 0x04 stop -> rescan
```

Console result: **error 2115-0176 "This is not an amiibo"** (changed from the
earlier 2115-0088). Confirmed facts:

- **The serve path works.** The console recognizes a tag, begins a read, and pulls
  chunks — the fix cleared the stall. It read ~1024 bytes (15×70), i.e. more than a
  540 amiibo's ~9 chunks, so it is doing an *extended* read, not a 540 read.
- The chunk data was served correctly (prefix + tag bytes verified against the
  known layout: `0F E0` lock at 0x0A, `F1 10 FF EE` CC at 0x0C, identity at 0x54).
- The console **never received a `last=1` flag** yet stopped at 15 chunks on its
  own, so its read length is intrinsic (it decided ~1024 bytes was the tag), not
  driven by our buffer size.
- Rejection is a **validation** failure, not a transport failure.

Open question — is the reject (a) the community dumps' **crypto/structure** (they
are emulator-normalized, not faithful raw dumps — see the capture audit above; the
Switch 2 is known to validate amiibo crypto, see
[amiibo-identity-and-generation.md](amiibo-identity-and-generation.md)), or (b) a
**read-length/format** gap (the console stopped at image ~0x3DE, 33 bytes short of
the machine block end 0x3FF, and we do not yet know the field that sets its read
length)? The originality signature (prefix +0x13) is ruled out as the sole cause:
plain 540-byte dumps with a zero signature already validate on this console.

Next experiments: (1) trace the **System Settings → amiibo** read for comparison
(different reader, may read a different length); (2) obtain a **faithful raw** v3
dump (not emulator-normalized) to separate crypto-invalid input from a serve gap;
(3) decode the 60-byte prefix fields against a genuine 540 read to find the tag
size/type field, then describe a true 2 KB tag so the console reads the full 2048.

**Experiment 1 result (2026-07-26) — System Settings reads identically**
(`dumps/kirby-v3-serve-trace-sysmenu-2026-07-26.jsonl`, two scans). The System
Settings amiibo reader issues the **same** sequence and the **same** read length
as Kirby Air Riders: per read operation, `0x03/0x05/0x06` then exactly **15 ×
0x15** chunks at offsets `0,70,…,980` (identical to the Kirby read), then reject.
So the ~1024-byte read length is intrinsic to the console's amiibo reader, not
app-specific, and both independent readers reject the same served bytes. This
rules out a Kirby-specific quirk and points the remaining failure at **amiibo
validation of the tag content itself**. Combined with the known crypto-validation
wall and the capture audit's finding that these community files are
emulator-normalized (not faithful raw dumps), the leading hypothesis is that the
**dumps are not valid to the console's amiibo verifier** — the same wall that
rejects generated 540 tags — and a faithful raw dump of a genuine Kirby Air
Riders amiibo is the gating dependency to confirm the serve path end-to-end.

Still not fully excluded: the console read image ~0x3DE (34 bytes short of the
machine-block end 0x3FF) because our 60-byte prefix consumes part of its fixed
~1050-byte read window. If a faithful dump also reads short and rejects, revisit
whether v3 needs a different prefix size so the fixed read window covers the whole
1024-byte tag region.

**Experiment 2 result (2026-07-26) — clean accepted read after dropping the prefix**
(`dumps/kirby-v3-serve-trace-3-noprefix-2026-07-26.jsonl`). After serving the raw
1024-byte sector 0 with **no 60-byte prefix**, the console read:

```
0x03 scan -> 0x05 status -> 0x06 begin -> 15x 0x15 at offsets 0..980
15th chunk (offset 980): last=1, len=44  -> 980+44 = 1024 = full sector 0
0x05 status -> (stop; no rescan loop, no error)
```

**No error** (previously 2115-0176), and the scan→stop→rescan retry storm is gone —
the console accepted the completed read. This confirms the read protocol for a
2 KB tag over the controller vendor channel:

- The console reads the vendor read buffer **until the `last=1` chunk flag**.
- For a 2 KB tag it reads **sector 0 = 1024 bytes** (15 chunks) and the tag must be
  served **raw at offset 0 with no 540-style prefix**, so `last=1` lands on chunk
  15 with the whole aligned image (identity, crypto, machine block, 0x3FE trailer).

**Experiment 3 result (2026-07-26) — console recognizes the tag, then crashes**
(`dumps/kirby-v3-serve-trace-sysmenu-crash-2026-07-26.jsonl`,
`dumps/kirby-v3-serve-trace-4-full2048-2026-07-26.jsonl`). After the clean
sector-0 read the console does **not** reject — instead:

- **Kirby Air Riders:** scanning kicks back to the menu (the game processes the
  amiibo), then closing the game **crashes with 2011-0301**.
- **System Settings:** scanning **crashes immediately with 2011-0301**.

The trace shows only `0x03/0x05/0x06` + 15× `0x15` + a status poll — **no writes,
no re-reads** — before the crash, so it is a crash in the console's *processing*
of the tag, not a write path.

Two hard facts nailed down:

1. **The console caps the vendor read at 15 chunks (~1024 B = sector 0).** Serving
   the full 2048-byte image did not help: the console still stopped at chunk 15
   (offset 980) and never requested chunk 16, even with no `last=1`. It cannot
   pull sector 1 / SRAM through `0x15`.
2. **It recognizes the v3 amiibo from sector 0** (Kirby reaches a menu; the earlier
   "not an amiibo" reject is gone), then crashes needing something sector 0 alone
   plus our current status does not provide.

Leading suspects for the crash, in order — all require the **2 KB vendor framing
that pixl.js (RF-only) does not contain**:
- the **SRAM pass-through** handshake a real Pro Controller 2 performs for a 2 KB
  tag (Switch 2 polls NS_REG page 0xED for `SRAM_RF_READY`, then reads SRAM pages
  0xF0–0xFF — pixl.js `ntag_emu_v2.c`), relayed to the console over vendor
  subcommands we have not identified;
- the **originality signature** (RF `READ_SIG`), which a 540 tag does not require
  but a 2 KB tag may; we removed the 60-byte prefix that carried it and have no
  other channel for it;
- the **`nfc_identity` (0x0C)** query the console issues for a 2 KB tag, which we
  currently only bare-ACK.

### Crash root cause found (2026-07-26): SRAM_RF_READY was never signalled

Resolved from the dumps + pixl.js alone — **no genuine-amiibo capture required.**

NTAG I2C 2K keeps its session register **NS_REG in page `0xED`** (byte offset
`0xED*4 = 0x3B4`); **byte 2 bit `0x08` is `SRAM_RF_READY`**. The SRAM window is
pages `0xF0–0xFF` = **offsets `0x3C0–0x3FF`** — i.e. **exactly the Figure Player
machine block**. The Switch 2 **polls `SRAM_RF_READY` and only reads the SRAM
window once the bit is set** (pixl.js `ntag_emu_v2.c`, both the `READ` path for
page `0xEC` and the `FAST_READ` path for page `0xED`: `ed_page[2] |= 0b1000`,
commented "switch 2 will poll until this is set, and *then* read sram pages").

Genuine dumps store the bit **clear** — verified in the Kirby files: page `0xED`
at `0x3B4` = `08 01 21 00`, byte 2 = `0x21`, bit `0x08` **not** set. Emulators
therefore raise it **dynamically on every read** rather than storing it set. We
were serving the raw stored bytes, so the console saw SRAM never become ready
after recognizing the tag → **2011-0301**.

**Fix:** `ns2_v3_serve` now sets `image[0x3B6] |= 0x08` on the **served copy**
(stored flash image untouched), matching pixl.js exactly. Audit of the emulator
confirms this is the *only* dynamic read-side transformation for a 2 KB tag — all
other special-casing there is on the write path. Build-verified; hardware test
pending.

Also keep in mind: do not restore the 60-byte prefix (it truncates sector 0 within
the 15-chunk cap and regresses to "not an amiibo").

### Served-data integrity verified byte-exact (2026-07-26)

Cross-checking `dumps/kirby-v3-serve-trace-5-sramready-2026-07-26.jsonl` against the
source dump (`BWD & Winged Star.bin`, matched by comparing all chunks — the four
BWD variants share a UID, so the machine block at `0x3D4` is what distinguishes
them): **0 mismatches across all 15 chunks**, accounting for the injected
`SRAM_RF_READY` bit. Every byte the console received is identical to the dump that
works on pixl.js/flashiibo. The crash is therefore **not** corrupted, misaligned,
or truncated data — the console receives a faithful sector 0 and still crashes.

### Next experiment: mirror a GENUINE controller (no genuine amiibo needed)

The repo already has the instrument for this: the **UART-gated NFC mirror**
(`ns2_nfc_mirror_*`, `src/bt_hid/bt/btstack/btstack_host.c`), which forwards the
console's command-`0x01` NFC packets to a **connected genuine Pro Controller 2**
over Bluetooth and returns that controller's **genuine replies**, all visible to
the protocol tracer.

Presenting an existing flashiibo/pixl.js-emulated Kirby tag to the genuine
controller therefore captures the **real controller→console framing for a 2 KB
tag** without owning a genuine Kirby amiibo. Requirements: a genuine Switch 2
controller connected over BT (PID `0x2066/0x2067/0x2069/0x2073`), `nfcmirror on`,
mirror state `ACTIVE`, and **the virtual amiibo slot ejected** — otherwise
`ns2_virtual_nfc_dispatch_usb()` serves the tag itself and the mirror never sees
the commands.

Decisive question the capture answers immediately, from chunk offset 0's first
bytes: does a genuine controller send the **60-byte prefix** for a 2 KB tag
(`04 00 00 00 01 02 00 07 <uid>`) or the **raw tag** (`04 2a 35 …`)? Plus the
chunk count, `last` flags, and the `0x05` status bytes (where a tag-type/version
field we currently zero would live).

### Phase 2 — serve the confirmed protocol (hardware loop)
- Implement the console-facing read/response for the traced framing so a real Switch 2 builds the
  Figure Player. Iterate against captures until rider+machine are correct in Kirby Air Riders.

### Phase 3 — persistence — IMPLEMENTED
- The v3 slot now persists in the **existing amiibo journal banks**, not a new region: the record
  header gains version 3 with a single 2048-byte payload, and `VIRTUAL_AMIIBO_RECORD_SIZE` grew to 9
  pages (2304 B) to hold it — still within one 4 KB bank sector, so no flash-layout change and the
  install-reset region is unchanged (a fresh UF2 still wipes it).
- **Mutual exclusion:** only one amiibo (540/572 v2 OR 2048 v3) is ever stored. Committing a v3 tag
  clears the 540 store and vice-versa; the journal erases both banks on the type switch so the old
  format can never out-rank the new one at boot. `init` loads a v3 record if present, else the 540
  record. Eject (`amiibo clear`) wipes both banks and both slots.
- This is why a v3 tag survives moving the dongle between the PC (config upload) and the Switch — it
  is written to flash on commit, exactly like a 540 tag.

Each phase is a separate commit with host tests; Phases 1–3 each need a flash-and-test round with
real hardware because the console behavior cannot be validated any other way.

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
no BCC recompute; UID parsed as 7 contiguous bytes). Each combo is content-keyed, so a rider's four
machine files are four distinct stored entries. In the carousel they **collapse to one item per
rider** (the four machines share one catalog identity and one static AmiiboAPI image, so showing
four look-alikes was noise). The carousel control row is centered as `← Load Amiibo · Swap Combo →`;
**Swap Combo** (shown only for a multi-combo rider) cycles the active machine and the combo name
(from the filename, e.g. "Kirby & Warp Star") appears in the detail box. Each rider defaults to its
first combo on arrival. Workflow: navigate to the rider → Swap Combo to the machine you want → Load
Amiibo → Activate Amiibo. Activate shows a "serving in progress" message rather than attempting a 540/572
upload the firmware would reject. Import/library are correct and ready; only the firmware serve path
remains.


## Source review (2026-07-27) — full sweep of the published research

Compiled from every published source rather than inference. **Primary source:
xSke, [N3evin/AmiiboAPI#243 comment 3591686037](https://github.com/N3evin/AmiiboAPI/issues/243#issuecomment-3591686037).**

- Tags are **NTAG I2C Plus 2K**. The data format is standard amiibo **plus 64 bytes
  of random data inserted between pages 0x20-0x30 (bytes `0x80`-`0xA0`)**. The
  encrypted sections shift by `0x40` accordingly (HMAC -> `0xC0`, data -> `0xE0`),
  and the data still spans to page `0x92`. xSke: *"This seems unused for now? If I
  change it to all-zeros and emulate a bin, it loads fine."*
- **The standard amiibo keys are unchanged.** xSke: *"If you cut out the new
  64-byte random data buffer from the middle, it'll encrypt and decrypt fine with
  eg. pyamiibo."* Corroborated by
  [bettse/amiitool b066e85](https://github.com/bettse/amiitool/commit/b066e85d344355224cd0390064f9b5a995b47b2d),
  which threads a `tag_v3` flag applying exactly that `0x40` shift.
- **Backwards compatibility is built into the console.** xSke: *"I've already
  looked through the Switch 1 NFC sysmodule, and it does have handling for
  skipping the extra data chunks and reading standard Amiibo data from the right
  offsets, which is how they're backwards compatible - but I haven't found any
  code for handling the SRAM data or any extra chunks."*
- Config pages are `0xE2`-`0xE6`; the **SRAM buffer is pages `0xF0`-`0xFF`**. The
  console FAST_WRITEs a request block there, polls page `0xED` until
  `SRAM_RF_READY` (byte 2, bit 3) is set, then FAST_READs the response. xSke
  advises a static dump *"hardcode page 0xED ... with `0x29` in the third byte ...
  otherwise it's going to be polling forever"* — matching our earlier fix.
- **The machine identity lives only in the SRAM block**; the amiibo **ID is
  rider-only** (Kirby & Warp Star and Kirby & Winged Star share
  `1F00000004C41E03`). The SRAM block is authenticated by an unknown checksum over
  its first 26 bytes.
- Kirby Air Riders writes its own data to pages `0x9A`-`0xA1` and `0x11A`-`0x131`,
  encrypted with keys that would require a Switch 2 softmod. **Not recoverable.**
- [solosky/pixl.js#381](https://github.com/solosky/pixl.js/pull/381) is merged and
  xSke tested emulation on a real Switch 2 with the game. Crucially it emulates the
  **tag over RF**, where the reader learns the type from `GET_VERSION`
  (`00 04 04 05 02 02 15 03`) and uses `SECTOR_SELECT`. PicoSwitch2 is the
  **controller**, so none of that RF signalling is available to us: the console
  tells *us* which pages to read.

### Why every attempt so far failed

The `0x06` descriptor (decoded in `dumps/research/ndeadly-switch2-research.json`)
is `D0 | uid_len | uid | McuTagType | block_count | (start,end) x N`. The console
sends `McuTagType 1 (NTAG 215)` and asks for pages `0x00-0x86` — **exactly 540
bytes, in NTAG215 layout**. We were answering with **raw v3 bytes** at those
offsets, whose encrypted regions sit `0x40` higher, so the console's standard
crypto could never validate them. Sweeping the advertised type byte changed
nothing because no field we control alters the console's chosen page set.

### Fix: serve the NTAG215-compatibility view

Reconstruct the classic layout by removing the inserted block:

```
standard[0x000..0x07F] = v3[0x000..0x07F]   (128 B)
standard[0x080..0x21B] = v3[0x0C0..0x25B]   (412 B)   -> 540 bytes
```

`tools/test_ns2_v3_compat_view.mjs` proves over **all 16 dumps** that
`tag_to_internal(compat, v3=false)` is **byte-identical** to
`tag_to_internal(full_v3, v3=true)` — so a console validating this view as a plain
NTAG215 computes exactly the HMACs the real tag was signed with. This is the same
path the console's own sysmodule uses for backwards compatibility.

**Expected scope:** the rider amiibo should be recognised and registrable (owner /
nickname) as a standard amiibo. The **machine/Figure Player cannot** be conveyed —
it lives in the SRAM block, which the console only requests after learning the tag
is 2 KB, and we have no confirmed way to signal that over the controller protocol.
That remains the one genuinely open question.

## References

- Community dump set: "Kirby Air Riders amiibo for Pixl.js/allmiibo/flashiibo" (2026-02-07),
  README notes even those emulators "need an update before they work."
- [`amiibo-identity-and-generation.md`](amiibo-identity-and-generation.md),
  [`nfc-implementation.md`](nfc-implementation.md), [`nfc-protocol-inventory.md`](nfc-protocol-inventory.md)
