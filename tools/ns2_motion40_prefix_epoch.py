#!/usr/bin/env python3
"""WHEN does a length-0x28 orientation prefix describe? Measured, not assumed.

Why this exists
---------------
The 0x28 prefix is a modular slice of the same orientation carrier the length-
0x1E PDU sends outright. A generator therefore has to answer a question the
byte layout never asks: *which instant* should the sliced orientation describe?

Getting this wrong is invisible to every byte-exactness test. The packet is
well formed, every field is in range, the encoder round-trips -- and the
console still receives an orientation from the wrong moment. If it anchors on
the prefix and then integrates the packet's own IMU samples forward across the
window, an end-of-window prefix double-counts the window's rotation.

`ns2_ds5_motion40_build()` originally passed the CURRENT carrier, i.e. the
orientation at the packet's own tick. This tool exists because that turned out
to be wrong on hardware.

Method
------
Interleaved captures carry both PDUs from the same controller, so a 0x28's
prefix can be checked against a real 0x1E orientation interpolated to any
candidate instant. For each candidate the prefix is unwrapped with
`decode_motion40_prefix_orientation` against the preceding 0x1E state, and the
angular error against the interpolated truth is recorded. The candidate that
minimises that error is the epoch.

Two models are compared, because they are NOT the same thing once the window
length varies:

    A   coordinate = tick - elapsed + c    window-relative; the prefix sits a
                                           fixed distance after the window START
    B   coordinate = tick + c              a fixed lag behind the packet tick

Both are swept independently rather than assuming the 4.0 constant that was
previously hardcoded in ns2_motion_reference.

Run:
    python tools/ns2_motion40_prefix_epoch.py
    python tools/ns2_motion40_prefix_epoch.py --per-capture
"""

from __future__ import annotations

import argparse
import bisect
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_reference as R
from gen_motion40_fixture import captures

MIN_PACKETS = 8


def paired_timeline(path):
    """Both PDU streams from one capture on a common unwrapped tick axis.

    Returns (orientation30, pdu40) or (None, None). The tick axis is unwrapped
    across the 12-bit rollover using EVERY PDU, not just the 0x28s, so the two
    streams stay on the same timeline.
    """
    try:
        notifications, _ = R.read_motionpair_jsonl(path)
    except Exception:
        try:
            handles, _ = R.read_blecap_jsonl(path)
            notifications = max(handles.values(), key=len) if handles else []
        except Exception:
            return None, None

    pdus = []
    for notification in notifications:
        report = notification.value
        if len(report) <= 0x0E:
            continue
        length = report[0x0E]
        end = 0x0F + length
        if length in (0x1E, 0x28) and len(report) >= end:
            pdus.append(report[0x0F:end])

    timeline = []
    previous = None
    unwrapped = 0
    for pdu in pdus:
        tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
        unwrapped = tick if previous is None else unwrapped + ((tick - previous) & 0x0FFF)
        timeline.append((pdu, unwrapped))
        previous = tick

    orientation30 = [
        (float(tick), R.decode_motion30_orientation(pdu))
        for pdu, tick in timeline
        if len(pdu) == 0x1E
    ]

    items = [item for item in timeline if len(item[0]) == 0x28]
    decoded = []
    previous_tick = None
    for pdu, _tick in items:
        sample = R.decode_motion40(pdu, previous_tick)
        decoded.append(sample)
        previous_tick = sample.tick
    pdu40 = [
        (float(tick), sample)
        for (_pdu, tick), sample in zip(items, decoded)
        if sample.layout != "unknown"
    ]
    return orientation30, pdu40


def prefix_errors(pdu40, orientation30, coordinate_of):
    """Angular error of every decodable prefix against the 0x1E reference."""
    times = [item[0] for item in orientation30]
    errors = []
    for tick, sample in pdu40:
        coordinate = coordinate_of(tick, sample.elapsed_ticks)
        truth = R._interpolate_motion30_carrier(coordinate, orientation30)
        if truth is None:
            continue
        insertion = bisect.bisect_right(times, coordinate)
        if insertion <= 0:
            continue
        prior = orientation30[insertion - 1][1]
        # A chart change between the reference and the prefix would compare two
        # different parameterizations of the same rotation.
        if prior.state != truth[0]:
            continue
        decoded = R.decode_motion40_prefix_orientation(sample, prior)
        errors.append(R._q_delta_degrees(decoded.quaternion_wxyz, truth[2]))
    return errors


def sweep(data, coordinate_maker, lo_tenths, hi_tenths):
    curve = []
    for tenths in range(lo_tenths, hi_tenths + 1):
        constant = tenths / 10.0
        per_capture = []
        for _name, orientation30, pdu40 in data:
            errors = prefix_errors(pdu40, orientation30, coordinate_maker(constant))
            if errors:
                per_capture.append(statistics.median(errors))
        if per_capture:
            curve.append((constant, statistics.median(per_capture), len(per_capture)))
    return curve


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--per-capture", action="store_true",
                        help="also print the best offset for each capture")
    args = parser.parse_args()

    data = []
    for path in captures():
        orientation30, pdu40 = paired_timeline(path)
        if not orientation30 or not pdu40 or len(pdu40) < MIN_PACKETS:
            continue
        data.append((path.name, orientation30, pdu40))
    if not data:
        print("no interleaved captures (need both 0x1E and 0x28)", file=sys.stderr)
        return 2

    elapsed = [s.elapsed_ticks for _n, _o, p in data for _t, s in p]
    print(f"{len(data)} interleaved captures, {len(elapsed)} paired 0x28 packets")
    print(f"elapsed {min(elapsed)}..{max(elapsed)} ticks, "
          f"{len(set(elapsed))} distinct, median {statistics.median(elapsed):.0f}\n")

    model_a = sweep(data, lambda c: (lambda t, e: t - e + c), -20, 120)
    model_b = sweep(data, lambda c: (lambda t, e: t + c), -200, 60)
    best_a = min(model_a, key=lambda row: row[1])
    best_b = min(model_b, key=lambda row: row[1])

    print("MODEL A   coordinate = tick - elapsed + c   (prefix sits after the window START)")
    print(f"   best c = {best_a[0]:+.1f} ticks, pooled median error {best_a[1]:.5f} deg")
    print("MODEL B   coordinate = tick + c            (fixed lag behind the packet tick)")
    print(f"   best c = {best_b[0]:+.1f} ticks, pooled median error {best_b[1]:.5f} deg")

    ratio = max(best_a[1], best_b[1]) / min(best_a[1], best_b[1])
    print(f"\nseparation: {ratio:.2f}x")
    if ratio < 1.5:
        span = max(elapsed) - min(elapsed)
        print("  INDISTINGUISHABLE on this corpus. The two models only differ when the")
        print(f"  window length varies, and elapsed is {statistics.median(elapsed):.0f} in almost every")
        print(f"  paired packet (spread {span}). Catch-up packets, where elapsed is 15+,")
        print("  appear only in 0x28-ONLY captures, which carry no 0x1E to compare")
        print("  against. Resolving this needs a capture of INTERLEAVED traffic at a")
        print("  long notification interval.")
    else:
        winner = "A" if best_a[1] < best_b[1] else "B"
        print(f"  Model {winner} wins.")

    print("\nWhat both models agree on, which is what the firmware needs:")
    for label, best, coord in (("A", best_a, lambda e: -e + best_a[0]),
                               ("B", best_b, lambda e: best_b[0])):
        for window in (8, 16):
            lag = -coord(window)
            print(f"   model {label}, elapsed {window:2d}: prefix lags the packet tick by "
                  f"{lag:5.1f} ticks ({lag * 1.25:5.2f} ms)")
    print("   In no model is the correct answer 0. Passing the CURRENT carrier")
    print("   makes the prefix describe the wrong instant by at least ~3 ticks.")

    if args.per_capture:
        print(f"\n{'capture':46s} {'best A':>8s} {'err':>10s} {'best B':>8s} {'err':>10s}")
        for name, orientation30, pdu40 in data:
            a = min(((c, statistics.median(prefix_errors(pdu40, orientation30,
                                                         lambda t, e, c=c: t - e + c)))
                     for c in [i / 10 for i in range(-20, 121)]
                     if prefix_errors(pdu40, orientation30,
                                      lambda t, e, c=c: t - e + c)),
                    key=lambda kv: kv[1])
            b = min(((c, statistics.median(prefix_errors(pdu40, orientation30,
                                                         lambda t, e, c=c: t + c)))
                     for c in [i / 10 for i in range(-200, 61)]
                     if prefix_errors(pdu40, orientation30,
                                      lambda t, e, c=c: t + c)),
                    key=lambda kv: kv[1])
            print(f"{name[:46]:46s} {a[0]:8.1f} {a[1]:10.5f} {b[0]:8.1f} {b[1]:10.5f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
