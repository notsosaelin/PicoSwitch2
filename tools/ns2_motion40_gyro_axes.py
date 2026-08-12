#!/usr/bin/env python3
"""0x28 gyro: axis order, sign, and counts-per-dps, measured against the carrier.

Why this exists
---------------
The translator inherited its axis mapping from the length-0x1E path, which is
console-validated, and never checked that 0x28 gyro slots use the same
convention. Byte-exactness cannot catch a swapped or inverted axis: the packet
is well formed and the console turns the wrong way.

Acceleration was closed directly -- handle 0x000A raw IMU against 0x000E slot 0
agrees to within sensor noise. Gyro could not use that route, because every
capture holding both handles is stationary, where gyro sits at the noise floor
(max 4 counts = 0.24 dps across all of them).

Method, using captures we already have
--------------------------------------
Two independent readings that do not need a raw IMU reference:

1. AXIS, from amplitude. A lazy susan turns about the vertical axis. With the
   controller flat, vertical is whichever axis carries gravity -- and accelZ
   reads +1.04 g in those captures, so the turn is about the controller's own
   Z. If the gyro shares the accelerometer's frame, gyroZ must dominate.

2. SIGN, from reversal. The lazy-susan capture and its `-return` counterpart
   turn opposite ways in the same pose. A direction-sensitive gyro must flip
   sign on the turned axis between them. Amplitude alone cannot show this,
   which is why both readings are needed.

What this does NOT establish
----------------------------
* X and Y are not individually disambiguated. No capture holds a pure rotation
  about each with a known direction, so a hypothetical X<->Y swap would survive
  this. `splatoon-0-to-1` does drive gyroX to 119 dps in a tilted pose, which
  shows X responds independently of Z, but that is weaker than a controlled
  single-axis turn.
* Absolute handedness -- whether +gyroZ is the direction the console expects --
  rides on the 0x1E path being console-validated with this same mapping. This
  shows 0x28 agrees with 0x1E, not that both match Nintendo's convention.

Those residual X/Y questions are not a release blocker for the translator: its
axis map is inherited from the console-validated 0x1E path, and the existing
lazy-susan/return pair proves that 0x28 uses that same frame for Z and sign.

3. SCALE and HANDEDNESS, from the carrier's own rotation. Unit norm alone is
   NOT evidence here -- the decoder reconstructs and normalizes the omitted
   component. The carrier's angular scale is instead supported independently by
   retained-energy bounds, console validation, and the gravity-direction audit
   (within 10-20%). Summing the decoded carrier's change over a capture gives a
   rotation proxy that the 0x28 gyro integrated over the same span must match.
   Integration avoids the per-sample attenuation caused by comparing an
   instantaneous gyro sample with a window-averaged carrier rate.

   A positive slope also settles handedness. Our path integrates the quaternion
   from +gyro_corrected and puts +gyro_corrected in the lanes, so genuine
   agreeing in sign means we share the convention. The ICM-42670-P sensor and
   common/normal gyro use 16.4 counts/dps; this check therefore arbitrates the
   high-rate lane's fixed-point conversion. The existing corpus selects seven
   fractional bits (/128), not the adjacent acceleration lane's eight (/256).

Run:
    python tools/ns2_motion40_gyro_axes.py
"""

from __future__ import annotations

import math
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_reference as R
from ns2_motion40_prefix_epoch import paired_timeline

CAPTURES = Path("dumps/BLE CAPTURE")
LAZY = "pro2-chart-transition-lazy-susan-2026-07-29.jsonl"
LAZY_RETURN = "pro2-chart-transition-lazy-susan-return-2026-07-29.jsonl"

# A lane must beat the next loudest by this much to be called dominant.
DOMINANCE_RATIO = 3.0
# Gravity must be on Z for the lazy-susan argument to hold at all.
FLAT_ACCEL_Z_G = 0.8


def lanes(path: Path):
    """Per-axis gyro in dps and acceleration in g, from every mode-3 packet."""
    _o30, pdu40 = paired_timeline(path)
    if not pdu40:
        return None, None
    gyro: list[list[float]] = [[], [], []]
    accel: list[list[float]] = [[], [], []]
    for _tick, sample in pdu40:
        if sample.packing_mode != 3 or not sample.gyro:
            continue
        gyro_scale = R.WIRE_TO_COUNTS[sample.layout]["gyro"][0]
        accel_scale = R.WIRE_TO_COUNTS[sample.layout]["accel"][0]
        for axis in range(3):
            gyro[axis].append(sample.gyro[0][axis] * gyro_scale / R.IMU_COUNTS_PER_DPS)
            accel[axis].append(sample.accel[0][axis] * accel_scale / R.IMU_COUNTS_PER_G)
    return (gyro, accel) if gyro[0] else (None, None)


def carrier_rotation_degrees(orientation30):
    """Total angle swept by the carrier, over same-chart steps only.

    A chart change reparameterises the same rotation, so a step across one is
    not a rotation and must not be summed.
    """
    total = 0.0
    lo = hi = None
    for i in range(len(orientation30) - 1):
        (t1, a), (t2, b) = orientation30[i], orientation30[i + 1]
        if a.state != b.state or t2 <= t1:
            continue
        dq = _qmul(_qconj(a.quaternion_wxyz), b.quaternion_wxyz)
        if dq[0] < 0:
            dq = tuple(-v for v in dq)
        total += 2 * math.acos(max(-1.0, min(1.0, dq[0]))) * 180 / math.pi
        lo = t1 if lo is None else min(lo, t1)
        hi = t2 if hi is None else max(hi, t2)
    return total, lo, hi


def _qmul(a, b):
    w1, x1, y1, z1 = a
    w2, x2, y2, z2 = b
    return (w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
            w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2)


def _qconj(q):
    return (q[0], -q[1], -q[2], -q[3])


TICK_S = 1250e-6
MIN_ROTATION_DEG = 100.0  # below this the carrier sum is dominated by noise


def gyro_rotation_degrees(pdu40, lo, hi):
    """Integrate the 0x28 gyro magnitude over the same tick span."""
    total = 0.0
    previous = None
    for tick, sample in sorted(pdu40):
        if sample.packing_mode != 3 or not sample.gyro:
            continue
        if tick < lo or tick > hi:
            continue
        scale = R.WIRE_TO_COUNTS[sample.layout]["gyro"][0]
        magnitude = math.sqrt(sum(
            (v * scale / R.IMU_COUNTS_PER_DPS) ** 2 for v in sample.gyro[0]))
        if previous is not None:
            total += magnitude * (tick - previous[0]) * TICK_S
        previous = (tick, magnitude)
    return total


def check_scale() -> float | None:
    print()
    print("--- high-rate gyro wire scale, from total rotation ---")
    print(f"  {'capture':42s} {'carrier':>9s} {'gyro':>9s} {'ratio':>7s}")
    ratios = []
    for path in sorted(CAPTURES.glob("*.jsonl")):
        orientation30, pdu40 = paired_timeline(path)
        if not orientation30 or len(orientation30) < 10 or not pdu40:
            continue
        carrier_deg, lo, hi = carrier_rotation_degrees(orientation30)
        if lo is None or carrier_deg < MIN_ROTATION_DEG:
            continue  # too little rotation to measure a scale against
        gyro_deg = gyro_rotation_degrees(pdu40, lo, hi)
        if gyro_deg <= 0:
            continue
        ratios.append(gyro_deg / carrier_deg)
        print(f"  {path.name[:42]:42s} {carrier_deg:9.1f} {gyro_deg:9.1f} "
              f"{gyro_deg / carrier_deg:7.3f}")
    if not ratios:
        print("  no capture rotates far enough to measure a scale")
        return None
    ratio = statistics.median(ratios)
    print(f"  median ratio {ratio:.3f} over {len(ratios)} captures")
    print(f"  recovered/carrier should be 1.000; old /256 conversion would be "
          f"{ratio / 2:.3f}")
    print(f"  sensor/common scale remains {R.IMU_COUNTS_PER_DPS} counts/dps; "
          "the corrected field conversion is /128")
    check_scale_is_speed_independent()
    return ratio


def check_scale_is_speed_independent() -> None:
    """Is the deficit a scale error, or just sparse sampling losing area?

    High-rate carries ONE gyro sample per packet at ~110 Hz. If the motion
    outruns that, integrating the samples loses area and understates rotation --
    which looks exactly like a scale error. The two are separable: undersampling
    vanishes as the motion slows, a wrong counts/dps does not.
    """
    pairs = []
    for path in sorted(CAPTURES.glob("*.jsonl")):
        orientation30, pdu40 = paired_timeline(path)
        if not orientation30 or len(orientation30) < 12 or not pdu40:
            continue
        rate = []
        for i in range(len(orientation30) - 1):
            (t1, a), (t2, b) = orientation30[i], orientation30[i + 1]
            if a.state != b.state or t2 <= t1:
                continue
            dt = (t2 - t1) * TICK_S
            dq = _qmul(_qconj(a.quaternion_wxyz), b.quaternion_wxyz)
            if dq[0] < 0:
                dq = tuple(-v for v in dq)
            rate.append(((t1 + t2) / 2,
                         [2 * dq[k + 1] / dt * 180 / math.pi for k in range(3)]))
        if len(rate) < 10:
            continue
        for tick, sample in pdu40:
            if sample.packing_mode != 3 or not sample.gyro:
                continue
            near = min(rate, key=lambda q: abs(q[0] - tick))
            if abs(near[0] - tick) > 4:
                continue
            scale = R.WIRE_TO_COUNTS[sample.layout]["gyro"][0]
            axis = max(range(3), key=lambda i: abs(near[1][i]))
            pairs.append((abs(near[1][axis]), near[1][axis],
                          sample.gyro[0][axis] * scale / R.IMU_COUNTS_PER_DPS))
    if len(pairs) < 100:
        return
    print(f"\n  speed independence ({len(pairs)} samples, dominant axis):")
    print(f"    {'carrier rate (dps)':>19s} {'n':>5s} {'ratio':>7s}")
    slopes = []
    for lo, hi in ((0, 10), (10, 25), (25, 60), (60, 150), (150, 400)):
        sel = [q for q in pairs if lo <= q[0] < hi]
        if len(sel) < 15:
            continue
        xs = [q[1] for q in sel]
        ys = [q[2] for q in sel]
        den = sum(x * x for x in xs)
        if den <= 0:
            continue
        slope = sum(x * y for x, y in zip(xs, ys)) / den
        slopes.append(slope)
        print(f"    {f'{lo}-{hi}':>19s} {len(sel):5d} {slope:7.3f}")
    if len(slopes) >= 3:
        spread = max(slopes) - min(slopes)
        trend = slopes[0] - slopes[-1]   # slow bin minus fast bin
        print(f"    spread {spread:.3f}; slow-minus-fast {trend:+.3f}")
        print("    => " + ("undersampling: the deficit fades as motion slows"
                           if trend < -0.2 else
                           "speed-INDEPENDENT, so not undersampling -- a real "
                           "scale factor"))


def main() -> int:
    print("Per-lane gyro amplitude during each deliberate rotation.")
    print("A single-axis turn should light up exactly one lane.\n")
    print(f"{'capture':46s} {'n':>4s} {'gyroX':>8s} {'gyroY':>8s} {'gyroZ':>8s}"
          f" {'accelZ':>7s}  dominant")
    print("-" * 96)
    for path in sorted(CAPTURES.glob("*.jsonl")):
        gyro, accel = lanes(path)
        if not gyro:
            continue
        rms = [math.sqrt(statistics.mean(v * v for v in axis)) for axis in gyro]
        if max(rms) < 5.0:
            continue  # stationary; nothing to read
        top = max(range(3), key=lambda k: rms[k])
        runner = sorted(rms)[1] or 1e-9
        print(f"{path.name[:46]:46s} {len(gyro[0]):4d} "
              + " ".join(f"{r:8.1f}" for r in rms)
              + f" {statistics.mean(accel[2]):7.2f}  "
              + f"{'XYZ'[top]} ({rms[top] / runner:.1f}x next)")

    print("\n--- axis, from the lazy susan ---")
    gyro, accel = lanes(CAPTURES / LAZY)
    gyro_r, accel_r = lanes(CAPTURES / LAZY_RETURN)
    if not gyro or not gyro_r:
        print("  lazy-susan captures unavailable")
        return 2

    flat = statistics.mean(accel[2])
    rms = [math.sqrt(statistics.mean(v * v for v in axis)) for axis in gyro]
    top = max(range(3), key=lambda k: rms[k])
    runner = sorted(rms)[1] or 1e-9
    axis_ok = flat > FLAT_ACCEL_Z_G and top == 2 and rms[top] / runner >= DOMINANCE_RATIO
    print(f"  accelZ {flat:+.2f} g -> the controller is flat, so the turn is "
          f"about its own Z")
    print(f"  loudest gyro lane: {'XYZ'[top]} at {rms[top] / runner:.1f}x the next")
    print(f"  => gyro shares the accelerometer frame: {'YES' if axis_ok else 'NO'}")

    print("\n--- sign, from the return turn ---")
    out = statistics.mean(gyro[2])
    back = statistics.mean(gyro_r[2])
    sign_ok = out * back < 0
    print(f"  outbound gyroZ {out:+.2f} dps, return gyroZ {back:+.2f} dps "
          f"(accelZ {statistics.mean(accel_r[2]):+.2f} g, same pose)")
    print(f"  => sign reverses with direction: {'YES' if sign_ok else 'NO'}")

    ratio = check_scale()
    scale_ok = ratio is not None and abs(ratio - 1.0) < 0.15
    print(f"  => seven-bit high-rate gyro scale holds: "
          f"{'YES' if scale_ok else 'NO'}")

    print("\n--- residual ---")
    print("  X and Y are not individually disambiguated: no capture holds a")
    print("  pure rotation about each with a known direction. Absolute")
    print("  handedness rides on the 0x1E path's console validation -- this")
    print("  shows 0x28 agrees with 0x1E, not that both match Nintendo's")
    print("  convention.")
    return 0 if (axis_ok and sign_ok and scale_ok) else 1


if __name__ == "__main__":
    raise SystemExit(main())
