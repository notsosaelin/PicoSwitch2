# Report 0x09 Motion — Detailed Decode Analysis

**Status:** ✅ conclusions **verified** against this repo's own USB capture
(a session-local analysis script (not retained): accel Q16.16 |g| = 1.0005 CV 0.007; timing high-nibble ==
800 Hz tick-delta 299/299). This is the long-form analysis; the **canonical summary + firmware
mapping** live in [report-0x09-motion.md](report-0x09-motion.md). (Relocated from the repo root;
was `other-findings.md`.)

---

## Main finding

I believe the native Switch 2 gyro format is now substantially decoded, and it **does not match the current `ns2-testing` PicoSwitch2 interpretation**.

The 30-byte motion block in report `0x09` appears to contain:

1. an 800 Hz timing counter,
2. temperature,
3. three 32-bit integrated angular phase values,
4. three 32-bit Q16.16 accelerometer values,
5. one unresolved 16-bit tail field.

It is **not** two repeated samples of interleaved 16-bit gyro and accelerometer values.

This conclusion is based on both uploaded captures, the observed byte-level continuity, physical plausibility, and the known ICM-42670-P scale options.

---

# Exact 30-byte native motion layout

The native controller input report is 64 bytes:

```text
packet[0] = 0x09       // HID report ID
packet[1..63] = body
```

Within the body:

```text
body[0x0E] = motion data length
body[0x0F] = first motion byte
```

Therefore, in absolute packet offsets:

```text
packet[0x0F] = motion length
packet[0x10..0x2D] = 30-byte motion block
```

The layout inferred from the captures is:

| Motion offset | Size | Type        | Meaning                                     |
| ------------: | ---: | ----------- | ------------------------------------------- |
|        `0x00` |    2 | `uint16_le` | Packed 800 Hz tick counter and sample count |
|        `0x02` |    2 | `int16_le`  | IMU temperature                             |
|        `0x04` |    4 | `int32_le`  | X angular phase                             |
|        `0x08` |    4 | `int32_le`  | Y angular phase                             |
|        `0x0C` |    4 | `int32_le`  | Z angular phase                             |
|        `0x10` |    4 | `int32_le`  | Accelerometer X, Q16.16                     |
|        `0x14` |    4 | `int32_le`  | Accelerometer Y, Q16.16                     |
|        `0x18` |    4 | `int32_le`  | Accelerometer Z, Q16.16                     |
|        `0x1C` |    2 | unresolved  | Zero in both uploaded captures              |

Equivalent C structure:

```c
typedef struct __attribute__((packed)) {
    uint16_t timing;
    int16_t  temperature;
    int32_t  angle_phase[3];
    int32_t  accel_q16[3];
    uint16_t tail;
} ns2_motion30_t;

_Static_assert(sizeof(ns2_motion30_t) == 30, "Unexpected layout");
```

Explicit little-endian serialization is preferable on the Pico rather than relying entirely on packed-structure behavior.

---

# 1. Timing word

The first two bytes form a little-endian 16-bit value:

```c
uint16_t timing = read_le16(&motion[0]);

uint16_t tick = timing & 0x0FFF;
uint8_t sample_count = timing >> 12;
```

## Low 12 bits: 800 Hz tick counter

The lower 12 bits increment modulo 4096.

The differences observed are almost always:

```text
3, 3, 3, 3, 4, 3, 3, 3, 3, 4, ...
```

That averages to:

```text
3.2 IMU ticks per USB report
```

The USB input reports arrive every approximately 4 ms, or 250 Hz:

```text
800 Hz / 250 Hz = 3.2
```

This is exceptionally strong evidence that the counter represents an **800 Hz internal IMU sampling timeline**.

## High 4 bits: number of IMU ticks represented

After the first active motion packet, the upper nibble exactly matches the modulo-4096 tick difference:

```c
sample_count ==
    (current_tick - previous_tick) & 0x0FFF;
```

For example:

```text
timing = 0x3004
sample_count = 3
tick = 4
```

or:

```text
timing = 0x400D
sample_count = 4
tick = 13
```

So the current PicoSwitch2 implementation's artificial `mtime += 4` is not correct. The real controller tracks an 800 Hz clock and usually advances by three samples, occasionally four.

A suitable generator is:

```c
static const uint8_t sample_pattern[5] = {3, 3, 3, 3, 4};

sample_count = sample_pattern[pattern_index++ % 5];
imu_tick = (imu_tick + sample_count) & 0x0FFF;

uint16_t timing =
    ((uint16_t)sample_count << 12) |
    imu_tick;
```

The phase of this pattern may not matter, but the long-term average and counter consistency probably do.

---

# 2. Temperature

Bytes `0x02–0x03` are consistently:

```text
00 0C
```

which is:

```text
0x0C00 = 3072
```

The ICM-42670-P temperature conversion is:

```text
temperature °C = raw / 128 + 25
```

Therefore:

```text
3072 / 128 + 25 = 49 °C
```

The ICM documentation specifies this temperature conversion and also supports the accelerometer sensitivity used below. 

For emulation, using:

```c
temperature = 0x0C00;
```

is consistent with every uploaded packet.

---

# 3. The supposed “gyro” values are 32-bit angular accumulators

Bytes `0x04–0x0F` contain three little-endian 32-bit values:

```c
int32_t phase_x = read_le32s(&motion[0x04]);
int32_t phase_y = read_le32s(&motion[0x08]);
int32_t phase_z = read_le32s(&motion[0x0C]);
```

They behave like **binary-angle phase accumulators**:

```text
2^32 units = one complete 360-degree turn
```

The conversion is:

```c
double degrees =
    (double)phase * 360.0 / 4294967296.0;
```

These should probably not yet be called strict Euler angles. A safer description is:

> Integrated, filtered angular phase values for the three sensor axes.

The exact Nintendo filtering and coordinate transform remain unknown.

## Why this interpretation fits

When parsed as three `int32_t` values:

* they evolve smoothly,
* natural 32-bit wrapping explains apparent discontinuities,
* differentiated values produce plausible angular velocities,
* derived maximum rates stay within approximately ±2000°/s,
* X and Y strongly correlate with roll and pitch inferred independently from gravity,
* the values start near approximately `0°, 0°, -180°` after motion reporting begins.

When parsed as twelve independent `int16_t` fields, several values jump almost randomly between positive and negative full-scale values while the controller is stationary.

Those “random gyro values” are simply the low fractional halves of larger 32-bit quantities.

## Deriving angular velocity

To recover average rate over one USB report:

```c
uint32_t current;
uint32_t previous;

/* Unsigned subtraction gives natural modulo-2^32 wrapping. */
int32_t delta = (int32_t)(current - previous);

double dps =
    (double)delta *
    360.0 *
    800.0 /
    (4294967296.0 * sample_count);
```

This gives the average angular velocity across the three or four 800 Hz IMU intervals represented by the report.

## Generating the phase values

Starting from angular velocity in degrees per second:

```c
phase_increment =
    gyro_dps *
    (4294967296.0 / 360.0) *
    dt;
```

At 800 Hz:

```c
phase_increment =
    gyro_dps *
    (4294967296.0 / 360.0) /
    800.0;
```

Then:

```c
phase[axis] += (int32_t)llround(phase_increment);
```

Natural unsigned 32-bit wrap should be preserved.

An observed startup state compatible with the captures is:

```c
uint32_t phase[3] = {
    0x00000000,
    0x00000000,
    0x80000000
};
```

That is:

```text
X ≈   0°
Y ≈   0°
Z ≈ -180°
```

I would reproduce this initially, although it is not yet proven that the console requires those exact initial values.

---

# 4. Accelerometer is signed Q16.16

Bytes `0x10–0x1B` are three signed little-endian 32-bit fixed-point values:

```c
int32_t ax_q16 = read_le32s(&motion[0x10]);
int32_t ay_q16 = read_le32s(&motion[0x14]);
int32_t az_q16 = read_le32s(&motion[0x18]);
```

Convert to ordinary ICM accelerometer counts with:

```c
double accel_counts = accel_q16 / 65536.0;
```

The captures fit the ICM-42670-P ±8 g sensitivity:

```text
4096 counts = 1 g
```

So:

```c
double accel_g =
    accel_q16 /
    (65536.0 * 4096.0);
```

The ICM-42670-P documentation gives 4096 LSB/g for the ±8 g setting. 

For a minimum viable emulator with integer accelerometer samples:

```c
int32_t accel_q16 =
    (int32_t)accel_counts << 16;
```

The real controller retains meaningful fractional low bits, so a more accurate implementation should preserve fractional values where available.

---

# Worked example from the capture

The first live-motion block in `PC2_Gyro_Default.pcapng` is:

```text
01 20
00 0c
00 de fe ff
01 a7 ff ff
00 1c 00 80
ab 7c 99 ff
e0 04 e9 fd
80 80 1e 10
00 00
```

Decoded:

```text
timing       = 0x2001
tick         = 1
sample_count = 2
temperature  = 0x0C00
```

The first packet's count of two appears to be a startup boundary condition. Subsequent packets obey the counter-delta relationship.

Angular phases:

```text
X = 0xFFFEDE00 = -74240
Y = 0xFFFFA701 = -22783
Z = 0x80001C00 = -2147476480
```

Converted:

```text
X ≈ -0.00622°
Y ≈ -0.00191°
Z ≈ -179.9994°
```

Accelerometer:

```text
X raw = 0xFF997CAB = -6718293
Y raw = 0xFDE904E0 = -35060512
Z raw = 0x101E8080 = 270434432
```

Converted first to counts:

```text
X ≈ -102.51 counts
Y ≈ -534.98 counts
Z ≈ 4126.50 counts
```

Then to g:

```text
X ≈ -0.0250 g
Y ≈ -0.1306 g
Z ≈  1.0074 g
```

That is a very plausible stationary gravity vector:

```text
sqrt(x² + y² + z²) ≈ 1.016 g
```

This is one of the strongest confirmations of the Q16.16 interpretation.

---

# Why the current PicoSwitch2 decoder looked partially correct

The existing branch breaks the block into 16-bit pieces and treats it approximately as repeated gyro/accelerometer samples. The relevant implementation is still described as awaiting validation. ([GitHub][1])

It appeared to decode accelerometer data because the upper 16 bits of a Q16.16 accelerometer value are approximately the ordinary integer accelerometer count:

```text
32-bit Q16.16 value:

[ integer high 16 ][ fractional low 16 ]
```

So, for example:

```c
(int16_t)(accel_q16 >> 16)
```

naturally gives values around ±4096 and produces a plausible gravity norm.

However, the supposed gyro field immediately before it is merely:

```c
(uint16_t)(accel_q16 & 0xFFFF)
```

That is the fractional portion of the accelerometer value. It can cover nearly the entire 16-bit range and therefore looks like wildly noisy gyro data.

Similarly, the “mysterious first sample” consists of the low and high halves of the 32-bit angular phase fields.

The current conceptual layout:

```text
gyro16, accel16, gyro16, accel16...
```

should therefore be replaced with:

```text
phase32 × 3, accel_q16_32 × 3
```

---

# Report activation behavior

In both captures:

* report ID `0x09` is emitted at approximately 250 Hz,
* initially the motion-data length is zero,
* after the feature set is enabled, the motion-data length becomes 30,
* there are approximately 174 zero-length reports before active motion,
* that corresponds to about 696 ms at 250 Hz.

The host sends a feature mask of `0x27`, including the IMU feature bit:

```text
0c 91 00 02 00 04 00 00 27 00 00 00
0c 91 00 04 00 04 00 00 27 00 00 00
```

The research documentation identifies the feature-mask command and the separate IMU and magnetometer feature bits. `0x27` enables the IMU but does not enable a separate magnetometer feature. ([GitHub][2])

The host also sends:

```text
03 91 00 0a 00 04 00 00 09 00 00 00
```

to select native report `0x09`.

However, in the captures, `0x09` is already the active report type and the motion payload can begin once the relevant feature configuration is applied. The report-selection command does not appear to be the sole trigger.

Existing research documents report `0x09` and its variable native motion payload, including observed lengths of 30 and, in other environments, 40 bytes. ([GitHub][3])

The uploaded captures only contain lengths:

```text
0
30
```

There are no 40-byte motion blocks in either file.

---

# Gyro calibration behavior

The calibration process in these captures is much clearer than the interpretation of the calibration contents.

## Calibration storage region

The controller uses the 64-byte region:

```text
address: 0x001FC000
length:  0x40
```

Existing research also places user motion calibration in this area. ([GitHub][4])

## Reset to default

In `PC2_Gyro_Default.pcapng`, the host writes:

```text
command:
02 91 00 05 00 48 00 00

memory operation header:
40 7e 00 00 00 c0 1f 00

data:
ff ff ff ff ... ff
```

The data is exactly 64 bytes of `0xFF`.

So “Reset to default” means:

```c
memset(calibration_block, 0xFF, 64);
```

It does not appear to write a special factory coefficient set.

## Calibrate

In `PC2_Gyro_Calibrate.pcapng`, the host writes:

```text
command:
02 91 00 05 00 48 00 00

memory operation header:
40 7e 00 00 00 c0 1f 00

data:
b2 a1 00 00 00 00 ... 00
```

That is:

```text
B2 A1 + 62 zero bytes
```

The response to both writes is:

```text
02 01 00 05 00 f8 00 00
00 00 00 00 00 c0 1f 00
```

The memory-operation framing corresponds to the documented read/write command family. ([GitHub][2])

## Meaning of `B2 A1`

`B2 A1` is almost certainly a little-endian validity or “user calibration present” marker:

```text
raw bytes: B2 A1
LE value:  0xA1B2
```

The same marker convention is used for other Nintendo controller user-calibration regions, including stick calibration.

The uploaded calibration capture does **not** provide nonzero coefficient fields. Therefore, these captures prove:

* the address,
* the length,
* the validity marker,
* reset behavior,
* write timing,
* reboot persistence,

but they do **not** reveal the internal scale or ordering of gyro bias coefficients.

## When calibration takes effect

The input-report stream continues normally during the flash write. The write response arrives roughly 35 ms later, but report `0x09` does not pause or restart.

After unplugging and reconnecting, the controller starts with the newly selected calibration state.

The initialization sequence does not visibly read `0x1FC000` over the host protocol. This suggests the controller firmware reads and applies its own motion-calibration region internally during startup rather than sending those coefficients to the console.

---

# Suggested replacement implementation

A minimal generator could look conceptually like this:

```c
typedef struct {
    uint16_t imu_tick;
    uint8_t pattern_index;

    uint32_t phase[3];
    int32_t accel_q16[3];
    int16_t temperature;
} ns2_imu_state_t;

static const uint8_t imu_counts[5] = {3, 3, 3, 3, 4};

static void build_motion30(
    uint8_t out[30],
    ns2_imu_state_t *state)
{
    uint8_t count =
        imu_counts[state->pattern_index++ % 5];

    state->imu_tick =
        (state->imu_tick + count) & 0x0FFF;

    uint16_t timing =
        ((uint16_t)count << 12) |
        state->imu_tick;

    write_le16(out + 0x00, timing);
    write_le16(out + 0x02, (uint16_t)state->temperature);

    write_le32(out + 0x04, state->phase[0]);
    write_le32(out + 0x08, state->phase[1]);
    write_le32(out + 0x0C, state->phase[2]);

    write_le32(out + 0x10, (uint32_t)state->accel_q16[0]);
    write_le32(out + 0x14, (uint32_t)state->accel_q16[1]);
    write_le32(out + 0x18, (uint32_t)state->accel_q16[2]);

    write_le16(out + 0x1C, 0);
}
```

Phase update:

```c
static void integrate_axis(
    uint32_t *phase,
    double angular_velocity_dps,
    unsigned imu_samples)
{
    double increment =
        angular_velocity_dps *
        (4294967296.0 / 360.0) *
        ((double)imu_samples / 800.0);

    *phase += (uint32_t)(int32_t)llround(increment);
}
```

Initial state matching the captures:

```c
state.temperature = 0x0C00;

state.phase[0] = 0x00000000;
state.phase[1] = 0x00000000;
state.phase[2] = 0x80000000;
```

For stationary emulation:

```c
state.accel_q16[0] = 0;
state.accel_q16[1] = 0;
state.accel_q16[2] = 4096 << 16;
```

Real controller orientation and axis mapping may require signs or permutations before integration.

---

# Confidence assessment

**Very high confidence**

* 30-byte field boundaries.
* 800 Hz low-12-bit counter.
* high nibble representing elapsed IMU ticks.
* accelerometer as signed 32-bit Q16.16.
* accelerometer sensitivity corresponding to 4096 counts/g.
* user calibration address `0x1FC000`.
* reset data being 64 bytes of `0xFF`.
* calibrated marker being `B2 A1`.
* report rate being approximately 250 Hz.

**High confidence, but still inferred**

* the three 32-bit values are full-turn binary angular phase accumulators.
* `2^32 == 360°`.
* differentiating them gives angular rate.
* initial Z phase near `0x80000000`.

**Still unresolved**

* exact coordinate-system signs and permutations expected by the console.
* whether the phases are Euler angles, sensor-axis integrated angles, or the output of a proprietary filtered integrator.
* precise filtering and drift correction.
* semantic meaning of the final two bytes.
* structure of nonzero motion-calibration coefficients.
* the separate 40-byte motion format seen in other research.
* whether the console performs strict validation beyond timing and physically plausible values.

The most useful next capture would be a controlled sequence with the controller initially still, followed by exactly one slow rotation around each physical axis separately. That would lock down axis order, sign, phase scale, and whether the phase represents body-axis integration or orientation angles.

[1]: https://github.com/notsosaelin/PicoSwitch2/blob/ns2-testing/docs/switch2/report-0x09-motion.md "https://github.com/notsosaelin/PicoSwitch2/blob/ns2-testing/docs/switch2/report-0x09-motion.md"
[2]: https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md "https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md"
[3]: https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md "https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md"
[4]: https://github.com/ndeadly/switch2_controller_research/blob/master/memory_layout.md "switch2_controller_research/memory_layout.md at master · ndeadly/switch2_controller_research · GitHub"
