# Agent brief — motion

Read [COMMON.md](COMMON.md) first. Protocol detail:
[`../switch2/report-0x09-motion.md`](../switch2/report-0x09-motion.md).

## The single most important fact

**There is exactly ONE Switch 2 motion encoder, and it is `src/bt_hid/motion/ns2_ds5_motion.c`.**

The file is named for the DualSense for historical reasons only. It is not DualSense-specific: it
consumes the shared interchange scale every driver publishes and carries an explicit host-clock
fallback for sources with no IMU clock of their own. The only genuinely DualSense-specific code in
it is the 0.33 µs sensor-timestamp conversion, which is already gated on
`input->motion_timestamp_valid`. A rename to a neutral name is an open follow-up (PLAN.md); do not
infer from the name that a second encoder is needed.

## The pipeline — CONFIRMED

```
raw device data
      ↓  device driver              parse HID; publish shared interchange units:
                                    16.384 counts/dps, 8192 counts/g
      ↓  ns2_motion_seam.c          ONE row per source: a proper rotation (det +1) from that
                                    sensor's frame onto the carrier frame
      ↓  switch_pro_input_t         normalized motion state, frame-correct
      ↓  ns2_motion_consume()       switch_pro2.c: bias, stillness, warmup, quaternion integration
      ↓  ns2_ds5_motion_build()     the Switch 2 wire encoding
      ↓  report 0x09, motion length 0x1E
```

A genuine Pro Controller 2 does not enter this pipeline at all: its native PDU is forwarded as
opaque passthrough. `ns2_build_report()` has exactly two motion branches — genuine passthrough and
the translator — and **no fallback**. A source the translator cannot represent emits motion
length 0. That is deliberate and better than emitting bytes the console cannot decode.

## The generic phase encoder — REFUTED AND DELETED

The former `ns2_motion_tick()` / `ns2_encode_motion30()` in `switch_pro2.c` wrote three independent
int32 "angular phase" accumulators into motion bytes `0x04..0x0F`. **It never produced correct
motion on real hardware.** Every controller family routed through it produced violent, spamming
motion.

It was not mistuned. Those twelve bytes are ONE packed quaternion (G0 26 bits, G1 25 bits, G2 24
bits, plus a 2-bit chart state in G2's bits 25:24), so an int32 angle straddles slot and state
boundaries and decodes as an arbitrary orientation that jumps whenever a carry crosses bit 24 or
26. Deleted 2026-08-14. Full record:
[`../experiments/refuted-hypotheses.md`](../experiments/refuted-hypotheses.md).

Do not reintroduce a per-axis angle model, and do not add a second encoder "for generic
controllers". Its existence in the architecture was never evidence that it was correct.

## Wire format — CONFIRMED

Length `0x1E` motion block, offsets relative to the block:

| off | size | field |
|---|---|---|
| `0x00` | 2 | timing: low 12 bits = 800 Hz tick, high 4 = ticks elapsed |
| `0x02` | 2 | temperature, constant `0x0C00` |
| `0x04` | 12 | packed orientation carrier (26/25/24 bits + 2-bit state) |
| `0x10` | 12 | accel X/Y/Z, Q16.16, `65536 × 4096 = 1 g` |
| `0x1C` | 2 | tail |

Each `Gn` is a **26-bit** little-endian field stored as 24 bits plus 2 high bits in a following
byte — they are not aligned 32-bit words:

```
0x04 bits 1:0  G2 bits 25:24 -> CHART STATE      0x05..0x07  G0 bits 23:0
0x08 bits 1:0  G0 bits 25:24                     0x09..0x0B  G1 bits 23:0
0x0C bits 1:0  G1 bits 25:24                     0x0D..0x0F  G2 bits 23:0
```

Scales: `G0 = (c/√2 + 0.5) × 2^26` (all 26 bits), `G1 × 2^25` (bit 25 always 0), `G2 × 2^24`
(bits 25:24 hold the state instead). So the chart state is the **low 2 bits of byte `0x04`**.
State numbers components in wire order `w/x/y/z`, so identity is state 0.
`src/bt_hid/motion/ns2_motion_pdu.c` is the authority.

Motion is a **negotiated feature**: emit length 0 until the console enables the IMU via `0x0C/0x04`.

Length `0x28` is a separate packed multi-sample carrier. It is a **deferred** research target,
default-off. There is no magnetometer lane. Do not resume it or restore the refuted static-template
generator without an explicit maintainer decision.

## The carrier frame — CONFIRMED

`X = right, Y = forward (toward the top edge), Z = face normal (out of the front face)`.

The anchor is measured, not assumed: a resting controller puts gravity on carrier slot 2 at `+4096`
counts (matching genuine Pro Controller 2 captures at `+4279`/`+4309`). Every seam row must land
its own source's face-up gravity there.

**Every seam row must have determinant +1.** A row describes a physical sensor remount, which is a
rotation; determinant −1 is a reflection and cannot describe any real mounting. Gravity alone
cannot reveal the error — a single vector looks correct reflected — so an improper row passes every
static check and misbehaves only under rotation. This exact bug cost the SWITCH1 row a week
(det −1 until 2026-07-27: accelerometer matched genuine hardware to 1% while the gyro produced no
horizontal aim at all). `tools/test_ns2_motion_seam.c` enforces it.

## Traps already encountered

- **Yaw survives frame errors that pitch and roll do not.** **"Yaw works, pitch/roll do not" is a
  frame or axis diagnosis, not a filtering, smoothing, deadzone, scale, or noise diagnosis.**
- **The mechanism, stated precisely (corrected 2026-08-14).** Both the app's screen-rotation remap
  and every seam row apply the *same* transform to accel and gyro. A wrong row or a missed rotation
  is therefore a proper rotation of the whole motion state: the adapter's output stays internally
  self-consistent and the console sees **no** accel/gyro disagreement. What breaks is the console's
  gravity-referenced aim mapping reading a consistently mis-posed controller. For a 90° error about
  the face normal: yaw is rotation about the gravity axis and is still identified as yaw, so
  horizontal aim mostly works; physical pitch arrives as rotation about the reported forward/up axis
  and is read as roll, so vertical aim is absent or cross-coupled.
  Earlier notes (this pass's first report, and commit `4d12db3`'s message) described accel and gyro
  "fighting" the console's gravity correction. That is the wrong mechanism — there is nothing to
  fight, because the error is applied to both. The observable prediction is unchanged; only the
  explanation was loose.
- The old Android row was copied from the DualSense row, which silently assumed the two frames share
  a face normal. They do not: the DualSense's face normal is its Y, Android's is its Z.
- Integrating the low-pass EMA rather than the current calibrated sample attenuates brief fast
  motion and looks like clipping even with a correct carrier.
- Declaring the first full motion report to be the zero-rate reference permanently subtracts a
  startup transient and causes nonstop rotation at rest. The bounded stillness warmup owns initial
  bias instead.
- A stillness gate on raw gyro *magnitude* is self-defeating: a MEMS gyro's constant bias is part of
  its magnitude, so the gate never opens. Gate on the frame-to-frame *derivative*.

## Current hardware state

- ✅ genuine Pro Controller 2 native passthrough
- ✅ DualSense / DualSense Edge translation
- 🟢 Switch 1 Joy-Con / Pro Controller
- 🟡 Wii Remote — marked 🟢 on 2026-07-27, but the translator whitelist was DualSense-only
  then, so its report-0x09 motion reached the deleted encoder. Either that confirmation was
  report 0x05 (Steam/PC, unaffected) or "working" meant the console *responded*. A console
  responds to a garbage orientation with wild movement, so response is not correctness.
  Re-confirm on hardware before carrying the 🟢 forward.
- 🟡 Android companion (AYN Thor): see [ANDROID.md](ANDROID.md). After the screen-rotation fix,
  hardware reports left/right smooth and correct and up/down now in the correct physical
  direction but choppy/stepped with brief excursions. Pure-axis transforms are proven exact in
  software (zero leakage, see below), so the remaining defect is timing, not mapping; the source
  IMU clock is now forwarded. Awaiting hardware.

## Axis mapping — CONFIRMED exact in software

`tools/test_ns2_motion_quality.c` drives the production encoder with synthetic pure yaw, pure
pitch and pure roll and decodes the wire output back to a rotation axis. Leakage into each
unintended axis is **0.00000** and the driven-axis magnitude is **1.00000**, with the integrated
angle within 0.005% of the analytic value, on all three axes. So an axis complaint is NOT an axis
mapping bug in the encoder or the seam — look at timing, at the app's frame conversion, or at the
console's own interpretation instead. Do not reshuffle seam rows to chase a smoothness symptom.

## Sample timing — CONFIRMED

**A source's own IMU clock is what the encoder must integrate against.** The alternative is the
host clock, which measures packet ARRIVAL, and Bluetooth delivers a steady sender in bursts.
Pairing a rate sample with an interval it did not occur over costs real trajectory accuracy.

Measured with `tools/test_ns2_motion_quality.c` on a 2 Hz / 120 dps sweep (realistic aiming),
same mean cadence, only the arrival pattern differing:

| clock | arrival | worst trajectory error |
|---|---|---|
| host | even | 0.002° |
| host | bursty | **0.505°** |
| source | bursty | 0.002° |

A CONSTANT rate cannot show this — the accepted intervals still sum to the true elapsed time, so
the endpoint is right either way (measured: −0.17° over 28.8°). Only varying-rate motion exposes it,
which is exactly when a player notices.

Two further host-clock-only hazards, both bypassed entirely when a source clock is present:

- **`NS2_DS5_MOTION_PERIOD_US` (3800 µs) silently DROPS** any sample arriving inside it. It also
  halves a fast source's effective rate, which doubles bias-warmup time — and if the source starts
  moving before warmup completes, the encoder integrates **nothing at all**, silently. Measured.
- **The 16 ms anti-lurch clamp discards rotation** beyond it. Measured: 40 ms gaps report 57.6° of a
  true 144°.

The timestamp unit travels with the sample (`switch_pro_input_t.motion_timestamp_unit`), so the
encoder never has to know which controller produced it. DualSense authors 1/3 µs ticks with a
32-bit wrap; the Android bridge authors 100 µs ticks with a 16-bit wrap. **Take the delta in the
source's own modulus, then scale** — converting an absolute DualSense tick count to microseconds
before differencing is not wrap-safe, because 2^32 is not divisible by 3.

## Acceleration scale — CONFIRMED, question closed

`NS2_MOTION30_ACCEL_Q16_PER_COUNT = 68963` is **correct and universal**, not a DualSense
correction. Genuine Pro Controller 2 captures report a resting magnitude of **4310.1 ordinary
counts** over 147 blocks; `4096 × 68963/65536 = 4310.2` matches to 0.002%, while exact Q16.16 at
4096 counts/g would be 5.23% low. The wire's counts-per-g is ~4310, not 4096. Earlier notes calling
this "5.2% high, open" used the wrong reference and are corrected.

## Chart / state transitions — CONFIRMED CONTINUOUS, not a defect source

Measured 2026-08-14 by `tools/test_ns2_motion_quality.c`, which decodes the encoder's own wire
output back to an orientation and compares consecutive samples with a **sign-invariant** angular
metric (`2·acos|dot|`) — necessary because the encoder canonicalizes the omitted component
positive, so every zero crossing of the omitted lane flips all three transmitted lanes and a naive
comparison would report a nonexistent 180° jump.

The test compares the worst per-sample orientation step **across a chart change** against the worst
step **not** at a chart change. They are identical:

| trajectory | charts | transitions | step across change | step elsewhere | nominal |
|---|---|---|---|---|---|
| slow 30 dps (each axis) | 2 | 3 | 0.2439° | 0.2439° | 0.2400° |
| moderate 180 dps | 2 | 5 | 1.4396° | 1.4407° | 1.4400° |
| fast yaw 720 dps | 2 | 13 | 5.7586° | 5.7589° | 5.7600° |
| 6 revolutions | 2 | 24 | 2.8799° | 2.8802° | 2.8800° |
| **phased axis changes** | **4** | 30 | 1.9237° | 1.9237° | 1.9200° |

Zero build failures anywhere; packed round-trip exact over 400 state/value combinations with all
unrelated bits preserved. **A chart change costs nothing.** Do not add hysteresis to the chart
selector, and do not pursue chart transitions as a motion-quality cause again without new evidence.

Two facts worth keeping, both of which cost a test iteration to learn:

- **A constant angular-velocity vector only ever reaches TWO charts**, even with incommensurate
  per-axis rates ({137, 89, 211} dps for 3000 samples still visits only states 0 and 3). Constant
  omega is rotation about a *fixed* body axis, so the same component stays largest. Reaching all
  four requires a **changing** axis.
- The state alternating A→B→A during a single-axis rotation is the representation correctly
  following the largest component, **not** thrashing. Continuity is what decides that, not the
  transition count.

## Bias absorption of slow rotation — MEASURED DEFECT, unfixed

The stillness gate calls a sample "steady" when the de-biased rate is under
`NS2_DS5_GYRO_STILL_LIMIT` (40 counts = **2.44 dps**) *and* the frame-to-frame derivative is small.
A slow, smooth, deliberate rotation satisfies **both**, so the zero-rate estimator adapts toward the
real rotation rate and subtracts it. Measured surviving fraction of a constant rotation:

| rate | survives |
|---|---|
| 0.5 / 1.0 / 2.0 dps | **~52%** |
| 3.0 dps | 97.2% |
| 5–30 dps | 100.1% |

The cliff sits exactly on the 2.44 dps threshold. Fine aiming below it loses about half its motion
and "recovers" the moment the player speeds up.

**Deliberately NOT fixed.** The bias tracker exists because a DualSense at rest drifts without it,
and its current form took two hardware passes to get right (magnitude-gating was self-defeating; the
derivative gate replaced it). Raising or removing the limit trades slow-aim fidelity against
at-rest drift and deserves a decision plus a hardware A/B, not a constant tweak.

The principled fix, if reopened: a gyro alone genuinely cannot distinguish "still" from "rotating
slowly and smoothly" — both have near-zero derivative. The **accelerometer** can: a real rotation
moves the gravity vector, a still controller's does not. Gate stillness on gravity-vector stability
as well as gyro steadiness, rather than moving the gyro threshold.

## Diagnostics

UART/CDC `imu` reports `src` (the motion source class, i.e. which seam row ran), the normalized
accel/gyro the encoder consumed, and the encoder's live bias, corrected rate, quaternion, and
rejection count. Read `src` first for any axis complaint.
