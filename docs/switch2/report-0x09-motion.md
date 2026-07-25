# Switch 2 Pro Controller — Report 0x09 Motion (IMU) Format

**Current status (2026-07-24):** ✅ a genuine Pro Controller 2's native BLE motion is passed
byte-for-byte into console-facing report `0x09` and is hardware-confirmed in Splatoon 3. Correct
aim axes, stationary behavior, controller power-cycle, bonded reconnect, and source-off hold all
pass. DualSense gyro translation is also hardware-confirmed, including rapid motion and reconnect.
The genuine stream's length-`0x28` G6/G7/G8 lanes are partially decoded as a controller-processed
vector. A body-frame magnetic/reference vector and the vector part of a second “magneto
quaternion” remain competing interpretations; direct raw signed-16-bit magnetometer samples are
ruled out by the observed 22/22/20-bit wire values. Remaining leading/middle lanes and escalation
semantics are still under investigation with
[`uart-magprobe.md`](uart-magprobe.md).

The legacy length-30 USB field analysis below remains useful for the generic encoder and historical
captures. It is no longer the production path for a genuine PID `057E:2069` source; that controller
provides its own variable-length `0x1E`/`0x28` PDU. See
[`native-pro2-motion-passthrough-2026-07-21.md`](../experiments/native-pro2-motion-passthrough-2026-07-21.md).

> **Supersedes the earlier int16 interpretation.** This document previously described the block as
> two interleaved `int16` `[gyro,accel]` samples. That model is **refuted** — see §"Why the int16
> model looked partially right". The corrected layout below is what the firmware must emit.

---

## Question

Report `0x09` (the console-facing input report) carries an IMU block whose packing ndeadly's
`hid_reports.md` calls an *"unknown packed format"* (length byte `{0,30,40}` at offset `0x0E`, data
at `0x0F`). Earlier blind hypotheses (3 samples length 40; int16 interleaved length 30) produced
**no usable motion**. We need the real layout and scale.

## Method — decode + independent re-verification

1. **Primary decode** ([report-0x09-motion-analysis.md](report-0x09-motion-analysis.md)): two clear
   USB captures (`PC2_Gyro_Default.pcapng`, `PC2_Gyro_Calibrate.pcapng`) analysed by byte-level
   continuity, physical plausibility, and known ICM-42670-P scales.
2. **Independent re-verification** (this repo's `usb.pcapng`, a Packetry/Cynthion raw USB-2.0 wire
   capture, 50,650 motion reports) via `scratchpad/usb_c_verify_int32.py`. Results:

   | Test | Result |
   |---|---|
   | Temperature @ `motion[0x02]` | constant `3072` (0x0C00) across 2000 reports |
   | Accel parsed as int32 **Q16.16**, stationary window | \|g\| **mean 1.0005, CV 0.0068** |
   | Q16.16 half-variance discriminator (3 axes) | low-16 SD ≈ 16–20k (fractional), high-16 SD ≈ 11–35 (stable gravity) |
   | Timing high-nibble == 800 Hz tick-delta | **299/299 reports** |
   | Phase Z at motion start | `0x802a1300` ≈ `0x80000000` (−180°) |

   The half-variance test is the proof: the bytes the old model read as "gyro" are literally the
   **fractional low-16 bits of the Q16.16 accelerometer** — hence their random full-scale look.

## Format (length = 30)

Motion block begins at report offset `0x0F` (`motion[k]` below = report byte `0x0F + k`). The
length byte is at report offset `0x0E` (`0` = IMU off, `30` = IMU on). The original direct USB
captures contained no length-40 records; the live native BLE bridge now observes and successfully
forwards genuine length-40 blocks alongside length-30 blocks.

| motion off | Size | Type | Field | Notes |
|---|---|---|---|---|
| `0x00` | 2 | `uint16_le` | **Timing** | low 12 bits = 800 Hz IMU tick (mod 4096); high 4 bits = ticks elapsed since previous report (usually 3–4). |
| `0x02` | 2 | `int16_le` | **Temperature** | constant `0x0C00` (3072). ICM: °C = raw/128 + 25 ⇒ 49 °C. |
| `0x04` | 4 | `int32_le` | **Angular phase X** | binary angle, `2^32 = 360°`. Integrated/filtered angular phase. |
| `0x08` | 4 | `int32_le` | **Angular phase Y** | |
| `0x0C` | 4 | `int32_le` | **Angular phase Z** | starts near `0x80000000` (≈ −180°). |
| `0x10` | 4 | `int32_le` | **Accel X** | **Q16.16 fixed point**; `65536 × 4096 = 1 g`. |
| `0x14` | 4 | `int32_le` | **Accel Y** | ICM-42670-P ±8 g ⇒ 4096 LSB/g. |
| `0x18` | 4 | `int32_le` | **Accel Z** | |
| `0x1C` | 2 | — | **Tail** | zero in all captures; unresolved. |

```c
typedef struct __attribute__((packed)) {
    uint16_t timing;        // (count << 12) | (tick & 0x0FFF)
    int16_t  temperature;   // 0x0C00
    int32_t  phase[3];      // binary angle, 2^32 == 360 deg
    int32_t  accel_q16[3];  // Q16.16, 65536*4096 == 1 g
    uint16_t tail;          // 0
} ns2_motion30_t;           // sizeof == 30
```

### Conversions
```
gyro_dps    = (int32_t)(phase - prev_phase) * 360.0 * 800.0 / (2^32 * sample_count)
accel_g     = accel_q16 / (65536.0 * 4096.0)
accel_count = accel_q16 / 65536.0            // ordinary ICM count (~±4096 = 1 g)
```

## Activation — motion is a *negotiated feature* (not always-on)

Confirmed by both `report-0x09-motion-analysis.md` and this repo's Experiment C
([gyro-experiment-c-results.md](../experiments/gyro-experiment-c-results.md)): report `0x09`
streams from power-up with **motion-len = 0** and only flips to `30` after the host enables the
IMU feature over bulk EP2:
```
0c 91 00 06 00 04 00 00 …          # configure motion features (ids 0x02/0x03)
0c 91 00 04 00 04 00 00 27 00 00 00 # feature mask 0x27 (IMU bit set, no magnetometer) -> motion ON
```
(~174–251 zero-length reports precede motion-on in the captures.) The current firmware gates motion
on this negotiated feature, matching the genuine device. See Experiment C for the original trace.

## Why the int16 model looked partially right

A Q16.16 accel value is `[ int16 high ][ frac16 low ]`. Reading `(int16)(accel_q16 >> 16)` yields
the ordinary gravity count (~±4096) — so the old "accel" lane looked correct and produced a
plausible 1 g magnitude. But the adjacent "gyro" lane was `(uint16)(accel_q16 & 0xFFFF)` — the
**fractional half** — which sweeps most of the 16-bit range and looks like random full-scale gyro.
Likewise the "first sample" was the low/high halves of the 32-bit phase fields. Replace
`gyro16,accel16 × repeated` with `phase32 × 3, accel_q16_32 × 3`.

## Calibration (from report-0x09-motion-analysis.md; not yet needed for basic gyro)

- User motion-calibration region: address `0x001FC000`, length `0x40`.
- "Reset to default" writes 64 × `0xFF`. "Calibrate" writes validity marker `B2 A1` (LE `0xA1B2`)
  + zeros. Nonzero coefficient fields were **not** captured — internal scale/order still unknown.
- The controller appears to apply its calibration internally at startup (not sent over the host
  protocol), so basic emulation can ignore it initially.
- **Cross-checked 2026-07-10, two ways:** (1) a genuine unit's own SPI flash dump confirms this
  region is `0xFF`-filled (uncalibrated) on real hardware exactly as expected — see
  `docs/experiments/spi-dump-analysis-2026-07-10.md` §3.3. (2) External research into
  `Dycool/NS-PC-Control` (`server/src/switch2_native.cpp`) independently confirms `0x1FC000` as the
  real motion-calibration write target (`mem_write_user_motion_cal()`, persisted to a local file)
  — but its write handler is for a **72-byte** payload, not 64. 🔵 Unresolved discrepancy, not
  contradiction: 72 could be a wrapped/header-prefixed write frame around a 64-byte payload (their
  code wasn't read at the byte-layout level, just the address/size it declares), or our documented
  length could be incomplete. Worth resolving before ever implementing calibration *write* support
  (not currently planned — basic emulation still doesn't need it).

### ✅ Factory motion calibration (`0x13040`, `0x13100`) — decoded, distinct from the user region above

Not to be confused with the *user* motion-calibration region (`0x1FC000`, still unprogrammed/unknown
on every unit seen so far). This is a **separate, always-populated factory block**, and as of
2026-07-10 its layout is confirmed by a working third-party BLE client (`tools/switch2_input_viewer.py`,
added to this repo for reference — a PyQt/bleak tool that pairs with a genuine controller over BLE
GATT and parses its reports/calibration live). Its `handle_read_response()` decodes:

| Address | Size | Fields | Type |
|---|---|---|---|
| `0x13040` | 16 | `temperature` @ `[0:4]`, `gyro_bias` (x,y,z) @ `[4:16]` | `float32` each (IEEE-754 LE) |
| `0x13100` | 24 | `magnetometer_bias` (x,y,z) @ `[0:12]`, `accelerometer_bias` (x,y,z) @ `[12:24]` | `float32` each |

**Verified against our own SPI dump** (`dumps/2069_spi_dump_2026-07-10_1422.bin`, decoded with this
exact field mapping):

- `temperature` = **27.09** — plausible factory-calibration ambient temperature (°C).
- `gyro_bias` = **(-0.0205, -0.00058, 0.00328)** — tiny, consistent with (and roughly matching the
  order of magnitude of) this repo's already-documented "genuine gyro ~0.03 dps bias" figure. Units
  not independently confirmed but plausibly dps.
- `magnetometer_bias` = **(0, 0, 0)** — zero on this unit. Earlier work interpreted this as an
  unused magnetometer, but live 2026-07-24 `0x28` captures expose a stable G6/G7/G8 fused/reference
  lane even under mask `0x27`; zero bias therefore cannot establish that a physical sensor or
  internal fusion path is unused.
- `accelerometer_bias` = **(0.160, -0.0687, 10.38)** — the Z axis sits close to **standard gravity
  (9.8 m/s²)**, strongly confirming these are **physical SI-unit floats (m/s²), not raw ADC/LSB
  counts** — the controller was evidently lying flat (Z-up) during factory calibration capture.

**Why this matters for report 0x09:** this is the first *direct* evidence that the genuine
controller's internal calibration model operates in physical floating-point units (dps, m/s²), not
fixed-point LSB counts like this repo's current implementation. It does not by itself prove
anything about report-0x09's phase-field *semantics* (that calibration is applied before the raw
ADC counts ever reach any report), but it raises the prior slightly on "the firmware does real
floating-point sensor math internally" — relevant background for the still-open raw-integration-vs-
fused-orientation question below.

**Feature-flag bit table, also confirmed by this tool** (`FeatureFlagWidget.BIT_FLAGS`), cleanly
matches and *names* the bits behind this repo's already-observed enable mask: bit0=buttons,
bit1=analog sticks, **bit2=IMU**, bit3=unknown, bit4=mouse, bit5=battery current, bit6=unknown,
**bit7=magnetometer**. Our captured enable mask `0x27` = `0b00100111` = bits 0,1,2,5 — buttons +
sticks + IMU + battery-current, **not** bit 7. That naming remains useful protocol evidence, but
its scope is no longer assumed: the genuine controller's current `0x27` stream interleaves
length-`0x28` PDUs whose G6/G7/G8 values form a bounded, near-constant-norm fused/reference
representation. A direct PID-`0x2069` positive control later replayed the public `0x94` sequence
and re-subscribed handle `0x000A`; every command and ATT operation succeeded, but `0x000A`
emitted no reports. Bit 7 therefore does not expose that public signed-int16 raw channel on the
tested Pro Controller 2.

**Command-protocol cross-check:** `switch2_input_viewer.py`'s BLE `read_spi_memory()` sends
`[0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, size, 0x7E, 0x00, 0x00, addr(4, LE)]` — the same
`[id][0x91][transport][sub]...` shape, the same `id=0x02/sub=0x04` memory-read command, and the same
`c[8]=len, c[12:16]=addr` field positions this repo's `ns2_dispatch()` already implements. **The
command protocol is confirmed identical across USB and BLE transports** (only the framing changes),
which is a meaningful cross-validation of this repo's existing capture-derived protocol model, not
just this one field. (Its `write_spi_memory()` reuses `sub=0x04` rather than this repo's
capture-derived `sub=0x05` for writes — 🔵 unverified whether that's a real alternate protocol
detail or a bug in that tool; not adopted without a citation, since it conflicts with an existing
capture-derived fact.)

## Length-30 byte-budget result; length-40 magnetic lane supersedes the broader refutation

A research lead surfaced 2026-07-10 (after the second hardware test) asked whether the console's
"angular phase" depends on more than 3-axis gyro + 3-axis accel — magnetometer data, sensor fusion,
or a 9-axis IMU. **This does not fit the confirmed byte budget.** The motion block is **exactly**
30 bytes for length-30 (the only length seen on hardware other than 0): timing(2) + temp(2) +
phase×3(12) + accel×3(12) + tail(2) = 30, with **zero spare bytes** — every byte is already
accounted for by the half-variance/Q16.16 proof above, independent of the magnetometer question.
There is no room in the **length-30 form** for a magnetometer lane. The
"second lane group at 0x15/0x19/0x1D" observation that originally motivated this lead was from the
**refuted int16 model** (see "Why the int16 model looked partially right" above) — those offsets
are inside the Q16.16 accel fields' fractional bytes, already explained, not a separate sensor.

**Updated by direct live evidence (2026-07-24):** the normal length-`0x28`, status-`0x0D` form has
additional G6/G7/G8 lanes with a nearly constant norm. Direct signed-int16 sensor samples are
excluded by their 22/22/20-bit ranges; a normalized reference vector or vector part of a second
quaternion remains numerically possible. The form is emitted by the real controller in the
current `0x27` production profile and was unchanged by a fully acknowledged `0x94` positive
control. That same positive control produced no handle-`0x000A` reports despite a successful
post-init CCC subscription. Thus `0x28` is a separate controller-processed/fused representation,
not the public raw-magnetometer channel. The narrower length-30 byte-budget proof remains valid.
See [`uart-magprobe.md`](uart-magprobe.md) for the decoder and evidence boundary.

A four-epoch stationary capture strengthens the quaternion interpretation: the live G0/G1/G2
orientation advanced `16.740°` while G6/G7/G8 direction changed only `0.099°`. Reconstructing
`w = sqrt(1 - G6² - G7² - G8²)` yielded a unit-quaternion candidate that changed only `0.077°`,
a `216.7x` stability split. The working model is therefore “drifting live quaternion plus stable
corrected/reference quaternion,” with correction source and exact component semantics still open.

**Diagnostic encoder boundary (2026-07-24):** `ns2_motion_pdu40_get_reference()` and
`ns2_motion_pdu40_set_reference()` now provide a host-tested, byte-exact codec for the understood
signed 22/22/20-bit G6/G7/G8 lanes. Golden testing uses a genuine UART-captured `0x28` PDU and
also covers all signed endpoints while proving that the shared unassigned bits and every byte
outside the three lanes remain untouched. The DualSense translator also maintains an optional
second quaternion: it integrates the same calibrated body rate and applies conservative
accelerometer feedback to observable pitch/roll, while correctly leaving yaw relative because a
DualSense has no magnetometer.

This is **not production `0x28` generation**. The 2026-07-24 live test conclusively rejected the
complete template-derived packet: enabling `ds5motion ref28 on` caused immediate random motion
despite valid length-40 selection, changing G6/G7/G8 values, and zero encoder rejects. Turning the
gate off immediately restored the validated length-`0x1E` path without reflashing. The exact
G6/G7/G8 packer remains supported independently, but the experiment proves that the unresolved
leading/middle lanes cannot remain static; the console consumes or cross-validates them. The
generator, secondary quaternion, and UART gate were removed after the test so the rejected packet
model cannot be enabled accidentally. See
[`../experiments/refuted-hypotheses.md`](../experiments/refuted-hypotheses.md).

## Remaining unknowns / suggested experiments

1. **Coordinate signs & axis order** the console expects (body-axis integration vs orientation
   angles). 🔵 **Partially resolved 2026-07-10** — re-mining the genuine controller's own
   report-0x05 capture (its "still, then pitch/yaw/roll" protocol) identified raw gyro **X=pitch,
   Z=yaw, Y=roll** with medium confidence (X/Z well-supported by two independent signals each; Y's
   axis identity is by elimination and its sign is inferred from the rotation-properness
   constraint, not measured — the capture's roll segment was too messy to read cleanly). Seam fixed
   accordingly. See [gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md).
   *Still open:* an independently-measured roll sign, from a cleaner recapture (§4 of that doc).
2. **Phase semantics** — Euler vs sensor-axis integrated vs proprietary filtered integrator; drift
   correction unknown. Treat as "integrated angular phase," not strict Euler, until proven.
   🔵 **Reassessed 2026-07-10** against an unattributed third-party claim ("assumed to be some
   form of compressed quaternion... field sizes almost certainly not multiples of 8 bits... motion
   data size is also variable") relayed via `CORTEX_PARSE.md`. **The non-byte-aligned / bit-packed
   part of that claim conflicts with hard evidence already in hand** and is not adopted: the
   half-variance discriminator (§"Method" above) found that splitting each 32-bit accel field
   *exactly* at the 16-bit boundary produces one half with SD≈11-35 (stable, matches gravity) and
   the other with SD≈16-20k (fractional noise) — a genuinely bit-packed, non-byte-aligned format
   would not produce a clean split at a power-of-two bit boundary by chance, across all three axes,
   independently, on two separate captures. Field *boundaries* stay confirmed. The "variable size"
   part is already known and undisputed (length is 0 or 30 today; a `40`-byte variant is
   documented as seen elsewhere). **What does survive, and sharpens an existing open question**:
   whether each byte-aligned int32 "phase" field's *value* is naive integrated rate (this repo's
   current assumption) versus something more like a fused-orientation or quaternion-*component*
   representation still living in that same byte-aligned slot — i.e. the disagreement that matters
   is about semantics, not layout. This reading is also consistent with external evidence: see
   `gyro-hardware-validation-2026-07-10.md` §7.5 — `TommyWabg/Switch2Connect` found that even
   genuine hardware's *raw rate* stream (a different report, over BLE) needs full AHRS fusion
   downstream to be drift-free, i.e. "orientation" is not something Nintendo hardware hands over
   for free either. Distinguishing "raw integrated rate" from "fused orientation state" in the
   USB report-0x09 phase fields is the highest-value remaining experiment for this field —
   candidate method: hold the controller at a **fixed non-zero tilt** (not moving) and check
   whether the phase value matches a static orientation reading (fusion) or drifts/resets (pure
   rate integration has no "current orientation" concept without integrating from a known start).
   🔵 **New reasoning, 2026-07-10** (from `tools/switch2_input_viewer.py`, a working third-party BLE
   client added to this repo — see item 5 below): that tool shows the **same Pro Controller 2**
   reports a **14-byte** motion block over one BLE path (GATT handle `0x000A`, all device types —
   plausibly a single raw `temp+accelXYZ+gyroXYZ` int16 sample, per that tool's own commented-out
   dtype) and a **40-byte** block over another (handle `0x000E`, Pro/GCN device types — undecoded
   even by that tool). USB report 0x09 polls at 250 Hz; BLE typically negotiates a much lower
   notification rate. A plausible, mundane explanation for *why* USB carries a single "integrated
   phase" value rather than discrete samples: **at USB's high poll rate, few IMU ticks elapse
   between reports, so one running/integrated value per report loses little** — whereas BLE's
   slower cadence would need to batch multiple raw samples (hence a bigger, multi-sample block) to
   avoid discarding IMU data between notifications. This doesn't prove the phase field is naive
   rate-integration rather than fused orientation, but it **weakens the case that something exotic
   (fusion/quaternion) is *required*** — a simple "integrate raw rate since last report" model is
   sufficient to explain the format shape (single value, byte-aligned, scales with poll rate)
   without needing onboard fusion. Net effect: this repo's existing implementation approach (rate
   integration over real elapsed time) remains a reasonable default; the fixed-tilt experiment above
   is still the right way to settle it empirically.
   🔵 **Sharper alternative, 2026-07-10** (from `docs/experiments/switch2_native_motion_map_DyCOOL.md`,
   a carefully validated third-party decode of the Switch 2's *native BLE* motion report): that
   sibling format encodes raw, individually-clamped (±500°/s) gyro **samples**, not an accumulator —
   a concrete, working alternative value semantic. Not proof for report 0x09 itself (that document's
   own author leaves Pro Controller 2's report 0x09 explicitly unverified), and report 0x09's own
   prior worked-example evidence (smooth, physically-plausible differentiated rates —
   `report-0x09-motion-analysis.md`) still favors the accumulator model on its own terms. Ranked
   analysis of this tension and a new direct-observation experiment (watch the phase accumulator
   itself for discontinuities via a new debug field, no console needed):
   [gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md) §12.
3. **Tail `motion[0x1C]`** — zero in all captures; purpose unknown.
4. **Timing epoch/phase** — does the console require a specific starting tick, or only a consistent
   ~800 Hz progression with correct high-nibble deltas? (High-nibble delta is validated 299/299.)
5. **Length-40 variant** — seen elsewhere, absent from the original static USB capture. 🔵
   **Located 2026-07-10; partially decoded 2026-07-24:** `tools/switch2_input_viewer.py` confirms a genuine
   Pro Controller 2 emits exactly this 40-byte block over its BLE GATT notification at handle
   `0x000E` (report offset `0xF:0x37`) — a *different transport* from USB report 0x09, so this is
   not proof the USB `40` some other research mentioned is the same format. Direct UART captures
   now establish an interleaved 133 Hz `0x1E`/`0x28` stream. Normal status-`0x0D` G6/G7/G8 decode
   as a nearly constant-magnitude controller-processed vector that rotates back to a more stable
   world vector in both existing moving captures under the nearest/interpolated `0x1E`
   quaternion. The vector also fits numerically as three components of a second quaternion,
   matching the source bitmap's “Magneto Quaternion” name; its exact semantics remain unresolved.
   The leading/middle
   `0x28` lanes and
   status-`0x0F` escalation form remain unresolved; see
   [`uart-magprobe.md`](uart-magprobe.md).
6. **Whether the console strictly validates** beyond timing + physically plausible values.

## Implementation status

🟢 **Current genuine-source path:** a real Pro Controller 2's controller-generated `0x1E`/`0x28`
blocks are transported opaquely and hardware-confirmed in Splatoon 3. This avoids relying on the
generated value semantics discussed below. On disconnect, the last genuine length-30 state is held
stationary while its timing word advances; source-slot and VID/PID ownership prevent reuse by a
different controller.

🟢 **Current DualSense production path:** only the host-tested length-`0x1E` smallest-three
quaternion carrier is emitted. The hardware-refuted length-`0x28` generator and its UART gate have
been removed; passive decoding and the exact field codec remain available for continued research.

🔵 **Historical/generic encoder path:** the Switch 2 reads the generated gyro pipeline (both Zeldas
and Splatoon respond), but its exact fidelity remains unresolved. `src/switch_pro2/switch_pro2.c`
`ns2_build_report()` emits the int32 layout above and **gates motion
on the `0x0C/0x04` IMU-enable** (`ns2_imu_enabled`, reset per host session in `tud_mount_cb`) —
length 0 until the console enables it, matching a real PC2. Angular phase = the integral of
`in.gyro` over **real elapsed time** (`time_us_32`), scaled `2^32 / (16.384 LSB/dps · 360° · 1e6) =
0.72818` per µs·LSB; the 800 Hz timing word tracks real time (`count = dt_us/1250`); accel is
`in.accel * 65536` (Q16.16).

**First hardware test (2026-07-10): pipeline confirmed, math wrong.** Splatoon accepted the
negotiated-feature gyro, but a **stationary controller still drifted the camera** — pure rate
integration accumulating the DualSense's gyro bias with no correction (the genuine ~0.03 dps bias is
negligible; a DualSense's is not, and any constant bias integrates without bound). **Take-1 fix**: a
slow, stillness-gated per-axis bias tracker in `ns2_build_report()` before integration.

**Second hardware test (2026-07-10, same day): take-1 fix made no observable difference** — Splatoon
and Zelda both still showed unusable, unstable motion while stationary. Root cause: the take-1
stillness gate tested raw gyro **magnitude** (`|raw| < 40 LSB`) against a threshold that a MEMS
gyro's own constant zero-rate bias can plausibly exceed on its own — self-defeating (the gate would
never open, so the bias estimate would never adapt, reproducing exactly the un-fixed behavior).
**Take-2 fix**: the gate now tests the gyro's frame-to-frame **derivative** (steadiness) instead of
its magnitude, so it opens correctly regardless of how large the underlying bias is. The bias/gate
state is now exposed live via the config-mode `imu` debug command (`bias=[…] still=0|1`) so the next
test can confirm the mechanism directly instead of inferring it from in-game symptoms. Full writeup
(both tests + both fixes): [gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md).

**Axis signs / order** — reuses report-0x05's axis transform, which was **also found wrong** in the
same test (pitch/roll swapped, yaw correct) and **fixed** in `ns2_seam.c` from a re-analysis of the
genuine controller's own capture (🔵 medium confidence — see above and the hardware-validation doc).
Both fixes are build-clean but **not yet hardware-tested**.

Remaining knobs if jitter (not drift) persists: **low-pass strength** (EMA α 0.25→0.125). **Scale**
`0.72818` (from 16.384 LSB/dps) and **initial phase** `Z = 0x80000000` (−180°) are unchanged from the
first cut and still unverified against a moving on-console reference.

**Symptom reclassified, 2026-07-10 (later).** The in-game stationary symptom is **abrupt
multidirectional jumps, not gradual drift** — a meaningful correction, since "drift" implied
gradual bias-accumulation error and biased the investigation toward bias-tracker work specifically.
That work independently confirmed the take-2 stillness gate works correctly on hardware (a
DualSense reads `still=1` stationary / `still=0` moving), which rules the gate itself out as the
jump's cause without explaining the jumps. A new, carefully-validated third-party decode of the
Switch 2's **native BLE** motion format (`docs/experiments/switch2_native_motion_map_DyCOOL.md`)
shows that format uses raw, bounded, clamped gyro *samples* rather than an unbounded accumulator —
a concrete alternative value semantic for report 0x09's still-open "phase field semantics"
question (item 2 below), though not proof either way for report 0x09 specifically (that document's
own author explicitly leaves Pro Controller 2's report 0x09 "to be verified"). Also gyro-scale
mismatch was checked and **ruled out** as an explanation for the current DualSense test (the
joypad-os framework's own `event->gyro_range = 2000` for DS4/DS5 matches this repo's `16.384
LSB/dps` constant exactly). New instrumentation (`ns2_dbg_motion_phase()`, exposed on the
config-mode `imu` line as `phase=[...]`) now lets the phase accumulator itself be watched directly
for discontinuities, independent of any console. Full analysis:
[gyro-hardware-validation-2026-07-10.md](../experiments/gyro-hardware-validation-2026-07-10.md) §12.
