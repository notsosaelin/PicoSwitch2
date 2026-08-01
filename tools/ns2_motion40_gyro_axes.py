#!/usr/bin/env python3
"""Do the 0x28 gyro lanes share the accelerometer's axis order and sign?

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

Closing those needs one capture: the controller turned deliberately about each
body axis in turn, both directions, with 0x1E and 0x28 subscribed. No console
and no flash.

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

    print("\n--- residual ---")
    print("  X and Y are not individually disambiguated: no capture holds a")
    print("  pure rotation about each with a known direction. Absolute")
    print("  handedness rides on the 0x1E path's console validation -- this")
    print("  shows 0x28 agrees with 0x1E, not that both match Nintendo's")
    print("  convention.")
    return 0 if (axis_ok and sign_ok) else 1


if __name__ == "__main__":
    raise SystemExit(main())
