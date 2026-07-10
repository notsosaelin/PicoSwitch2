# Switch 2 Pro Controller — Report 0x09 Motion (IMU) Format

**Status:** ✅ **Format confirmed** (int32 angular-phase + Q16.16 accelerometer), cross-validated
on two independent captures. 🔵 Coordinate signs/axis order and phase filtering still inferred.
**Confidence:** Very high for field boundaries, timing word, accel Q16.16 scale; High (inferred)
for the 32-bit *angular-phase* semantics; Unknown for exact console-expected signs/permutation.

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
   capture, 50,650 motion reports) via `scratchpad/usb_c_verify_int32.py`. Results:

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
length byte is at report offset `0x0E` (`0` = IMU off, `30` = IMU on; a `40` variant seen in other
research is absent from all our captures).

| motion off | Size | Type | Field | Notes |
|---|---|---|---|---|
| `0x00` | 2 | `uint16_le` | **Timing** | low 12 bits = 800 Hz IMU tick (mod 4096); high 4 bits = ticks elapsed since previous report (usually 3–4). |
| `0x02` | 2 | `int16_le` | **Temperature** | constant `0x0C00` (3072). ICM: °C = raw/128 + 25 ⇒ 49 °C. |
| `0x04` | 4 | `int32_le` | **Angular phase X** | binary angle, `2^32 = 360°`. Integrated/filtered angular phase. |
| `0x08` | 4 | `int32_le` | **Angular phase Y** | |
| `0x0C` | 4 | `int32_le` | **Angular phase Z** | starts near `0x80000000` (≈ −180°). |
| `0x10` | 4 | `int32_le` | **Accel X** | **Q16.16 fixed point**; `65536 × 4096 = 1 g`. |
| `0x14` | 4 | `int32_le` | **Accel Y** | ICM-42670-P ±8 g ⇒ 4096 LSB/g. |
| `0x18` | 4 | `int32_le` | **Accel Z** | |
| `0x1C` | 2 | — | **Tail** | zero in all captures; unresolved. |

```c
typedef struct __attribute__((packed)) {
    uint16_t timing;        // (count << 12) | (tick & 0x0FFF)
    int16_t  temperature;   // 0x0C00
    int32_t  phase[3];      // binary angle, 2^32 == 360 deg
    int32_t  accel_q16[3];  // Q16.16, 65536*4096 == 1 g
    uint16_t tail;          // 0
} ns2_motion30_t;           // sizeof == 30
```

### Conversions
```
gyro_dps    = (int32_t)(phase - prev_phase) * 360.0 * 800.0 / (2^32 * sample_count)
accel_g     = accel_q16 / (65536.0 * 4096.0)
accel_count = accel_q16 / 65536.0            // ordinary ICM count (~±4096 = 1 g)
```

## Activation — motion is a *negotiated feature* (not always-on)

Confirmed by both `report-0x09-motion-analysis.md` and this repo's Experiment C
([gyro-experiment-c-results.md](../experiments/gyro-experiment-c-results.md)): report `0x09`
streams from power-up with **motion-len = 0** and only flips to `30` after the host enables the
IMU feature over bulk EP2:
```
0c 91 00 06 00 04 00 00 …          # configure motion features (ids 0x02/0x03)
0c 91 00 04 00 04 00 00 27 00 00 00 # feature mask 0x27 (IMU bit set, no magnetometer) -> motion ON
```
(~174–251 zero-length reports precede motion-on in the captures.) **Our firmware currently emits
motion always-on — the inverse of the hardware.** See Experiment C for the exact packet trace.

## Why the int16 model looked partially right

A Q16.16 accel value is `[ int16 high ][ frac16 low ]`. Reading `(int16)(accel_q16 >> 16)` yields
the ordinary gravity count (~±4096) — so the old "accel" lane looked correct and produced a
plausible 1 g magnitude. But the adjacent "gyro" lane was `(uint16)(accel_q16 & 0xFFFF)` — the
**fractional half** — which sweeps most of the 16-bit range and looks like random full-scale gyro.
Likewise the "first sample" was the low/high halves of the 32-bit phase fields. Replace
`gyro16,accel16 × repeated` with `phase32 × 3, accel_q16_32 × 3`.

## Calibration (from report-0x09-motion-analysis.md; not yet needed for basic gyro)

- User motion-calibration region: address `0x001FC000`, length `0x40`.
- "Reset to default" writes 64 × `0xFF`. "Calibrate" writes validity marker `B2 A1` (LE `0xA1B2`)
  + zeros. Nonzero coefficient fields were **not** captured — internal scale/order still unknown.
- The controller appears to apply its calibration internally at startup (not sent over the host
  protocol), so basic emulation can ignore it initially.

## Remaining unknowns / suggested experiments

1. **Coordinate signs & axis order** the console expects (body-axis integration vs orientation
   angles). *Best next capture:* controller still, then one slow rotation about each physical axis
   separately — locks down sign, order, and phase scale.
2. **Phase semantics** — Euler vs sensor-axis integrated vs proprietary filtered integrator; drift
   correction unknown. Treat as "integrated angular phase," not strict Euler, until proven.
3. **Tail `motion[0x1C]`** — zero in all captures; purpose unknown.
4. **Timing epoch/phase** — does the console require a specific starting tick, or only a consistent
   ~800 Hz progression with correct high-nibble deltas? (High-nibble delta is validated 299/299.)
5. **Length-40 variant** — seen elsewhere, absent here; likely 3 samples at a higher rate.
6. **Whether the console strictly validates** beyond timing + physically plausible values.

## Implementation status

`src/switch_pro2/switch_pro2.c` `ns2_build_report()` still emits the **old int16 model** and
**always-on** motion. Rewriting it to the int32 layout above + gating on the `0x0C` feature-enable
is the top firmware task (tracked in PLAN.md / STATUS.md). Not yet done — pending the current
reverse-engineering pass (Experiment A resolves the Steam/report-0x05 side before implementation).
