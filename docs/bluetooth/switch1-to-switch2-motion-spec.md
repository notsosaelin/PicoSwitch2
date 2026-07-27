# Switch 1 → Switch 2 Motion Translation — End-to-End Specification

**Status:** 🟡 Pipeline fully traced and confirmed; one unknown remains (the sensor-frame remount
`R_sw1`, §6), to be **measured** by the protocol in §8 rather than guessed.

**Why this document exists.** Three successive hardware attempts to fix Switch 1 motion by
adjusting axis signs failed, each in a different way (§9). Every one of them changed a sign
because a symptom "looked like" an inversion, without tracing what the symptom actually implied.
This document traces the complete path from the LSM6DS3 in a Switch 1 Pro Controller to the pixels
the console moves, states exactly which stage owns which transform, identifies the single remaining
unknown, and gives a measurement procedure that determines it in one session.

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
      | uses OUR ACCEL as its gravity/"up" reference to split body rotation
      | into horizontal aim (about world up) and vertical aim
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
| **Axis identity** | §8 says "roughly roll(X)/pitch(Y)/yaw(Z)" | 🔴 **"Roughly" is not a spec** |
| **Axis directions** | — | 🔴 **Unknown; §8 says do not hard-code from memory** |

Accel and gyro come from the **same die in the same package**, so they share one sensor frame by
construction: `gyro[i]` is rotation about the axis `accel[i]` measures. This is the fact that makes
§8's measurement protocol possible.

## 3. Stage 1 — what the Switch 1 driver must produce (the contract)

**The output contract is exact:** for identical physical motion, `switch_pro_bt.c` must publish the
same `event.accel[]` and `event.gyro[]` values a **DualSense** would publish.

That is the whole specification. Everything downstream is shared and already validated against
genuine Pro Controller 2 behaviour and Splatoon 3, so matching the DualSense at this boundary is
both necessary and sufficient.

### 3.1 Units (✅ Confirmed, already correct in the firmware)

Verified directly against `ds5_motion_calibration.c:6-11`:

| Quantity | Convention | DualSense | Switch 1 | Match |
|---|---|---|---|---|
| Gyro | 16.384 counts/dps (±32767 = ±2000 dps) | `DS5_GYRO_CARRIER_RANGE/DS5_GYRO_RANGE_DPS` | `SW1_GYRO_CAL_NUM = 936 × 16.384 = 15335` | ✅ |
| Accel | 8192 counts/g (±4 g) | `DS5_ACCEL_COUNTS_PER_G = 8192` | `SW1_ACCEL_CAL_NUM = 4.0 × 8192 = 32768` | ✅ |

> Note: `dualsense-motion.md` §6 quotes Linux's 1024 counts/dps. The firmware deliberately does
> **not** use that (it needs a 32-bit axis); `ds5_motion_calibration.c` rescales to the 16.384
> carrier. Read the code, not the Linux constant, when checking scale.

### 3.2 Axis convention (the DualSense event frame)

`gyro[0]=pitch, gyro[1]=yaw, gyro[2]=roll`; `accel[0]=X, accel[1]=Y, accel[2]=Z`
(`dualsense-motion.md` §5, and the `input_event_t` struct comment).

### 3.3 Calibration (✅ implemented, correct)

```
accel = raw * 32768 / (acc_sens - acc_origin)
gyro  = (raw - gyro_origin) * 15335 / (gyro_sens - gyro_origin)
```
Optional: rejected calibration falls back to nominal constants, so motion never depends on the SPI
read completing.

### 3.4 Frame averaging (✅ correct, and worth understanding)

The three IMU frames are averaged into one published sample. This is right *because* Stage 4
integrates over **real elapsed host time**: mean rate × full interval preserves the exact angular
area. Publishing only the newest frame would under-integrate fast motion by discarding 2/3 of the
samples.

### 3.5 The remount `R_sw1` (🔴 THE ONLY UNKNOWN)

A single 3×3 signed permutation from the Switch 1 sensor frame to the DualSense event frame,
applied **identically to accel and gyro**, with **determinant +1** (a remount is a rotation; a
reflection is not physically realizable). See §6.

## 4. Stages 2–6 — the shared path (traced, confirmed, off-limits)

### Stage 2 — `ns2_seam.c:277-282` (SHARED)

```c
in.accel[0] =  e->accel[0] / 2;    in.gyro[0] =  e->gyro[0];
in.accel[1] = -e->accel[2] / 2;    in.gyro[1] = -e->gyro[2];
in.accel[2] =  e->accel[1] / 2;    in.gyro[2] =  e->gyro[1];
```

- The same rotation `[+e0, −e2, +e1]` is applied to accel and gyro — **the seam already honours
  the shared-frame invariant.** It is a proper rotation (det +1).
- `/2` converts 8192 counts/g → 4096 counts/g, matching the genuine PC2's ±8 g range.
- Gyro passes 1:1; the 16.384 counts/dps scale is preserved end to end.
- Resulting **Pro2 carrier frame**: `index 0 = pitch, 1 = roll, 2 = yaw` — consistent with the
  genuine-controller finding "raw gyro X=pitch, Y=roll, Z=yaw"
  ([`report-0x09-motion.md`](../switch2/report-0x09-motion.md) §"Remaining unknowns" item 1).

### Stage 3 — `switch_pro2.c` (SHARED)

`motion_source ∈ {DUALSENSE, SWITCH1}` **and** `has_motion` selects the quaternion translator. Any
other source falls through to the **known-bad generic phase encoder**, which is what produced the
original "violent motion spam". Do not route a new IMU family through it.

### Stage 4 — `ns2_ds5_motion.c` (SHARED) — the stage that surprised us

| Behaviour | Detail |
|---|---|
| **Accel is never fused** | `memcpy(state->accel, input->accel, …)` at `:337` and `:520`, then written to the PDU at `:594`. The translator does **zero** accelerometer feedback. |
| Gyro low-pass | EMA, shift 2 |
| Bias | stillness-gated tracker; **`bias_ready` requires 32 consecutive still samples before *any* orientation is integrated** (`:446-476`) |
| Stillness limits | `GYRO_STILL_LIMIT = GYRO_WARMUP_LIMIT = 40` counts ≈ 2.4 dps |
| `gyro_map` | identity — the seam already did the physical remount. **Do not add a second remap here.** |
| Rate | `ω = corrected × (π/180) / 16.384` rad/s |
| Integration | body-frame right-multiply, sub-stepped |
| Minimum interval | `NS2_DS5_MOTION_PERIOD_US = 3800` — Switch 1 at 8.3 ms (Pro) / 16.7 ms (Joy-Con) always passes |
| Host-time clamp | `[500, 16000] µs` — 🔵 a 60 Hz Joy-Con at 16.7 ms is clamped to 16 ms, a ~4 % under-integration. Harmless for the Pro; note it before trusting Joy-Con rate fidelity. |

**This is the single most important fact in this document:** because the translator never fuses
accel, the accelerometer we publish is not a local correction term — it is shipped verbatim to the
console (Stage 6) as the **gravity reference**. Getting it wrong does not degrade gracefully.

### Stage 5 — the PDU

Length `0x1E` smallest-three quaternion carrier + accel as Q16.16
(`accel × NS2_DS5_ACCEL_PDU_PER_COUNT`, `= 68963`).

> 🔵 **Open, pre-existing, not a Switch 1 issue:** with 4096 counts/g arriving, exact Q16.16 would
> need `65536`, but the constant is `68963` — 5.2 % high. This is on the hardware-validated
> DualSense path and applies to every source equally, so it is recorded here as an observation, not
> touched. Worth resolving separately.

### Stage 6 — the console

The console receives an orientation quaternion **and** our accel. It uses gravity to establish
world "up", which is what lets it decompose body rotation into **horizontal aim** (rotation about
world up) and **vertical aim**. Gravity carries **no yaw information** — rotating about the gravity
vector does not change the measured vector — but it determines the *axis* the console calls
vertical, so a wrong gravity vector still destroys horizontal aiming.

---

## 5. Symptom → cause table (derived from the traced pipeline)

This is the payoff of the trace: symptoms now map to causes instead of to guesses.

| Symptom | What it implies | What it does **not** imply |
|---|---|---|
| One axis inverted, everything else sharp and smooth | A sign error in `R_sw1`, applied consistently to accel+gyro | Not a filtering or scale problem |
| Yaw perfect, pitch/roll lag ~1–2 s then snap | accel and gyro are in **different frames** — the console's gravity correction is fighting the quaternion | Not a gyro sign problem; flipping the gyro sign cannot fix it |
| Horizontal aim dead, vertical still moves | The **gravity vector** is wrong, so "world up" is wrong and rotation no longer projects onto the horizontal | Not a yaw gyro problem — check accel first |
| Needs far more movement than a DualSense | Scale (`counts/dps`) | Not a sign or remount problem |
| Violent spam in every direction | Routed to the generic phase encoder instead of the quaternion translator | Not a data problem — check `motion_source` |
| No motion at all for the first ~0.3 s | `bias_ready` warmup (32 still samples) — expected, not a bug | — |

## 6. The remaining unknown: `R_sw1`

One 3×3 signed permutation matrix. Constraints already established:

1. **Applies to accel and gyro identically** — they share the sensor die (§2).
2. **Determinant +1** — a remount is a rotation, not a reflection.
3. Maps the Switch 1 sensor frame onto the DualSense event convention (§3.2).

That is 24 candidates (6 permutations × 4 sign patterns with det +1). Guessing has a 1-in-24 hit
rate, which is precisely why three attempts failed. **It must be measured.**

### Why the permutation itself is in doubt, not just the signs

Every previous attempt assumed the permutation `[1,2,0]` (from §8's *"roughly* roll(X)/pitch(Y)/
yaw(Z)") and treated only signs as unknown. But §8 says "roughly", gives no directions, and
explicitly instructs measuring rather than hard-coding. If the true axis identity is, say,
`0=pitch, 1=roll, 2=yaw`, then no sign pattern on `[1,2,0]` can ever be right — which is consistent
with the observed history, where no sign combination produced correct behaviour on all three axes.

## 7. Why gravity alone determines the whole matrix

Because accel and gyro share the sensor frame, **`R_sw1` can be determined entirely from static
gravity measurements — no rotation measurements are needed.** Three orthogonal static poses give
three linearly independent gravity directions, which fully determine a 3×3 matrix. The gyro
transform is then the same matrix, by the shared-frame invariant.

This converts a fiddly dynamic measurement ("rotate about one axis and watch which lane moves, and
in which direction, while it is moving") into three still readings. It is also far more robust:
a stationary reading has no rate, no bias, no filter lag, and no timing dependence.

## 8. Measurement protocol (the actual next step)

**Reference first, then subject.** The DualSense is the known-good device; the goal is to make the
Switch 1 produce the same numbers.

For each of the two controllers, hold each pose still and run the config-mode `imu` command,
recording the `a:[…]` accel triple:

| Pose | Description |
|---|---|
| **A** | Resting flat on a table, face up (buttons up, sticks up) |
| **B** | Held vertical, nose/USB port pointing at the ceiling |
| **C** | Rolled 90° onto its left edge (right grip toward the ceiling) |

In each pose exactly one axis should read ≈ ±1 g (±4096 in the Pro2 frame that `imu` prints, since
it reads post-seam) and the other two ≈ 0.

**Derivation.** Build the 3×3 from the three DualSense readings and the three Switch 1 readings;
`R_sw1` is the matrix taking the Switch 1 triples to the DualSense triples. Because both are signed
permutations, this reduces to simple inspection: for each pose, note which slot holds the ±1 g and
its sign in each device, and read the permutation and signs straight off.

**Validation.** After applying `R_sw1`, re-run all three poses on the Switch 1 and confirm the
triples now match the DualSense's. Only then test motion in-game. `det(R_sw1)` must be +1; if the
readings imply −1, one pose was mis-held — repeat rather than shipping a reflection.

## 9. Attempt history (so these are not repeated)

| # | `R_sw1` | Frames consistent? | det | Hardware result |
|---|---|---|---|---|
| 0 | perm `[1,2,0]`, all `+` | ✅ yes | +1 | X fine; **Y inverted**; Y under-scaled (scale since fixed) |
| A | perm `[1,2,0]`, gyro pitch `−` only | ❌ **no** — accel unsigned | −1 (gyro) | X sharp and correct; **Y inverted, ~1–2 s lag, then violent snap** |
| B | perm `[1,2,0]`, pitch `−` roll `−`, both sensors | ✅ yes | +1 | **Horizontal turning completely dead** |

**What the history proves.** Attempt B negated `accel[0]` and `accel[2]`, mirroring the gravity
vector, and horizontal aim died — confirming Stage 6's dependence on our accel as its "up"
reference, and confirming that attempt 0/A's *unsigned* accel permutation was much closer to
correct. But attempt 0 was already frame-consistent with an unsigned accel and still showed an
inverted Y. **Those two facts cannot both be satisfied by any sign pattern on the permutation
`[1,2,0]`** — which is the direct evidence that the permutation itself is wrong, not merely its
signs. Hence §6 and the measurement in §8.

## 10. Confirmed / Hypothesis / Unknown

| Item | Status |
|---|---|
| Report `0x30` IMU layout, 3 frames, 5 ms | ✅ Confirmed |
| Gyro 16.384 counts/dps, accel 8192 counts/g interchange | ✅ Confirmed (read from `ds5_motion_calibration.c`) |
| SPI calibration formulas and precedence | ✅ Confirmed |
| Seam applies one proper rotation to accel and gyro | ✅ Confirmed (code) |
| Translator never fuses accel; accel is passthrough | ✅ Confirmed (code) |
| Console uses our accel as its gravity/up reference | 🟢 Strong Evidence (attempt B killed horizontal aim) |
| Pro2 carrier frame = `[pitch, roll, yaw]` | 🟢 Strong Evidence (seam + report-0x05 capture, medium confidence upstream) |
| Switch 1 sensor axis identity and directions | 🔴 **Unknown — measure (§8)** |
| Joy-Con L/R frames (mirrored per §8) | 🔴 Unknown — measure separately per half |
| `NS2_DS5_ACCEL_PDU_PER_COUNT` 5.2 % excess | 🔵 Open, pre-existing, affects all sources |

## 11. References

- [`switch1-motion.md`](switch1-motion.md) — source format, calibration, §8 axis guidance
- [`dualsense-motion.md`](dualsense-motion.md) — the reference convention
- [`../switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md) — the console-facing carrier
- `src/bt_hid/ns2_seam.c:277-282`, `src/bt_hid/motion/ns2_ds5_motion.c:322-523`,
  `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_motion_calibration.c:6-11`
