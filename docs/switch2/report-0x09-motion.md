# Switch 2 Pro Controller — Report 0x09 Motion (IMU) Format

**Current status (2026-07-29):** ✅ a genuine Pro Controller 2's native BLE motion is passed
byte-for-byte into console-facing report `0x09` and is hardware-confirmed in Splatoon 3. Correct
aim axes, stationary behavior, controller power-cycle, bonded reconnect, and source-off hold all
pass. DualSense gyro translation is also hardware-confirmed, including rapid motion and reconnect.
The genuine stream's length-`0x28` form is now established as a packed multi-sample IMU payload,
not an independent magnetic/reference quaternion. A matching handle-`0x000A` PCAP also exposes a
plain timestamp/temperature/accel/gyro sample that can serve as ground truth. A controlled live
cadence matrix now decodes and hardware-validates all high-rate, normal, and catch-up accel/gyro
lanes. The prefix field boundary, split third carrier, fixed carrier scales, and packet-derived
`preceding carrier + 4 ticks` epoch are also established. A causal history decoder is validated
against the dynamic corpus. The high-rate/normal tail is decoded as two Q3 IMU temperature
samples, and catch-up bit 287 is observed reserved-zero padding. Exact integer projection and
rounding now re-encode every captured mode-3 packet byte-for-byte. Future work is limited to new
genuine packet modes or protocol behavior outside the captured range;
see
[`../experiments/pro2-mode3-carrier-prefix-2026-07-29.md`](../experiments/pro2-mode3-carrier-prefix-2026-07-29.md)
and
[`../experiments/pro2-carrier-chart-transition-2026-07-29.md`](../experiments/pro2-carrier-chart-transition-2026-07-29.md).

The legacy length-30 USB field analysis below remains useful for the generic encoder and historical
captures. It is no longer the production path for a genuine PID `057E:2069` source; that controller
provides its own variable-length `0x1E`/`0x28` PDU. See
[`native-pro2-motion-passthrough-2026-07-21.md`](../experiments/native-pro2-motion-passthrough-2026-07-21.md).

> **Supersedes the earlier int16 interpretation.** This document previously described the block as
> two interleaved `int16` `[gyro,accel]` samples. That model is **refuted** — see §"Why the int16
> model looked partially right". The corrected layout below is what the firmware must emit.

---

## Question

Report `0x09` (the console-facing input report) carries an IMU block whose packing ndeadly's
`hid_reports.md` calls an *"unknown packed format"* (length byte `{0,30,40}` at offset `0x0E`, data
at `0x0F`). Earlier blind hypotheses (3 samples length 40; int16 interleaved length 30) produced
**no usable motion**. We need the real layout and scale.

## Method — decode + independent re-verification

1. **Primary decode** ([report-0x09-motion-analysis.md](report-0x09-motion-analysis.md)): two clear
   USB captures (`PC2_Gyro_Default.pcapng`, `PC2_Gyro_Calibrate.pcapng`) analysed by byte-level
   continuity, physical plausibility, and known ICM-42670-P scales.
2. **Independent re-verification** (this repo's `usb.pcapng`, a Packetry/Cynthion raw USB-2.0 wire
   capture, 50,650 motion reports). Results:

   | Test | Result |
   |---|---|
   | Temperature @ `motion[0x02]` | constant `3072` (0x0C00) across 2000 reports |
   | Accel parsed as int32 **Q16.16**, stationary window | \|g\| **mean 1.0005, CV 0.0068** |
   | Q16.16 half-variance discriminator (3 axes) | low-16 SD ≈ 16–20k (fractional), high-16 SD ≈ 11–35 (stable gravity) |
   | Timing high-nibble == 800 Hz tick-delta | **299/299 reports** |
   | Phase Z at motion start | `0x802a1300` ≈ `0x80000000` (−180°) |

   The half-variance test is the proof: the bytes the old model read as "gyro" are literally the
   **fractional low-16 bits of the Q16.16 accelerometer** — hence their random full-scale look.

## Format (length = 30)

Motion block begins at report offset `0x0F` (`motion[k]` below = report byte `0x0F + k`). The
length byte is at report offset `0x0E` (`0` = IMU off, `30` = IMU on). The original direct USB
captures contained no length-40 records; the live native BLE bridge now observes and successfully
forwards genuine length-40 blocks alongside length-30 blocks.

| motion off | Size | Type | Field | Notes |
|---|---|---|---|---|
| `0x00` | 2 | `uint16_le`| **Timing**          | low 12 bits = 800 Hz IMU tick (mod 4096); high 4 bits = ticks elapsed since previous report (usually 3–4). |
| `0x02` | 2 | `int16_le` | **Temperature**     | constant `0x0C00` (3072). ICM: °C = raw/128 + 25 ⇒ 49 °C. 												 |
| `0x04` | 12 | packed | **Orientation carrier** | **NOT three independent angles.** One packed quaternion: `G0` 26 bits, `G1` 25 bits, `G2` 24 bits, plus a 2-bit chart state in `G2`'s bits 25:24. See "Orientation carrier" below. |
| `0x10` | 4 | `int32_le` | **Accel X**         | **Q16.16 fixed point**; `65536 × 4096 = 1 g`. 															 |
| `0x14` | 4 | `int32_le` | **Accel Y** 		| ICM-42670-P ±8 g ⇒ 4096 LSB/g. 																			 |
| `0x18` | 4 | `int32_le` | **Accel Z** 		| 																											 |
| `0x1C` | 2 | — | **Tail** | zero in all captures; unresolved. |

```c
typedef struct __attribute__((packed)) {
    uint16_t timing;        // (count << 12) | (tick & 0x0FFF)
    int16_t  temperature;   // 0x0C00
    uint32_t orientation[3];// packed carrier; widths 26/25/24 + 2-bit state, NOT 3 angles
    int32_t  accel_q16[3];  // Q16.16, 65536*4096 == 1 g
    uint16_t tail;          // 0
} ns2_motion30_t;           // sizeof == 30
```

### Orientation carrier — ✅ hardware-validated for generated output

The three 32-bit-aligned words at `0x04..0x0F` carry ONE bounded quaternion, not one angle per
axis. Each slot is a centered unsigned value over the usual ±1/√2 smallest-three physical range:

```
Each Gn is a 26-bit little-endian value stored as 24 bits + 2 high bits in a
following byte. The three fields are NOT aligned 32-bit words:

  0x04 bits 1:0   G2 bits 25:24  -> the CHART STATE (see below); bits 7:2 unrelated
  0x05..0x07      G0 bits 23:0
  0x08 bits 1:0   G0 bits 25:24;                                bits 7:2 unrelated
  0x09..0x0B      G1 bits 23:0
  0x0C bits 1:0   G1 bits 25:24;                                bits 7:2 unrelated
  0x0D..0x0F      G2 bits 23:0

Scales (the encoder uses fewer bits than the field provides for G1 and G2):

  G0 = (component / sqrt(2) + 0.5) * 2^26     all 26 bits used
  G1 = (component / sqrt(2) + 0.5) * 2^25     bit 25 always 0 in generated output
  G2 = (component / sqrt(2) + 0.5) * 2^24     bits 25:24 carry the state instead
```

The chart state therefore lives in the **low two bits of motion byte `0x04`**, reached as G2's
bits 25:24 after extraction. Extract 26 bits per field, then apply that field's scale.
`ns2_motion_pdu30_get_orientation()` / `..._set_orientation()` in
`src/bt_hid/motion/ns2_motion_pdu.c` are the authority; the setter preserves the six unrelated bits
of bytes `0x04`, `0x08`, and `0x0C`.

The state numbers components in wire order `w/x/y/z`, so identity is state 0, and the omitted
component is recovered with the positive square root. This is what
`ns2_ds5_motion.c::pack_switch2_smallest_three()` writes, and it is the only representation that
has ever produced correct motion on a console. See the carrier sections further down for what is
and is not established about the *genuine* controller's internal model — direct captures refute
strict smallest-three as an exact description of genuine hardware, while confirming the cyclic
chart topology and the paired sign-branch transition law.

**The earlier "angular phase, 2^32 = 360°" reading of these bytes is REFUTED.** It survived in this
table long after the carrier was resolved, and a firmware encoder was written against it: three
independent integrated-rate accumulators written straight into `0x04..0x0F`. On hardware that
produced violent, meaningless motion for every source routed through it. The encoder was deleted
2026-08-14; the entry in
[`../experiments/refuted-hypotheses.md`](../experiments/refuted-hypotheses.md) is the durable
record. Do not reintroduce a per-axis angle model for this field.

### Conversions
```
accel_g     = accel_q16 / (65536.0 * 4096.0)
accel_count = accel_q16 / 65536.0            // ordinary ICM count (~±4096 = 1 g)
```
Angular rate is not carried directly in a length-30 block; it is the time derivative of the
orientation carrier. The length-`0x28` block is the one that carries packed IMU samples.

## Activation — motion is a *negotiated feature* (not always-on)

Confirmed by both `report-0x09-motion-analysis.md` and this repo's Experiment C
([gyro-experiment-c-results.md](../experiments/gyro-experiment-c-results.md)): report `0x09`
streams from power-up with **motion-len = 0** and only flips to `30` after the host enables the
IMU feature over bulk EP2:
```
0c 91 00 06 00 04 00 00 …          # configure motion features (ids 0x02/0x03)
0c 91 00 04 00 04 00 00 27 00 00 00 # feature mask 0x27 (IMU bit set, no magnetometer) -> motion ON
```
(~174–251 zero-length reports precede motion-on in the captures.) The current firmware gates motion
on this negotiated feature, matching the genuine device. See Experiment C for the original trace.

## Why the int16 model looked partially right

A Q16.16 accel value is `[ int16 high ][ frac16 low ]`. Reading `(int16)(accel_q16 >> 16)` yields
the ordinary gravity count (~±4096) — so the old "accel" lane looked correct and produced a
plausible 1 g magnitude. But the adjacent "gyro" lane was `(uint16)(accel_q16 & 0xFFFF)` — the
**fractional half** — which sweeps most of the 16-bit range and looks like random full-scale gyro.
Likewise the "first sample" was the low/high halves of the 32-bit orientation words. Replace
`gyro16,accel16 × repeated` with `orientation32 × 3, accel_q16_32 × 3`.

## Calibration (from report-0x09-motion-analysis.md; not yet needed for basic gyro)

- User motion-calibration region: address `0x001FC000`, length `0x40`.
- "Reset to default" writes 64 × `0xFF`. "Calibrate" writes validity marker `B2 A1` (LE `0xA1B2`)
  + zeros. Nonzero coefficient fields were **not** captured — internal scale/order still unknown.
- The controller appears to apply its calibration internally at startup (not sent over the host
  protocol), so basic emulation can ignore it initially.
- **Cross-checked 2026-07-10, two ways:** (1) a genuine unit's own SPI flash dump confirms this
  region is `0xFF`-filled (uncalibrated) on real hardware exactly as expected — see
  `docs/experiments/spi-dump-analysis-2026-07-10.md` §3.3. (2) External research into
  `Dycool/NS-PC-Control` (`server/src/switch2_native.cpp`) independently confirms `0x1FC000` as the
  real motion-calibration write target (`mem_write_user_motion_cal()`, persisted to a local file)
  — but its write handler is for a **72-byte** payload, not 64. 🔵 Unresolved discrepancy, not
  contradiction: 72 could be a wrapped/header-prefixed write frame around a 64-byte payload (their
  code wasn't read at the byte-layout level, just the address/size it declares), or our documented
  length could be incomplete. Worth resolving before ever implementing calibration *write* support
  (not currently planned — basic emulation still doesn't need it).

### ✅ Factory motion calibration (`0x13040`, `0x13100`) — decoded, distinct from the user region above

Not to be confused with the *user* motion-calibration region (`0x1FC000`, still unprogrammed/unknown
on every unit seen so far). This is a **separate, always-populated factory block**, and as of
2026-07-10 its layout is confirmed by a working third-party BLE client (`tools/switch2_input_viewer.py`,
added to this repo for reference — a PyQt/bleak tool that pairs with a genuine controller over BLE
GATT and parses its reports/calibration live). Its `handle_read_response()` decodes:

| Address | Size | Fields | Type |
|---|---|---|---|
| `0x13040` | 16 | `temperature` @ `[0:4]`, `gyro_bias` (x,y,z) @ `[4:16]` | `float32` each (IEEE-754 LE) |
| `0x13100` | 24 | `magnetometer_bias` (x,y,z) @ `[0:12]`, `accelerometer_bias` (x,y,z) @ `[12:24]` | `float32` each |

**Verified against our own SPI dump** (`dumps/2069_spi_dump_2026-07-10_1422.bin`, decoded with this
exact field mapping):

- `temperature` = **27.09** — plausible factory-calibration ambient temperature (°C).
- `gyro_bias` = **(-0.0205, -0.00058, 0.00328)** — tiny, consistent with (and roughly matching the
  order of magnitude of) this repo's already-documented "genuine gyro ~0.03 dps bias" figure. Units
  not independently confirmed but plausibly dps.
- `magnetometer_bias` = **(0, 0, 0)** — zero on this unit. The field name comes from the reference
  client; zero bias alone cannot establish that a physical sensor exists or is used. The former
  G6/G7/G8 “reference lane” interpretation has since been refuted: those aliases cut across packed
  gyro and acceleration samples in the length-`0x28` PDU.
- `accelerometer_bias` = **(0.160, -0.0687, 10.38)** — the Z axis sits close to **standard gravity
  (9.8 m/s²)**, strongly confirming these are **physical SI-unit floats (m/s²), not raw ADC/LSB
  counts** — the controller was evidently lying flat (Z-up) during factory calibration capture.

**Why this matters for report 0x09:** this is the first *direct* evidence that the genuine
controller's internal calibration model operates in physical floating-point units (dps, m/s²), not
fixed-point LSB counts like this repo's current implementation. It does not by itself prove
anything about report-0x09's phase-field *semantics* (that calibration is applied before the raw
ADC counts ever reach any report), but it raises the prior slightly on "the firmware does real
floating-point sensor math internally" — relevant background for the still-open raw-integration-vs-
fused-orientation question below.

**Feature-flag bit table, also confirmed by this tool** (`FeatureFlagWidget.BIT_FLAGS`), cleanly
matches and *names* the bits behind this repo's already-observed enable mask: bit0=buttons,
bit1=analog sticks, **bit2=IMU**, bit3=unknown, bit4=mouse, bit5=battery current, bit6=unknown,
**bit7=magnetometer**. Our captured enable mask `0x27` = `0b00100111` = bits 0,1,2,5 — buttons +
sticks + IMU + battery-current, **not** bit 7. A direct PID-`0x2069` positive control later replayed
the public `0x94`/bit-7 sequence and re-subscribed handle `0x000A`; every command and ATT operation
succeeded, but that tested sequence emitted no `0x000A` reports. This remains a valid negative for
the bit-7 route only. The reference
`btle_procon2_motion_0x000A.pcapng` capture proves the same controller family can emit raw IMU
samples on handle `0x000A` when bit 2, report selection, and the `0x000A` CCC are initialized
differently. An exact live replay now confirms that selector and reversible CCC ownership.

**Command-protocol cross-check:** `switch2_input_viewer.py`'s BLE `read_spi_memory()` sends
`[0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, size, 0x7E, 0x00, 0x00, addr(4, LE)]` — the same
`[id][0x91][transport][sub]...` shape, the same `id=0x02/sub=0x04` memory-read command, and the same
`c[8]=len, c[12:16]=addr` field positions this repo's `ns2_dispatch()` already implements. **The
command protocol is confirmed identical across USB and BLE transports** (only the framing changes),
which is a meaningful cross-validation of this repo's existing capture-derived protocol model, not
just this one field. (Its `write_spi_memory()` reuses `sub=0x04` rather than this repo's
capture-derived `sub=0x05` for writes — 🔵 unverified whether that's a real alternate protocol
detail or a bug in that tool; not adopted without a citation, since it conflicts with an existing
capture-derived fact.)

## Length-30 byte budget and corrected length-40 multi-sample layout

A research lead surfaced 2026-07-10 (after the second hardware test) asked whether the console's
"angular phase" depends on more than 3-axis gyro + 3-axis accel — magnetometer data, sensor fusion,
or a 9-axis IMU. **This does not fit the confirmed byte budget.** The motion block is **exactly**
30 bytes for length-30 (the only length seen on hardware other than 0): timing(2) + temp(2) +
phase×3(12) + accel×3(12) + tail(2) = 30, with **zero spare bytes** — every byte is already
accounted for by the half-variance/Q16.16 proof above, independent of the magnetometer question.
There is no room in the **length-30 form** for a magnetometer lane. The
"second lane group at 0x15/0x19/0x1D" observation that originally motivated this lead was from the
**refuted int16 model** (see "Why the int16 model looked partially right" above) — those offsets
are inside the Q16.16 accel fields' fractional bytes, already explained, not a separate sensor.

**Corrected by exact component documentation, reference PCAPs, and a live cadence matrix
(2026-07-29):** the
length-`0x28` form is a 4-byte timer/elapsed/status prefix plus a 288-bit, LSB-first, multi-sample
IMU payload. Its encoded 12-bit elapsed count selects one of three exact layouts:

| Tick delta | Layout after the 4-byte PDU prefix | Scale to ordinary ICM counts |
|---:|---|---|
| `0..10` | mode3, carrier s24+s23+s25, accel22, gyro22, accel22, tail16 | carrier ÷4 to common precision; accel ÷256, gyro ÷128 |
| `11..14` | mode2, carrier s22+s21+s23, accel14, gyro13, accel13, gyro14, accel14, tail16 | 13-bit gyro/accel ×2 |
| `15+` | mode2, carrier s22+s21+s23, accel14, gyro16, accel13, gyro16, accel14, reserved-zero pad1 | gyros ÷4; 13-bit accel ×2 |

The exact payload bit ranges and all capture counts are in
[`../experiments/pro2-raw-native-motion-pcap-2026-07-29.md`](../experiments/pro2-raw-native-motion-pcap-2026-07-29.md).
Every corrected acceleration lane measured approximately `1.052 g` in the live same-pose matrix,
and the high-rate accel/gyro axes agree with the bracketed raw handle-`0x000A` stream. The prefix
field boundaries are now exact. High-rate payload bits are
`mode[0..1], carrier0[2..25], carrier1[26..48], carrier2[49..73]`; normal/catch-up uses
`mode[0..1], carrier0[2..23], carrier1[24..44], carrier2[45..67]`. Carrier 2's low two bits
precede its signed high bits. All 2,592 analyzed records use packing mode `3`; the former
“state” values are simply those two low data bits. This corrects both the original equal-width
map and the intermediate false state split.

The elapsed count is self-contained:

```text
elapsed_ticks = (pdu[2] << 4) | (pdu[1] >> 4)
```

It matched the immediately preceding carrier's timer delta in `1274/1274` comparisons across 44
zero-drop files. PDU byte 3 redundantly identifies the layout: `0x0D` high-rate (`865/865`),
`0x0E` normal (`158/158`), and `0x0F` catch-up (`269/269`). This corrects the historical
“secondary status” label for PDU byte 2 and makes layout decoding safe even when a capture omits
the predecessor.

Both length-`0x1E` and length-`0x28` are native Pro Controller 2 forms on this
one clock and carrier trajectory. Length-`0x1E` is not a Switch 1 compatibility
fallback; Switch 1 motion uses the separate report-`0x30` protocol. Length-
`0x28` adds cadence-dependent packed sample history, not an intrinsically newer
or more accurate gyro mode. A 2026-08-01 hardware A/B exposed a generator that
violated this shared-timeline rule; see
[`ds5-pdu40-interleaved-hardware-2026-08-01.md`](../experiments/ds5-pdu40-interleaved-hardware-2026-08-01.md).

The high-rate/normal tail carries two Q3 IMU-temperature samples. Bits `15..6` are their shared
signed ten-bit integer part, bits `2..0` are sample A's fractional eighths, and bits `5..3` are
sample B's fractional eighths. Thus:

```text
integer = sign_extend_10(tail >> 6)
temperature_a_raw = (integer * 8 + tail[2:0]) / 8
temperature_b_raw = (integer * 8 + tail[5:3]) / 8
temperature_c = 25 + temperature_raw / 128
```

The low fractions matched in `993/1023` zero-drop records because the adjacent temperature samples
usually shared the same sub-count value; the unequal cases are real sample variation, not padding.
Two independent zero-drop raw/native/raw A/B/A captures provide the positive control. At 7.5 ms,
handle-`0x000A` raw temperature averaged `4.357` before native and `4.167` after it, while the
decoded native pair averaged `4.28125` (`4.0..4.625`). At 15 ms the raw values were exactly `4`
before and after, while native averaged `3.951923` (`3.25..4.0`). This also explains why the field
had essentially no correlation with tick or motion axes.

A carrier-state- and epoch-aware comparison resolves the earlier capture-dependent prefix fit. After
dividing the high-rate wire lanes by four, the carrier maps affinely to the retained
length-`0x1E` carrier with fixed slopes `sqrt(2)/2^24`, `sqrt(2)/2^23`, and
`sqrt(2)/2^23`. The packet-derived epoch is:

```text
current tick - encoded elapsed + 4 = preceding carrier tick + 4
```

This gives `0.999996` mean absolute correlation in both dynamic captures. The mixed-cadence pitch
fixed-scale normalized RMSE falls from the best constant-offset result `0.008728` to `0.002718`;
the retained moving window remains `0.002771`. Using only the latest preceding length-`0x1E` chart
and retained components to choose modular windows reproduces the interpolated reference with
median/max angular errors `0.000968°/0.010508°` and `0.004682°/0.060233°`, respectively, and zero
observed chart mismatches.

The old `0.268`/`0.370` mismatch came from equal-width decoding; the later apparent state law came
from splitting carrier 2 at the wrong boundary; the differing constant offsets came from ignoring
the encoded elapsed value. This establishes a history-decodable truncated orientation carrier,
but the interpolated reference does not yet prove exact integer rounding.

**Chart-transition correction (2026-07-29):** reciprocal zero-drop lazy-susan captures directly
crossed `3 → 0` and `0 → 3`. Both transitions are smooth when state 0 retains wire
`(G0,G1,G2)` and state 3 uses the state-0-boundary projection `(G1,G2,G0)`. The
boundary deltas are only `0.002563` and `0.001132`. A later zero-drop Splatoon
capture selected the state-0-boundary projection `(G2,G0,G1)` across a `0 → 1` boundary;
its residual is `0.017025`, roughly 29 times smaller than the next permutation.
In the rapid return, 16 of 93 genuine
length-`0x1E` records have retained-vector energy above one (maximum `1.026738`), so the state is
not an exact strict-smallest-three omitted-component selector.
The one prefix epoch straddling each boundary selected chart 0 in both directions with canonical
residuals `0.000144` and `0.000790`; this is direct seam evidence, not yet a universal chart-handoff
law. See
[`../experiments/pro2-mode3-carrier-prefix-2026-07-29.md`](../experiments/pro2-mode3-carrier-prefix-2026-07-29.md)
and
[`../experiments/pro2-carrier-chart-transition-2026-07-29.md`](../experiments/pro2-carrier-chart-transition-2026-07-29.md).

A second zero-drop gameplay capture supplied a held-out `3 → 1 → 0` stress
case. The immediate `1 → 0` edge cannot be made continuous by any unsigned
lane permutation (minimum residual `1.185389`), and one global
permutation-per-state model rises to RMS/max `0.818124/1.252822`. Allowing a
paired sign flip on the two non-boundary lanes reduces that edge to `0.024716`.
The same cyclic omitted-component topology fits all five observed boundaries:
same-sign branches retain the topology permutation; the opposite-sign branch
keeps the boundary lane and negates the other two. Its RMS/max is
`0.025302/0.047878`, and every selected branch beats its alternative by at
least `0.324174`. The earlier permutations are therefore the same-sign branch,
not universal stateless maps. `tools/ns2_motion_chart_solver.py` reports each
edge independently, rejects noncomposable unsigned candidates, and validates
the structured sign branches. State 1 now has both branch types captured;
the later zero-drop state-2 trigger crossed `3 → 2 → 3`. Both reciprocal
state-2 boundaries select topology `(G2,G0,G1)` with opposite-branch signs
`(+,−,−)`, at residuals `0.036162` and `0.011824`. Across all nine accepted
boundaries, all four chart states are directly represented and the cyclic
model has RMS/max `0.023541/0.047878`. Exact integer projection/rounding
remains unresolved.
The same local-frame audit resolves two direct nonzero/nonzero prefix seams:
`3 → 1` selects chart 1 (`0.008416` versus `0.242898`), while `3 → 2`
selects chart 3 (`0.003833` versus `0.196168`).

The historical G6/G7/G8 codec selected payload bits `204..225`, `228..249`, and `252..271`.
Those ranges cross gyro/acceleration fields in every cadence layout: for example, the catch-up
form places gyro 2 at `197..244` and accel 3 at `245..286`, while high-rate places the end of its
gyro at `204..205` and accel 2 at `206..271`. They are not independent 22/22/20-bit sensor lanes.
Their bounded values, near-constant derived norm, and apparently stable reconstructed “second
quaternion” were artifacts of slicing across packed gyro plus gravity-bearing acceleration
samples.

The exact ICM-42670-P FIFO Packet-3/Packet-4 structures were also tested against all 2,275
length-`0x28` records. No header alignment persisted; Packet-4-looking headers at offsets 0/20
occurred in only `10/2275` records by chance. Nintendo/controller firmware repacks the IMU data;
the block is not two native sensor FIFO records. See
[`../experiments/pro2-raw-native-motion-pcap-2026-07-29.md`](../experiments/pro2-raw-native-motion-pcap-2026-07-29.md)
and [`tools/ns2_motion_reference.py`](../../tools/ns2_motion_reference.py).

**Controlled magnetic-stimulus result (2026-07-29):** a zero-drop genuine-Pro2 campaign bracketed
each stimulus with stationary baseline/recovery captures and removed time-weighted drift. It tested
no-magnet controls, two faces/polarities, 100 mm and 50 mm ceramic-magnet distances, and a
neodymium disc down to the closest stable sub-10 mm placement. No candidate lane showed repeatable
polarity reversal or distance scaling. The matched no-magnet G6/G7/G8 residual (`0.0652°`) exceeded
every 100 mm ceramic result (`0.0357°..0.0620°`), and the 50 mm ceramic results did not increase.
This independently refuted G6/G7/G8 as a simple externally responsive magnetic-field vector under
the tested conditions. The later packed-layout correction explains the negative result more
directly: those aliases are mixed gyro/accel bit slices, so they should not respond coherently to a
magnet. The campaign does **not** prove that the controller lacks a magnetometer or that internally
consumed magnetic correction is impossible. See
[`../experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md`](../experiments/pro2-magnetic-stimulus-matrix-2026-07-29.md).

**Historical diagnostic boundary (2026-07-24):** `ns2_motion_pdu40_get_reference()` and
`ns2_motion_pdu40_set_reference()` are byte-exact for the three bit ranges they manipulate, but
their semantic names are obsolete. They are retained only for reproducibility of the failed
experiment; they must not be used to generate production motion or described as magnetic/reference
lanes.

This is **not production `0x28` generation**. The 2026-07-24 live test conclusively rejected the
complete template-derived packet: enabling `ds5motion ref28 on` caused immediate random motion
despite valid length-40 selection, changing the legacy aliases, and zero encoder rejects. Turning
the gate off immediately restored the validated length-`0x1E` path without reflashing. We now know
why: changing those aliases corrupted portions of the newest packed gyro and acceleration samples
while other multi-sample fields remained static. The generator, secondary quaternion, and UART gate
were removed after the test so the rejected packet model cannot be enabled accidentally. See
[`../experiments/refuted-hypotheses.md`](../experiments/refuted-hypotheses.md).

## Remaining unknowns / suggested experiments

1. **Coordinate signs & axis order** the console expects (body-axis integration vs orientation
   angles). 🔵 **Partially resolved 2026-07-10** — re-mining the genuine controller's own
   report-0x05 capture (its "still, then pitch/yaw/roll" protocol) identified raw gyro **X=pitch,
   Z=yaw, Y=roll** with medium confidence (X/Z well-supported by two independent signals each; Y's
   axis identity is by elimination and its sign is inferred from the rotation-properness
   constraint, not measured — the capture's roll segment was too messy to read cleanly). Seam fixed
   accordingly. See [gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md).
   *Still open:* an independently-measured roll sign, from a cleaner recapture (§4 of that doc).
2. **Phase semantics** — Euler vs sensor-axis integrated vs proprietary filtered integrator; drift
   correction unknown. Treat as "integrated angular phase," not strict Euler, until proven.
   🔵 **Reassessed 2026-07-10** against an unattributed third-party claim ("assumed to be some
   form of compressed quaternion... field sizes almost certainly not multiples of 8 bits... motion
   data size is also variable") relayed via `CORTEX_PARSE.md`. **The non-byte-aligned / bit-packed
   part of that claim conflicts with hard evidence already in hand** and is not adopted: the
   half-variance discriminator (§"Method" above) found that splitting each 32-bit accel field
   *exactly* at the 16-bit boundary produces one half with SD≈11-35 (stable, matches gravity) and
   the other with SD≈16-20k (fractional noise) — a genuinely bit-packed, non-byte-aligned format
   would not produce a clean split at a power-of-two bit boundary by chance, across all three axes,
   independently, on two separate captures. Field *boundaries* stay confirmed. The "variable size"
   part is already known and undisputed (length is 0 or 30 today; a `40`-byte variant is
   documented as seen elsewhere). **What does survive, and sharpens an existing open question**:
   whether each byte-aligned int32 "phase" field's *value* is naive integrated rate (this repo's
   current assumption) versus something more like a fused-orientation or quaternion-*component*
   representation still living in that same byte-aligned slot — i.e. the disagreement that matters
   is about semantics, not layout. This reading is also consistent with external evidence: see
   `gyro-hardware-validation-2026-07-10.md` §7.5 — `TommyWabg/Switch2Connect` found that even
   genuine hardware's *raw rate* stream (a different report, over BLE) needs full AHRS fusion
   downstream to be drift-free, i.e. "orientation" is not something Nintendo hardware hands over
   for free either. Distinguishing "raw integrated rate" from "fused orientation state" in the
   USB report-0x09 phase fields is the highest-value remaining experiment for this field —
   candidate method: hold the controller at a **fixed non-zero tilt** (not moving) and check
   whether the phase value matches a static orientation reading (fusion) or drifts/resets (pure
   rate integration has no "current orientation" concept without integrating from a known start).
   🔵 **New reasoning, 2026-07-10** (from `tools/switch2_input_viewer.py`, a working third-party BLE
   client added to this repo — see item 5 below): the later direct reference-PCAP decode corrects
   that tool's 14-byte slice. The **same Pro Controller 2** reports an **18-byte** block at
   `0x000A[0x2A..0x3B]`: uint32 microsecond timestamp plus
   `temp+accelXYZ+gyroXYZ` int16. It reports a **40-byte** packed multi-sample block over
   handle `0x000E`. USB report 0x09 polls at 250 Hz; BLE typically negotiates a much lower
   notification rate. A plausible, mundane explanation for *why* USB carries a single "integrated
   phase" value rather than discrete samples: **at USB's high poll rate, few IMU ticks elapse
   between reports, so one running/integrated value per report loses little** — whereas BLE's
   slower cadence would need to batch multiple raw samples (hence a bigger, multi-sample block) to
   avoid discarding IMU data between notifications. This doesn't prove the phase field is naive
   rate-integration rather than fused orientation, but it **weakens the case that something exotic
   (fusion/quaternion) is *required*** — a simple "integrate raw rate since last report" model is
   sufficient to explain the format shape (single value, byte-aligned, scales with poll rate)
   without needing onboard fusion. Net effect: this repo's existing implementation approach (rate
   integration over real elapsed time) remains a reasonable default; the fixed-tilt experiment above
   is still the right way to settle it empirically.
   🔵 **Sharper alternative, 2026-07-10** (from `docs/experiments/switch2_native_motion_map_DyCOOL.md`,
   a carefully validated third-party decode of the Switch 2's *native BLE* motion report): that
   sibling format encodes raw, individually-clamped (±500°/s) gyro **samples**, not an accumulator —
   a concrete, working alternative value semantic. Not proof for report 0x09 itself (that document's
   own author leaves Pro Controller 2's report 0x09 explicitly unverified), and report 0x09's own
   prior worked-example evidence (smooth, physically-plausible differentiated rates —
   `report-0x09-motion-analysis.md`) still favors the accumulator model on its own terms. Ranked
   analysis of this tension and a new direct-observation experiment (watch the phase accumulator
   itself for discontinuities via a new debug field, no console needed):
   [gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md) §12.
3. **Tail `motion[0x1C]`** — zero in all captures; purpose unknown.
4. **Timing epoch/phase** — does the console require a specific starting tick, or only a consistent
   ~800 Hz progression with correct high-nibble deltas? (High-nibble delta is validated 299/299.)
5. **Length-40 variant** — seen elsewhere, absent from the original static USB capture. 🟢
   **Located 2026-07-10; corrected 2026-07-29:** `tools/switch2_input_viewer.py` confirms a genuine
   Pro Controller 2 emits exactly this 40-byte block over its BLE GATT notification at handle
   `0x000E` (report offset `0xF:0x37`) — a *different transport* from USB report 0x09, so this is
   not proof the USB `40` some other research mentioned is the same format. Direct UART captures
   establish an interleaved `0x1E`/`0x28` stream. A zero-drop UART cadence matrix from 7.5 through
   30 ms proves exact packet boundaries at tick 11 and tick 15, while raw/native/raw bracketing
   validates all signed field widths and scales. Production seven-tick high-rate packets contain
   two signed22 acceleration vectors with eight fractional bits and one signed22 gyro vector with
   seven fractional bits.
   Normal packets contain three acceleration and two gyro samples in mixed 13/14-bit fields.
   Catch-up packets contain three acceleration and two gyro samples in mixed 13/14/16-bit fields.
   The encoded 12-bit elapsed field and `0x0D`/`0x0E`/`0x0F` layout status are resolved.
   The mode-3 prefix has asymmetric carrier lanes, with carrier 2 split around a two-bit low
   fragment, and maps to the length-`0x1E` retained carrier at fixed power-of-two scales. Its epoch
   is four ticks after the preceding carrier. The high-rate/normal tail is two Q3 temperature
   samples, and bit 287 is observed reserved-zero padding. Reciprocal transitions directly resolve
   the stateful cyclic chart topology across all four states and five prefix seam choices. A held-out
   `3 → 1 → 0` capture refutes composition into one stateless unsigned map while validating the
   cyclic paired-sign branch. A later reciprocal `3 → 2 → 3` crossing closes all four chart
   states under the same model; exact integer projection/rounding is now resolved against the
   complete corpus fixtures.
6. **Whether the console strictly validates** beyond timing + physically plausible values.

## Implementation status

🟢 **Current genuine-source path:** a real Pro Controller 2's controller-generated `0x1E`/`0x28`
blocks are transported opaquely and hardware-confirmed in Splatoon 3. This avoids relying on the
generated value semantics discussed below. On disconnect, the last genuine length-30 state is held
stationary while its timing word advances; source-slot and VID/PID ownership prevent reuse by a
different controller.

🟢 **Current DualSense production path:** the host-tested length-`0x1E` carrier remains the default
and is hardware-validated. A complete length-`0x28` high-rate generator and genuine-base hybrid
harness are retained behind default-off UART gates. Their layout, prefix epoch, sample cadence,
field scales, shared clock, and internal physical coherence are host-tested, but the complete
generated recipe was hardware-rejected despite healthy transport counters. Hardware separately
validated byte-identical genuine, acceleration-only, and gyro-only hybrid substitutions; the first
prefix run was invalidated by alternating genuine and donor orientation histories. Its corrected
sequence-wide prefix ownership is host/build validated and intentionally unflashed. The campaign
was deferred on 2026-08-01 because `0x28` adds cadence-dependent history rather than a distinct
higher-fidelity gyro mode. Reopen it only for a concrete `0x1E` deficiency or a new observation
point capable of resolving controller-private filter/FIFO/state behavior.

🔴 **Removed: the per-axis "phase" encoder (2026-07-10 → 2026-08-14).**

`src/switch_pro2/switch_pro2.c` used to contain a second encoder, `ns2_motion_tick()` +
`ns2_encode_motion30()`, that integrated `in.gyro` over real elapsed time into three independent
`uint32` accumulators (`2^32 = 360°`, scale `0.72818` per µs·LSB, `Z` initialized to `0x80000000`)
and wrote them straight into `0x04..0x0F`. It was written against the refuted layout at the top of
this document and was the fallback for any motion source not explicitly routed to the translator.

It never worked on hardware, and it could not have: those bytes are a packed 26/25/24-bit
quaternion with a 2-bit chart state, so an int32 angle lands across slot and state boundaries and
decodes as an arbitrary orientation that jumps whenever a carry crosses bit 24 or 26. That is a
representation error, not a tuning error — which is why the 2026-07-10 bias-tracker and
stillness-gate work correctly fixed the mechanisms it examined (a DualSense really does read
`still=1` stationary) without improving the in-game symptom, and why the symptom was
"abrupt multidirectional jumps" rather than drift. Every controller family later reported as
"motion spams everywhere" was a family that had not yet been added to the translator's source list.

Those experiments remain valid observations of what was measured; see
[gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md) and
[refuted-hypotheses.md](../experiments/refuted-hypotheses.md). What changed is the conclusion drawn
from them.

✅ **Current path: one encoder for every translated source.** `ns2_build_report()` now has exactly
two motion branches — opaque genuine passthrough, and `ns2_motion_consume()` feeding
`ns2_ds5_motion.c` — with no third fallback and no per-source whitelist. A source with motion that
the translator cannot represent emits motion length 0, which is strictly better than emitting a
representation the console cannot decode. Motion remains gated on the `0x0C/0x04` IMU-enable
(`ns2_imu_enabled`, reset per host session in `tud_mount_cb`), matching a real PC2, and accel is
still `in.accel * 65536` (Q16.16).

Per-source frame differences live in exactly one place, `src/bt_hid/motion/ns2_motion_seam.c`: one
row per source, each a proper rotation (determinant +1) from that sensor's frame onto the carrier
frame (X right, Y forward, Z face normal). Adding a controller family means adding a seam row, not
a branch in the encoder.

The bias tracker, stillness gate and warmup described in the 2026-07-10 experiments now live inside
the translator (`ns2_ds5_motion_update()`), which is where they belong: they operate on normalized
input, ahead of the wire encoding.
