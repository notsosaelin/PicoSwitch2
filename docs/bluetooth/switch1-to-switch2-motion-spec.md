# Switch 1 → Switch 2 Motion Translation — End-to-End Specification

**Status:** 🟢 Resolved and hardware-validated (2026-07-27). The sensor-frame remount `R_sw1` is
`src {1,0,2}`, `sign {-1,1,1}` in `ns2_motion_seam.c`, applied identically to accelerometer and
gyroscope, determinant **+1**. A/B against a natively-connected Switch 1 Pro Controller measured
98–100 % identical.

**Why this document exists.** This is the reference trace of the complete path from the LSM6DS3 in a
Switch 1 Pro Controller to the pixels the console moves: which stage owns which transform, what the
interchange units are, and how a new motion source is added. §9 keeps the attempt history because
the failure modes it catalogues are diagnostic for any future IMU family.

**Architecture rule.** Stages 2–6 are shared with the hardware-validated DualSense path. A new
motion source is added by decoding into the §3 interchange units and adding **one row** to the
`ns2_motion_seam.c` table. No other shared stage should need to change; if a proposed fix touches
one, it is almost certainly a decode or seam-row error in the new driver.

---

## 1. The pipeline at a glance

```
 [0] LSM6DS3 in the controller
      | report 0x30, bytes 13..48: 3 frames x (accel XYZ, gyro XYZ) int16 LE, 5 ms apart
      v
 [1] switch_pro_bt.c            <-- decode + calibrate ONLY
      | accel: mean of 3 frames ; gyro: newest frame ; SPI calibration
      | OUTPUT CONTRACT: the sensor's OWN axes, in the §3.1 interchange units
      v
 [2] ns2_seam.c -> ns2_motion_seam.c   (SHARED; per-source table)
      | signed permutation for this motion_source, applied to BOTH accel and gyro
      | accel additionally /2 (8192 counts/g -> the Pro2 carrier's 4096 counts/g)
      v
 [3] switch_pro2.c              (SHARED)
      | motion_source in {DUALSENSE, SWITCH1, WII} -> quaternion translator
      v
 [4] ns2_ds5_motion.c           (SHARED)
      | gyro: EMA -> stillness-gated bias -> integrate into a quaternion
      | accel: PURE PASSTHROUGH (memcpy, never fused)
      v
 [5] report 0x09 motion block, length 0x1E (smallest-three quaternion carrier + accel Q16.16)
      v
 [6] Console
      | blends yaw and roll so turning stays horizontal as the controller tilts
```

---

## 2. Stage 0 — the source (Confirmed)

Report `0x30`, bytes 13–48, three 12-byte IMU frames 5 ms apart; each frame is `accel[XYZ]` then
`gyro[XYZ]`, `int16` little-endian ([`switch1-motion.md`](switch1-motion.md) §5).

| Property | Value | Confidence |
|---|---|---|
| Accel full scale | ±8 g (≈4096 counts/g) | ✅ Confirmed |
| Gyro full scale | ±2000 dps | ✅ Confirmed |
| Gyro sensitivity | 0.070 dps/count (LSM6DS3), **not** the 0.06103 nominal | ✅ Settled on hardware |
| Report rate | 120 Hz Pro / 60 Hz Joy-Con; IMU itself ~200 Hz | ✅ Confirmed |
| Axis frame | X longitudinal, Y +left, Z +face normal | ✅ Confirmed — §6.1 |

Accel and gyro come from the **same die in the same package**, so they share one sensor frame:
`gyro[i]` is rotation about the axis `accel[i]` measures. Every transform must therefore be applied
to both.

## 3. Stage 1 — what the Switch 1 driver produces (the contract)

`switch_pro_bt.c` decodes and calibrates. It does **not** remount axes — that is the seam table's
job (`switch_pro_bt.c:432`, `:610`). The driver publishes the sensor's own axes so that the seam row
is a statement about how the sensor is physically mounted, which is reviewable against a datasheet
or a Linux driver rather than being an opaque product of two transforms.

### 3.1 Interchange units (✅ Confirmed)

Verified directly against `ds5_motion_calibration.c:6-11`:

| Quantity | Convention | DualSense | Switch 1 | Match |
|---|---|---|---|---|
| Gyro | 16.384 counts/dps (±32767 = ±2000 dps) | `DS5_GYRO_CARRIER_RANGE/DS5_GYRO_RANGE_DPS` | `SW1_GYRO_CAL_NUM = 936 × 16.384 = 15335` | ✅ |
| Accel | 8192 counts/g (±4 g) | `DS5_ACCEL_COUNTS_PER_G = 8192` | `SW1_ACCEL_CAL_NUM = 4.0 × 8192 = 32768` | ✅ |

> `dualsense-motion.md` §6 quotes Linux's 1024 counts/dps. The firmware deliberately does **not**
> use that (it needs a 32-bit axis); `ds5_motion_calibration.c` rescales to the 16.384 carrier. Read
> the code, not the Linux constant, when checking scale.

### 3.2 Calibration (✅ implemented, correct)

```
accel = raw * 32768 / (acc_sens - acc_origin)
gyro  = (raw - gyro_origin) * 15335 / (gyro_sens - gyro_origin)
```
Rejected/absent calibration falls back to nominal constants, so motion never depends on the SPI read
completing.

### 3.3 Frame selection (✅ resolved on hardware)

The three IMU frames are **not** treated alike (`switch_pro_bt.c:507-521`):

- **Accel: mean of all three.** It is the console's gravity reference, where steadiness beats
  latency, and gravity does not change fast.
- **Gyro: the newest frame only.** The three frames span 15 ms while the Pro reports every 8.3 ms,
  so averaging added ~7.5 ms of group delay. The newest frame is a zero-order hold that preserves
  angular area exactly (Stage 4 integrates over real elapsed host time), so it does not
  under-integrate. It costs ~√3 more noise, which the encoder's stillness-gated bias tracker and
  low-pass absorb at rest and which is dwarfed by real signal during motion.

## 4. Stages 2–6 — the shared path

### Stage 2 — `ns2_seam.c:279` → `ns2_motion_seam.c` (SHARED)

`ns2_seam.c` selects `motion_source` from the **bound decoder**, never from SDP identity, then calls
`ns2_motion_seam_apply()`. The table:

| Source | accel/gyro `src` | accel/gyro `sign` | det |
|---|---|---|---|
| `GENERIC` | `{0,2,1}` | `{1,-1,1}` | +1 |
| `DUALSENSE` | `{0,2,1}` | `{1,-1,1}` | +1 |
| `WII` | `{0,2,1}` | `{1,-1,1}` | +1 |
| `SWITCH1` | `{1,0,2}` | `{-1,1,1}` | +1 |

- The same signed permutation is applied to accel and gyro — the shared-frame invariant is
  structural, since one loop writes both.
- Accel is additionally halved: 8192 counts/g → 4096 counts/g, matching the genuine PC2's ±8 g.
- Gyro passes 1:1; the 16.384 counts/dps scale is preserved end to end.
- Resulting **Pro2 carrier frame**: `index 0 = pitch, 1 = roll, 2 = yaw`.

**Every row must have determinant +1.** See §6.5 — this is enforced by
`tools/test_ns2_motion_seam.c`, not by comment.

### Stage 3 — `switch_pro2.c:1343` (SHARED)

`motion_source ∈ {DUALSENSE, SWITCH1, WII}` **and** `has_motion` selects the quaternion translator.
Anything else falls through to the **known-bad generic phase encoder** — the original "violent
motion spam". Do not route a new IMU family through it.

### Stage 4 — `ns2_ds5_motion.c` (SHARED)

| Behaviour | Detail |
|---|---|
| **Accel is never fused** | `memcpy` at `:337` and `:520`, written to the PDU at `:594`. Zero accelerometer feedback. |
| Gyro low-pass | EMA, shift 2 |
| Bias | stillness-gated; **`bias_ready` requires 32 consecutive still samples before *any* integration** (`:446-476`) |
| Stillness limits | `GYRO_STILL_LIMIT = GYRO_WARMUP_LIMIT = 40` counts ≈ 2.4 dps |
| `gyro_map` | identity — the seam already did the remount. **Do not add a second remap here.** |
| Rate | `ω = corrected × (π/180) / 16.384` rad/s |
| Integration | body-frame right-multiply, sub-stepped |
| Minimum interval | `NS2_DS5_MOTION_PERIOD_US = 3800` — Switch 1 at 8.3/16.7 ms always passes |
| Host-time clamp | `[500, 16000] µs` — 🔵 a 60 Hz Joy-Con at 16.7 ms is clamped to 16 ms, ~4 % under-integration. Harmless for the Pro. |

Because the translator never fuses accel, the accelerometer we publish is shipped verbatim to the
console.

### Stage 5 — the PDU

Length `0x1E` smallest-three quaternion carrier + accel as Q16.16
(`accel × NS2_DS5_ACCEL_PDU_PER_COUNT`, `= 68963`).

> ✅ **CLOSED 2026-08-14 — `68963` is CORRECT, and 4096 counts/g was the wrong reference.**
> Measured directly from genuine Pro Controller 2 captures: decoding the length-`0x1E` Q16.16
> acceleration lanes of 147 resting motion blocks across
> `dumps/BLE CAPTURE/pro2-magraw-stationary-2026-07-24.jsonl` and
> `pro2-native-baseline-2026-07-24.jsonl` gives a mean resting magnitude of **4310.1 ordinary
> counts** (range 4289–4336). Exact Q16.16 at 4096 counts/g predicts 4096.0 (ratio 1.0523 — wrong);
> `4096 × 68963 / 65536` predicts 4310.2 (ratio **1.0000**). Genuine hardware reports 1 g as ~4310
> wire counts, so `68963` is the target protocol's own scale, not a DualSense correction leaking
> into the shared encoder. Do not "fix" it toward 65536.

### Stage 6 — the console

The console receives an orientation quaternion and our accel. Console gyro aiming blends **yaw and
roll** so that turning stays horizontal as the controller tilts (the standard "player space"
construction). This matters: it means an inverted **roll** lane can destroy **horizontal** aim,
which is not obvious and is exactly what attempt B hit.

---

## 5. Symptom → cause table

| Symptom | What it implies |
|---|---|
| One axis inverted, everything else sharp | A sign error on that lane, applied consistently to accel+gyro |
| Yaw fine, pitch/roll lag ~1–2 s then snap | accel and gyro are in **different frames** (a sign applied to one sensor only) |
| Horizontal aim dead, vertical still moves | The **roll** lane is inverted — it cancels the yaw term at normal hold angles |
| Accel matches genuine hardware but gyro gives no horizontal aim | The row is a **reflection** (det −1). Gravity cannot detect this; only rotation does. |
| Needs far more movement than a DualSense | Scale (`counts/dps`) |
| Violent spam in every direction | Routed to the generic phase encoder — check `motion_source` |
| No motion for the first ~0.3 s | `bias_ready` warmup (32 still samples) — expected |

## 6. `R_sw1` — how the row was resolved

### 6.1 The Switch 1 sensor frame (✅ Confirmed — Linux `hid-nintendo`)

The driver states its frame outright:

> *"X: positive is pointing toward the triggers; Y: positive is pointing to the left; Z: positive is
> pointing up (out of the buttons/sticks)"*

Critically, the only axis transform in that driver is applied to the **right Joy-Con**:

```c
if (jc_type_is_joycon(ctlr) && jc_type_has_right(ctlr)) {
    for (j = 1; j < 6; ++j) { if (j == 3) continue; value[j] *= -1; }
}
```

`jc_type_is_joycon()` is false for a Pro Controller, so **the Pro Controller's raw axes are already
in the stated frame** — no inference needed. (That negation flips Y and Z on both sensors: a 180°
rotation about X, i.e. how the right half is mounted. It also confirms the two halves differ by a
proper rotation, as `switch1-motion.md` §8 says.)

By axis type: **X = longitudinal, Y = lateral (+left), Z = face normal (+out of the face).**

### 6.2 Slot 2 was measured, not assumed (✅ Confirmed)

Reading the accelerometer of a *resting* controller over UART (`input status` reports accel for
exactly this purpose) ended four sessions of sign guessing:

```
accel [-9, 724, 4245]      |a| = 4306 at 4096 counts/g = 1.05 g
```

Gravity sat almost entirely on Pro2 slot 2, and the genuine Pro Controller 2 capture reads *"gravity
≈ all on accel-Z (4279/4309, controller flat)"* — a match to within 1 %. That pinned slot 2 with
nobody touching the controller.

### 6.3 Slots 0 and 1 follow from the frame plus the determinant rule

With slot 2 pinned and det = +1 required, only two rows remain:

| Row | Meaning | Hardware result |
|---|---|---|
| `{1,-1,1}` over `src {1,0,2}` | (−R, −F, +U) | Horizontal aim restored, **pitch inverted** |
| `{-1,1,1}` over `src {1,0,2}` | (+R, +F, +U) | ✅ **Correct** — shipped |

The two differ by a 180° yaw, so they share yaw and invert pitch and roll relative to each other.
One test therefore selected between them.

### 6.4 The resulting map

```
pro2.accel[0] = -sw_accel[1] / 2   pro2.gyro[0] = -sw_gyro[1]    (pitch / lateral, +right)
pro2.accel[1] = +sw_accel[0] / 2   pro2.gyro[1] = +sw_gyro[0]    (roll / longitudinal, +forward)
pro2.accel[2] = +sw_accel[2] / 2   pro2.gyro[2] = +sw_gyro[2]    (yaw / face normal, +up)
```

Validated by A/B against a Switch 1 Pro Controller connected **natively** to the console:
**98–100 % identical**. The small residual lag is present on the native connection too, so it
belongs to the controller (120 Hz reports, no sensor timestamp), not to this firmware.

### 6.5 The determinant rule — enforced, not advisory

A sensor remount is a physical rotation, so the signed permutation must have determinant +1
(permutation parity × product of signs). A determinant of −1 is a reflection and cannot describe any
real mounting.

This is not pedantry, and it is the single most expensive lesson in this document. **Gravity cannot
detect a reflected frame** — one vector looks correct reflected — so an improper row passes every
static check and every resting measurement, then behaves wrongly only under rotation. The SWITCH1
row shipped at det −1 and produced exactly that: an accelerometer matching genuine hardware to
within 1 % while the gyro delivered no horizontal aim at all.

`tools/test_ns2_motion_seam.c:211-235` computes the determinant of every accel and gyro row in the
table and fails the build-time test suite on anything but +1. It is verified against the shipped bug.

## 7. Adding a motion source — the contract

Adding a motion source is a change to one driver file plus one table row:

1. Decode the sensor and calibrate it into the §3.1 interchange units
   (gyro 16.384 counts/dps, accel 8192 counts/g).
2. Publish the sensor's **own axes**. Do not remount in the driver.
3. Tag provenance with a new `SWITCH_MOTION_SOURCE_*` so the quaternion translator is selected
   (§ Stage 3) and per-family policy has somewhere to live.
4. Add one `ns2_motion_seam.c` row describing how that sensor is mounted relative to the Pro2
   carrier frame (`0 = pitch/+right, 1 = roll/+forward, 2 = yaw/+up`). It must have determinant +1.

Nothing downstream of the seam needs to know the source exists.

## 8. Attempt history

Kept because the symptom→lane mapping it establishes is reusable.

Attempts 0–C predate the per-source seam table. In that architecture the driver remounted into the
DualSense event frame and the seam applied one fixed rotation, so these signs are **driver-stage**
signs and are recorded here in their original coordinates.

| # | accel signs | gyro signs | Frames agree? | Hardware result |
|---|---|---|---|---|
| 0 | `+,+,+` | `+,+,+` | ✅ | X fine; **Y inverted**; under-scaled (scale since fixed) |
| A | `+,+,+` | `−,+,+` | ❌ | X sharp; **Y inverted, ~1–2 s lag, then violent snap** — accel and gyro in different frames |
| B | `−,+,−` | `−,+,−` | ✅ | **Horizontal turning completely dead** — read as an inverted roll lane cancelling the yaw term (Stage 6) |
| C | `−,+,+` | `−,+,+` | ✅ | Accel matched genuine hardware to 1 %; gyro produced no horizontal aim |

Attempt C is the configuration that shipped. Composed with the fixed seam of the time it is
`src {1,0,2}` with signs multiplying to +1 — permutation parity −1, so **determinant −1**: the
reflection bug of §6.5.

Post-refactor, with the row stated directly in the seam table:

| # | src | sign | det | Hardware result |
|---|---|---|---|---|
| D | `{1,0,2}` | `{1,-1,1}` | +1 | Horizontal aim restored; **pitch inverted** |
| **E (shipped)** | `{1,0,2}` | `{-1,1,1}` | +1 | ✅ 98–100 % A/B parity with a natively-connected Pro Controller |

What made attempts 0–C expensive was searching signs while assuming the *permutation* was known. It
was not — [`switch1-motion.md`](switch1-motion.md) §8 only ever claimed the axes map "roughly".
Measuring gravity on a resting controller (§6.2) determined permutation and signs together and cut
the remaining search space to two.

> Note for anyone re-deriving this: attempt B's driver signs compose to the same seam row that
> ultimately shipped (E), yet B was recorded as killing horizontal aim outright. The two records
> cannot both be right. The most likely explanation is that the driver-stage permutation was not
> constant across the early attempts — which is precisely the assumption §6.2 overturned — so the
> pre-refactor sign columns should be treated as narrative, not as reproducible coordinates. Only
> rows D and E are stated in coordinates that still exist in the code.

## 9. Confirmed / Hypothesis / Unknown

| Item | Status |
|---|---|
| Report `0x30` IMU layout, 3 frames, 5 ms | ✅ Confirmed |
| Gyro 16.384 counts/dps, accel 8192 counts/g interchange | ✅ Confirmed (`ds5_motion_calibration.c`) |
| SPI calibration formulas and precedence | ✅ Confirmed |
| Gyro newest-frame, accel three-frame mean | ✅ Confirmed on hardware |
| Seam applies one signed permutation to accel and gyro | ✅ Confirmed (code; single loop) |
| Every seam row has determinant +1 | ✅ Enforced (`tools/test_ns2_motion_seam.c`) |
| Translator never fuses accel; accel is passthrough | ✅ Confirmed (code) |
| Switch 1 sensor frame; Pro gets no transform in Linux | ✅ Confirmed (`hid-nintendo`) |
| Pro seam row `src {1,0,2}`, `sign {-1,1,1}` | ✅ Confirmed on hardware (A/B, 2026-07-27) |
| Console blends yaw+roll for horizontal aim | 🟢 Strong Evidence (attempt B) |
| Joy-Con L (`0x2006`) / R (`0x2007`) rows | 🟡 Unverified — they share the Pro's row today. §6.1 says the halves mount the IMU mirrored, so at least one axis is likely wrong on at least one half. The halves differ from the Pro by a *proper* rotation, so any correction must keep determinant +1. |
| `NS2_DS5_ACCEL_PDU_PER_COUNT` 5.2 % excess | 🔵 Open, pre-existing, affects all sources |

## 10. References

- Linux `hid-nintendo` — Switch 1 IMU frame and the right-Joy-Con negation:
  <https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c>
- Linux `hid-playstation` — DualSense parsing, no axis manipulation:
  <https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c>
- SDL `SDL_hidapi_ps5.c` / `SDL_sensor.h` — DualSense passthrough and documented sensor ordering:
  <https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c>
- dekuNukem, `imu_sensor_notes.md` — conversion formulas; confirms the halves differ by a reversed
  axis: <https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/imu_sensor_notes.md>
- [`switch1-motion.md`](switch1-motion.md), [`dualsense-motion.md`](dualsense-motion.md),
  [`wii-motion.md`](wii-motion.md),
  [`../switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md),
  [`../experiments/gyro-hardware-validation-2026-07-10.md`](../experiments/gyro-hardware-validation-2026-07-10.md)
- `src/bt_hid/motion/ns2_motion_seam.c`, `src/bt_hid/ns2_seam.c:255-280`,
  `src/bt_hid/motion/ns2_ds5_motion.c:322-523`,
  `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch_pro_bt.c:395-626`,
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_motion_calibration.c:6-11`,
  `tools/test_ns2_motion_seam.c`
