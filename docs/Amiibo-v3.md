# Figure-v3 amiibo (NTAG I2C Plus 2K) — protocol reference

**Status: ✅ Read and write are hardware-confirmed on a real Switch 2**, for both ordinary v3
figure data and the Kirby Air Riders extended (game-data) operations. All 16 available Air Riders
v3 dumps completed real-console reads and writes. Power-cycle recovery, second reuse, and
learned-gameplay-state saves are confirmed.

Last updated: 2026-07-29.

This is the authoritative reference for 2048-byte "figure v3" amiibo support in PicoSwitch2 — the
chip, the console-facing wire protocol, the storage geometry, and what remains unknown. Dated
primary evidence lives in `docs/experiments/` and `dumps/`; §14 lists the claims that were made and
refuted along the way so they are not re-derived.

**One remaining lifecycle check:** production-portal **Sync amiibo** of the intentionally retained
dirty generation, acknowledging dirty state only after IndexedDB persistence succeeds.

---

## 1. The chip and the format (Confirmed)

Primary source: **xSke**,
[N3evin/AmiiboAPI#243 comment 3591686037](https://github.com/N3evin/AmiiboAPI/issues/243#issuecomment-3591686037).
Corroborated by [bettse/amiitool b066e85](https://github.com/bettse/amiitool/commit/b066e85d344355224cd0390064f9b5a995b47b2d)
and [solosky/pixl.js#381](https://github.com/solosky/pixl.js/pull/381) (merged; xSke verified
emulation on a real Switch 2 with the game).

| Property | Value |
|---|---|
| Chip | NTAG I2C Plus 2K — 2048 bytes = sector 0 (1024) + sector 1 (1024) |
| `GET_VERSION` | `00 04 04 05 02 02 15 03` (vs NTAG215 `00 04 04 02 01 00 11 03`; byte [6] `0x15` is the 2 KB storage code) |
| UID | 7 contiguous bytes, **no BCC interleave**; `data[7]=0x00`, `data[8]=0x44` |
| v3 delta | **64 bytes inserted at `0x80`**; encrypted HMAC `0x80`→`0xC0`, data `0xA0`→`0xE0`, data ends page `0x92` (`0x248`) |
| Keys | **Standard retail amiibo keys, unchanged** |
| Identity | `0x54`; **format byte `0x5B` = `0x03`** (standard amiibo = `0x02`) |
| Config pages | `0xE2`–`0xE6` (moved from the NTAG215 positions) |
| SRAM buffer | pages `0xF0`–`0xFF` = bytes `0x3C0`–`0x3FF` |

The `0x40` crypto shift is why rider data runs to `~0x247` instead of ending at `0x208`. Tag HMAC
(`0x34`), identity (`0x54`), and the UID mirror (`0x000`) are unchanged.

The console's own NFC sysmodule has backwards-compatibility handling that skips the inserted chunk
and reads standard amiibo data from the shifted offsets. `tools/test_ns2_v3_compat_view.mjs` proves
over all 16 dumps that removing the inserted block yields a 540-byte image whose
`tag_to_internal(compat, v3=false)` is **byte-identical** to `tag_to_internal(full_v3, v3=true)`.

### 1.1 Rider versus machine

**The amiibo ID is rider-only.** Kirby & Warp Star and Kirby & Winged Star are both
`1F00000004C41E03`. The machine ("Warp Star" etc.) exists **only in the SRAM block**, outside amiibo
crypto.

Byte map, derived by diffing all 16 files (4 riders × 4 machines) holding one axis constant:

| Region | Varies by | Meaning |
|---|---|---|
| `0x01`–`0x05`, `0x54`–`0x59` | rider | UID + amiibo identity block |
| `0x14`–`0x53` | rider | app-data + tag HMAC |
| `0x60`–`0x247` | rider | keygen salt + encrypted data, extending past the classic 540-byte (`0x21C`) boundary |
| `0x248`–`0x3C1` | constant | fixed template / zero |
| `0x3C2`–`0x3FF` | **machine** | Figure-Player machine block (see below) |

The four machine variants of a rider are byte-identical across the whole encrypted body and differ
only inside `0x3C0..0x3FF`:

```
Kirby & Shadow Star   0200 D5403A7B9CE69F88983EF9B0 6200 2D 00 05 "PB5T432" 01 01 04 000000
Kirby & Tank Star     0200 8BFBA2A22B0D0ABA21162C9E 2D00 61 00 09 "PC6V628" 01 01 04 000000
Kirby & Warp Star     0200 4C980F696FCF5128F89ED4B5 AB00 9C 00 01 "PB4W717" 01 01 02 000000
Kirby & Winged Star   0200 93ADD81D37051AA353841E0B 4400 9F 00 07 "PB4W717" 01 01 04 000000
genuine Warp Star     0200 732AB41C4AC291B9A5983C03 9400 C9 00 0A "PB4W717" 01 01 02 000000
```

All four downloaded variants share one UID (`04B4438ADB1F90`) — they are the *same physical figure*
re-configured four times, not four figures. The 12-byte blob at `0x3C2` is **per-unit** (it differs
even between the genuine and downloaded Warp Star); the ASCII product code plus the `01 01 0X` byte
is **per-machine**. Two machines can share the ASCII code (Warp Star and Winged Star are both
`PB4W717`) but differ in the blob and the `01 01 0X` byte, so machine identity is the binary block,
not the ASCII string.

The standard app-data region (`0xA0`–`0x1B4`) is identical across a rider's four machines, so the
machine is **not** in the classic writable save area.

### 1.2 The RF layer (context only — not our wire)

What a real reader does, for orientation: `SECTOR_SELECT` (`0xC2`) + sector byte, then
`READ`/`FAST_READ` addressing `full_page = sector*256 + page`. For SRAM: `FAST_WRITE` a request
block to `0xF0`–`0xFF`, poll page `0xED` until `NS_REG` byte 2 bit 3 (`SRAM_RF_READY`) is set, then
`FAST_READ` `0xF0`–`0xFF`. Genuine dumps store that bit **clear** (`0x21`); emulators raise it
dynamically (`0x29`), and PicoSwitch2 injects it while serving and restores it before commit.

**None of these commands appear on the controller↔console wire.** See §2.

## 2. Why the layer matters

pixl.js and Flashiibo emulate the **tag** over RF. A genuine controller is their reader; it issues
`GET_VERSION`, learns the chip is a 2K part, and performs the sector selects and SRAM handshake
itself.

PicoSwitch2 emulates the **controller**. The console tells *us* which pages to fetch, and we
synthesize the read-buffer prefix a genuine controller would have built.

| | pixl.js / Flashiibo | PicoSwitch2 |
|---|---|---|
| Emulates | the **tag** (RF side) | the **controller** (USB/BLE side) |
| Who builds the 60-byte read prefix | a genuine Pro Controller 2, by reading the tag | **we** must synthesize it |
| Must answer `READ_SIG`, `SECTOR_SELECT` | yes, as a tag | n/a |

This is why their reference implementation cannot be copied directly — it solves the adjacent
problem — and why the v3 signal turned out to live in a field we generate rather than in the tag
data.

Flashiibo Pro firmware `Pro_Firmware_OTA_26.7.2` contains `pixljs.bin`: Flashiibo Pro **is**
pixl.js, so PR #381 is the direct reference implementation rather than an analogue.

## 3. Console-facing protocol map (Confirmed)

Top-level command `0x01` = MCU/NFC.

| Sub | Meaning | v3-only |
|---|---|---|
| `0x03` | start polling — `00 <timeout u16le> <interval u16le>` | |
| `0x04` | stop polling | |
| `0x05` | MCU/NFC state — 61-byte payload | |
| `0x06` | read device (issue tag read) | |
| `0x08` | commit staged ordinary write | |
| `0x14` | write buffer (stages a device command **or** a tag write — see §3.3) | |
| `0x15` | read buffer — `[last:u8][len:u16le][data ≤70]` | |
| `0x16` | empty-state status reported after a genuine `0x20` completion | ✅ |
| `0x18` | result-buffer state for a staged device command | ✅ |
| `0x1E` | sector-aware extended read (written-tag reuse) | ✅ |
| `0x20` | extended (Air Riders game-data) operation terminator | ✅ |
| `0x21` | execute the staged SRAM device command | ✅ |

`0x1E`, `0x20`, and `0x21` appear in **no** NTAG215 flow. Counted across every capture in `dumps/`:
the NTAG215 control, the working 540 read/write, and the whole `virtual-amiibo-*` lifecycle set
contain zero occurrences; the v3 captures contain several each.

### 3.1 The `0x06` read descriptor

```
[0..1] timeout u16 LE   [2..8] UID   [9] tag type   [10] block count   [11..] (start,end) × N
```

> ⚠️ Bytes 0–1 are a **timeout**, not a fixed `D0 07` marker. `D0 07` = 2000 ms; the extended
> descriptor uses 3000 ms (`B8 0B`). A gate that required literal `D0 07` silently rejected every
> extended descriptor. Gate on structure, accept any timeout.

Observed descriptors in one genuine v3 session:

| # | Timeout | Blocks | Ranges | Size |
|---|---|---|---|---|
| 1 | 2000 ms | 3 | `00-3B, 3C-77, 78-86` | 540 B — the NTAG215 set |
| 2 | 3000 ms | **4** | `00-3B, 3C-77, 78-91, E2-E6` | **604 B** |
| 3 | 2000 ms | 1 | `03-03` | 4 B — the targeted pre-write read |

The NTAG215 control issues **only** descriptor #1 and stops. The escalation is v3-specific and
happens *after* the first read.

### 3.2 The read buffer and its prefix

Reply layout: `[8-byte header][flags, len16][60-byte prefix][tag image]`.

| Off | Size | Meaning |
|---:|---:|---|
| 0 | 4 | `04 00 00 00` (result type `0x04` = tag read) |
| 4 | 4 | identity/type `01 02 00 07` |
| 8 | 7 | UID |
| 15 | 3 | zero |
| **18** | **1** | **v3 escalation byte — `0x06`** |
| 19 | 32 | originality-signature slot (zero is accepted; see §14) |
| 51 | 9 | echo of the `0x06` block count and ranges |
| 60 | … | tag image |

**Prefix byte 18 = `0x06` is the entire v3 signal, and it is sufficient on its own.** Verified
directly: with bytes 19–50 left zero, the console still issued the 4-block descriptor and read all
664 bytes. Bytes 19–50 have no bearing on the escalation decision.

**Read loop:** the console pulls 70-byte chunks until it receives `last=1`, with a hard cap of
**15 chunks / offset < 1024** (~1050 bytes). Proven twice: given a 2108-byte buffer it stopped at
chunk 15 without `last=1`.

The NTAG215 control read is `8×70 + 1×40 = 600 = 60 + 540`. The v3 extended read is 664 bytes.

### 3.3 The two `0x14` forms

Distinguished by capture, not guesswork:

| Form | Offset-zero body | Length / completion |
|---|---|---|
| SRAM device command | `<timeout> <uid> 01 01 …` | exactly 74 bytes, executed with `0x21` |
| Mutable tag write | `<timeout> <uid> 01 06 …` | 454 bytes across six chunks, committed with `0x08` |
| Extended Air Riders operation | `<timeout> <uid> 01 06 …` | 355 or 167 bytes, terminated with `0x20` (§6) |

### 3.4 The `0x05` status can never discriminate tag type

Genuine bytes: `09 00 00 00 01 01 02 00 07 <uid×7>` then 45 zeros — **byte-identical** between a
v3 tag and an NTAG215 except for the UID, confirmed directly by the paired genuine captures.

`status[4..8]` is a straight passthrough of the NCI `RF_INTF_ACTIVATED_NTF` from the controller's
**PN7160** front-end:

| Byte | NCI field | Value |
|---|---|---|
| `[4]` | RF Discovery ID | `01` |
| `[5]` | RF Interface | `01` = Frame |
| `[6]` | RF Protocol | `02` = T2T |
| `[7]` | RF Technology & Mode | `00` = NFC_A passive poll |
| `[8]` | NFCID1 length | `07` |
| `[9..]` | NFCID1 | UID |

An NTAG I2C 2K is **also** T2T / NFC_A passive, so the status is identical for both chips. This
closes that avenue permanently. Bytes `[16..60]` are provably ignored (all 45 set to `0xFF`:
descriptor and read byte-identical).

Deviating from the genuine values is destructive, which is why the elimination sweep was expensive:

| `status[7]` | Console behaviour |
|---|---|
| `00` (genuine, NFC_A passive) | Normal read |
| `01` / `03` (NFC_B / NFC_F) | **Hard crash 2011-0301** |
| `02` (NFC_A active) | Never issues a read; loops poll → status → stop |

`status[4] = 02` also hard-crashes. `status[6]` must be `0x02`; `03` aborts before `0x06`.

## 4. Read path

1. Console polls (`0x03`), reads status (`0x05`), issues the 3-block 540-byte descriptor (`0x06`).
2. We serve the descriptor-selected pages behind the 60-byte prefix with **byte 18 = `0x06`**.
3. Console escalates: a 4-block, 604-byte descriptor reaching page `0x91` plus a separate `E2-E6`
   config block. We serve those ranges from the raw 2048-byte image.
4. Console stages the SRAM device command (`0x14`) and executes it (`0x21`) — §5.
5. Console issues the targeted `03-03` read, then either accepts the figure or proceeds to a write.

A 540-byte request still routes to the compatibility view; anything reaching past it serves from the
raw 2 KB image.

## 5. The `0x21` device command and the SRAM response

Genuine sequence:

```
C->D  0x14  offset=0 len=0x4A   <timeout> <uid> 01 01 00…    stage device command
D->C  0x14  ACK
C->D  0x21  (no payload)                                     execute
D->C  0x21  ACK
C->D  0x05  status
D->C  0x05  state = 0x18            <-- not 0x04
C->D  0x15  offset 0   -> 70 bytes, flags 0x00
C->D  0x15  offset 0x46 -> 13 bytes, flags 0x01              83-byte result buffer
```

Answering `0x21` with a bare ACK is **not** sufficient: the console has no result to read, abandons
the tag, and restarts polling — the "just waits for an amiibo to be scanned" symptom. The `0x21`
handler must also bump the report NFC state, and `0x05` in state `0x18` must return an **empty**
payload (matching genuine).

### 5.1 The 83-byte result buffer

```
[0..18]   19-byte controller header
[19..82]  image[0x3C0..0x3FF] — the COMPLETE 64-byte SRAM response
```

Header fields: `[0]` result type `0x18`; `[4..7]` `01 02 00 07`; `[8..14]` UID; `[18]` `0x06`, the
same escalation byte as the read prefix.

**SRAM bytes 62–63 are the big-endian CRC-16/MCRF4XX over bytes 0–61.** They are per response, not
a fixed controller constant:

| Image | SRAM CRC |
|---|---:|
| Captured genuine Warp Star | `7A C4` |
| Downloaded Warp Star (all four riders) | `E5 11` |
| Downloaded Winged Star (all four riders) | `BB 21` |
| Downloaded Tank Star (all four riders) | `25 63` |
| Downloaded Shadow Star (all four riders) | `30 61` |

All 16 supplied dumps pass both amiibo HMAC verification and the full-SRAM CRC check. The CRC
groups by machine because a rider's four variants reuse the same machine response.

Response structure, against a stored image:

```
02 00 | <12-byte per-unit blob> 00 | <3 bytes> | "PB4W17 " | 01 01 0X 00 00 00 | … | <CRC16>
```

This is what xSke's PR #381 means by *"a 2048-byte file with the expected response already placed in
the SRAM buffer"*: the response is **stored in the image**, not computed. That is why key-less tools
support these tags, and why no originality signature or `key_retail.bin` is required.

Evidence:
[`experiments/v3-full-sram-response-validation-2026-07-28.md`](experiments/v3-full-sram-response-validation-2026-07-28.md).

## 6. Write paths

### 6.1 Ordinary write (`0x14` × 6 → `0x08`)

Six chunks at offsets `0, 76, 152, 228, 304, 380` with declared lengths `76,76,76,76,76,74` = 454
bytes. Its three records:

| Page | Address | Length |
|---|---:|---:|
| `0x05` | `0x014` | 32 |
| `0x30` | `0x0C0` | 240 |
| `0x6C` | `0x1B0` | 152 |

424 mutable bytes ending exactly at `0x248`. `ns2_amiibo_v3_write_commit()` validates full coverage,
UID, header, record count, every record/range, and trailing padding before changing the image. It
protects bytes below `0x14` and at/above `0x248`, and restores the synthetic SRAM-ready bit before
commit. Completion reports status `05 00`.

### 6.2 Kirby Air Riders extended operation (`0x14` × N → `0x20`)

Two envelope shapes, both sector-aware record lists. Byte 22 is the record count; each record is
`(sector:u8, page:u8, length:u8, data[length])`.

| Body | Records | Purpose |
|---|---|---|
| 355 bytes | `(0, 0x92, 0xF0)`, `(0, 0xCE, 0x50)` | all-zero clear of `0x248..0x387` |
| 167 bytes | `(0, 0x04, 4)`, `(0, 0x92, 0x20)`, `(1, 0x01, 0x60)` | the update |

Envelope header (167-byte update form):

```text
88 13 <uid:7> 01 06 01 01 <cap_page> FF FF FF FF A5 00 <next_gen> 00 03 …
        ^byte 2          ^byte 11    ^byte 13              ^bytes 18..21   ^byte 22 = record count
```

- `88 13` = 5000 ms timeout, little-endian.
- **Byte 13 selects the sector-1 capability page**; the explicit 96-byte data record begins on the
  following page. Kirby sends `0x00`, King Dedede & Tank Star sends `0x64`.
- **Bytes 18–21 are the *next* chip-managed sector-1 page-0 value**, not current sector-0 page 4.

**Genuine `0x20` completion semantics.** Genuine hardware returns a **bare ACK**, and its next
status is `0x16` followed by 60 zero bytes — **not** `05 00`. The console then requests
selected-UID page 3, reads the 60-byte operation prefix plus that page, stages the ordinary
454-byte encrypted-body write, commits it with `0x08`, observes `05 00`, and sends Stop. Only that
later ordinary commit ejects. The sequence occurs twice per save (clear stage, then update stage).

The physical positive control confirmed the mapping: page 4 advanced `A5 00 02 00 → A5 00 03 00`
(the capability container's first two bytes are read-only), and sector-0 pages `0x92..0x99` changed
from zero to exactly the 32 data bytes in the second record.

### 6.3 The sector-1 capability generation is implicit dynamic state

Sector-1 page 0 advances **independently** from sector-0 page 4. Two consecutive genuine physical
cycles:

| | sector-0 page 4 | sector-1 page 0 |
|---|---|---|
| before first save | `A5 00 02 00` | `A5 00 01 00` |
| after first save | `A5 00 03 00` | `A5 00 02 00` |
| after second save | `A5 00 04 00` | `A5 00 03 00` |

Every explicit sector-1 record still begins at page 1 (Kirby's allocation); the page-0 value is
never written explicitly. Discarding it and continuing to serve `A5 00 01 00` is the **only**
mismatched extended-read state, and it produces "This amiibo is corrupted" on reuse.

The runtime validates a one-step generation advance, stores the four bytes at the envelope-selected
image offset (`0x400` for Kirby's allocation) in the otherwise-zero ecosystem-dump slot,
persists/exports it with the 2048-byte image, and serves the retained value through `0x1E`.
Zero-filled first-use and legacy images retain the hardware-confirmed generation-1 fallback,
injected descriptor-relative at the first page of the selected sector-1 range.

### 6.4 Learned gameplay state uses the same format

A save after completing an Air Riders level — the first explicitly non-cosmetic control — used the
same one three-chunk 167-byte/`0x20` update plus one six-chunk 454-byte/`0x08` commit. The
before/after diff changes 552 bytes, all inside modeled ranges:

| Region | Changed bytes | Meaning |
|---|---:|---|
| `0x000..0x247` | 423 | ordinary encrypted/writable body and HMAC-dependent bytes |
| `0x248..0x267` | 32 | sector-0 Air Riders record |
| `0x402` | 1 | numeric byte of the sector-1 page-0 generation |
| `0x404..0x463` | 96 | complete sector-1 Air Riders record |
| `0x464..0x7FF` | 0 | no new or unknown storage touched |

Learned gameplay data therefore needs no additional command, record layout, or allocation.

## 7. Written-tag reuse: sector-aware read `0x1E`

Reusing a successfully written tag froze while `0x1E` was bare-ACKed and state left at `0x18`,
causing repeated three-second Stop/restart loops. A genuine capture proves the **bare ACK itself is
correct**; the missing behaviour is a report-state edge, empty status `0x15`, and a 196-byte result
served through ordinary `0x15` chunks (70 + 70 + 56).

Request body, 23 bytes: timeout, selected UID, tag type, **two three-byte ranges
`(sector, start, end)`**, and six zero reserved bytes.

Result buffer, 196 bytes: a 64-byte identity/signature/descriptor prefix, then sector-0 pages
`0x92..0x99` and sector-1 pages `0x00..0x18`. Sector-1 page 0 carries the chip-managed capability
value (§6.3); portable v3 dumps leave that chip-owned page zero and begin mutable sector-1 data at
page 1.

## 8. Slot geometry — the allocation is a slot index

The Air Riders allocation is **not** fixed and **not** derived from identity. A third allocation
observed on 2026-07-29 (`dumps/experiments/20260729-102744-v3-reuse/`) requested on the wire:

```text
158  sector_read ranges=['s0:0x9A-0xA1(32B)', 's1:0x19-0x31(100B)']
330  stage envelope=extended_update records=['s0:0x04+4', 's0:0x9A+32', 's1:0x1A+96']
```

All three observed allocations fit one formula exactly. For slot *n*:

| | sector 0 | sector 1 |
|---|---|---|
| first page | `0x92 + 8n` | `25n` (capability) |
| extent | 8 pages / 32 B | 25 pages / 100 B (4 B capability + 96 B data) |

| Observation | s0 page | s1 capability | slot |
|---|---|---|---|
| Kirby | `0x92` | `0x00` | 0 |
| King Dedede & Winged Star | `0x9A` | `0x19` | 1 |
| King Dedede & Tank Star | `0xB2` | `0x64` | 4 |

Three independent checks agree the tag reserves exactly **ten** slots:

1. The 355-byte clear wipes sector-0 pages `0x92`–`0xE1` in two records (`0x92`+240 B, `0xCE`+80 B)
   = 320 B = 80 pages = **10 × 8 pages**.
2. The sector-0 bound `page + 7 <= 0xE1` permits slots 0–9 and rejects slot 10.
3. The independent sector-1 bound `data_page + 23 <= 0xFF` also permits slots 0–9 and rejects
   slot 10 (`25×10 + 1 + 23 = 274`).

Bounds 1 and 2 were derived from the clear operation's extent alone, before the slot geometry was
understood; that they land on the same cutoff as the independent sector-1 arithmetic is strong
support for the model. **Confidence: Strong** — an exact fit to three observations plus two agreeing
structural bounds, not a directly observed slot allocator.

⬜ **Unknown: what selects the slot index.** It is *not* the rider — two images with the same UID
`0465B0228F2190` used slots 1 and 4. It is not a simple machine-variant enumeration either (Winged
Star → 1 but Tank Star → 4, where a Warp/Winged/Tank/Shadow order would give 1 and 2). Registration
order in the game's own save is the leading hypothesis and is untested.

**Until it is known, never derive a slot from identity — consume the self-describing envelope.** The
runtime does exactly that, with no UID, character, product, or known-dump table:

- selected-image UID and command/type must match;
- update prefix `01 01` and suffix `FF FF FF FF` remain exact;
- sector-0 application data is bounded to the proven cleared window `0x92..0xE1`;
- the sector-1 capability page plus the following 96-byte record must fit inside the 2 KB image;
- the data page must equal capability page + 1;
- the capability generation must advance exactly one step at the selected page;
- record count, lengths, gap-free staging, and trailing-zero validation remain strict.

The portal's Initialize operation clears the complete second user-memory sector for the same reason,
so it resets Kirby, Dedede, and unseen allocations without a rider list.

Evidence:
[`experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`](experiments/v3-air-riders-dynamic-allocation-2026-07-28.md).

## 9. Storage, persistence, and lifecycle

- **2048-byte v3 store**, flash-persistent in the existing amiibo journal banks, mutually exclusive
  with the 540/572 store. Survives power cycles and mode changes.
- **The board holds exactly one amiibo.** The two flash banks are alternating persistence
  *generations* of that single image, not two selectable saves.
- On a successful commit the store increments generation, marks dirty/unpersisted, requests an
  alternating-bank snapshot, schedules the 700 ms report-state completion edge, and defers Stop's
  logical TagRemoved until the snapshot verifies. The next scan re-presents the updated image.
- Automatic re-presentation for both 540-byte and v3 virtual tags is suppressed for 3000 ms after a
  removal edge; later scans present the retained image normally.
- Config/UART status reports v3 size, contiguous UID, generation, payload CRC, dirty and persisted
  state. Bounded reads expose all 2048 bytes. The portal's Sync path replaces the content-keyed
  IndexedDB record only after validation.

Two real bugs found during this work, both affecting ordinary use and not just v3:

- **Flash region collision (serious).** Amiibo journal bank 0 sat on BTstack's TLV region on RP2350
  (pico-sdk 2.2.0 moves it one sector lower there), so writing a tag destroyed the Bluetooth bonds
  and BTstack destroyed the stored tag. Banks moved to `SIZE-6S`/`SIZE-5S`; asserts now check
  `PICO_FLASH_BANK_STORAGE_OFFSET`.
- **v3 uploads were never durable.** `amiibo persist` gated on the 540 store's `loaded` flag, making
  it a silent no-op for v3, and the portal never called it.

## 10. Portal behaviour

The library detects the 2048-byte NTAG I2C 2K format and stores the **whole** tag (no truncation, no
BCC recompute; UID parsed as 7 contiguous bytes). Each combo is content-keyed, so a rider's four
machine files are four distinct stored entries; in the carousel they collapse to one item per rider
(the four machines share one catalog identity and one static AmiiboAPI image). **Swap Combo**
(shown only for a multi-combo rider) cycles the active machine, and the combo name from the filename
appears in the detail box.

Workflow: navigate to the rider → Swap Combo to the machine → **Load amiibo**. Connected Load
uploads the complete 2048-byte image immediately; offline **Select amiibo** only remembers it.

## 11. Tooling

| Tool | Purpose |
|---|---|
| `v3mode`, `v3probe`, `v3hdr`, `v3reply` (UART) | runtime research overlays, no reflash per experiment |
| `amiibo v3diag` (UART) | staged/result/chunk/commit/write-error counters |
| `amiibo v3sig` (UART) | RAM-only originality-signature override — **research only**, not needed in normal operation |
| `amiibo journal` (UART) | inspect the alternating persistence banks |
| `nfcmirror` + `tools/nfc_probe.ps1` | initiator mode: UART originates NFC commands at a paired genuine controller with **no console attached** (§11.1) |
| `tools/rebuild_v3_from_capture.py` | reassemble a tested tag from its own capture; must copy the full 64-byte device response |
| `tools/test_ns2_amiibo_v3_write.c`, `tools/test_ns2_amiibo_v3.c` | captured-layout and failure-mode coverage |
| `tools/test_ns2_v3_compat_view.mjs` | crypto-equivalence and SRAM CRC across all 16 dumps |

### 11.1 Initiator mode

`nfcmirror` was purely reactive: every question cost a full console capture, and questions the
console never happens to ask could not be asked at all. Initiator mode makes a genuine controller an
interrogatable oracle at bench pace — BLE-only, gated on `sw2_init_state` and a connected `0x2069`,
needing nothing but a powered dongle and a paired Pro Controller 2.

```
nfcmirror initiator on|off
nfcmirror send HEX          # >=8 bytes, <=40 (bounded by the 96-byte RX line)
nfcmirror reply             # {"ready":false} until the genuine reply lands
```

The two directions are mutually exclusive by construction. `tools/nfc_probe.ps1` does the sequencing
on the PC, keeping the firmware a dumb auditable transport:

```powershell
.\tools\nfc_probe.ps1 -Port COM11 -Dump kirby.bin              # complete NTAG215 / partial v3
.\tools\nfc_probe.ps1 -Port COM11 -Ranges '00-3B,3C-77,78-91,E2-E6'
.\tools\nfc_probe.ps1 -Port COM11 -Raw '0191000500000000'      # one command, print the reply
```

🔵 **Limitation:** the `0x06` descriptor uses 8-bit page addresses and cannot directly address
sector 1, so the default v3 snapshot is 668 bytes (pages `00-A1` plus `E2-E6`), not a loadable
2048-byte image.

## 12. Hard constraints (do not re-litigate)

- Air Riders' extended payload is **opaque ciphertext**. PicoSwitch2 does not need its keys:
  preserve and replay the console-supplied bytes exactly. Do not synthesize or transform it.
- Do **not** put a 60-byte prefix in front of a 1024-byte payload — the total exceeds the console's
  ~1050-byte read window, `last=1` never arrives, and it regresses to "not an amiibo".
- The amiibo identity/format bytes are inside the signed region and cannot be rewritten to
  masquerade as v2.
- **Never key runtime behaviour on rider/figure identity.** Consume the self-describing envelope.
- Do **not** sweep enum values by crash-oracle. Each wrong `status[7]` hard-crashes the console
  (`2011-0301`), and the space is unbounded.
- Do **not** serve a v3 tag as a 540-byte amiibo (compat view, or re-signed). Both produce
  cryptographically valid tags and both are wrong in principle: the rider figurine does not scan at
  all without its machine — the machine is the antenna *and* holds the I2C device that answers the
  SRAM pass-through.
- A protocol change introducing a new record count, length, memory region, or command must **fail
  closed** and be added from a capture, never guessed.

## 13. Remaining work

| Item | Status |
|---|---|
| Production-portal Sync of the retained dirty generation | 🟡 The one remaining lifecycle check |
| What selects the Air Riders slot index | ⬜ Unknown; registration order is the untested hypothesis |
| The SRAM response's internal check over its first 26 bytes | ⬜ Unidentified (the outer CRC-16/MCRF4XX is solved) |
| Native physical-tag **write** through a genuine controller | 🔵 Pending capture and implementation |
| Full 2048-byte dump via `nfc_probe.ps1` | 🔵 Blocked on 8-bit descriptor page addressing |
| Whether Pro Controller 2 and Joy-Con 2 Right use byte-identical NFC transactions | ⬜ Untested |

## 14. Corrections and refuted claims

Kept so they are not re-derived. Every row below was believed, tested, and abandoned.

| Claim | Why it was wrong | How it was settled |
|---|---|---|
| The tag type is signalled in the `0x05` status (`status[7]` is a model byte) | `status[4..8]` is an NCI `RF_INTF_ACTIVATED_NTF` passthrough; an NTAG I2C 2K is also T2T/NFC_A passive, so the status is byte-identical for both chips | Paired genuine v3/NTAG215 captures; §3.4 |
| The console always requests the NTAG215 page set, so v3 is unreachable | It escalates to a 4-block 604-byte descriptor — but only after our prefix gives it a reason to | Genuine capture, §3.1 |
| `prefix[18]` has no effect on Switch 2 | That test set byte 18 **alone in a UART overlay that did not survive a reflash**, so it tested nothing. Byte 18 = `0x06` in the serve path is sufficient by itself | §3.2; the byte-18-only capture |
| The console validates a per-tag NTAG21x originality signature at `prefix[19..50]` for v3 | The comparison controlled only the 60-byte prefix; the 604 bytes of tag data also differed because the capture was of a *different physical figure*. Untouched downloaded dumps were later accepted with the signature field left **zero** | §5; the full-SRAM validation run |
| Serving a v3 amiibo requires a matched image **and** that tag's captured signature; generated or UID-randomised v3 amiibo are impossible by this route | Follows only from the refuted signature claim | Same |
| `prefix[19..50]` is a dynamic SRAM window alternating between two values | Constant across all 11 genuine reads in two sessions. The second value belonged to the `0x21` result buffer (first byte `0x18`, not `0x04`) and was misattributed | Record-by-record re-read of both captures |
| The `0x21` result body is invariant, and `7A C4` is a fixed controller trailer | `7A C4` was constant only because both captures used the same physical figure. It is that figure's SRAM CRC-16/MCRF4XX | §5.1; five distinct machine CRCs |
| A v3 image needs its own machine's SRAM block and its own signature; a dump from a different physical machine can never be accepted | The experiment changed three variables at once — three firmware bugs were fixed between the last failing downloaded-dump test and the first success, and the downloaded dump was never retried on the fixed firmware | §5; the downloaded `Kirby & Warp Star.bin` read/write |
| `key_retail.bin` is needed to import or serve known-good dumps | Keys were used only offline, to verify evidence before and after the write | Same |
| Air Riders' `0x20` is one fixed 355-byte no-op | Two sector-aware envelopes: a 355-byte two-record clear and a 167-byte three-record update | Genuine positive control, §6.2 |
| Genuine `0x20` completion reports `05 00` | It reports empty state `0x16`; only the later ordinary `0x08` commit reports `05 00` and ejects on Stop | Same |
| Envelope bytes 18–21 are the current sector-0 page 4 value | They are the **next** chip-managed sector-1 page-0 value; the two counters advance independently | Two genuine save/read pairs, §6.3 |
| The Air Riders record pages are fixed (Kirby's `0x92` / `0x00` / `0x01`) | Allocation is a slot index; King Dedede & Tank Star uses `0xB2` / `0x64` / `0x65` | §8 |
| Published research placing Air Riders data at pages `0x9A`-`0xA1` and `0x11A`-`0x131` | Superseded by the genuine controller trace plus physical-tag diff | §6.2, §8 |
| `2115-0176` is a tag-type rejection | It is the console's generic "this tag failed validation". A **genuine retail** Kirby amiibo produces identical symptoms across multiple Switch 2s when the tag is defective (r/Switch `1pvfm1v`) | External corroboration |

Repo-wide refuted-hypothesis index:
[`experiments/refuted-hypotheses.md`](experiments/refuted-hypotheses.md).

## 15. Evidence and references

**Dated experiment reports**

- [`experiments/v3-full-sram-response-validation-2026-07-28.md`](experiments/v3-full-sram-response-validation-2026-07-28.md)
  — full 64-byte SRAM response; no signature, no keys.
- [`experiments/v3-air-riders-extended-operation-2026-07-28.md`](experiments/v3-air-riders-extended-operation-2026-07-28.md)
  — the complete `0x20`/`0x1E` decode chronology.
- [`experiments/v3-air-riders-dynamic-allocation-2026-07-28.md`](experiments/v3-air-riders-dynamic-allocation-2026-07-28.md)
  — allocation-relative storage.
- [`experiments/v3-amiibo-genuine-capture-runbook.md`](experiments/v3-amiibo-genuine-capture-runbook.md)
  — the reusable genuine-controller capture procedure.
- [`experiments/generated-amiibo-console-rejection-2026-07-26.md`](experiments/generated-amiibo-console-rejection-2026-07-26.md)
  — the Switch 2 validates amiibo cryptography.

**Related repository documents**

- [`switch2/nfc-implementation.md`](switch2/nfc-implementation.md) — the firmware NFC runtime.
- [`switch2/nfc-protocol-inventory.md`](switch2/nfc-protocol-inventory.md) — the ordinary
  540/572-byte transport evidence map.
- [`switch2/amiibo-identity-and-generation.md`](switch2/amiibo-identity-and-generation.md),
  [`switch2/amiibo-decrypted-data-surface.md`](switch2/amiibo-decrypted-data-surface.md).
- [`re-methodology/nfc-investigation-workflow.md`](re-methodology/nfc-investigation-workflow.md) —
  the offline lab that makes these questions answerable without hardware.
- [`LLM/amiibo-v3-investigation-retrospective.md`](LLM/amiibo-v3-investigation-retrospective.md) —
  what the investigation cost and why.

**External**

- xSke, [N3evin/AmiiboAPI#243 comment 3591686037](https://github.com/N3evin/AmiiboAPI/issues/243#issuecomment-3591686037)
  — the primary format description.
- [solosky/pixl.js#381](https://github.com/solosky/pixl.js/pull/381) — figure-v3 tag emulation
  (RF side).
- [bettse/amiitool b066e85](https://github.com/bettse/amiitool/commit/b066e85d344355224cd0390064f9b5a995b47b2d)
  — the `tag_v3` `0x40` shift.
- [Switchbrew Switch 2 NFC services](https://www.switchbrew.org/wiki/NFC_services) —
  `InitializeWithExtendedApplicationArea` / `GetExtendedApplicationArea` /
  `SetExtendedApplicationArea`. 🔵 A strong candidate mapping for the `0x20` path, but the public
  service API does not document this controller-wire body.
- CTCaer/jc_toolkit `jctool.cpp` — the Switch **1** MCU NFC read, whose header layout first located
  prefix offset 18.
- Community dump set: "Kirby Air Riders amiibo for Pixl.js/allmiibo/flashiibo" (2026-02-07).
