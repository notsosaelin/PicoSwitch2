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

## 10. PicoSwitch2 current state (audited)

**Present:**
- Full-report negotiation: `switch_pro_bt.c` sets input mode `0x30` and parses the `0x30` report's
  buttons/sticks (`switch_pro_bt.c:378-399`), and defines `SWITCH_SUBCMD_ENABLE_IMU 0x40`
  (`:34`).
- A motion carrier in the normalized event (`input_event.h:249-253`): `int16_t accel[3]`,
  `int16_t gyro[3]`, `uint16_t gyro_range` (dps), `uint16_t accel_range` (milli-g), `bool
  has_motion` — shared with the DS3/DS4/DS5 paths.

**Missing (the entire Switch-1 motion path):**
1. **IMU never enabled.** The init state machine is `WAIT_READY → SET_INPUT_MODE → ENABLE_VIBRATION
   → SET_PLAYER_LED → ACTIVE` (`switch_pro_bt.c:128-136`) — there is **no ENABLE_IMU step**, so
   `0x40` is defined but never sent.
2. **Motion never parsed.** The `0x30` handler reads no bytes in the 13–48 range; no
   `gyro`/`accel`/`has_motion` assignment exists in the file.
3. **No SPI calibration read.** There is no subcommand-`0x10` path, so factory/user calibration
   (`0x6020`/`0x8026`) is never fetched — motion would be uncalibrated even once parsed.
4. **Ranges not set** for Switch-1 (would fall to the DS4/DS5 defaults, wrong units without §7).

## 11. Implementation checklist (so there are no blockers)

1. **Add an `ENABLE_IMU` state** to the init machine: send subcommand `0x40` arg `0x01` (after
   set-mode `0x03`/`0x30`). Optionally `0x41` for explicit range/rate.
2. **Add a subcommand-`0x10` SPI read** step: fetch `0x6020` (24 B factory) and check `0x8026`
   magic `B2 A1` → if present read `0x8028` (24 B user); optionally `0x6080` (6 B horizontal).
3. **Parse the calibration** into per-axis `origin`/`sensitivity` (accel + gyro).
4. **Parse the `0x30` IMU block**: three frames at bytes 13–24 / 25–36 / 37–48, each `accel[XYZ]`
   + `gyro[XYZ]` int16 LE.
5. **Apply §7.4** per axis/frame → calibrated g and deg/s; set `gyro_range = 2000`,
   `accel_range = 8000` (Switch-1 is ±8 g, unlike the DualSense's ±4 g).
6. **Verify per-device axis signs empirically** (§8) for JC-L, JC-R, and Pro before any downstream
   remap.
7. **(Optional)** feed all three frames per report for smooth ~200 Hz integration; add rest-based
   drift correction (see `dualsense-motion.md` §11).

Every step is Switch-1-local and testable in isolation (log calibrated g/deg-s while moving the
controller); none depends on any output personality.

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
