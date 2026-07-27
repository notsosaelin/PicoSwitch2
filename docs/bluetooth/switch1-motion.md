# Nintendo Switch 1 Joy-Con / Pro Controller Motion / IMU — Definitive Technical Reference

> A complete, implementation-ready breakdown of **how the original Nintendo Switch (Switch 1)
> Joy-Con and Pro Controller report gyroscope and accelerometer data** — the enable handshake, the
> on-wire report format, byte offsets, data types, nominal units, the SPI-flash calibration
> mechanism and its exact math, coordinate/axis notes, and where PicoSwitch2 stands today. Scope is
> the **Switch-1 controller side only**: what the controller sends and how to turn it into physical
> units. Consuming that motion for a specific output personality is a **separate** problem, out of
> scope here.
>
> **Documentation only — no code changed.** Confidence: **Confirmed** (matches dekuNukem's
> reverse-engineering notes and the Linux `hid-nintendo` driver) · **Strong Evidence** ·
> **Hypothesis** · **Unknown**. Companion to [`dualsense-motion.md`](dualsense-motion.md) (the DualSense
> equivalent); the two share this repo's `input_event_t` motion carrier.

## 0. Summary up front

- **The IMU must be turned on.** Unlike the DualSense (which streams motion passively), the Switch-1
  controller ships with the IMU **disabled** and in a **simple** report mode. You must (a) switch to
  **full input report `0x30`** and (b) send **Enable-IMU** before any motion appears. (Confirmed.)
- **6-axis sensor** (STMicroelectronics **LSM6DS3**-class): 3-axis gyroscope + 3-axis accelerometer.
  No magnetometer.
- **Motion is embedded in report `0x30`** at **bytes 13–48**, as **three 12-byte frames** sampled
  **5 ms apart** — i.e. you get **3 IMU samples per report**, each `accel[XYZ]` then `gyro[XYZ]` as
  **signed 16-bit little-endian**. (Confirmed.)
- **Nominal scale:** accelerometer **±8 g** (`≈0.000244 g/count`, ~4096 counts/g); gyroscope
  **±2000 dps** (`≈0.06103 dps/count`; the LSM6DS3 datasheet value is `0.070 dps/count`). Real
  accuracy comes from **factory calibration stored in SPI flash** (§7). (Confirmed.)
- **PicoSwitch2 today does neither.** `switch_pro_bt.c` *defines* `SWITCH_SUBCMD_ENABLE_IMU 0x40`
  but **never sends it** (not in the init state machine), and the `0x30` parser reads buttons/sticks
  but **no motion**. Switch-1 motion is entirely absent — the largest gap of the motion work. §10.

## 1. The sensor

Joy-Con (L and R) and the Switch Pro Controller each contain a **6-axis MEMS IMU** (an
STMicroelectronics LSM6DS3 or compatible): a 3-axis **accelerometer** (specific force, g) and a
3-axis **gyroscope** (angular rate, deg/s). There is **no magnetometer** (no absolute heading). The
two Joy-Con halves mount the sensor in **different physical orientations**, so per-axis signs differ
between L, R, and Pro — see §8.

## 2. Where motion lives, and why you must enable it

Motion rides inside the **standard full input report `0x30`** — but the controller does **not** send
`0x30` (or any IMU data) until the host configures it:

1. It boots into a **simple HID report (`0x3F`)** with buttons only.
2. The host must **set the input report mode to full (`0x30`)** and **enable the 6-axis sensor**.
3. Only then do `0x30` reports arrive with the 36-byte IMU block populated.

This is the fundamental difference from the DualSense (`dualsense-motion.md` §9): Switch-1 motion is an
**opt-in, subcommand-driven** feature, and accurate values additionally require **reading
calibration out of the controller's SPI flash**.

## 3. The enable handshake (subcommands)

All are HID **output report `0x01`** subcommands (rumble + subcommand). Send in order after connect:

| Step | Subcommand | Argument | Purpose |
|---|---|---|---|
| 1 | **`0x03`** Set input report mode | **`0x30`** | switch to full report mode (`0x30`) — *"pushes current state @60 Hz, or @120 Hz if Pro Controller"* |
| 2 | **`0x40`** Enable IMU (6-axis) | **`0x01`** | turn the accelerometer + gyroscope **on** (`0x00` = off) |
| 3 | *(optional)* **`0x41`** IMU sensitivity/config | defaults | set gyro range (±2000 dps), accel range (±8 g), and sample rate; defaults are usually fine |
| 4 | **`0x10`** SPI flash read | addr+len | read **factory calibration** at `0x6020` (and **user** at `0x8026` if present) — see §7 |

The repo already performs steps like set-mode and enable-vibration in its init state machine; **step
2 (`0x40`) is defined but missing from that machine**, and step 4 (SPI reads) has no path yet (§10).

## 4. Standard input report `0x30` byte layout (Confirmed)

Byte positions are absolute offsets in the raw report (byte 0 = report ID):

| Byte | Field |
|---|---|
| 0 | Report ID `0x30` |
| 1 | Timer (increments each report; wraps) |
| 2 | Battery level (high nibble) + connection info (low nibble) |
| 3–5 | Button status (3 bytes) |
| 6–8 | Left analog stick (12-bit X + 12-bit Y, packed) |
| 9–11 | Right analog stick (12-bit X + 12-bit Y, packed) |
| 12 | Vibrator input report byte |
| **13–48** | **6-axis IMU data — 36 bytes, three 12-byte frames** (see §5) |

(Reports `0x31`/`0x32`/`0x33` share the same first 49 bytes and append NFC/IR or other payloads;
the IMU block is identical.)

## 5. The IMU block: 3 frames × (accel + gyro)

Bytes 13–48 are **three consecutive 12-byte frames**, each an independent IMU sample **5 ms apart**.
One frame:

| Frame offset | Field | Type |
|---|---|---|
| +0 (bytes 13–14) | **accel X** | s16 LE |
| +2 (15–16) | **accel Y** | s16 LE |
| +4 (17–18) | **accel Z** | s16 LE |
| +6 (19–20) | **gyro X** (roll) | s16 LE |
| +8 (21–22) | **gyro Y** (pitch) | s16 LE |
| +10 (23–24) | **gyro Z** (yaw) | s16 LE |

- **Frame 0** at bytes 13–24, **Frame 1** at 25–36, **Frame 2** at 37–48. Order is `accel[XYZ]` then
  `gyro[XYZ]` in each. (dekuNukem: *"3 frames of 2 groups of 3 Int16LE each. Group is Acc followed
  by Gyro."*)
- **Use all three frames.** Frame 2 is the newest; Frames 1 and 0 are 5 ms and 10 ms older. For
  smooth integration feed all three (oldest→newest) at 5 ms spacing rather than discarding two.
- Read each axis as `(int16_t)(lo | (hi << 8))` (little-endian).

## 6. Ranges and nominal conversion (uncalibrated)

Defaults after `0x40`/`0x41`:

| Sensor | Default range | Nominal raw→physical |
|---|---|---|
| Accelerometer | **±8 g** | `g = raw * 0.000244` (≈ **4096 counts/g**) |
| Gyroscope | **±2000 dps** | `dps = raw * 0.06103` (Nintendo nominal) **or** `raw * 0.070` (LSM6DS3 datasheet) |

The two gyro coefficients differ because Nintendo's nominal maps ±2000 dps to the full ±32768
int16, while the LSM6DS3 datasheet sensitivity is 70 mdps/LSB. **Calibration (§7) supersedes both** —
use the nominal only as a fallback when calibration hasn't been read.

## 7. Calibration — from SPI flash (the crux)

Accurate motion requires calibration read out of the controller's internal SPI flash via subcommand
**`0x10`** (SPI read: address + length → response with the bytes). Three regions:

### 7.1 Factory sensor calibration — `0x6020`–`0x6037` (24 bytes, Confirmed)
Four groups of three `int16 LE` (X, Y, Z):

| Offset | Group |
|---|---|
| `0x6020`–`0x6025` | **Accelerometer origin** (X, Y, Z) |
| `0x6026`–`0x602B` | **Accelerometer sensitivity** (X, Y, Z) |
| `0x602C`–`0x6031` | **Gyroscope origin** (X, Y, Z) |
| `0x6032`–`0x6037` | **Gyroscope sensitivity** (X, Y, Z) |

### 7.2 User calibration — `0x8026`–`0x803F` (26 bytes, Confirmed)
- `0x8026`–`0x8027` = **magic `B2 A1`** → user calibration **present**; if absent, use factory.
- `0x8028`–`0x803F` = 24 bytes, **same layout** as §7.1. Prefer user cal when the magic is present.

### 7.3 Horizontal offset — `0x6080`–`0x6085` (6 bytes, Confirmed)
Three `int16 LE` (X, Y, Z): an **additional accelerometer offset** applied when the controller rests
flat on a surface (a secondary zeroing term).

### 7.4 Conversion with calibration (Confirmed — dekuNukem)
Per axis, letting `origin`/`sensitivity` be the cal values for that axis:

```c
// Accelerometer -> g
acc_coeff = (1.0 / (acc_sensitivity - acc_origin)) * 4.0;
acc_g     = raw_accel * acc_coeff;                 // (+ horizontal-offset term, §7.3)

// Gyroscope -> deg/s
gyro_coeff = 936.0 / (gyro_sensitivity - gyro_origin);
gyro_dps   = (raw_gyro - gyro_origin) * gyro_coeff;
```

**Note the asymmetry** (same shape as the DualSense, `dualsense-motion.md` §7): the **gyro subtracts its
origin** (zero-rate offset) then scales, while the **accel** scales about its origin/sensitivity
span with the fixed `4.0`/`936.0` reference constants coming from the factory-cal reference
conditions. The Linux `hid-nintendo` driver implements the equivalent normalization; cross-check
signs/scale there if a value looks off.

## 8. Coordinate system & per-device axis orientation

- The **two Joy-Con halves mount the IMU mirrored**, so identical physical motion produces
  **different axis signs** on L vs R; the **Pro Controller** has its own orientation again. A single
  hard-coded sign table is therefore wrong for all three. (Strong Evidence.)
- Gyro axes map roughly to **roll (X) / pitch (Y) / yaw (Z)**; accel to **X/Y/Z** in the controller
  body frame. Exact directions are documented with diagrams in dekuNukem's notes but are easiest to
  **verify empirically**: enable IMU, rotate the physical controller about one axis at a time, and
  watch which `gyro[i]`/`accel[i]` moves and in which direction — 10 minutes once §10's plumbing
  exists. **Do not hard-code signs from memory.**
- Any remap into an output personality's frame (e.g. Switch 2) is **downstream and out of scope**
  here — but budget for a per-device (L/R/Pro) sign/axis map at that boundary.

## 9. Report rate, timing, and the frame timer

- **`0x30` report rate: 60 Hz** (Joy-Con) or **120 Hz** (Pro Controller) in full mode.
- Each report carries **3 IMU frames at 5 ms spacing**, so effective IMU sampling is **~200 Hz**
  regardless of the 60/120 Hz report cadence — feed all three frames for correct integration.
- **Byte 1 (timer)** increments per report and can bound Δt / detect dropped reports, but the IMU
  frames' own cadence is the fixed 5 ms; prefer the 5 ms spacing for per-frame integration and the
  timer only as a drop/gap check.

## 10. PicoSwitch2 current state (implemented 2026-07-26/27)

Status: ✅ **Implemented and hardware-validated** for the Switch 1 Pro Controller in Pro
Controller 2 personality. Joy-Con halves stream motion but their axis signs are 🟡 unverified
(§8).

All of it lives in `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch_pro_bt.c`:

| Piece | Where | Status |
|---|---|---|
| `ENABLE_IMU` init step (subcommand `0x40` arg `0x01`) | `SWITCH_STATE_ENABLE_IMU` | ✅ |
| SPI read of factory cal `0x6020` (24 B) | `SWITCH_STATE_READ_FACTORY_CAL` → `switch_spi_read()` | ✅ |
| SPI read of user cal `0x8026` (26 B, magic `B2 A1`) | `SWITCH_STATE_READ_USER_CAL` | ✅ |
| Report `0x21` reply parse into `sw1_imu_cal_t` | `switch_parse_spi_reply()` / `sw1_parse_imu_cal()` | ✅ |
| Three-frame IMU decode + per-axis mean | `sw1_average_imu()` | ✅ |
| §7.4 conversion folded into the interchange scale | motion publish in `switch_process_report()` | ✅ |
| Per-device axis signs (accel + gyro, det +1) | `sw1_axis_signs_for()` | 🟡 Pro verified, Joy-Con inherited |
| Provenance tag so the quaternion translator is selected | `SWITCH_MOTION_SOURCE_SWITCH1` | ✅ |

Two design points worth keeping in mind before changing any of it:

**Calibration is optional, not required.** `sw1_imu_cal_t.valid` gates the calibrated path. If the
SPI reads never arrive, or the block is erased flash (`0xFF`), or any origin/sensitivity span is
zero, `sw1_parse_imu_cal()` rejects it and the nominal constants of §6 are used instead. Motion
therefore works on the first report, before the SPI round-trip completes, and degrades to
"slightly wrong scale" rather than "divide by zero" or "no motion".

**The §6 disagreement was settled by hardware, not by the datasheet.** The nominal reading (±2000
dps over the full int16 → raw passes through unscaled) under-reported rate on real hardware: the
controller needed noticeably more movement than a DualSense for the same on-screen result. The
LSM6DS3 `0.070` dps/count figure is the one that matches, and it is also what a typical factory
block implies (`936/(sens-origin) ≈ 936/13371 ≈ 0.070`). The fallback constants encode that as
`raw × 1147/1000`. The calibrated path makes this moot per-unit, which is the whole reason it
exists.

**Why the mean of three frames.** Each `0x30` report carries three IMU frames 5 ms apart spanning
the report interval. The downstream encoder integrates angular phase over real elapsed time, so
the representative rate for that interval is the mean; using only the newest frame discards two
thirds of the samples and under-integrates fast motion.

**Motion is routed to the quaternion translator, not the generic phase encoder.** Provenance
(`SWITCH_MOTION_SOURCE_SWITCH1`) is set in `ns2_seam.c` from the bound decoder and consumed in
`switch_pro2.c`. The generic encoder produced the violent output first seen on hardware; this is
the same known-bad path the DualSense work already diagnosed. Do not route a new IMU family
through it without checking that first.

### 10.1 The accel/gyro shared-frame invariant (learned the hard way, twice)

**Signs and remounts apply to the accelerometer and the gyroscope identically, and the signed
permutation must have determinant +1.** This is not style. Violating it produces a failure that
looks like a scaling or smoothing problem and wastes a hardware session.

Slot `i` of both arrays refers to the same body axis: `gyro[i]` is the rotation about the axis
`accel[i]` measures. The console fuses the two to correct attitude against gravity. Gravity
constrains **pitch and roll** but carries **no yaw information whatsoever** — rotating about the
gravity vector does not change the measured vector.

So when accel and gyro are delivered in mutually mirrored frames, the symptom is diagnostic:

| Axis | Gravity-corrected? | Symptom |
|---|---|---|
| Yaw | No | **Perfect.** Sharp, smooth, indistinguishable from real hardware. |
| Pitch / roll | Yes | Laggy by ~1–2 s (the correction's time constant), then a violent snap as the estimator gives up arguing with the gyro; steady state follows gravity, so an inverted-accel axis stays inverted no matter what sign the gyro carries. |

If yaw is flawless and pitch is slow-then-snappy, do not reach for filtering or scale — look for a
sign or remount applied to one sensor and not the other.

Observed on hardware 2026-07-27, when the Switch 1 sign table was applied to gyro only. The
DualSense work hit the same class of bug earlier; `ns2_seam.c` records that an earlier mapping
there "made the console's gravity correction bleed one axis into another".

**Determinant.** A sensor remount is a rotation, never a reflection, so the signed permutation must
have determinant +1. The Switch 1 permutation is cyclic (sign +1), so the determinant reduces to
`pitch × yaw × roll`, which must equal +1 — **signs flip in pairs, never singly.** A lone flip is
not a physically realizable orientation and reintroduces the mirrored-frame bug.

> ⚠️ **Correction, 2026-07-27 (same day).** The paragraph that previously followed concluded from
> this invariant that the Pro's roll "must" be `-1`. **Hardware refuted it:** that build killed
> horizontal aim completely. The invariant itself stands, but it constrains the answer without
> determining it — and the deeper error was assuming the *index permutation* was known and only the
> signs were in doubt. §8 above only ever claimed the axes map "roughly", and told us to measure.
> The full trace, the corrected symptom table, and a gravity-based measurement protocol that
> determines permutation and signs together now live in
> [`switch1-to-switch2-motion-spec.md`](switch1-to-switch2-motion-spec.md). The sign table in
> `switch_pro_bt.c` has been returned to the identity pending that measurement.
>
> One correction to the mechanism described just above, from reading the translator: the quaternion
> translator does **not** fuse accelerometer data at all — it `memcpy`s accel straight into the PDU.
> The gravity correction that produces the drift-then-snap symptom happens on the **console**, which
> also uses our accel to decide which way "up" is. That is why a wrong accel frame can kill
> *horizontal* aim, which a purely local gravity-correction model does not predict. See spec §4
> Stage 4/6 and §5.

Both rules are enforced in code rather than by comment: the publish path is a single loop over the
three slots writing accel and gyro together, so a sign cannot reach one without the other, and a
`_Static_assert` on the sign table fails the build if the product is not +1.

### 10.2 Resolved on hardware (2026-07-27) — 🟢 at parity with genuine hardware

Final seam row (`ns2_motion_seam.c`, `SWITCH_MOTION_SOURCE_SWITCH1`):
`src {1,0,2}`, `sign {-1,1,1}`, applied identically to accel and gyro.

Validated by A/B against a Switch 1 Pro Controller connected **natively** to the console:
**98–100 % identical**. The small residual lag is present on the native connection too, so it
belongs to the controller, not to this firmware.

**How it was finally resolved — measurement, not iteration.** Four sign guesses had failed. What
ended it was reading the accelerometer of a *resting* controller over UART (`input status` now
reports accel for exactly this purpose):

```
accel [-9, 724, 4245]      |a| = 4306 at 4096 counts/g = 1.05 g
```

Gravity sat almost entirely on slot 2, and the genuine Pro Controller 2 capture reads *"gravity ≈
all on accel-Z (4279/4309, controller flat)"* — a match to within 1 %. That pinned slot 2 without
anyone touching the controller.

**The bug that had survived every previous attempt: the row was a reflection.** `src {1,0,2}` swaps
indices 0 and 1, so its permutation parity is −1; the signs multiplied to +1; determinant **−1**. A
row describes a physical sensor remount, which is a *rotation* — −1 is a reflection and cannot
describe any real mounting. Every other row in the table was +1.

That also explains why it hid for so long. **Gravity cannot detect a reflected frame** — a single
vector looks perfectly correct reflected — so the accelerometer matched genuine hardware to within
1 % while the gyro produced no horizontal aim at all. Every static check passed; only rotation
exposed it.

With slot 2 measured and the determinant rule applied, exactly **two** candidate rows remained:
`{-1,1,1}` = (+R,+F,+U) and `{1,-1,1}` = (−R,−F,+U). They differ by a 180° yaw, so they share yaw
and invert pitch/roll relative to each other. One hardware test between them was decisive — the
search space was two, not twenty-four.

**Latency work.** Gyro now takes the **newest** of the three IMU frames instead of their mean. The
frames span 15 ms (5 ms apart) while a Pro reports every 8.3 ms, so consecutive reports overlap and
averaging produced a 15 ms moving average — roughly 7.5 ms of group delay. Because the encoder
integrates rate over real elapsed host time, the newest frame is a zero-order hold that preserves
angular area exactly; it does **not** under-integrate, contrary to an earlier note here. Accel keeps
the three-frame mean deliberately: it is the console's gravity reference, where steadiness matters
more than latency.

**Tooling note: `motionprobe` is the wrong instrument for body axes.** It drives the packed carrier
lanes of the smallest-three quaternion, so driving one lane is not a rotation about one body axis —
observed directly, with carrier axes 0 and 1 both producing camera pitch. It remains correct for
what it was built for: validating the PDU encoding.

### 10.3 Why the last of the lag cannot be removed safely

The ceiling is the source hardware, not this pipeline:

| Source | Report rate | Sensor timestamp |
|---|---|---|
| Switch 1 Pro | **120 Hz** | ✗ none |
| DualSense | 250 Hz | ✅ 0.33 µs clock |
| Pro Controller 2 (native) | its own high-rate PDU, passed through | ✅ |

That is the whole difference in feel, and it is not something the translation layer can close.

One genuine lever remains, deliberately **not** taken: each `0x30` report carries **three** samples
at 5 ms spacing — 200 Hz of *real* sensor data, of which we integrate one. Feeding all three would
raise effective integration from 120 Hz to 200 Hz with no fabricated data. It is not done because
the shared translator takes one sample per update and gates on a 3800 µs minimum period, so two of
every three would be dropped; making it work means changing the hardware-validated DualSense path
to buy a refinement on a source that already matches genuine hardware. Not worth the risk at
present.

Explicitly rejected: disabling sniff mode (the A/B shows the link is not the bottleneck, and it
would cost idle power and compatibility), and any prediction or extrapolation between samples
(fabricates motion the sensor never reported).

## 11. Remaining work

0. 🟡 **Confirm the current Switch 1 axis map on hardware** (spec §7). It is derived from the
   documented Linux/SDL frames plus the three previous test results. Note that its determinant is
   −1 and that this is expected, not a defect: the DualSense convention it targets is itself not
   self-consistent (spec §6.5). Do not "tidy" it back to +1, and do not change `ns2_seam.c`.
1. 🟡 **Verify Joy-Con axis signs on hardware** (§8). JC-L (`0x2006`) and JC-R (`0x2007`)
   currently inherit the Pro's signs in `sw1_axis_signs_for()`. §8 is explicit that the two halves
   mount the IMU mirrored, so at least one axis is likely inverted on at least one half. Test:
   pitch/yaw/roll each half in isolation and compare on-screen direction against the Pro.
2. ⬜ **Horizontal offset `0x6080`** (§7.3) is not read. It affects the resting orientation
   reference rather than rate, so it matters for pointer-style use, not for the current
   phase-integrating encoder. Low priority until something needs absolute attitude.
3. ⬜ **Subcommand `0x41`** (explicit IMU range/rate) is never sent; the controller's defaults are
   accepted. Worth revisiting only if a unit is found that does not default to ±2000 dps / 208 Hz.
4. ⬜ **No log of which calibration source won.** `switch_parse_spi_reply()` prints on success, but
   there is no runtime command to dump the parsed origins/sensitivities. A `input cal` UART verb
   would make the next axis-sign investigation much cheaper.

## 12. References

- **dekuNukem, Nintendo Switch Reverse Engineering** — the authoritative source (quoted in §4–§7):
  - IMU data format & conversion: <https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/imu_sensor_notes.md>
  - Report `0x30` layout & subcommands `0x03`/`0x40`/`0x41`/`0x10`: <https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/bluetooth_hid_notes.md>
  - SPI flash calibration addresses: <https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/spi_flash_notes.md>
- **Linux `hid-nintendo`** — a working driver implementing IMU enable, SPI calibration read, and
  the normalization math (cross-check for signs/scale):
  <https://raw.githubusercontent.com/torvalds/linux/master/drivers/hid/hid-nintendo.c>
- This repo: `src/bt_hid/bt/bthid/devices/vendors/nintendo/switch_pro_bt.c` (Switch-1 driver: `0x30`
  parse, init state machine, `SWITCH_SUBCMD_ENABLE_IMU` define),
  `src/bt_hid/core/input_event.h:249-253` (motion carrier fields).
- Sibling: [`dualsense-motion.md`](dualsense-motion.md) (DualSense equivalent — same `input_event_t` carrier,
  same calibrate-then-scale pattern, and the drift-correction note).
