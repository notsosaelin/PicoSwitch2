# Experiment — Genuine Joy-Con 2 (L + R) SPI Flash Dump Analysis (2026-07-14)

**Status:** ✅ Complete for what these dumps can answer. First-ever direct hardware evidence for
Joy-Con 2 (previously a complete blank slate, per `docs/switch2-gc/usb-personality.md`). **Confirms**
the Nintendo factory-block layout already RE'd for Pro Controller 2 / NSO GameCube Controller
extends structurally to Joy-Con 2; **discovers** a per-model hardware type code, confirms the DSP
audio blob is Pro2-exclusive, and finds a genuinely new fact: the accelerometer's factory bias
values land on different axis positions for Joy-Con 2 than for Pro Controller 2, implying a
different physical IMU mounting orientation between the two form factors.

**Date:** 2026-07-14. **Hardware:** two full 2 MB SPI flash dumps from the project owner's own
genuine Joy-Con 2 Left and Joy-Con 2 Right controllers, connected via a USB adapter and dumped
directly — the first real Joy-Con 2 hardware this project has had access to.

**A note on redaction:** per this project's existing practice (see
`spi-dump-analysis-2026-07-10.md` §3.4), real per-unit identifiers — serial numbers, the BT bond
table's host address and key — are described structurally below but their actual values are not
reproduced, since they identify the owner's specific physical hardware and paired console.

---

## 1. Question

Now that genuine Joy-Con 2 L/R SPI dumps exist, does the known Pro2/GC factory-data layout
(`ns2_factory_init()` in `switch_pro2.c`, cross-validated in the two prior SPI dump analyses)
extend to Joy-Con 2 at the same flash offsets? What, if anything, differs — and does any of that
difference matter for a future Joy-Con 2 output personality?

## 2. Method

Four dumps compared byte-for-byte and at the known factory-data offsets: `SWITCH2_JOYCON_L_1.bin`,
`SWITCH2_JOYCON_R_1.bin` (both new, 2026-07-14), `2069_spi_dump_2026-07-10_1422.bin` (genuine Pro
Controller 2, previously analyzed), and `NSO_GC_SPI_DUMP_1.bin` (genuine GameCube controller,
previously analyzed). Pure Python: full L-vs-R byte diff with contiguous-region grouping, a
per-16KB Shannon entropy map across all four dumps, and targeted structured reads at every offset
`ns2_factory_init()` already hard-codes.

## 3. Results

### 3.1 Overall shape: L and R share nearly all firmware, diverge in two mirrored regions
All four dumps are `0x200000` bytes. L vs R differ in only **22.95%** of bytes overall, and the
per-16KB entropy map shows L and R have **identical entropy at nearly every offset** — i.e. mostly
shared compiled firmware, not two unrelated images. Two regions genuinely diverge in *structure*
(not just per-unit calibration noise): `0x048000`–`0x050000` and `0x0A8000`–`0x0B0000` (exactly
`0x60000` apart — consistent with a dual-bank A/B firmware layout). In both regions, R stays
high-entropy (compiled code) while L drops to zero (erased/empty) partway through. This is a
plausible signature of genuine L/R-specific firmware code — e.g. IR camera/NFC support that only
one side's silicon needs — but this analysis cannot read the actual instructions (same "high
entropy, no recoverable plaintext logic" limitation as the Pro2 analysis, §3.8 there).

### 3.2 Confirmed from a second, independent source: Joy-Con 2 L/R USB VID/PID at `0x13012`
The identity block encodes its own USB VID/PID as raw little-endian bytes, immediately after the
serial number and two reserved zero bytes (`0x13012`–`0x13015`):

| Unit | VID | PID |
|---|---|---|
| Joy-Con 2 Left | `0x057E` | `0x2067` |
| Joy-Con 2 Right | `0x057E` | `0x2066` |
| Pro Controller 2 | `0x057E` | `0x2069` |
| NSO GameCube Controller | `0x057E` | `0x2073` |

Pro2 and GC match this project's own already-Confirmed values exactly. **The Joy-Con 2 L/R values
match the real Linux kernel "HID: nintendo" driver source** (`USB_DEVICE_ID_NINTENDO_NS2_JOYCONL` =
`0x2067`, `_JOYCONR` = `0x2066` — see `docs/switch2-gc/usb-personality.md`) **exactly.** This is
independent cross-validation from a second source entirely unrelated to the kernel driver — the
genuine unit's own flash — for the one PID assignment this project previously had from only a
single origin. The Joy-Con 2 PID assignment is now **Confirmed**, not merely "kernel-source-only,"
and the earlier L/R mix-up fixed across this project's docs on 2026-07-14 (an older, ndeadly/
BLE-sourced entry had them reversed) is now doubly settled.

### 3.3 New: a per-model hardware type code at `0x13002`
Every unit's identity block (`0x13000`) carries a 2-byte ASCII code at offset `0x13002`, immediately
before the serial number field:

| Unit | Code |
|---|---|
| Joy-Con 2 Left | `HB` |
| Joy-Con 2 Right | `HC` |
| Pro Controller 2 | `HE` |
| NSO GameCube Controller | `HH` |

Not previously documented anywhere in this project. Reads as a Nintendo-internal hardware model
code (`H` + type letter) — distinct from the USB VID/PID, which is a protocol-layer identifier, not
a flash-programmed one. Given four known values (`B`/`C`/`E`/`H`), the letter is almost certainly
not contiguous/enumerable in an obvious way (no `A`, `D`, `F`, `G` samples to test against) — treat
this as a confirmed fact about these four models only, not a decodable scheme.

### 3.4 Serial number field, `0x13004`–`0x1300F` (12 bytes, ASCII)
Confirmed present and printable ASCII on both Joy-Cons, same 12-character shape as Pro2's and GC's
already-documented serial (`W` + 11 digits). Real values not reproduced here (see redaction note).

### 3.5 Colour bytes, `0x13018`+: structurally consistent, values genuinely differ per unit
A flag byte (`02` on both Joy-Cons, `01` on Pro2/GC — open question, see §5) followed by what reads
as 3-byte RGB colour triplets. Values differ between L, R, and Pro2 as expected for genuinely
different physical unit colours — not pursued further (no reason to expect this project's existing
colour-passthrough code needs anything from here).

### 3.6 Factory motion calibration, `0x13040` (temperature + gyro bias, float32×6)
Present and populated on both Joy-Cons, same layout as Pro2/GC. Temperature reads ~26–27°C on all
three units (plausible ambient/self-heat reading). Gyro bias values are small (consistent with a
real, physically plausible per-unit bias). **The last two of the six float32 slots are `NaN` on
all three units** (Joy-Con L, Joy-Con R, and Pro2) — consistently, not just on one — suggesting
these two slots are unused/padding in this block rather than genuine calibration fields. Worth
noting for whoever next touches `ns2_factory_init()`'s Joy-Con support: don't try to interpret
these as real values.

### 3.7 New finding: accelerometer bias lands on a different axis position for Joy-Con vs Pro2
Factory motion calibration continues at `0x13100` (magnetometer bias + accelerometer bias,
float32×6 — magnetometer is always `(0.0, 0.0, 0.0)` on all three units, consistent with none of
these controllers having a magnetometer). The accelerometer bias triplet (expected to read close to
standard gravity, ~9.8, on whichever axis is aligned with "down" when the calibration was taken) is:

- **Joy-Con 2 L**: `(9.91, 0.15, -0.08)` — gravity on the **1st** float
- **Joy-Con 2 R**: `(9.92, -0.01, 0.14)` — gravity on the **1st** float
- **Pro Controller 2**: `(0.16, -0.07, 10.38)` — gravity on the **3rd** float

Both Joy-Cons agree with each other and disagree with Pro2 on *which axis position* carries the
~9.8 gravity reading. This is genuinely new information: it implies the IMU is physically mounted
in a different orientation relative to the logical axis order between the Joy-Con and Pro
Controller form factors — unsurprising given how differently the two are held, but not something
this project had any prior evidence for. **Relevant to a future Joy-Con 2 gyro/accel
implementation**: naively reusing Pro2's axis mapping for Joy-Con would very likely swap/negate axes
incorrectly. Flagged as **Strong evidence, not yet cross-validated** against a second source (e.g. a
BLE capture of Joy-Con 2 motion data in a known orientation) — the exact axis correspondence (which
Joy-Con axis maps to which Pro2 axis, and any sign flips) is not derived here, only the fact that a
remapping is needed.

### 3.8 Stick calibration: confirms the classic 9-byte packed format, and reveals a "one stick vs two" tell
Two calibration slots exist in the shared layout (`0x13080`/`0x130A8` and `0x130C0`/`0x130E8`),
matching the well-documented Switch 1 Joy-Con/Pro Controller 9-byte packed calibration format (two
12-bit values per 3 bytes). On Joy-Con 2:

- **`0x13080` (a small header/range block) is byte-identical between L and R** — despite being two
  genuinely different physical sticks. This suggests it's a shared nominal/default range constant,
  not a true per-unit-trimmed value (contrast with the next item).
- **`0x130A8` (the actual 9-byte packed calibration) differs between L, R, and Pro2** — this is the
  real per-unit stick calibration data.
- **The *second* slot (`0x130C0`/`0x130E8`) is entirely unprogrammed (`0xFF`) on both Joy-Cons**,
  while fully populated on Pro Controller 2. This is a clean, sensible confirmation of the physical
  hardware difference: Pro Controller 2 has two analog sticks and uses both calibration slots; a
  single Joy-Con has only one stick and only the first slot is ever programmed. A future Joy-Con 2
  implementation should read/expose only the first slot per side, not assume a second stick's worth
  of calibration exists.

### 3.9 Battery discharge curve, `0x1FB000`: byte-identical between L and R, close to Pro2's
The full curve table is **byte-identical between Joy-Con 2 L and R** (same battery cell/part
number, as expected), and very close in shape to Pro2's own curve (`spi-dump-analysis-2026-07-10.md`
§3.5) — same monotonically-decreasing `uint16_le` structure, slightly different absolute values
(plausibly a different-capacity cell for the smaller Joy-Con battery). One header byte pair differs
between Joy-Con (`06 01`) and Pro2 (`00 00`) at a fixed offset within the pre-curve header — meaning
and significance not determined, flagged as an open question.

### 3.10 Confirms: the Pro2-only "DSPH" audio blob is genuinely absent on both Joy-Con and GameCube
`0x175000` (the DSP firmware/coefficient blob identified in `spi-dump-analysis-2026-07-10.md` §3.6,
hypothesized there as "almost certainly audio or haptic-waveform DSP" given Pro2's 3 USB audio
interfaces) is **entirely erased (`0xFF`) on both Joy-Con 2 L and R**, matching the NSO GameCube
controller (also confirmed absent in the earlier GC dump analysis). This is clean, independent
cross-validation, from two more controller types, that this blob is tied to audio (or at minimum to
something Pro2-exclusive) rather than being a general HD-rumble waveform table that all Switch 2
controllers would need — if it were generic rumble-DSP data, Joy-Con 2 (which does have
HD-rumble-class haptics) would very likely have its own populated copy.

**Likely explanation (project owner, 2026-07-14): Pro Controller 2 is the only Switch 2 controller
with a headphone jack.** This is a cleaner, more specific explanation than "has audio hardware
generally" — a 3.5mm headphone output needs its own audio-mixing/DSP path (volume, EQ, mic
passthrough for the 3 USB audio interfaces) that has no equivalent on Joy-Con or GameCube, neither
of which has any audio jack or onboard speaker/mic. This fits every observed fact (Pro2-only
presence, populated real coefficient-shaped data, absence on two otherwise-similar controller
families) better than a vaguer "audio-related" label, and effectively resolves the "audio vs
haptics" open question from the original analysis in favor of audio specifically.

### 3.11 Bond table, `0x1FA000`: same host address recorded on both L and R, as expected
Both Joy-Cons have a populated bond table entry (unlike Pro2's zero-bonds default this project's own
firmware currently synthesizes). The recorded host address bytes are **identical between L and R**
— exactly what's expected, since both were paired to the same physical Switch 2 console. The header
shape differs somewhat from Pro2's own bond-table header (different length/flag bytes before the
address) — not fully decoded here, flagged as a minor open question, not pursued since the address/
key bytes themselves are the sensitive part and are already out of scope to publish.

## 4. Conclusion

The Nintendo factory-data layout is confirmed to extend cleanly across the whole Switch 2 controller
family (Pro2, GameCube, Joy-Con 2 L/R) at consistent flash offsets, with sensible, physically
plausible per-form-factor differences (one stick vs two, no audio DSP blob, different IMU mounting
orientation). This gives a Joy-Con 2 output personality a real, hardware-confirmed factory-data
foundation to build against — a meaningfully different starting point than the "complete blank
slate, 60-70% templatable at best" status quo described in `docs/switch2-gc/usb-personality.md`
before this pass.

## 5. Remaining questions / suggested future work

1. **Axis remapping for Joy-Con 2 motion** (§3.7) — the exact correspondence between Joy-Con and
   Pro2 accelerometer/gyro axes (not just "it differs") needs either a BLE motion capture with the
   controller in a known orientation, or comparing against the gyro bias floats at `0x13040` too
   (not done in this pass — only the accelerometer triplet at `0x13100` was checked for this).
2. **The L/R dual-firmware-bank divergent regions** (§3.1, `0x048000`/`0x0A8000`) — this analysis
   can't read compiled instructions; if a bootrom/glitch capability is ever available (per
   `CORTEX_PARSE.md`, still not attempted anywhere in this project), this would be a natural target
   to understand what's genuinely L-only vs R-only in firmware.
3. **The `0x13018` flag byte** (`02` Joy-Con vs `01` Pro2/GC) and the **battery-header byte pair**
   difference (§3.9) — both noted but not resolved; low priority, revisit if a Joy-Con 2 personality
   ever needs byte-exact factory-block replication rather than just structural understanding.
4. **The bond-table header shape difference** (§3.11) — not decoded field-by-field; low priority
   given the sensitive fields involved aren't going to be published regardless.
