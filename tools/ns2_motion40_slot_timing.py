#!/usr/bin/env python3
"""Where in the emit window do a genuine catch-up packet's IMU slots sit?

Why this exists
---------------
``ns2_motion_pdu40_build_catchup`` is byte-exact against 981 genuine packets,
which proves the bit layout is a correct bijection. It does NOT prove that the
values we put in those slots describe the right physical timeline. A translator
that fills the slots from the first three samples to arrive produces packets
that are perfectly well-formed and still wrong: they cover only the head of the
window and discard the freshest data.

This measures which end of the window each slot belongs to, and it is the
evidence behind the slot-placement policy in ``ns2_ds5_motion40.c``. Rerun it
after any capture-corpus change.

Method
------
The corpus is stationary, so raw difference magnitudes are noise-dominated and
carry almost no information -- an earlier reading of this data drew the right
conclusion from a comparison that had no statistical power. Two things do work:

1. A paired sign test on the same packets, which cancels the per-capture noise
   floor and assumes nothing about scale.
2. Mean-square differences expressed against the value the accelerometer
   structure function saturates at, i.e. one full window. Every gap shorter
   than a window must land below 1.000; anything above it means the statistic
   is contaminated.

Both must be computed WITHIN capture. Pooling across captures mixes resting
orientations and inflates the within-packet gap past the asymptote, which looks
like a per-axis scale error and is not one.

What this can and cannot resolve
--------------------------------
It can ORDER the gaps. It cannot MEASURE them: the structure function saturates
before one window elapses, so the map from mean-square difference back to
elapsed time is compressive. Gyro sits at the noise floor throughout and is
reported for completeness, not as a result.

Run:
    python tools/ns2_motion40_slot_timing.py
"""

from __future__ import annotations

import math
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_reference as R
from gen_motion40_fixture import captures, pdus

ACCEL_SCALE = R.WIRE_TO_COUNTS["catchup"]["accel"]
GYRO_SCALE = R.WIRE_TO_COUNTS["catchup"]["gyro"]

# Slot 1 is 13 bits at x2, so its ordinary-count value is forced even. Uniform
# quantization on a step of 2 adds (2^2 - 1)/12 per axis, 0.75 over three axes,
# to any comparison involving it.
SLOT1_QUANTIZATION = 0.75

MIN_PACKETS_PER_CAPTURE = 20


def d2(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b))


def catchup_runs():
    """Per capture, the catch-up packets and their tick-contiguous pairs.

    Tick continuity -- ``tick[N+1] - tick[N] == elapsed[N+1]`` -- is what proves
    no packet was dropped between two records. Without it a "seam" measurement
    silently spans a gap.
    """
    for path in captures():
        decoded = [R.decode_motion40(pdu, None) for pdu in pdus(path)]
        decoded = [s for s in decoded if s.layout == "catchup"]
        if len(decoded) < MIN_PACKETS_PER_CAPTURE:
            continue
        accel = [
            [tuple(v * ACCEL_SCALE[i] for v in s.accel[i]) for i in range(3)]
            for s in decoded
        ]
        gyro = [
            [tuple(v * GYRO_SCALE[i] for v in s.gyro[i]) for i in range(2)]
            for s in decoded
        ]
        pairs = [
            (n, n + 1)
            for n in range(len(decoded) - 1)
            if (decoded[n + 1].tick - decoded[n].tick) & 0x0FFF
            == decoded[n + 1].elapsed_ticks
        ]
        yield path.name, accel, gyro, pairs


def sign_test(samples) -> tuple[int, int, float]:
    """Fraction of pairs where the second value is smaller, plus a z score."""
    wins = sum(1 for x, y in samples if y < x)
    losses = sum(1 for x, y in samples if y > x)
    n = wins + losses
    z = (wins - n / 2) / math.sqrt(n / 4) if n else 0.0
    return wins, n, z


def main() -> int:
    totals = {k: [0.0, 0] for k in ("seam", "a0->a1", "a1->a2", "a0->a2", "full")}
    accel_pairs: list[tuple[float, float]] = []
    gyro_pairs: list[tuple[float, float]] = []
    captures_used = 0

    for _name, accel, gyro, pairs in catchup_runs():
        captures_used += 1

        def add(key, values):
            for value in values:
                totals[key][0] += value
                totals[key][1] += 1

        add("a0->a1", [d2(r[0], r[1]) - SLOT1_QUANTIZATION for r in accel])
        add("a1->a2", [d2(r[1], r[2]) - SLOT1_QUANTIZATION for r in accel])
        add("a0->a2", [d2(r[0], r[2]) for r in accel])
        add("seam", [d2(accel[i][2], accel[j][0]) for i, j in pairs])
        add("full", [d2(accel[i][0], accel[j][0]) for i, j in pairs])

        accel_pairs += [
            (d2(accel[i][0], accel[i][2]), d2(accel[i][2], accel[j][0]))
            for i, j in pairs
        ]
        gyro_pairs += [
            (d2(gyro[i][0], gyro[i][1]), d2(gyro[i][1], gyro[j][0]))
            for i, j in pairs
        ]

    if not captures_used:
        print("no catch-up captures found", file=sys.stderr)
        return 2

    asymptote = totals["full"][0] / totals["full"][1]
    print(f"{captures_used} captures, {totals['a0->a2'][1]} catch-up packets, "
          f"{totals['seam'][1]} tick-contiguous pairs\n")
    print("within-capture mean |delta|^2, quantization-corrected:\n")
    print(f"{'pair':22s} {'mean|d|^2':>10s} {'/asymptote':>11s} {'n':>7s}")
    order = ["seam", "a0->a1", "a1->a2", "a0->a2", "full"]
    label = {
        "seam": "seam a2[N]->a0[N+1]",
        "full": "one whole window",
    }
    for key in order:
        mean = totals[key][0] / totals[key][1]
        print(f"{label.get(key, key):22s} {mean:10.2f} "
              f"{mean / asymptote:11.3f} {totals[key][1]:7d}")

    ok = all(totals[k][0] / totals[k][1] <= asymptote for k in order)
    print(f"\nevery gap below the saturated asymptote: {'yes' if ok else 'NO'}")
    if not ok:
        print("  A gap above 1.000 is impossible for a saturating structure")
        print("  function. Check that the statistic is computed within capture.")

    wins, n, z = sign_test(accel_pairs)
    print(f"\npaired sign test, accel: seam < within-packet a0->a2 in "
          f"{wins}/{n} pairs ({100 * wins / n:.1f}%), z = {z:+.1f}")
    wins, n, z = sign_test(gyro_pairs)
    print(f"paired sign test, gyro : seam < within-packet g0->g1  in "
          f"{wins}/{n} pairs ({100 * wins / n:.1f}%), z = {z:+.1f}")

    print("""
Reading this
------------
Slot 0 is the OLDEST sample in the window and the last slot the NEWEST. The
decisive fact is the seam: if a packet held three consecutive samples taken at
the start of its window, the step from its last slot to the next packet's first
slot would be the LARGEST gap in the stream, not the smallest.

The gyro line is near the noise floor -- a stationary gyro has no signal -- so
its sign is not evidence for a placement. Its weak opposite sign is consistent
with quarter-point spacing, which makes the within-packet and seam gaps equal.

These ratios do NOT convert to times. The structure function saturates before
one window elapses, so the mapping is compressive; the gaps can be ordered but
not measured.""")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
