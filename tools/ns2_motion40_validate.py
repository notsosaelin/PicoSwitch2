#!/usr/bin/env python3
"""Readiness gate for length-0x28 generation. Run this before asking for a flash.

Why this exists
---------------
Three "offline-validated" claims preceded a hardware failure. Every test built
for that feature compared one of our implementations against another -- encoder
against decoder, C against Python -- and a consistency test cannot detect a
wrong semantic choice, because both sides share the assumption being tested.

This tool only asks questions whose answer comes from genuine captures, and it
prints what it CANNOT answer as loudly as what it can.

The ceiling, established 2026-07-31
-----------------------------------
**Byte-exact validation of a generated 0x28 is impossible from BLE captures.**
Not difficult -- impossible, because the controller transmits derived products
and not the inputs that produced them:

* the internal 800 Hz IMU stream is never sent. Handle 0x000A carries a raw
  reference, but at the BLE notification rate (~133 Hz, 6-tick spacing), so the
  samples that actually filled a genuine packet's slots are not in any capture;
* the orientation carrier at the prefix's epoch instant is never sent either.
  Across 449 genuine 0x28 packets the epoch coordinate landed on a transmitted
  0x1E exactly **0** times.

So a generator can never be proven to reproduce a genuine packet bit for bit.
The achievable bar is physical accuracy against interpolated ground truth, with
an explicit tolerance per field -- which is what caught the epoch defect
(0.00023 deg at the right epoch against 0.9 deg at the wrong one) when
byte-exactness had passed 981/981 while the field was semantically wrong.

Anything this tool reports as UNKNOWN is a reason not to flash.

Run:
    python tools/ns2_motion40_validate.py
"""

from __future__ import annotations

import bisect
import statistics
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_reference as R
from gen_motion40_fixture import captures
from ns2_motion40_prefix_epoch import paired_timeline, prefix_errors

# What instant the FIRMWARE's prefix describes, as a coordinate on the tick
# axis. This must track ns2_ds5_motion40_build(); the gate is measuring the
# firmware, not the corpus.
#
#   current behaviour: passes the latest carrier, so the prefix describes the
#   packet's own tick -- `lambda tick, elapsed: tick`.
#   correct behaviour: `lambda tick, elapsed: tick - elapsed + 4.0`
FIRMWARE_EPOCH = lambda tick, elapsed: tick  # noqa: E731

# How much worse than the achievable floor the firmware's epoch may be. A ratio
# rather than an absolute angle, because the floor is set by interpolating a
# ~133 Hz carrier and is itself 0.056 deg during fast rotation.
EPOCH_RATIO_TOLERANCE = 2.0

# Accel slots must track the raw IMU reference to within sensor noise, measured
# at ~2.0 counts/axis on the stationary corpus.
ACCEL_TOLERANCE_COUNTS = 8.0

LAYOUT_BANDS = {"high_rate": (0, 10), "normal": (11, 14), "catchup": (15, 4095)}


class Gate:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []

    def record(self, name: str, verdict: str, detail: str) -> None:
        self.rows.append((name, verdict, detail))

    def report(self) -> int:
        width = max(len(row[0]) for row in self.rows)
        print(f"\n{'check':{width}s}  {'verdict':8s}  detail")
        print("-" * (width + 60))
        for name, verdict, detail in self.rows:
            print(f"{name:{width}s}  {verdict:8s}  {detail}")
        failed = [r for r in self.rows if r[1] == "FAIL"]
        unknown = [r for r in self.rows if r[1] == "UNKNOWN"]
        print()
        if failed:
            print(f"{len(failed)} FAILED. Do not flash.")
        if unknown:
            print(f"{len(unknown)} UNKNOWN. Each is a reason not to flash:")
            for name, _v, detail in unknown:
                print(f"   - {name}: {detail}")
        if not failed and not unknown:
            print("All checks pass. A flash is justified -- state the predicted")
            print("console behaviour before running it, so the test can falsify it.")
        return 1 if (failed or unknown) else 0


def check_layout_bands(gate: Gate) -> None:
    """Layout must follow the elapsed count, with no overlap between bands."""
    seen: dict[str, list[int]] = {}
    for path in captures():
        try:
            notifications, _ = R.read_motionpair_jsonl(path)
        except Exception:
            try:
                handles, _ = R.read_blecap_jsonl(path)
                notifications = max(handles.values(), key=len) if handles else []
            except Exception:
                continue
        for notification in notifications:
            report = notification.value
            if len(report) <= 0x0E:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length != 0x28 or len(report) < end:
                continue
            sample = R.decode_motion40(report[0x0F:end], None)
            seen.setdefault(sample.layout, []).append(sample.elapsed_ticks)
    bad = []
    for layout, values in seen.items():
        lo, hi = LAYOUT_BANDS.get(layout, (None, None))
        if lo is None or min(values) < lo or max(values) > hi:
            bad.append(f"{layout} {min(values)}..{max(values)}")
    total = sum(len(v) for v in seen.values())
    gate.record(
        "layout bands",
        "FAIL" if bad else "PASS",
        "; ".join(bad) if bad else
        f"{total} packets, every layout inside its elapsed band",
    )


def check_constants(gate: Gate) -> None:
    """Fields a generator emits as constants must actually be constant."""
    modes, tails, statuses = Counter(), Counter(), Counter()
    for path in captures():
        try:
            notifications, _ = R.read_motionpair_jsonl(path)
        except Exception:
            try:
                handles, _ = R.read_blecap_jsonl(path)
                notifications = max(handles.values(), key=len) if handles else []
            except Exception:
                continue
        for notification in notifications:
            report = notification.value
            if len(report) <= 0x0E:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length != 0x28 or len(report) < end:
                continue
            pdu = report[0x0F:end]
            sample = R.decode_motion40(pdu, None)
            if sample.layout != "catchup":
                continue
            modes[sample.packing_mode] += 1
            tails[sample.tail_value] += 1
            statuses[pdu[3]] += 1
    for name, counter, expected in (
        ("packing_mode", modes, 3),
        ("catch-up tail bit", tails, 0),
        ("catch-up status", statuses, 0x0F),
    ):
        if not counter:
            gate.record(name, "UNKNOWN", "no catch-up packets in the corpus")
        elif set(counter) == {expected}:
            gate.record(name, "PASS", f"{expected} in all {sum(counter.values())}")
        else:
            gate.record(name, "FAIL", f"not constant: {dict(counter)}")


def check_orientation_epoch(gate: Gate) -> None:
    """The prefix must describe the instant the console expects."""
    data = []
    for path in captures():
        orientation30, pdu40 = paired_timeline(path)
        if orientation30 and pdu40 and len(pdu40) >= 8:
            data.append((orientation30, pdu40))
    if not data:
        gate.record("prefix epoch", "UNKNOWN", "no interleaved captures")
        return

    # WORST capture, not the pooled median. A wrong epoch only misplaces
    # orientation while the controller is ROTATING, and most of the corpus is
    # stationary -- so a median washes the defect out entirely. Scored by
    # median this check reports 0.0006 deg for the epoch that failed on
    # hardware, i.e. it would have passed the broken firmware. Scored by worst
    # capture it reports 0.9 deg and fails it. Aggregate away the only
    # condition that exposes a defect and the check stops being a check.
    def worst(coordinate_of):
        per = [statistics.median(e) for o, p in data
               if (e := prefix_errors(p, o, coordinate_of))]
        return max(per) if per else float("nan")

    # Score against the achievable FLOOR, not an absolute number. On a fast
    # rotation the floor is set by interpolating a ~133 Hz carrier, which is
    # 0.056 deg on the worst capture -- a measurement limit, not a defect. An
    # absolute tolerance below the floor fails everything including a correct
    # generator; one above it passes the wrong epoch. The ratio is what matters.
    floor_constant, floor = min(
        ((c / 10.0, worst(lambda t, e, c=c: t - e + c / 10.0))
         for c in range(0, 81, 5)), key=lambda kv: kv[1])
    firmware = worst(FIRMWARE_EPOCH)
    ratio = firmware / max(floor, 1e-9)
    gate.record(
        "prefix epoch",
        "PASS" if ratio <= EPOCH_RATIO_TOLERANCE else "FAIL",
        f"firmware {firmware:.5f} deg vs floor {floor:.5f} deg at "
        f"tick-elapsed+{floor_constant:.1f} = {ratio:.1f}x "
        f"(tolerance {EPOCH_RATIO_TOLERANCE:.0f}x, worst capture)",
    )
    gate.record(
        "epoch model resolved",
        "UNKNOWN",
        "fixed lag vs window-relative indistinguishable: elapsed is ~7 in "
        "almost every paired packet; they differ by 9 ticks at elapsed 16",
    )


def check_accel_against_raw(gate: Gate) -> None:
    """Slot values must match the controller's own raw IMU reference."""
    diffs: list[int] = []
    used = 0
    for path in sorted(Path("dumps/BLE CAPTURE").glob("*.jsonl")):
        try:
            handles, _ = R.read_blecap_jsonl(path)
        except Exception:
            continue
        if 0x000A not in handles or 0x000E not in handles:
            continue
        raw = [(R.decode_report05_raw_imu(n.value), n.time_seconds)
               for n in handles[0x000A]]
        raw = [(s, t) for s, t in raw if any(s.accel)]
        if not raw:
            continue
        for notification in handles[0x000E]:
            report = notification.value
            if len(report) <= 0x0E:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length != 0x28 or len(report) < end:
                continue
            sample = R.decode_motion40(report[0x0F:end], None)
            if sample.layout == "unknown":
                continue
            scale = R.WIRE_TO_COUNTS[sample.layout]["accel"]
            nearest = min(raw, key=lambda rt: abs(rt[1] - notification.time_seconds))
            slot0 = [int(round(v * scale[0])) for v in sample.accel[0]]
            diffs += [slot0[k] - nearest[0].accel[k] for k in range(3)]
            used += 1
    if not diffs:
        gate.record("accel vs raw IMU", "UNKNOWN", "no capture holds both streams")
        return
    worst = max(abs(d) for d in diffs)
    gate.record(
        "accel vs raw IMU",
        "PASS" if worst <= ACCEL_TOLERANCE_COUNTS else "FAIL",
        f"{used} packets, max |diff| {worst} counts "
        f"(tolerance {ACCEL_TOLERANCE_COUNTS:.0f}); axis order, sign and scale",
    )


def check_unverifiable(gate: Gate) -> None:
    """Questions no capture can answer. Listing them is the point."""
    gate.record("gyro axis/sign", "UNKNOWN",
                "needs a MOVING raw+native capture; every existing one is "
                "stationary, where gyro sits at the noise floor")
    gate.record("chart bootstrap", "UNKNOWN",
                "17 captures carry 0x28 with zero 0x1E, so the console takes "
                "chart state from somewhere unidentified; our decoder cannot "
                "decode those captures at all")
    gate.record("repeat tolerance", "UNKNOWN",
                "a 1 kHz poll sends each 0x28 ~20x; genuine never repeats one. "
                "Whether the console deduplicates by tick is untested")
    gate.record("byte-exact replay", "N/A",
                "impossible in principle: the 800 Hz source samples and the "
                "epoch-instant carrier are never transmitted")


def main() -> int:
    gate = Gate()
    check_layout_bands(gate)
    check_constants(gate)
    check_accel_against_raw(gate)
    check_orientation_epoch(gate)
    check_unverifiable(gate)
    return gate.report()


if __name__ == "__main__":
    raise SystemExit(main())
