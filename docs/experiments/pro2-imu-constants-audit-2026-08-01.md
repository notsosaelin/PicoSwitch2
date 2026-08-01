# Pro Controller 2 IMU constants — audit without hardware — 2026-08-01

## Question

Which of the IMU constants this project relies on were **measured**, and which
were **assumed and then treated as measured**? Resolve what can be resolved
from captures alone, and name what cannot.

Prompted by the gyro sensitivity turning out to be an assumption. The concern
generalises: a constant that is wrong but self-consistent survives every test
that compares our code against our own code.

## Method — what is measurable with no controller, console or UART

Three references in the existing captures need no hardware and no protocol
assumption:

1. **Gravity is exactly 1 g.** At rest the accelerometer measures it and
   nothing else, so `|a|` in counts *is* counts/g. Across many orientations a
   sphere fit separates a scale error from a per-axis bias, which look
   identical in a single pose.
2. **The `0x1E` carrier is a unit quaternion.** Verified: `|q| = 1.0000` in
   every capture, so `2·acos(w)` is a real angle and the carrier can serve as
   an angular reference.
3. **Host timestamps.** `motionpair` records carry `t_us` beside each PDU, so
   the controller's own tick can be measured against wall time.

Nintendo's earlier motion protocols are *not* used as a source of values here.
A constant that happens to match the Wii or Switch 1 is a hypothesis, not
evidence, and adopting one would reproduce the error being audited.

## ✅ Acceleration: 4096 counts/g is right; the 5% is a sensor bias

At rest the corpus reads `|a| = 4307.5` counts (1686 packets, p05 4302.4 to
p95 4312.4 — a 0.23% spread). Under the assumed 4096 that is **1.052 g**.

This number was already in the repository. The scale-normalization work
recorded that all eight acceleration slots "agree on 1.051–1.052 g" and treated
the agreement as success. The agreement validated *relative* wire scaling
across layouts; the absolute 5% error was visible in the same figure and was
never questioned.

It is **not** a scale error. Every at-rest packet in the corpus is in one pose
(Z+ 1678, everything else 0), where a scale error and a Z bias are
indistinguishable. Fitting a sphere over all 1983 samples, which do span
orientations, separates them:

| | |
|---|---|
| centre (bias) | `(+85.7, −51.1, +233.2)` counts = 0.062 g, mostly Z |
| radius | **4070.2** counts/g |
| residual | median 2.6 counts |

4070 is within 0.6% of 4096. Checking both models against only the 104 samples
in a discriminating pose:

| model | median residual | p90 |
|---|---|---|
| scale error, r = 4307.5, no bias | 160.3 | 562.1 |
| bias, r = 4070.2, c = (86, −51, 233) | **82.2** | **266.7** |

The bias model fits about twice as well. 🔵 Not decisive — the off-axis samples
come from moving periods, so linear acceleration inflates both residuals — but
the evidence leans clearly, and the independent handle-`0x000A` raw stream
agrees at 4308.8 counts at rest.

**Consequence:** `IMU_COUNTS_PER_G = 4096` stands. The corpus controller has a
~0.06 g accelerometer bias. Any future comparison of genuine against synthetic
acceleration magnitude must subtract it — the earlier "1.0517 g genuine vs
1.0116 g synthetic" comparison was scoring a *biased* genuine reading against
an unbiased synthetic one, and the synthetic was the closer of the two to a
true 1 g.

**To close it properly:** at-rest captures in two or more orientations (flat,
then on its side). No console, no flash.

## 🔵 Tick period: 1257 µs measured against 1250 assumed

| capture | ticks | span | µs/tick |
|---|---|---|---|
| `ds5-pro2-paired-stationary` | 751 | 0.95 s | 1258.34 |
| `ds5-pro2-paired-stationary-clean` | 489 | 0.61 s | 1257.55 |
| `pro2-native-stationary-live` | 752 | 0.94 s | 1256.59 |

Three captures within 0.15% of each other: **1257 µs = 795.5 Hz**, against the
assumed 1250 µs = 800 Hz. A 0.56% deviation.

Too small to matter for motion, and it is at least as likely to be the host
clock as the controller's crystal — the host timestamp is the reference here,
and nothing independently calibrates it. Recorded so that "800 Hz" is known to
be nominal-and-confirmed-to-0.6%, not exact.

(A fourth capture, `ds5-pro2-paired-pitch`, gives 748 µs/tick. It is excluded
as an outlier: dropped notifications inflate the tick sum against wall time.)

## 🔵 DualSense: 8288.7 counts/g measured against 8192 assumed

219 at-rest DualSense samples give `|raw_a| = 8288.7` counts, i.e. 1.0118 g
under the assumed 8192 — 1.2% high, and very likely the same kind of bias.

The translator halves DualSense acceleration into Pro2 counts. Using the fitted
values the exact ratio is 4070/8288 = 0.4911 against the 0.5 used, an error of
1.8%. Negligible; no change warranted.

The at-rest DualSense gyro magnitude is 15.3 counts, which is its zero-rate
offset — already removed by `gyro_corrected`, and not a scale.

## 🔴 Gyro: the conversion is wrong by roughly a factor of two, and unresolved

**What is actually measured** is the *product* of two constants: the wire→counts
factor in `WIRE_TO_COUNTS[layout]["gyro"]` and `IMU_COUNTS_PER_DPS`. Only the
product is observable, and the product is wrong.

Three independent methods, with different failure modes, all land below the
assumed value:

| method | reference | implied counts/dps | quality |
|---|---|---|---|
| total rotation vs carrier | `0x1E` quaternion | 9.08 | ratios 0.497–0.663 |
| signed cumulative regression | `0x1E` quaternion | 10.10 | r 0.986–0.996 |
| gravity-direction rate | accelerometer only | ~7.1 | r 0.06–0.44, attenuated |

The first two share the carrier as reference; the third is fully independent of
it, using only the accelerometer, and still lands low. Errors-in-variables
biases the third *downward*, so it is a lower bound.

The benign explanation is ruled out. High-rate carries one gyro sample per
packet at ~110 Hz, and integrating too-sparse samples understates rotation —
but that fades as motion slows, and this does not: slopes 0.706 / 0.636 /
0.546 / 0.703 / 0.673 across bins from 0–10 up to 150–400 dps, slow-minus-fast
+0.032. Flat means scale, not sampling.

**Why it cannot be pinned here.** The estimates span 7–11.6 counts/dps. A
factor of exactly **2** is the only structurally plausible candidate, and it
has two indistinguishable forms:

* the gyro carries **7** fractional bits where acceleration carries 8, so the
  high-rate wire factor is 1/128 rather than 1/256; or
* the gyro full scale is **±1000 dps** (8.192 counts/dps) rather than ±2000
  (16.4).

Both predict identical wire values, and both would make our translated gyro run
at double rate — we pass DualSense counts through 1:1 on the assumption that
both sensors sit near 16.4. A factor of 2 leaves ~11% residual against the
measurement, which is inside the spread but not obviously zero.

Cross-layout consistency cannot help: the wire factors were derived by matching
layouts against each other, so a common factor error affects all layouts
equally and is invisible to that check. This is the same blind spot as the
acceleration bias, in a different constant.

**To close it:** integrate the gyro across a **known** rotation — a turntable
through an exact 360°, or 90° against a square edge — with `0x1E` and `0x28`
subscribed. Slow and deliberate beats fast. That yields counts/dps directly
with no reliance on the carrier, the accelerometer, or any assumed constant.
No console and no flash.

## 🔵 The temperature tail decodes to an implausible absolute value

`decode_temperature_tail16_value` reads the 16-bit tail as two Q3 samples
sharing a signed integer part, and over 1002 genuine packets the integer part
spans **3 to 10** with a median of 5.25.

A controller die at 3–10 °C is not plausible; room temperature is 20–25 °C.
ICM parts conventionally report temperature as an offset from 25 °C
(`T = raw/sensitivity + 25`), which would put these at 28–35 °C — entirely
plausible for a die under load.

So the *field layout* is well evidenced (it round-trips byte-exactly and the
two samples track each other), but the **absolute scale and reference point are
opaque**, and the name "temperature in °C" is not supported. This does not
affect generation — the firmware replays the modal genuine value rather than
computing one — but the decoder should not present an unreferenced number as a
temperature.

## Remaining opaque values, not yet audited

| value | where | status |
|---|---|---|
| prefix lane-2 formula `2(g₂−2²³)+2²⁴` | `encode_prefix` | 🔵 supported indirectly — the epoch fit reaches 0.0002° with it |
| `NS2_DS5_MOTION_PERIOD_US = 3800` | `ns2_ds5_motion.c` | ⬜ a heuristic rate gate, not a protocol value |
| `NS2_DS5_INTEGRATION_STEP_US = 4000` | `ns2_ds5_motion.c` | ⬜ numerical, not protocol |
| mode-0 packet structure | 5 corpus packets | 🔴 unknown; tick 127, status 0x00, elapsed 0 |
| interleaved `elapsed` relation | emission | 🔴 unresolved; `0x28`-only rule does not hold |
| `status = 0x00` meaning | 5 high-rate packets | 🔴 unknown |

## Reproduce

```powershell
python tools/ns2_motion40_gyro_axes.py     # gyro axis, sign, and scale
python tools/ns2_motion40_validate.py      # readiness gate
```

## Conclusion

Of five constants audited, one was confirmed (acceleration counts/g), two were
confirmed to within a fraction of a percent (tick period, DualSense counts/g),
one is wrong by roughly a factor of two and cannot be pinned from captures
(gyro conversion), and one is structurally sound but semantically unsupported
(temperature).

The pattern behind the wrong ones is consistent: each was validated by a check
that could not have failed. Slots agreeing with each other cannot reveal a
common scale error; layouts agreeing with each other cannot either. Only an
external physical reference — gravity, a unit quaternion, a known angle — can.
