# Mobile Gyro → Switch 2 Motion Research (Android bridge, G5)

**Status:** research record, 2026-08-12. No code changes. Covers how a phone's IMU is captured on
Android and mapped to the Switch 2 motion our firmware emits — the missing research for
interface-audit gap **G5** (handheld/phone gyro reaches the Switch 2). Confirmed vs inferred marked.

Sources: [Android Motion Sensors dev docs](https://developer.android.com/develop/sensors-and-location/sensors/sensors_motion)
(traced), [Android sensor overview](https://developer.android.com/guide/topics/sensors/sensors_overview),
and [Dycool/NS-PC-Control](https://github.com/Dycool/NS-PC-Control) — a working phone-gyro→Switch 2
implementation that **cites PicoSwitch2 as its motion reference** (owner collaborated with Dycool on
the format), so it is corroboration of the approach, not an independent format source.

---

## 1. Android IMU sensors — which to use (CONFIRMED, dev docs)

| Sensor | Units | Notes for a Switch controller |
|---|---|---|
| `TYPE_ACCELEROMETER` | m/s² | **Includes gravity** — reads 9.81 m/s² (=1 g) at rest. This is what the Switch accel wants (gravity-inclusive, unit-g magnitude). |
| `TYPE_LINEAR_ACCELERATION` | m/s² | Excludes gravity — **not** what we want. |
| `TYPE_GYROSCOPE` | rad/s | Rotation rate, **drift-compensated** (calibrated). The default choice for rate-based motion. |
| `TYPE_GYROSCOPE_UNCALIBRATED` | rad/s | No drift compensation → "smoother, fewer jumps"; useful if calibration steps cause visible hitches. |
| `TYPE_ROTATION_VECTOR` | unitless quaternion | **Fused orientation** (accel+gyro+mag). "Ideal for games." Drift-corrected but magnetometer-influenced. |
| `TYPE_GAME_ROTATION_VECTOR` | unitless quaternion | Fused orientation **without magnetometer** → immune to magnetic interference; slow yaw drift only. Best for gameplay motion. |

**Coordinate frame (device):** X = right, Y = up (toward top of screen), Z = out of the screen toward
the user; gyro rotation **CCW-positive**. (The docs' East/North/sky frame is the *world* frame used by
the rotation-vector sensors.)

**Android 12+ rate limiting:** raw accel/gyro are capped (~200 Hz) without the
`HIGH_SAMPLING_RATE_SENSORS` permission. The Switch streams motion at ~250 Hz, but our seam/report
builder tolerates a lower input rate, so ~100–200 Hz phone sampling is fine.

---

## 2. The conversion to our seam (the engineering the app must do)

Our seam (`ns2_seam.c`) consumes **int16 accel (1 g = 4096 LSB)** and **int16 gyro (~16.384 LSB/dps)**
and applies a controller→Switch axis transform (DualSense today:
`sw.x = −ds.z, sw.y = −ds.x, sw.z = +ds.y`). A phone bridge produces the same `int16 accel/gyro` and
routes through that exact path. Conversions:

- **Accel:** `TYPE_ACCELEROMETER` (m/s²) → `g = v / 9.81` → `int16 = g × 4096` ⇒ `int16 ≈ v × 417.5`.
- **Gyro:** `TYPE_GYROSCOPE` (rad/s) → `dps = v × 180/π` → `int16 = dps × 16.384` ⇒ `int16 ≈ v × 938.7`.
- **Axis remap:** the phone's device frame ≠ the controller IMU frame, and depends on how the phone is
  held (landscape gaming grip vs portrait). A per-hold transform (or a one-time calibration) maps
  phone (X-right, Y-up, Z-out) → the Switch controller axes, analogous to the DualSense transform.

**Where the mapping lives:** cleanest is **on the phone** (send already-Switch-scaled int16 accel/gyro
in the HID contract), so the firmware treats the phone exactly like a DualSense and **no new firmware
motion path is needed** beyond adding motion fields to the Android HID contract + a generic-parser
route into the seam. (The seam's DualSense transform is the template.)

---

## 3. The valuable insight for report 0x09 (phase) — phones do the hard part

Our **report-0x09** motion is **integrated angular phase** (a 2³² = 360° binary angle), and
integrating raw gyro is exactly where we hit **drift/jitter** with the DualSense (see
`report-0x09-motion.md` and the console-gyro jitter work). **Android already exposes a fused,
drift-corrected orientation** via `TYPE_GAME_ROTATION_VECTOR` (no magnetometer artifacts). So a phone
bridge could feed the **phase from the phone's fused orientation quaternion** (quaternion → axis-angle
→ 2³² binary angle) instead of integrating raw rate — sidestepping the drift/jitter that made the
DualSense 0x09 path hard.

- **v1 recommendation (simple, reuses everything):** phone sends **rate + gravity-accel** (like a
  DualSense); the firmware's existing path handles report-0x05 directly and integrates for 0x09. No
  contract change beyond adding the two motion vectors.
- **v2 optimization (cleaner 0x09):** phone additionally sends **fused orientation**
  (`GAME_ROTATION_VECTOR`), and the firmware maps it straight to the 0x09 phase — better aim quality
  than integrating, because the phone's sensor fusion already removed drift. (INFERRED benefit; to
  validate on hardware.)

---

## 4. Calibration & edge cases (CONFIRMED conventions + INFERRED handling)

- **Accel includes gravity** on Android and the Switch expects gravity-inclusive accel — they match;
  do **not** use `LINEAR_ACCELERATION`. (CONFIRMED.)
- **Gyro bias/drift:** `TYPE_GYROSCOPE` is drift-compensated; if its corrections cause visible jumps,
  `TYPE_GYROSCOPE_UNCALIBRATED` is smoother. A stationary bias-capture (average gyro while still) is
  the standard extra step. (CONFIRMED sensor behavior; handling INFERRED from common practice.)
- **Hold orientation:** the phone-frame→controller-frame remap depends on grip; assume a landscape
  gaming hold or offer a one-time "hold still / point forward" calibration.
- **Rate limiting (Android 12+):** request `HIGH_SAMPLING_RATE_SENSORS` for >200 Hz, else ~200 Hz cap
  (fine for us).
- **Recentre/gyro-off:** games expect a way to recentre aim; the app should support a recentre gesture
  and a motion on/off toggle (app-side).

---

## 5. NS-PC-Control corroboration

NS-PC-Control (Pi Zero 2W, C++ USB-gadget server + Android/iOS/shared mobile clients, UDP transport)
implements phone-gyro→Switch 2 motion using our decoded format. Its existence and its citation of our
project are positive evidence that the pipeline (phone IMU → Switch 2 report) works with our format.
Deep-tracing its exact capture/packing files over the web was limited by GitHub's tree view; its
`server/src` USB-gadget motion packing remains a useful **future A/B cross-check** for our
report-0x05/0x09 byte packing if a discrepancy ever appears.

---

## 6. Confirmed vs inferred

**Confirmed:** Android sensor units, gravity-inclusion, drift-compensation behavior, the fused
orientation sensors and their properties, the device coordinate frame, and Android-12 rate limiting.
The conversion math follows directly from those units and our seam's known scales.

**Inferred / to validate on hardware:** the exact phone-frame→controller-frame axis transform (grip
dependent); whether feeding fused orientation to the 0x09 phase beats integrating rate (expected yes);
and the best gyro sensor choice (`GYROSCOPE` vs `UNCALIBRATED`) for feel.

**Impact on the plan:** G5 needs **no new firmware motion decoder** — the phone scales to our existing
int16 accel/gyro and routes through the seam like a DualSense. The only firmware additions are motion
fields in the Android HID contract + a generic-parser route to the seam. The report-0x09 phase quality
question is the one thing worth a hardware A/B (integrate-rate vs feed-fused-orientation).
