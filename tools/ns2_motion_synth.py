#!/usr/bin/env python3
"""Synthesize a genuine-shaped motion stream from an IMU source, and compare it
structurally against a genuine Pro Controller 2 stream.

Purpose
-------
Before any hardware trial, answer offline: does a stream we would generate look
like a stream a genuine controller produces? Byte equality is impossible -- the
DualSense and the Pro Controller 2 are different physical sensors with different
mounting and bias -- but *structure* is comparable, and a structural mismatch is
a defect we can find without a flash.

The paired captures under ``dumps/motion/`` carry both sides at the same instant:
each record has the genuine ``native`` PDU and the DualSense ``cal_g``/``cal_a``
that produced motion at that moment. That makes this an input-matched
comparison, not two unrelated sessions.

What is compared
----------------
* layout mix (high-rate / normal / catch-up) and status bytes
* chart-state distribution and swap rate -- the hysteresis is the part most
  likely to be wrong, and it is directly observable
* carrier lane occupancy across each lane's field
* packed IMU magnitude distributions

Why this matters
----------------
The previous length-`0x28` generator failed on hardware because it held the
other changing lanes static while varying G6/G7/G8 to inject a synthetic
magnetometer. Those bit ranges cross real packed gyro and acceleration samples,
so it was corrupting genuine IMU data; random motion was the correct outcome.
That failure mode is structurally impossible here -- every lane is generated
from the decoded field map -- but "impossible by construction" is a claim, and
this tool is how it gets checked.

Nothing here is wired into firmware.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ns2_motion_carrier as C
import ns2_motion_packet as K
import ns2_motion_reference as R

# 800 Hz internal tick.
TICK_HZ = 800.0
TICK_US = 1_000_000.0 / TICK_HZ


def integrate_gyro(quaternion: Sequence[float], rate_rad: Sequence[float],
                   dt_s: float) -> tuple[float, float, float, float]:
    """Advance an orientation by one gyro sample.

    Uses the fourth-order Maclaurin series for sin(t/2)/t and cos(t/2) rather
    than calling trig, which is what Nintendo's own Switch 1 packer does. The
    result is renormalized every step; the genuine controller apparently does
    not, which is the leading explanation for its carrier norm exceeding 1, but
    a generator has no reason to reproduce that drift.
    """
    ax, ay, az = (component * dt_s for component in rate_rad)
    squared = ax * ax + ay * ay + az * az
    vector_scale = squared * squared / 3840.0 - squared / 48.0 + 0.5
    scalar = squared * squared / 384.0 - squared / 8.0 + 1.0
    dw, dx, dy, dz = scalar, ax * vector_scale, ay * vector_scale, az * vector_scale
    w, x, y, z = quaternion
    out = (
        w * dw - x * dx - y * dy - z * dz,
        w * dx + x * dw + y * dz - z * dy,
        w * dy - x * dz + y * dw + z * dx,
        w * dz + x * dy - y * dx + z * dw,
    )
    norm = math.sqrt(sum(v * v for v in out))
    if norm == 0.0:
        return (1.0, 0.0, 0.0, 0.0)
    return tuple(v / norm for v in out)  # type: ignore[return-value]


@dataclass
class StreamStats:
    layouts: Counter = field(default_factory=Counter)
    statuses: Counter = field(default_factory=Counter)
    states: Counter = field(default_factory=Counter)
    swaps: int = 0
    carriers: int = 0
    lane_fraction: list = field(default_factory=lambda: [[], [], []])
    accel_norm: list = field(default_factory=list)
    gyro_norm: list = field(default_factory=list)

    def note_carrier(self, state: int, raw: Sequence[int],
                     previous_state: int | None) -> None:
        self.carriers += 1
        self.states[state] += 1
        if previous_state is not None and previous_state != state:
            self.swaps += 1
        for lane in range(3):
            self.lane_fraction[lane].append(
                raw[lane] / float(1 << C.CARRIER_BITS[lane]))

    def note_packet(self, sample) -> None:
        """Record one decoded packet, in physical units.

        Magnitudes MUST be normalized before pooling. Raw wire values differ by
        up to 256x between layouts, so pooling them produces a distribution
        that describes the layout mix rather than the motion -- which is
        exactly the artifact that made the first run of this tool report a
        genuine acceleration median of 1,101,581.
        """
        self.layouts[sample.layout] += 1
        self.statuses[sample.sensor_status] += 1
        for vector in R.normalized_vectors(sample, "accel"):
            self.accel_norm.append(
                math.sqrt(sum(v * v for v in vector)) / R.IMU_COUNTS_PER_G)
        for vector in R.normalized_vectors(sample, "gyro"):
            self.gyro_norm.append(
                math.sqrt(sum(v * v for v in vector)) / R.IMU_COUNTS_PER_DPS)

    def summary(self) -> dict:
        def five(values):
            if not values:
                return None
            ordered = sorted(values)
            n = len(ordered)
            return {
                "n": n,
                "p10": round(ordered[n // 10], 4),
                "median": round(ordered[n // 2], 4),
                "p90": round(ordered[9 * n // 10], 4),
            }
        return {
            "layouts": dict(self.layouts),
            "statuses": {f"0x{k:02X}": v for k, v in sorted(self.statuses.items())},
            "chart_states": dict(sorted(self.states.items())),
            "carriers": self.carriers,
            "chart_swaps": self.swaps,
            "swap_rate_per_100": round(100.0 * self.swaps / self.carriers, 3)
            if self.carriers else None,
            "lane0_fraction": five(self.lane_fraction[0]),
            "lane1_fraction": five(self.lane_fraction[1]),
            "lane2_fraction": five(self.lane_fraction[2]),
            "accel_norm": five(self.accel_norm),
            "gyro_norm": five(self.gyro_norm),
        }


def read_paired(path: Path) -> list[dict]:
    records = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if item.get("motionpair") == "record":
                records.append(item)
    return records


def genuine_stats(records: Sequence[dict]) -> StreamStats:
    stats = StreamStats()
    previous_state = None
    for item in records:
        native = item.get("native")
        if not native:
            continue
        pdu = bytes.fromhex(native)
        if len(pdu) == 0x1E:
            orientation = R.decode_motion30_orientation(pdu)
            stats.note_carrier(orientation.state, orientation.carrier_raw,
                               previous_state)
            previous_state = orientation.state
        elif len(pdu) == 0x28:
            sample = R.decode_motion40(pdu, None)
            if sample.layout != "unknown":
                stats.note_packet(sample)
    return stats


def genuine_reference_stats(paths: Sequence[Path]) -> StreamStats:
    """Statistics for genuine captures read directly, not via paired records.

    Pointed at the 0x28-only interval sweeps, this preserves the historical
    layout/scale comparison. It does not model the current interleaved firmware
    scheduler; `test_ns2_motion40_coherence.py` owns that sequence-level gate.
    """
    stats = StreamStats()
    previous_state = None
    for path in paths:
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
            if len(report) < end:
                continue
            pdu = report[0x0F:end]
            if length == 0x1E:
                orientation = R.decode_motion30_orientation(pdu)
                stats.note_carrier(orientation.state, orientation.carrier_raw,
                                   previous_state)
                previous_state = orientation.state
            elif length == 0x28:
                sample = R.decode_motion40(pdu, None)
                if sample.layout != "unknown":
                    stats.note_packet(sample)
    return stats


def synth_stats(records: Sequence[dict], gyro_scale: float,
                accel_scale: float, interval_ticks: int | None = None) -> StreamStats:
    """Drive the generator from the DualSense side of the same records.

    Orientation is integrated from gyro alone, starting at identity. That is
    not an oversight and not a shortcoming to be corrected: it is exactly what
    the shipping, console-validated length-0x1E path does
    (``ns2_ds5_motion.c``). The genuine controller carries true gravity-
    referenced attitude and ours does not, so **absolute lane occupancy is
    expected to differ** and is reported for information, not as a pass
    criterion. The console demonstrably accepts a relative orientation.

    What must match is structure: layout selection driven by real elapsed time,
    IMU magnitudes in physical units, and lanes that never leave the field.

    KNOWN HARNESS LIMITATION -- gyro bias. This does not model the firmware's
    zero-rate bias estimator, so its stationary gyro magnitude reads high: 0.90
    dps against genuine hardware's 0.15 dps on a matched stationary capture.
    The capture's ``cal_g`` is calibrated but not de-biased (it equals
    ``raw_g``). That gap is a property of this harness, not of the firmware --
    ``ns2_ds5_motion40`` is fed ``gyro_corrected``, the same de-biased sample
    the hardware-validated 0x1E path integrates. Feeding raw gyro instead was a
    real defect that this comparison caught, and the number is left visible
    here rather than papered over so the limitation stays legible.
    """
    stats = StreamStats()
    quaternion = (1.0, 0.0, 0.0, 0.0)
    state: int | None = None
    previous_state = None
    previous_us = None
    tick = 0
    pending_accel: list = []
    pending_gyro: list = []
    pending_ticks = 0
    for item in records:
        if not item.get("ds5_valid"):
            continue
        gyro = item.get("cal_g") or item.get("raw_g")
        accel = item.get("cal_a") or item.get("raw_a")
        if not gyro or not accel:
            continue
        now_us = item.get("ds5_t_us") or item.get("t_us")
        dt_s = 0.0 if previous_us is None else max(
            0.0, min(0.016, (now_us - previous_us) / 1_000_000.0))
        previous_us = now_us
        # DualSense gyro is 16.384 counts/dps against the Pro 2's 16.4 -- the
        # same unit for practical purposes, so counts pass through unscaled.
        rate_rad = tuple(math.radians(v / 16.384 * gyro_scale) for v in gyro)
        quaternion = integrate_gyro(quaternion, rate_rad, dt_s)

        sample = C.encode_carrier(quaternion, state)
        stats.note_carrier(sample.state, sample.raw, previous_state)
        previous_state = sample.state
        state = sample.state

        # Samples are held in ordinary ICM counts and converted to wire form
        # per layout only at pack time -- the layout is not known until the
        # elapsed count is, and the scale depends on it.
        pending_accel.append(tuple(v * accel_scale for v in accel))
        pending_gyro.append(tuple(float(v) for v in gyro))
        pending_ticks += max(1, int(round(dt_s * TICK_HZ)))

        # Firmware policy: emit one catch-up packet every fixed interval. The
        # module refuses to emit a partially filled packet rather than
        # inventing samples, so a starved interval simply waits.
        if interval_ticks is not None:
            if pending_ticks < interval_ticks:
                continue
            emit_ticks = pending_ticks
        else:
            emit_ticks = pending_ticks
        layout = K.layout_for_elapsed(emit_ticks)
        want_accel, want_gyro = K.LAYOUT_SAMPLE_COUNTS[layout]
        if len(pending_accel) < want_accel or len(pending_gyro) < want_gyro:
            continue
        pending_ticks = emit_ticks
        tick = (tick + pending_ticks) & 0xFFF
        pdu = K.build_motion40(K.MotionPacketFields(
            tick=tick,
            elapsed_ticks=pending_ticks,
            carrier=C.encode_prefix(sample.raw, layout == "high_rate"),
            accel=tuple(
                K.counts_to_wire(layout, "accel", slot, vector)
                for slot, vector in enumerate(pending_accel[:want_accel])),
            gyro=tuple(
                K.counts_to_wire(layout, "gyro", slot, vector)
                for slot, vector in enumerate(pending_gyro[:want_gyro])),
        ))
        stats.note_packet(R.decode_motion40(pdu, None))
        pending_accel = pending_accel[want_accel:]
        pending_gyro = pending_gyro[want_gyro:]
        pending_ticks = 0
    return stats


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paired", type=Path, nargs="+",
                        help="paired motion capture JSONL")
    parser.add_argument("--genuine", type=Path, nargs="*", default=None,
                        help="compare against these genuine captures instead "
                             "of the native side of the paired ones. Point "
                             "this at the 0x28-only interval sweeps for the "
                             "historical layout/scale comparison.")
    parser.add_argument("--interval-ticks", type=int, default=None,
                        help="emit one catch-up packet every N 800 Hz ticks, "
                             "modelling the firmware policy (16 = 20 ms). "
                             "Default follows the capture's own spacing.")
    parser.add_argument("--gyro-scale", type=float, default=1.0,
                        help="extra gain on DualSense gyro (1.0 = same unit)")
    parser.add_argument("--accel-scale", type=float, default=0.5,
                        help="Pro 2 counts per DualSense accel count "
                             "(DualSense 8192/g into Pro 2 4096/g = 0.5)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    records: list[dict] = []
    for path in args.paired:
        records.extend(read_paired(path))
    if not records:
        print("no paired records found", file=sys.stderr)
        return 2

    if args.genuine:
        genuine = genuine_reference_stats(args.genuine).summary()
    else:
        genuine = genuine_stats(records).summary()
    synthetic = synth_stats(records, args.gyro_scale, args.accel_scale,
                            args.interval_ticks).summary()
    report = {"records": len(records), "genuine": genuine, "synthetic": synthetic}
    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    print(f"paired records: {len(records)}\n")
    width = 30
    for heading, keys in _REPORT_SECTIONS:
        print(f"{heading}")
        for key in keys:
            print(f"  {key:<{width}} genuine   {genuine.get(key)}")
            print(f"  {'':<{width}} synthetic {synthetic.get(key)}")
        print()
    print("Acceleration must read one gravity on both sides; a synthetic median\n"
          "near 0.004 g or 258 g is the layout scale conversion being skipped.")
    return 0


# Not every difference is a defect, and treating them alike is how this tool
# gets misread. Only the first group is a pass criterion.
_REPORT_SECTIONS = (
    ("PHYSICAL -- must agree; disagreement is a real defect:",
     ("accel_norm", "gyro_norm")),
    ("REPRESENTATIONAL -- must be well-formed, need not match:\n"
     "  Chart choice is lossless, and our orientation is gyro-integrated from\n"
     "  identity exactly as the shipping 0x1E path is, so absolute lane\n"
     "  occupancy differs from a gravity-referenced controller by design.",
     ("chart_states", "chart_swaps", "swap_rate_per_100", "carriers",
      "lane0_fraction", "lane1_fraction", "lane2_fraction")),
    ("CADENCE -- informational only in this harness:\n"
     "  Layout selection follows the elapsed count, which here comes from the\n"
     "  capture's record spacing rather than the adapter's emit timing. A high\n"
     "  synthetic catch-up share reflects the capture, not the generator.",
     ("layouts", "statuses")),
)


if __name__ == "__main__":
    raise SystemExit(main())
