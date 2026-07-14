# NSO GameCube Controller — SPI Dump Analysis — 2026-07-13

> Confidence key: **Confirmed** (hardware-observed, cited capture, or ≥2 independent hardware-derived
> sources agreeing) / **Strong** (one hardware-derived source + a consistent independent
> implementation or analysis) / **Hypothesis** (inferred, not yet reproduced) / **Unknown**.

## Question

What does the genuine NSO GameCube Controller's SPI flash contain, and which regions are safe to
reproduce in PicoSwitch2 firmware versus per-unit/identity data that must never be copied?

## Method

Two raw SPI dumps (`dumps/NSO_GC_SPI_DUMP_1.bin`, `dumps/NSO_GC_SPI_DUMP_2.bin`) were provided by the
controller's owner (extraction method not performed by this session — files were already present).
Analyzed via a Python structural scan (16-byte-granularity FF/00/DATA region classification, full
printable-ASCII-string extraction, targeted hex dumps and 16-bit/12-bit-packed decoding of the
structured regions found). Cross-referenced against `E:\nso-gc-refs\switch2_controller_research`
(cloned this session, commit `d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92` — see
`docs/switch2-gc/protocol.md` for the full reference-repo audit), specifically its documented BLE
factory-data-read init sequence (`bluetooth_interface.md`) and generic SPI memory map
(`memory_layout.md`).

## Environment

Board/controller context: genuine Nintendo GameCube Controller (NSO), USB VID:PID `057E:2073`,
confirmed attached and enumerating cleanly on the analysis PC at capture time (see
`docs/switch2-gc/usb-personality.md` "Live device enumeration" for the USB-side confirmation, done
same session). SPI extraction method/tooling not recorded by this session (dumps were already
provided) — flag as a gap: **future dumps should record the exact extraction command/tool for
reproducibility**, per this project's own experiment-doc standard.

## File identity

| File | Size | SHA-256 |
|---|---|---|
| `NSO_GC_SPI_DUMP_1.bin` | 2,097,152 bytes (2 MiB exactly) | `4aee5a7c9170d98ec86bbbc624744732f02fa3aa20a1e63a7190bbc627ca6082` |
| `NSO_GC_SPI_DUMP_2.bin` | 2,097,152 bytes (2 MiB exactly) | `4aee5a7c9170d98ec86bbbc624744732f02fa3aa20a1e63a7190bbc627ca6082` |

**The two dumps are byte-for-byte identical** (`cmp -l` reports zero differing bytes). This is a
genuine finding, not an analysis artifact: either (a) the flash's content is fully stable/read-only
across two independent extraction passes (consistent with static factory data — a positive result),
or (b) the two files are copies of the same single read rather than two independent captures. This
session cannot distinguish those two explanations from the files alone — **flag for the controller's
owner to confirm whether DUMP_2 was a genuinely independent re-extraction**. Either way, there is
nothing to byte-diff between the two files; all findings below are from `DUMP_1` alone (identical to
`DUMP_2`).

## Structural map — Confirmed (directly observed)

16-byte-granularity scan classifying every block as all-`0xFF` (erased/unwritten flash), all-`0x00`,
or `DATA` (mixed/non-trivial bytes):

| Region | Size | Content |
|---|---|---|
| `0x000000`–`0x007710` | ~30.3 KB | High-entropy data (firmware code, likely compressed/encrypted — string-extraction found no readable text here, consistent with compiled/packed code rather than a config table) |
| `0x007710`–`0x013000` | ~46.9 KB | Unwritten (`0xFF`) |
| `0x013000`–`0x013F00` | ~3.8 KB (mostly `0xFF`, ~250 bytes of actual structured data) | **Factory/configuration region — see below** |
| `0x013F00`–`0x015000` | ~4.4 KB | Unwritten |
| `0x015000`–`0x0425C0` | ~185.4 KB | High-entropy data (firmware/application code) |
| `0x0425C0`–`0x0D5000` | ~586 KB | Unwritten |
| `0x0D5000`–`0x0DE0D0` | ~36.8 KB | High-entropy data (another firmware/code blob) |
| `0x0DE0D0`–`0x1FA000` | ~1.42 MB | Unwritten |
| `0x1FA000`–`0x1FA060` | 96 bytes | **Structured — see below** |
| `0x1FA060`–`0x1FB000` | ~4 KB | Unwritten |
| `0x1FB000`–`0x1FB090` | 144 bytes | **Structured — see below** |
| `0x1FB090`–`0x200000` | ~20.3 KB | Unwritten |

Total non-`0xFF` payload: 253,856 of 2,097,152 bytes (~12%); the remainder is erased/unprogrammed
NOR flash — consistent with a much larger flash chip than this controller's firmware+config actually
needs (headroom for future updates, or a chip shared across a product family).

## Cross-validation against ndeadly's documented SPI-read addresses — Confirmed (2 independent sources agree)

ndeadly's `bluetooth_interface.md` documents the exact BLE handle-write sequence a genuine
controller's own pairing/init flow uses to read its own factory data, including specific
address+length pairs (quoted verbatim in `docs/switch2-gc/protocol.md`). Every address that sequence
reads from **falls inside the `0x013000`–`0x013F00` structured region this dump's independent
structural scan flagged**, with one partial exception:

| ndeadly's documented read | Present in this dump? |
|---|---|
| `0x40` bytes from `0x13000` | ✅ structured data present (serial-like string, see below) |
| `0x10` bytes from `0x13040` | ✅ structured data present |
| `0x20` bytes from `0x13060` | ❌ **all `0xFF` in this dump** |
| `0x40` bytes from `0x13080` | ✅ structured data present |
| `0x40` bytes from `0x130C0` | ✅ structured data present |
| `0x18` bytes from `0x13100` | ✅ structured data present |
| `0x02` bytes from `0x13140` | ✅ structured data present |
| `0x40` bytes from `0x1FC040` | ❌ **all `0xFF` in this dump** |

Six of eight documented read addresses land exactly on real (non-`0xFF`) data in this independently
dumped controller, with no address mismatch — this is real cross-validation (our own hardware-derived
structural scan agreeing with a separately-authored, BLE-capture-derived address map from an
unrelated project), promoting "the `0x013000`-`0x013140` region is genuine factory/config data" from
Hypothesis to **Confirmed**. The two misses (`0x13060`, `0x1FC040` — the latter inside ndeadly's
documented "calibration" region per `memory_layout.md`) are a real, unresolved discrepancy — see
"Open questions" below, not silently resolved in either direction.

## Decoded fields — by confidence

### Confirmed: a per-unit-identity string at `0x013002`

```
013000: 01 00 48 48 57 35 30 30 30 31 35 35 31 38 31 30
013010: 00 00 7E 05 73 20 01 04 01 FF FF FF FF FF FF FF
```
Bytes `0x013002`–`0x01300F` decode as ASCII: **`HHW50001551810`**. Also visible in the same 32-byte
block, little-endian at `0x013012`: `7E 05` = `0x057E` (Nintendo's real USB VID — self-referential
confirmation this is a Nintendo factory-data block) and `73 20` = maybe `0x2073` byte-swapped-ish
(not a clean match either byte order — flag as **Hypothesis**, needs the reference repo's exact field
offsets to resolve cleanly, not confidently decoded this pass).

**`HHW50001551810` has the shape of a per-unit serial number** (letter prefix + long digit run,
consistent with Nintendo's known Joy-Con/Pro-Controller serial format convention). Per NSO-GC.md's
explicit instruction, **this string must never be copied into firmware** — treated here as excluded
material, recorded only to document that it exists and where, not to preserve its value for reuse.

### Strong: a second identity/lot string at `0x013E00`

```
013E00: 32 32 32 35 33 37 36 35 51 30 31 43 30 36 31 32
013E10: 4D 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
Decodes as ASCII: **`22253765Q01C0612M`**. Shorter numeric-then-alphanumeric shape suggests a
manufacturing lot/date code rather than the primary serial (which is already accounted for above) —
**not confidently classified as per-unit-unique vs per-batch-shared** this pass. Treated with the
same exclusion caution as the serial above until classified with more confidence (a second genuine
unit's dump, once available, would resolve this immediately: identical value across units = batch
code, safe-ish to hardcode as a plausible-looking constant; differing value = per-unit, must stay
excluded).

### Strong: paired calibration-shaped records at `0x013080`–`0x013100`, using the known Nintendo 12-bit-pack

```
013080: FF 47 79 94 B8 86 6B A0 00 0A A0 00 0A FF FF FF
013090: FF FF FF FF FF FF FF FF FF BE E3 3B BE E3 3B 06
0130A0: 65 50 06 65 50 0A FF FF 5D C8 84 BA 64 4A D0 B4
0130B0: 4E FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
0130C0: FF 47 79 94 B8 86 6B A0 00 0A A0 00 0A FF FF FF
0130D0: FF FF FF FF FF FF FF FF FF 18 83 31 18 83 31 5F
0130E0: F4 45 5F F4 45 0A FF FF C6 17 81 6F 14 42 36 F4
0130F0: 43 FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
```
`0x13080` and `0x130C0` are two structurally near-identical 64-byte blocks (matching ndeadly's two
separately-documented reads at exactly those two addresses) — both open with the identical 8-byte
sequence `FF 47 79 94 B8 86 6B A0`, then diverge. Decoding the 3-byte groups starting at `0x013081`
using **Nintendo's publicly-known original-Joy-Con 12-bit stick-calibration packing** (2 values per
3 bytes: `val0 = b0 | ((b1&0xF)<<8)`, `val1 = (b1>>4) | (b2<<4)`):

- `A0 00 0A A0 00 0A` → two adjacent packed pairs, **both decode to (160, 160)**
- `BE E3 3B BE E3 3B` → **both decode to (958, 958)**
- `06 65 50 06 65 50` → **both decode to (1286, 1286)**

Each group is internally self-consistent (both packed pairs in a group agree) and the whole 3-group
pattern repeats with different values between the `0x13080` and `0x130C0` copies — the shape (three
groups of "matching packed-pair, duplicated") is exactly what you'd expect from a small
max/center/min-style stick or trigger calibration record duplicated between two SPI banks for
redundancy (a documented original-Joy-Con convention). **Strong, not Confirmed**: the byte-packing
math is real and Nintendo's-own, and the redundant-copy structure is real, but this session did not
cross-check against ndeadly's *exact* per-field offsets within these 64-byte blocks (his docs treat
the whole `0x13000-0x14FFF` range as "generic factory data," not broken into a named field table for
this specific sub-range) — so *which* physical control each triplet calibrates (left stick / right
stick / L trigger / R trigger) is not established this pass.

### Hypothesis: a small index→value table at `0x013E20`–`0x013E38`

```
013E20: 01 02 02 02 FF FF FF FF FF FF FF FF FF FF FF FF
013E30: 03 02 04 01 05 02 06 00 07 02 FF FF FF FF FF FF
```
Reads as byte pairs: `(01,02) (02,02) (03,02) (04,01) (05,02) (06,00) (07,02)` — an index (1-7)
mapped to a small value (mostly 2, one 1, one 0). Shape is consistent with a per-control
feature/capability flag table (7 entries — plausibly matching a subset of the genuine controller's
physical buttons or axes), but nothing in either reference repo documents a table at this specific
offset — **pure Hypothesis**, flagged for future correlation once a byte-exact USB capture of a live
init/factory-read exchange is available.

### Hypothesis: possible pairing/journal data at `0x1FA000`

```
1FA000: 02 00 00 00 00 00 00 00 3C A9 AB FA DD 8D 00 00
1FA010: 00 00 00 00 00 00 00 00 00 00 34 C6 FD AC 46 AD
1FA020: EA DF 40 36 08 D7 0A 03 B0 8C 00 00 00 00 00 00
1FA030: 3C A9 AB FA DD 8C 00 00 00 00 00 00 00 00 00 00
1FA040: 00 00 34 C6 FD AC 46 AD EA DF 40 36 08 D7 0A 03
1FA050: B0 8C 00 00 00 00 00 00 FF FF FF FF FF FF FF FF
```
Two near-identical 48-byte records differing in exactly one byte (`...DD 8D...` vs `...DD 8C...`,
offset +12 within each record) — the shape of a wear-leveled journal entry (monotonic
counter/generation byte incrementing by one between two otherwise-identical redundant copies), each
containing a shared 16-byte value (`34 C6 FD AC 46 AD EA DF 40 36 08 D7 0A 03 B0 8C`) that could be a
checksum, a key fragment, or other opaque material. **ndeadly's `memory_layout.md` documents this
address range as "BT pairing info."** Per NSO-GC.md's explicit exclusion list (Bluetooth addresses,
pairing keys, cryptographic material), **this entire region is treated as off-limits for firmware
reuse regardless of what it turns out to mean** — recorded here only to document its existence and
general shape, not decoded further, and specifically **not** to be copied verbatim into PicoSwitch2.

### Hypothesis: a numeric lookup table at `0x1FB040`–`0x1FB090`

```
1FB040: FF FF 02 00 03 00 C8 00 78 00 98 0E DD 0D 4B 0D
1FB050: E5 0C B2 0C 76 0C 32 0C 18 0C A9 0B AA 0A FF FF
1FB070: 98 0E A9 0B FF FF 54 09 FF FF 54 0D 54 0C A9 0B
1FB080: A9 0A FF FF FF FF FF FF FF FF FF FF FF FF FF FF
```
As little-endian `uint16`: `200, 120, 3736, 3549, 3403, 3301, 3250, 3190, 3122, 3096, 2985, 2730, ...`
— a smoothly descending sequence in the ~2700–3700 range, with `3736` (`0x0E98`) and `2985` (`0x0BA9`)
recurring in the second block at `0x1FB070`. Shape is consistent with a response/travel curve table
(analog trigger or rumble-amplitude LUT are both plausible given the value range and monotonic
shape), but **nothing in either reference repo documents a table at this address** — pure Hypothesis.
Also unresolved: `0x1FB000` region (`02 00 02 00 01 00 11 03`) immediately precedes this and may be a
small header/count field for it, not analyzed further this pass.

### Confirmed (negative result): no other readable strings anywhere in the 2 MB image

Full printable-ASCII extraction (≥4 consecutive printable bytes) across the entire file found no
readable text outside the two strings above — everything else flagged by the extractor is
statistically-expected noise from high-entropy binary data (confirmed by manual inspection of a
sample; matches the "firmware/compressed-code" classification of those regions in the structural
map). This is useful negative evidence: **the two identity-shaped strings found are very likely the
only human-readable identity material in this controller's entire flash**, not one of several missed
in a partial scan.

## What is safe to reuse in firmware vs. what must be excluded

| Region | Content | Safe to reuse? |
|---|---|---|
| `0x013002` string | Per-unit serial (`HHW50001551810`) | **No — exclude.** Per-unit identity. |
| `0x013E00` string | Lot/batch code (`22253765Q01C0612M`) | **Unclear — treat as excluded** until classified as per-unit vs per-batch (needs a second unit's dump to compare) |
| `0x013080`/`0x130C0` calibration-shaped records | Likely stick/trigger calibration | **Hypothesis-only, not yet safe to reuse as literal bytes** — semantics (which axis/control) unconfirmed; a synthesized/documented-default value should be used instead once the field layout is actually understood, not this unit's real values |
| `0x1FA000` pairing-shaped record | Likely BT pairing/bonding data | **No — exclude, explicitly.** Matches ndeadly's own "BT pairing info" address-range documentation. |
| `0x1FB040` numeric table | Unknown curve/LUT | **Hypothesis-only** — safe to eventually reuse *if* independently corroborated as a non-identity, non-cryptographic constant table (e.g. a trigger response curve), but not yet confirmed as such |
| Firmware code blobs (`0x000000`, `0x015000`, `0x0D5000`) | Compiled controller firmware | **Never applicable** — PicoSwitch2 doesn't run this controller's firmware, only needs to emulate its externally-visible protocol behavior |

## Open questions

1. Why do `0x13060` and `0x1FC040` read as all-`0xFF` in this dump when ndeadly's documented BLE init
   sequence reads from both? Candidates: (a) this specific unit genuinely has those fields unset
   (never calibrated / using a hardware default that lives elsewhere), (b) the dump was taken via a
   method or timing that predates some initialization step, (c) ndeadly's addresses are for a
   firmware revision or controller unit that differs slightly from this one. Not resolved this pass —
   would benefit from either a second genuine unit's dump for comparison, or a live BLE/USB capture of
   this exact unit's own init sequence for direct correlation.
2. Is `22253765Q01C0612M` per-unit or per-batch? Needs a second unit's dump.
3. What do the `0x013080`/`0x130C0` calibration-shaped triplets actually calibrate (which stick axis
   or trigger)? Needs either a byte-exact reference offset table (neither cloned repo provides one at
   this granularity) or correlating live physical-control movement against a live SPI re-read.
4. Were `NSO_GC_SPI_DUMP_1.bin` and `_2.bin` genuinely two independent extractions, or copies of one
   read? Affects how much weight to put on "the two dumps agreeing" as a stability signal.

## Conclusion

The dump's structural layout is now **Confirmed** cross-validated against an independent
documentation source (6 of 8 documented read addresses land on real data, zero address mismatches).
Two identity-shaped ASCII strings and one pairing-shaped record are identified and correctly excluded
from any firmware reuse. One calibration-shaped record family and one numeric-table region are
identified with Strong/Hypothesis confidence respectively, pending exact field-offset documentation
before their *semantics* (not just their *existence*) can be trusted. No blind byte-copying of any
SPI region into PicoSwitch2 firmware is recommended until the relevant field's meaning is independently
corroborated — see `docs/switch2-gc/usb-personality.md` "Stage D" for how this bounds the initial
factory/SPI implementation (documented-safe defaults only, not this unit's raw bytes).
