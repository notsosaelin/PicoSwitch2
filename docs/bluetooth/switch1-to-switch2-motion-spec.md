# Switch 1 → Switch 2 Motion Translation — End-to-End Specification

**Status:** 🟢 Pipeline fully traced; the sensor-frame remount `R_sw1` is now **derived from
documented axis frames plus the three existing hardware results** (§6). One hardware confirmation
outstanding (§7).

**Why this document exists.** Three successive hardware attempts to fix Switch 1 motion by
adjusting axis signs failed, each in a different way (§9). Every one of them changed a sign because
a symptom "looked like" an inversion, without tracing what the symptom actually implied. This
document traces the complete path from the LSM6DS3 in a Switch 1 Pro Controller to the pixels the
console moves, states which stage owns which transform, and derives the remount from published
sources — no controller handling required.

**Scope rule, non-negotiable.** Stages 2–6 below are shared with the hardware-validated DualSense
path. **They must not be modified for Switch 1.** The entire Switch 1 problem is confined to
Stage 1. If a proposed fix touches anything downstream of Stage 1, it is the wrong fix.

---

## 1. The pipeline at a glance

```
 [0] LSM6DS3 in the controller
      | report 0x30, bytes 13..48: 3 frames x (accel XYZ, gyro XYZ) int16 LE, 5 ms apart
      v
 [1] switch_pro_bt.c            <-- THE ONLY STAGE WE OWN FOR SWITCH 1
      | average 3 frames -> calibrate -> remount R_sw1 -> publish
      | OUTPUT CONTRACT: input_event_t.accel/gyro in the *DualSense event convention*
      v
 [2] ns2_seam.c                 (SHARED - do not modify)
      | accel /2 ; fixed rotation [+e0, -e2, +e1] applied to BOTH accel and gyro
      v
 [3] switch_pro2.c              (SHARED)
      | motion_source in {DUALSENSE, SWITCH1} -> quaternion translator
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
`gyro[i]` is rotation about the axis `accel[i]` measures.

## 3. Stage 1 — what the Switch 1 driver must produce (the contract)

**The output contract is exact:** for identical physical motion, `switch_pro_bt.c` must publish the
same `event.accel[]` and `event.gyro[]` values a **DualSense** would publish.

That is the whole specification, and it is also what makes any downstream error harmless to reason
about: whatever the seam does, it does identically to both sources, so matching the DualSense at
this boundary is correct even if a downstream sign is wrong.

### 3.1 Units (✅ Confirmed, already correct in the firmware)

Verified directly against `ds5_motion_calibration.c:6-11`:

| Quantity | Convention | DualSense | Switch 1 | Match |
|---|---|---|---|---|
| Gyro | 16.384 counts/dps (±32767 = ±2000 dps) | `DS5_GYRO_CARRIER_RANGE/DS5_GYRO_RANGE_DPS` | `SW1_GYRO_CAL_NUM = 936 × 16.384 = 15335` | ✅ |
| Accel | 8192 counts/g (±4 g) | `DS5_ACCEL_COUNTS_PER_G = 8192` | `SW1_ACCEL_CAL_NUM = 4.0 × 8192 = 32768` | ✅ |

> `dualsense-motion.md` §6 quotes Linux's 1024 counts/dps. The firmware deliberately does **not**
> use that (it needs a 32-bit axis); `ds5_motion_calibration.c` rescales to the 16.384 carrier. Read
> the code, not the Linux constant, when checking scale.

### 3.2 Axis convention (the DualSense event frame)

`gyro[0]=pitch, gyro[1]=yaw, gyro[2]=roll`; `accel[0]=X, accel[1]=Y, accel[2]=Z`. Confirmed
identical to SDL's documented sensor ordering (§6.2).

### 3.3 Calibration (✅ implemented, correct)

```
accel = raw * 32768 / (acc_sens - acc_origin)
gyro  = (raw - gyro_origin) * 15335 / (gyro_sens - gyro_origin)
```
Rejected/absent calibration falls back to nominal constants, so motion never depends on the SPI read
completing.

### 3.4 Frame averaging (✅ correct)

The three IMU frames are averaged into one published sample. This is right *because* Stage 4
integrates over **real elapsed host time**: mean rate × full interval preserves the exact angular
area. Publishing only the newest frame would under-integrate fast motion.

## 4. Stages 2–6 — the shared path (traced, confirmed, off-limits)

### Stage 2 — `ns2_seam.c:277-282` (SHARED)

```c
in.accel[0] =  e->accel[0] / 2;    in.gyro[0] =  e->gyro[0];
in.accel[1] = -e->accel[2] / 2;    in.gyro[1] = -e->gyro[2];
in.accel[2] =  e->accel[1] / 2;    in.gyro[2] =  e->gyro[1];
```

- The same rotation is applied to accel and gyro — the seam honours the shared-frame invariant.
- `/2` converts 8192 counts/g → 4096 counts/g, matching the genuine PC2's ±8 g range.
- Gyro passes 1:1; the 16.384 counts/dps scale is preserved end to end.
- Resulting **Pro2 carrier frame**: `index 0 = pitch, 1 = roll, 2 = yaw`.
  ⚠️ **The roll lane's sign here was never measured** — see §6.5. This is the likely root cause of
  the whole Switch 1 saga.

### Stage 3 — `switch_pro2.c` (SHARED)

`motion_source ∈ {DUALSENSE, SWITCH1}` **and** `has_motion` selects the quaternion translator.
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

> 🔵 **Open, pre-existing, not a Switch 1 issue:** with 4096 counts/g arriving, exact Q16.16 would
> need `65536`, but the constant is `68963` — 5.2 % high. It is on the validated DualSense path and
> applies to every source equally, so it is recorded, not touched.

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
| Needs far more movement than a DualSense | Scale (`counts/dps`) |
| Violent spam in every direction | Routed to the generic phase encoder — check `motion_source` |
| No motion for the first ~0.3 s | `bias_ready` warmup (32 still samples) — expected |

## 6. `R_sw1` — resolved from documented frames + existing hardware results

No controller handling was required. Both frames are published; the permutation follows from
matching them by axis type, and the three signs follow from the three tests already run.

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

### 6.2 The DualSense event frame (✅ axis types confirmed)

SDL reads DualSense accel/gyro with **no reordering and no negation** — raw indices 0/1/2 pass
straight through — and SDL documents `values[0]=pitch, values[1]=yaw, values[2]=roll`, matching the
repo's convention exactly.

The face-normal axis is pinned locally and independently: the repo's paired Pro2/DualSense gravity
capture produced the seam relation `Pro2[2] = +e[1]`, and the genuine Pro2 capture shows
`gravity ≈ all on accel-Z (4279/4309, controller flat)` — so Pro2 index 2 is the face normal, and
therefore **DualSense Y is the face normal**. Consistent with `gyro[1] = yaw`, since yaw is rotation
about the face normal when flat.

By axis type: **X = lateral (pitch axis), Y = face normal (yaw axis), Z = longitudinal (roll
axis).**

### 6.3 The permutation (✅ now confirmed, previously only "roughly")

| Event slot | Axis type | Switch 1 source index |
|---|---|---|
| 0 — pitch / lateral | lateral | **1** (sw1 Y, +left) |
| 1 — yaw / face normal | face normal | **2** (sw1 Z) |
| 2 — roll / longitudinal | longitudinal | **0** (sw1 X) |

`remount = {1, 2, 0}` — the same permutation used all along, but now supported by two independent
documented sources instead of §8's word *"roughly"*. The earlier conclusion that the permutation
*must* be wrong rested on the determinant assumption that §6.5 shows to be unsound.

### 6.4 The signs (from the three hardware results)

| Lane | Sign | Evidence |
|---|---|---|
| yaw (slot 1) | **+1** | Attempts 0 and A both used `+sw[2]` and both gave sharp, correct horizontal aim |
| pitch (slot 0) | **−1** | Attempt 0 was fully frame-consistent with `+1`, and pitch was inverted |
| roll (slot 2) | **+1** | Attempt B flipped roll to `−1` and horizontal aim died outright |

On the last row: because the console blends yaw and roll (Stage 6), an inverted roll term partially
cancels the yaw term at normal hold angles. That is why flipping **roll** kills **horizontal** aim
while the yaw lane itself is untouched — it is the only lane whose flip produces that specific
symptom, so attempt B identifies it uniquely.

### 6.5 Why the result is improper — and why that is the finding, not the bug

`(−1) × (+1) × (+1) = −1`. A physical remount is a rotation and can never be improper, so something
downstream already carries a sign error.

It is recorded in this repo's own experiment log,
`docs/experiments/gyro-hardware-validation-2026-07-10.md`:

```
out_Y (roll)  = +src_roll   (was: -src_pitch)   <- sign chosen to keep det = +1, not independently measured
```

and §2.2 of that document rates the roll lane 🔵 **medium confidence**, its axis identity
established *"only by elimination"*, with the planned confirming recapture *"still outstanding"*.

**The seam's roll sign is therefore an assumption that was never verified.** If it is wrong, every
source routed through the seam needs a compensating flip on exactly one lane — precisely the `−1`
that falls out here. The improper determinant is the fingerprint of a known, recorded, unmeasured
assumption.

This also explains why three attempts failed: attempts 0 and B were the only two *proper* sign
patterns with yaw held at `+1`, so once properness was treated as a hard constraint the correct
answer was outside the search space entirely.

### 6.6 The resulting map

```
event.accel[0] = -sw_accel[1]    event.gyro[0] = -sw_gyro[1]     (pitch / lateral)
event.accel[1] = +sw_accel[2]    event.gyro[1] = +sw_gyro[2]     (yaw / face normal)
event.accel[2] = +sw_accel[0]    event.gyro[2] = +sw_gyro[0]     (roll / longitudinal)
```

Applied to accel and gyro identically. This has **never been tested**: attempt A used these gyro
signs but left accel unsigned, which is what produced its drift-and-snap.

## 7. Prediction and falsification

If correct: pitch correct, horizontal aim correct, no drift-then-snap, no dead axis.

- **Pitch still inverted, everything else right** → the pitch sign is wrong and the true determinant
  is `+1`, meaning the seam's roll sign is actually fine and the roll-cancellation reading of
  attempt B was wrong.
- **Horizontal aim dies again** → roll is the wrong lane to compensate on; move the compensation to
  the pitch lane.

Each outcome identifies a different lane, so one test is decisive either way.

## 8. The real fix, deferred deliberately

The root cause is the seam's unmeasured Pro2 roll sign. Fixing it there would let every source use
a proper transform and remove this compensation.

It is **not** done now because the seam is shared with the hardware-validated DualSense path, and
changing it would re-open a working configuration to improve a determinant. Correct sequencing:
confirm Switch 1 works with the local compensation → measure the Pro2 roll sign via the recapture
`gyro-hardware-validation-2026-07-10.md` §4 already specifies → fix the seam and flip both sources'
roll lanes together.

## 9. Attempt history

| # | accel signs | gyro signs | Frames agree? | det | Hardware result |
|---|---|---|---|---|---|
| 0 | `+,+,+` | `+,+,+` | ✅ | +1 | X fine; **Y inverted**; under-scaled (scale since fixed) |
| A | `+,+,+` | `−,+,+` | ❌ | −1 (gyro only) | X sharp and correct; **Y inverted, ~1–2 s lag, then violent snap** |
| B | `−,+,−` | `−,+,−` | ✅ | +1 | **Horizontal turning completely dead** |
| **C (current)** | `−,+,+` | `−,+,+` | ✅ | −1 | *pending* |

Attempts 0 and B are the only proper patterns with yaw `+1`; both are refuted, which is what forces
the improper C and points at the seam.

> **Correction to an earlier reading.** Attempt B was first explained as "the gravity vector was
> mirrored, so the console lost its up reference". That is wrong: attempts A and B differ by a 180°
> rotation **about the face normal**, which leaves the gravity vector unchanged. B's dead axis is
> therefore not a gravity problem, and the roll-lane explanation in §6.4 replaces it.

## 10. Confirmed / Hypothesis / Unknown

| Item | Status |
|---|---|
| Report `0x30` IMU layout, 3 frames, 5 ms | ✅ Confirmed |
| Gyro 16.384 counts/dps, accel 8192 counts/g interchange | ✅ Confirmed (`ds5_motion_calibration.c`) |
| SPI calibration formulas and precedence | ✅ Confirmed |
| Seam applies one rotation to accel and gyro | ✅ Confirmed (code) |
| Translator never fuses accel; accel is passthrough | ✅ Confirmed (code) |
| Switch 1 sensor frame; Pro gets no transform in Linux | ✅ Confirmed (`hid-nintendo`) |
| DualSense axis ordering/types | ✅ Confirmed (SDL + local paired capture) |
| Event-frame permutation `{1,2,0}` | ✅ Confirmed (axis-type matching) |
| Event-frame signs `(−1,+1,+1)` | 🟢 Strong Evidence — derived from three hardware results |
| Console blends yaw+roll for horizontal aim | 🟢 Strong Evidence (attempt B) |
| **Seam's Pro2 roll sign** | 🔴 **Never measured** — chosen for properness; likely root cause |
| Joy-Con L/R frames | 🔵 Derivable — Linux negates Y/Z on both sensors for the right half (§6.1) |
| `NS2_DS5_ACCEL_PDU_PER_COUNT` 5.2 % excess | 🔵 Open, pre-existing, affects all sources |

## 11. References

- Linux `hid-nintendo` — Switch 1 IMU frame and the right-Joy-Con negation:
  <https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c>
- Linux `hid-playstation` — DualSense parsing, no axis manipulation:
  <https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c>
- SDL `SDL_hidapi_ps5.c` / `SDL_sensor.h` — DualSense passthrough and documented sensor ordering:
  <https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c>
- dekuNukem, `imu_sensor_notes.md` — conversion formulas; confirms the halves differ by a reversed
  axis: <https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/imu_sensor_notes.md>
- [`switch1-motion.md`](switch1-motion.md), [`dualsense-motion.md`](dualsense-motion.md),
  [`../switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md),
  [`../experiments/gyro-hardware-validation-2026-07-10.md`](../experiments/gyro-hardware-validation-2026-07-10.md)
- `src/bt_hid/ns2_seam.c:277-282`, `src/bt_hid/motion/ns2_ds5_motion.c:322-523`,
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_motion_calibration.c:6-11`
