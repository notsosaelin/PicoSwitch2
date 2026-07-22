# DS_MOTION — DualSense (DS5) Motion / IMU: Definitive Technical Reference

> A complete, implementation-ready breakdown of **how the Sony DualSense reports gyroscope and
> accelerometer data** — the on-wire format, byte offsets, data types, units, the factory
> calibration mechanism and its exact math, the coordinate system, how motion is enabled over
> Bluetooth, and where PicoSwitch2 stands today. Scope is the **DualSense side only**: what the
> controller sends and how to turn it into physical units. Consuming that motion for a specific
> output personality (e.g. the Switch 2's own `0x09` motion) is a *separate* problem and is
> explicitly **out of scope** here.
>
> **Documentation only — no code changed.** Confidence: **Confirmed** (byte-verified against the
> Linux `hid-playstation.c` driver and this repo's own working parser) · **Strong Evidence** ·
> **Hypothesis** · **Unknown**. Every constant and formula below is quoted from the upstream kernel
> driver (see §13) and cross-checked against `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c`.

## 0. Summary up front

- **The IMU is a 6-axis sensor** (3-axis gyroscope + 3-axis accelerometer) whose data is **embedded
  directly in the main input report** — not a separate report you poll. If you can read buttons, the
  motion bytes are in the same packet.
- **Two transports, one layout, one-byte shift.** USB uses input report **`0x01`**; Bluetooth uses
  **`0x31`** with **one extra header byte**, so every field sits **+1 byte** later over BT. (Confirmed.)
- **Fixed data types:** `gyro[3]` and `accel[3]` are **signed 16-bit little-endian**, followed by a
  **32-bit little-endian sensor timestamp**. (Confirmed.)
- **Fixed nominal scale:** accelerometer **8192 counts/g** (±4 g); gyroscope **1024 counts/(deg/s)**
  (±2048 deg/s). (Confirmed — Linux `DS_ACC_RES_PER_G` / `DS_GYRO_RES_PER_DEG_S`.)
- **Accuracy needs the calibration report.** Raw counts are close to nominal scale but carry a
  per-unit **sensitivity error** (gyro) and **zero-offset** (accel). The controller exposes factory
  calibration via **HID feature report `0x05` (41 bytes)**; §7 gives the exact parse and formula.
- **PicoSwitch2 today passes RAW, uncalibrated motion** (`ds5_bt.c` copies `rpt->gyro[i]` straight
  through, never reads `0x05`, assumes ±2000 dps / ±4 g). That's the main implementation gap — §12.

## 1. The sensor

The DualSense contains a single 6-axis MEMS IMU (Bosch-class) providing a 3-axis **gyroscope**
(angular rate, deg/s) and a 3-axis **accelerometer** (specific force, g). It is the same class of
sensor and the same reporting model as the DualShock 4; the DualSense driver in Linux literally
shares the calibration approach. There is **no magnetometer** (no absolute heading).

## 2. Where motion lives: the main input report

Motion is **not** a separate feature/poll. It occupies a fixed region of the standard input report,
immediately after the buttons/counter block and before the touchpad block. So the parse is: locate
the report by ID, skip the transport header, index to the motion offset, read 6× int16 + 1× uint32.

## 3. Transports, report IDs, and the +1 shift

| Transport | Input report ID | Header before payload | Net effect |
|---|---|---|---|
| **USB** | `0x01` | 1 byte (the report ID) | baseline offsets |
| **Bluetooth** | `0x31` | 2 bytes (report ID **+ 1 seq/tag byte**) | **every field +1 byte** vs USB |

`ds5_bt.c` encodes exactly this: *"Full BT report: report_id (1) + header (1) = skip 2 bytes"* vs
*"USB-style report: skip just report_id"* (`ds5_bt.c:556-561`). Over BT the minimal `0x01` report
(no motion) may also appear before full mode is enabled — see §9.

## 4. Exact byte layout (Confirmed)

Byte positions are **absolute offsets within the raw report**, including the report-ID byte.

| Field | Type | USB (`0x01`) byte | BT (`0x31`) byte |
|---|---|---|---|
| Report ID | u8 | 0 (`0x01`) | 0 (`0x31`) |
| BT header (seq/tag) | u8 | — | 1 |
| LX, LY, RX, RY | u8 ×4 | 1–4 | 2–5 |
| L2, R2 (analog triggers) | u8 ×2 | 5–6 | 6–7 |
| Report counter (`seq_number`) | u8 | 7 | 8 |
| Buttons | u8 ×4 | 8–11 | 9–12 |
| Vendor / reserved | u8 ×4 | 12–15 | 13–16 |
| **Gyro X** (pitch) | **s16 LE** | **16–17** | **17–18** |
| **Gyro Y** (yaw) | s16 LE | 18–19 | 19–20 |
| **Gyro Z** (roll) | s16 LE | 20–21 | 21–22 |
| **Accel X** | **s16 LE** | **22–23** | **23–24** |
| **Accel Y** | s16 LE | 24–25 | 25–26 |
| **Accel Z** | s16 LE | 26–27 | 27–28 |
| **Sensor timestamp** | **u32 LE** | **28–31** | **29–32** |
| Reserved (temperature) | u8 | 32 | 33 |
| Touchpad (2 fingers ×4 B) | … | 33+ | 34+ |

This matches this repo's own `ds5_input_report_t` (`ds5_bt.c:88-102`): `reserved1` (4th button byte)
+ `reserved2[4]` (the vendor block) → `int16_t gyro[3]` → `int16_t accel[3]` → `uint32_t
sensor_timestamp` → `reserved3` (temperature). The packed struct is read directly at the post-header
pointer, which is why the same struct serves both USB and BT.

## 5. Data types, endianness, axis assignment

- **Endianness:** little-endian. Read each axis as `(int16_t)(lo | (hi << 8))`. The repo reads the
  packed struct directly (`__attribute__((packed))`) since the RP2350 is little-endian.
- **Signedness:** signed 16-bit. Values are roughly centered near 0 at rest (accel Z reads ~+1 g
  worth of counts when the pad is flat and face-up).
- **Gyro axis order:** `gyro[0]=pitch`, `gyro[1]=yaw`, `gyro[2]=roll` (the repo struct comments this
  as `x, y, z (pitch, yaw, roll)`; Linux maps them to `ABS_RX / ABS_RZ / ABS_RY`).
- **Accel axis order:** `accel[0]=X`, `accel[1]=Y`, `accel[2]=Z`.
- **Handedness:** PlayStation controllers use a **Y-up** convention. Exact per-axis sign/orientation
  needs care — see §8.

## 6. Resolution and ranges (Confirmed — Linux constants)

```c
#define DS_ACC_RES_PER_G        8192               // accel counts per 1 g
#define DS_ACC_RANGE            (4 * DS_ACC_RES_PER_G)   // ±4 g
#define DS_GYRO_RES_PER_DEG_S   1024               // gyro counts per 1 deg/s
#define DS_GYRO_RANGE           (2048 * DS_GYRO_RES_PER_DEG_S) // ±2048 deg/s
```

So **after calibration**: `physical_accel_g = calibrated / 8192`; `physical_gyro_deg_s =
calibrated / 1024`. Raw uncalibrated counts are already *near* this scale (the factory scale is
close to nominal), which is why uncalibrated motion "kind of works" — but it is off by the per-unit
sensitivity (gyro) and zero-offset (accel) that §7 corrects.

## 7. Calibration — HID Feature Report `0x05` (the crux)

The controller stores factory calibration retrievable via a **GET_REPORT (feature) on report ID
`0x05`**, length **41 bytes** (`DS_FEATURE_REPORT_CALIBRATION = 0x05`,
`DS_FEATURE_REPORT_CALIBRATION_SIZE = 41`). Over Bluetooth the reply additionally carries a CRC-32
tail (handled by the HID layer); the meaningful calibration bytes are the same.

### 7.1 Report `0x05` byte layout (Confirmed)

`buf[0]` = report ID `0x05`; all values **signed 16-bit little-endian**:

| Offset | Field | | Offset | Field |
|---|---|---|---|---|
| `buf[1]`  | gyro_pitch_bias  | | `buf[19]` | gyro_speed_plus |
| `buf[3]`  | gyro_yaw_bias    | | `buf[21]` | gyro_speed_minus |
| `buf[5]`  | gyro_roll_bias   | | `buf[23]` | acc_x_plus |
| `buf[7]`  | gyro_pitch_plus  | | `buf[25]` | acc_x_minus |
| `buf[9]`  | gyro_pitch_minus | | `buf[27]` | acc_y_plus |
| `buf[11]` | gyro_yaw_plus    | | `buf[29]` | acc_y_minus |
| `buf[13]` | gyro_yaw_minus   | | `buf[31]` | acc_z_plus |
| `buf[15]` | gyro_roll_plus   | | `buf[33]` | acc_z_minus |
| `buf[17]` | gyro_roll_minus  | | | (34–40 trailing/CRC) |

### 7.2 Deriving per-axis calibration (Confirmed)

For each **gyro** axis (pitch shown; yaw/roll identical with their fields):

```c
speed_2x   = gyro_speed_plus + gyro_speed_minus;
bias       = 0;                                   // NOTE: gyro bias is NOT subtracted
sens_numer = speed_2x * DS_GYRO_RES_PER_DEG_S;    // = speed_2x * 1024
sens_denom = abs(gyro_pitch_plus  - gyro_pitch_bias)
           + abs(gyro_pitch_minus - gyro_pitch_bias);
```

For each **accel** axis (x shown):

```c
range_2g   = acc_x_plus - acc_x_minus;
bias       = acc_x_plus - range_2g / 2;           // per-axis zero-g offset (subtracted)
sens_numer = 2 * DS_ACC_RES_PER_G;                // = 2 * 8192 = 16384
sens_denom = range_2g;
```

### 7.3 The conversion formula (Confirmed)

Per sample, per axis (`mult_frac(n, x, d) = n * x / d`, done in 64-bit to avoid overflow):

```c
// gyro: pure sensitivity scaling (bias == 0)
gyro_out[i]  = mult_frac(gyro_calib[i].sens_numer,  raw_gyro[i],                     gyro_calib[i].sens_denom);
// accel: zero-offset removed, then scaled
accel_out[i] = mult_frac(accel_calib[i].sens_numer, raw_accel[i] - accel_calib[i].bias, accel_calib[i].sens_denom);
```

Output units: `gyro_out / 1024 = deg/s`, `accel_out / 8192 = g`.

**Two things to internalize:**
1. **Gyro calibration is sensitivity-only.** The factory bias is baked into the raw values already,
   so `bias = 0` and no offset is subtracted — the report's `gyro_*_bias` fields are used *only* to
   form the sensitivity denominator. Residual runtime drift is a **separate** concern (§11).
2. **Accel calibration removes a real zero-offset** (`bias`) per axis and scales to 8192/g. Skipping
   it leaves a fixed tilt/gravity error.

## 8. Coordinate system & axis orientation

- PlayStation controllers report in a **Y-up** frame. Community implementations (SDL, JoyShock,
  `hid-playstation`) normalize DualSense axes to a common convention, and note that **the DualSense's
  first gyro axis is inverted and its accelerometer axes are ordered/signed differently** than a
  naive read expects — so a straight copy will have one or more axes flipped or swapped relative to
  any given target frame. (Strong Evidence — see the JoyShock/SDL/evdevhook references, §13.)
- **Practical guidance:** treat the six values as a right-handed set to be *remapped* to whatever
  frame you consume them in. Determine the exact per-axis sign by rotating the physical controller
  about one axis at a time and observing which `gyro[i]`/`accel[i]` moves and in which direction —
  this is a 10-minute empirical calibration once §12's plumbing exists. **Do not** hard-code signs
  from memory; verify them.
- The Switch-2-output axis remap already in `ns2_seam.c` (negating/swapping DS5 axes into the
  Switch's frame) is downstream of this and **out of scope** here — but it is concrete proof the
  two frames differ, and is a useful reference once you reach that stage.

## 9. Enabling full motion over Bluetooth

Over **USB**, the DualSense sends the full `0x01` report (with motion) immediately. Over
**Bluetooth**, it defaults to the **minimal `0x01`** report (buttons/sticks only, **no motion**)
until the host promotes it to the full **`0x31`** stream. Promotion is triggered by the host
**interacting** with the controller's feature/output pipeline — in practice, **issuing the calibration
GET_REPORT (`0x05`) and/or sending an output report** flips it into full-report mode. This repo's
`ds5_bt` "activation" state machine already performs this handshake to obtain full reports (it sends
output reports on connect); reading `0x05` fits naturally into that same activation step. (Confirmed
behavior; the exact minimal trigger is Strong Evidence — output-report interaction reliably works.)

## 10. Report rate & the sensor timestamp

- **Rate:** up to ~**1000 Hz over USB**; **Bluetooth is lower** (commonly ~250–500 Hz depending on
  connection interval). The IMU is sampled continuously; each report carries the latest sample.
- **`sensor_timestamp` (u32 LE):** a free-running counter used to compute the true **Δt between
  samples** for integration (angle = Σ gyro·Δt). Do **not** assume a fixed Δt from the nominal rate —
  BT interval jitter makes per-sample Δt vary, and using the timestamp delta is how you keep gyro
  integration stable. The tick is small (sub-µs; this repo's driver comment notes **~0.33 µs/tick**,
  `ds5_bt.c:94`) so the counter wraps at 2³² — handle wrap with unsigned subtraction. **Exact tick
  duration is Hypothesis** (community values vary); confirm empirically by comparing timestamp deltas
  to wall-clock over a known interval before relying on absolute timing.

## 11. Runtime drift / auto-calibration (separate from factory calibration)

Because factory gyro calibration is sensitivity-only (§7.2), a **small residual bias** remains and
the integrated angle **drifts** when the controller is still. Every serious gyro-aim implementation
adds a **runtime zero-rate calibration**: detect "controller at rest" (gyro magnitude below a
threshold for N samples), average the gyro output, and subtract that as a live bias. This is what
JoyShockLibrary / GamepadMotionHelpers do. It is **not** part of reading the DualSense — it's a
consumer-side filter — but budget for it, or gyro aiming will feel like it slowly slides. (Strong
Evidence — standard practice.)

## 12. Current PicoSwitch2 state (audited)

**Present:**
- Correct report parsing and offsets. `ds5_bt.c` reads `rpt->gyro[3]` / `rpt->accel[3]` /
  `sensor_timestamp` from the right place for both USB and BT (`ds5_bt.c:88-102,556-561,631-642`).
- A motion carrier in the normalized event: `input_event.h:249-253` — `int16_t accel[3]`,
  `int16_t gyro[3]`, `uint16_t gyro_range` (dps), `uint16_t accel_range` (milli-g), `bool
  has_motion`. DS3 and DS4 drivers populate `gyro_range`/`accel_range` (DS3 = 100 dps / 2000 mg;
  DS4/DS5 default 2000 dps / 4000 mg).

**Missing (the DualSense-side work to implement accurate motion):**
1. **No calibration read.** `ds5_bt.c` never issues the `0x05` GET_REPORT and never parses it — there
   is no `calib`/`0x05` code in the file. **Motion is uncalibrated.**
2. **Raw pass-through.** `event.gyro[i] = rpt->gyro[i]` (and accel) copies **raw counts** — no
   sensitivity scaling, no accel zero-offset removal (§7.3).
3. **Range not set for DS5.** `ds5_bt.c` never sets `gyro_range`/`accel_range`, relying on the
   `init_input_event` defaults (2000 dps / 4000 mg, `input_event.h:342-343`). Correct *by luck* for
   the range constants, but the values themselves are still raw counts, not normalized to that range.
4. **Timestamp dropped.** `sensor_timestamp` is parsed into the struct but **not propagated** into
   `input_event_t` (no timestamp field), so Δt-based integration isn't possible downstream yet.
5. **No runtime drift correction** (§11).

## 13. Implementation checklist (so there are no blockers)

1. **On DS5 connect, GET_REPORT feature `0x05`** (41 B). Fold it into the existing `ds5_bt`
   activation handshake (§9) — it also helps promote BT to full `0x31`.
2. **Parse `0x05`** per the §7.1 table into 17 int16s.
3. **Derive** `gyro_calib[3]` and `accel_calib[3]` (`bias`, `sens_numer`, `sens_denom`) per §7.2.
4. **Per report, apply** the §7.3 `mult_frac` formula (64-bit intermediate) to `rpt->gyro[i]` /
   `rpt->accel[i]`; store the calibrated int16 in `event.gyro`/`event.accel`.
5. **Set ranges** explicitly: `event.gyro_range = 2000`, `event.accel_range = 4000` for DS5 (units:
   calibrated `/1024` = deg/s, `/8192` = g).
6. **(Optional, recommended)** add a `sensor_timestamp` field to `input_event_t` and propagate it for
   Δt integration; add rest-detection drift correction (§11).
7. **Verify axis signs empirically** (§8) before trusting any downstream remap.

Each step is DualSense-local and testable in isolation (log calibrated deg/s and g while moving the
pad); none depends on any output personality.

## 14. References

- Linux kernel `drivers/hid/hid-playstation.c` — authoritative source for the `DS_*` constants,
  feature report `0x05` layout, calibration derivation, and the `mult_frac` conversion formula
  (quoted verbatim in §6–§7): <https://raw.githubusercontent.com/torvalds/linux/master/drivers/hid/hid-playstation.c>
- Roderick Colenbrander, "HID: playstation: add DualSense accelerometer and gyroscope support"
  (patch): <https://lore.kernel.org/all/20210117234435.180294-6-roderick@gaikai.com/>
- Sony DualSense reverse-engineering (report structures):
  <https://controllers.fandom.com/wiki/Sony_DualSense/Data_Structures> ·
  <https://nondebug.github.io/dualsense/>
- DS5Dongle — a Pico 2 W DualSense dongle (same platform as this project) that implements gyro;
  a practical implementation reference: <https://github.com/awalol/DS5Dongle>
- Gyro-aiming reference implementations (calibration, drift, sensor fusion):
  JoyShockLibrary / JoyShockMapper / GamepadMotionHelpers — <https://github.com/JibbSmart/JoyShockMapper>,
  <https://github.com/JibbSmart/GamepadMotionHelpers>
- DualSense axis-orientation notes (inverted first gyro axis, accel ordering):
  <https://github.com/v1993/evdevhook/issues/3>
- This repo: `src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c` (struct `ds5_input_report_t`,
  parser, activation), `ds4_bt.c` / `ds3_bt.c` (sibling IMU parsers, normalization convention),
  `src/bt_hid/core/input_event.h:249-253` (motion carrier fields).
