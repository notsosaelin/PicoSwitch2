#!/usr/bin/env python3
"""Validate that generated 0x1E/0x28 motion describes one physical trajectory.

This is the sequence-level gate missing from the byte-exact packer tests.  It
compiles and runs the real C DualSense translators against a deterministic,
physically consistent 800 Hz IMU source, then independently checks the wire
stream in Python:

* one shared PDU tick/elapsed timeline;
* every 0x1E carrier against the analytic orientation;
* every 0x28 modular prefix against the orientation at its claimed epoch;
* packed gyro area against both source samples and carrier rotation;
* packed acceleration against source samples and gravity at the same poses;
* a complete 0x1E -> 0x28 -> 0x1E transition around every generated batch.

The test then corrupts one ingredient at a time.  A green baseline is not
enough: wrong prefix epoch, acceleration scale, gyro scale, gyro axes, elapsed
clock, and following carrier must each be rejected for the expected reason.

This proves internal physical coherence of the generated sequence.  It cannot
prove Nintendo's private filtering or exact console consumption semantics;
those inputs are not transmitted by genuine hardware.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import statistics
import struct
import subprocess
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import ns2_motion_carrier as C
import ns2_motion_packet as K
import ns2_motion_reference as R

TICK_HZ = 800.0
COUNTS_PER_G = 4096.0
COUNTS_PER_DPS = 16.4

# Tight because the fixture is noiseless and source-authored.  Genuine-corpus
# errors are larger because its exact 800 Hz inputs and prefix-epoch carrier are
# never transmitted; do not reuse these limits as capture-quality thresholds.
MAX_TRANSLATOR_DEG = 0.04
MAX_CARRIER_DEG = 0.005
MAX_PREFIX_DEG = 0.005
MAX_GYRO_COUNTS = 0.51
MAX_GYRO_AREA_DEG = 0.03
MAX_ACCEL_COUNTS = 0.01
MAX_GRAVITY_COUNTS = 1.0
MAX_GRAVITY_DIRECTION_DEG = 0.03


@dataclass(frozen=True)
class SourceSample:
    index: int
    us: int
    tick: int
    bias_ready: bool
    gyro: tuple[int, int, int]
    accel: tuple[int, int, int]
    quaternion_xyzw: tuple[float, float, float, float]
    fixture_truth_xyzw: tuple[float, float, float, float]
    carrier: bytes


@dataclass(frozen=True)
class PduEvent:
    sample_index: int
    length: int
    payload: bytes


@dataclass(frozen=True)
class Failure:
    code: str
    detail: str


@dataclass
class Validation:
    failures: list[Failure]
    metrics: dict[str, float | int]

    @property
    def ok(self) -> bool:
        return not self.failures

    @property
    def codes(self) -> set[str]:
        return {failure.code for failure in self.failures}


def _q_normalize(q: Sequence[float]) -> tuple[float, float, float, float]:
    norm = math.sqrt(sum(value * value for value in q))
    if norm == 0.0:
        raise ValueError("zero quaternion")
    return tuple(value / norm for value in q)  # type: ignore[return-value]


def _q_right_integrate(
    quaternion_xyzw: Sequence[float], gyro_counts: Sequence[float], dt_s: float
) -> tuple[float, float, float, float]:
    """Exact body-frame integration, independent of the firmware Euler step."""

    omega = tuple(
        math.radians(value / COUNTS_PER_DPS) for value in gyro_counts
    )
    speed = math.sqrt(sum(value * value for value in omega))
    if speed == 0.0 or dt_s == 0.0:
        return tuple(quaternion_xyzw)  # type: ignore[return-value]
    half = 0.5 * speed * dt_s
    scale = math.sin(half) / speed
    dx, dy, dz = (value * scale for value in omega)
    dw = math.cos(half)
    x, y, z, w = quaternion_xyzw
    return _q_normalize((
        w * dx + x * dw + y * dz - z * dy,
        w * dy - x * dz + y * dw + z * dx,
        w * dz + x * dy - y * dx + z * dw,
        w * dw - x * dx - y * dy - z * dz,
    ))


def _q_delta_degrees(left: Sequence[float], right: Sequence[float]) -> float:
    left = _q_normalize(left)
    right = _q_normalize(right)
    dot = abs(sum(a * b for a, b in zip(left, right)))
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(2.0 * math.acos(dot))


def _xyzw_to_wxyz(q: Sequence[float]) -> tuple[float, float, float, float]:
    return (q[3], q[0], q[1], q[2])


def _gravity_body(q: Sequence[float]) -> tuple[float, float, float]:
    x, y, z, w = q
    return (
        2.0 * (x * z - w * y) * COUNTS_PER_G,
        2.0 * (y * z + w * x) * COUNTS_PER_G,
        (1.0 - 2.0 * (x * x + y * y)) * COUNTS_PER_G,
    )


def _vector_norm(vector: Sequence[float]) -> float:
    return math.sqrt(sum(value * value for value in vector))


def _vector_angle_degrees(left: Sequence[float], right: Sequence[float]) -> float:
    denominator = _vector_norm(left) * _vector_norm(right)
    if denominator == 0.0:
        return 180.0
    cosine = sum(a * b for a, b in zip(left, right)) / denominator
    return math.degrees(math.acos(max(-1.0, min(1.0, cosine))))


def _tick_and_elapsed(event: PduEvent) -> tuple[int, int]:
    tick = event.payload[0] | ((event.payload[1] & 0x0F) << 8)
    elapsed = event.payload[1] >> 4
    if event.length == 0x28:
        elapsed |= event.payload[2] << 4
    return tick, elapsed


def _compile_and_run() -> str:
    compiler = shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("gcc is required to build the coherence fixture")
    output = ROOT / "build" / "host-tests" / "ns2_motion40_coherence_fixture.exe"
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        compiler,
        "-Iinclude",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-o",
        str(output),
        "tools/ns2_motion40_coherence_fixture.c",
        "src/bt_hid/motion/ns2_ds5_motion.c",
        "src/bt_hid/motion/ns2_ds5_motion40.c",
        "src/bt_hid/motion/ns2_motion_pdu.c",
        "-lm",
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    completed = subprocess.run(
        [str(output)], cwd=ROOT, check=True, text=True,
        encoding="utf-8", capture_output=True,
    )
    return completed.stdout


def _load_fixture(text: str) -> tuple[list[SourceSample], list[PduEvent], dict]:
    samples: list[SourceSample] = []
    pdus: list[PduEvent] = []
    summary: dict = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"fixture line {line_number}: {error}") from error
        if item.get("kind") == "sample":
            samples.append(SourceSample(
                index=int(item["index"]),
                us=int(item["us"]),
                tick=int(item["tick"]),
                bias_ready=bool(item["bias_ready"]),
                gyro=tuple(int(value) for value in item["gyro"]),
                accel=tuple(int(value) for value in item["accel"]),
                quaternion_xyzw=tuple(float(value) for value in item["q_xyzw"]),
                fixture_truth_xyzw=tuple(
                    float(value) for value in item["truth_xyzw"]
                ),
                carrier=bytes.fromhex(item["carrier"]),
            ))
        elif item.get("kind") == "pdu":
            payload = bytes.fromhex(item["payload"])
            pdus.append(PduEvent(
                sample_index=int(item["sample_index"]),
                length=int(item["length"]),
                payload=payload,
            ))
        elif item.get("kind") == "summary":
            summary = item
    if not samples or not pdus or not summary:
        raise ValueError("fixture is missing samples, PDUs, or summary")
    return samples, pdus, summary


def _record_failure(
    failures: list[Failure], code: str, detail: str, limit: int = 3
) -> None:
    if sum(failure.code == code for failure in failures) < limit:
        failures.append(Failure(code, detail))


def validate(
    samples: Sequence[SourceSample], pdus: Sequence[PduEvent], summary: dict
) -> Validation:
    failures: list[Failure] = []
    metrics: dict[str, float | int] = {
        "samples": len(samples),
        "pdus": len(pdus),
        "carriers": sum(event.length == 0x1E for event in pdus),
        "batches": sum(event.length == 0x28 for event in pdus),
        "max_translator_deg": 0.0,
        "max_fixture_truth_deg": 0.0,
        "max_source_gravity_counts": 0.0,
        "max_source_gravity_direction_deg": 0.0,
        "max_carrier_deg": 0.0,
        "max_prefix_deg": 0.0,
        "max_gyro_counts": 0.0,
        "max_gyro_area_deg": 0.0,
        "max_accel_counts": 0.0,
        "closed_loops": 0,
    }
    if summary.get("fallbacks") or summary.get("starved") or summary.get("overlong"):
        _record_failure(
            failures, "scheduler_health",
            f"nonzero scheduler fault counters: {summary}",
        )
    if summary.get("sat_accel") or summary.get("sat_gyro"):
        _record_failure(
            failures, "scheduler_health",
            f"fixture unexpectedly saturated: {summary}",
        )

    by_index = {sample.index: sample for sample in samples}
    analytic: dict[int, tuple[float, float, float, float]] = {}
    q = (0.0, 0.0, 0.0, 1.0)
    previous_us: int | None = None
    for sample in samples:
        if previous_us is not None:
            q = _q_right_integrate(q, sample.gyro,
                                   (sample.us - previous_us) / 1_000_000.0)
        previous_us = sample.us
        analytic[sample.index] = q

        truth_error = _q_delta_degrees(q, sample.fixture_truth_xyzw)
        translator_error = _q_delta_degrees(q, sample.quaternion_xyzw)
        metrics["max_fixture_truth_deg"] = max(
            float(metrics["max_fixture_truth_deg"]), truth_error
        )
        metrics["max_translator_deg"] = max(
            float(metrics["max_translator_deg"]), translator_error
        )
        if truth_error > 0.001:
            _record_failure(
                failures, "fixture_truth",
                f"sample {sample.index}: C/Python truth differs by {truth_error:.6f} deg",
            )
        if translator_error > MAX_TRANSLATOR_DEG:
            _record_failure(
                failures, "translator_truth",
                f"sample {sample.index}: translator differs by {translator_error:.6f} deg",
            )

        expected_gravity = _gravity_body(q)
        gravity_error = max(
            abs(actual - expected)
            for actual, expected in zip(sample.accel, expected_gravity)
        )
        gravity_direction = _vector_angle_degrees(sample.accel, expected_gravity)
        metrics["max_source_gravity_counts"] = max(
            float(metrics["max_source_gravity_counts"]), gravity_error
        )
        metrics["max_source_gravity_direction_deg"] = max(
            float(metrics["max_source_gravity_direction_deg"]), gravity_direction
        )
        if gravity_error > MAX_GRAVITY_COUNTS:
            _record_failure(
                failures, "source_gravity",
                f"sample {sample.index}: gravity component error {gravity_error:.3f} counts",
            )
        if gravity_direction > MAX_GRAVITY_DIRECTION_DEG:
            _record_failure(
                failures, "source_gravity",
                f"sample {sample.index}: gravity direction error {gravity_direction:.6f} deg",
            )

    # Derive the console-facing acceleration gain from the already validated
    # 0x1E path. This is intentionally not a second hard-coded copy of 68963:
    # the coherence question is whether 0x28 describes the SAME calibrated
    # vector, even if a future hardware-backed calibration changes that gain.
    accel_gain_samples: list[float] = []
    for event in pdus:
        if event.length != 0x1E:
            continue
        source = by_index[event.sample_index]
        for axis in range(3):
            if abs(source.accel[axis]) < 128:
                continue
            actual = struct.unpack_from(
                "<i", event.payload, 16 + axis * 4
            )[0] / 65536.0
            accel_gain_samples.append(actual / source.accel[axis])
    if not accel_gain_samples:
        _record_failure(
            failures, "accel_calibration", "no 0x1E acceleration gain samples"
        )
        accel_gain = 1.0
    else:
        accel_gain = statistics.median(accel_gain_samples)
        spread = max(abs(value - accel_gain) for value in accel_gain_samples)
        metrics["carrier_accel_gain"] = accel_gain
        metrics["carrier_accel_gain_spread"] = spread
        if spread > 1e-5:
            _record_failure(
                failures, "accel_calibration",
                f"0x1E acceleration gain is inconsistent by {spread:.8f}",
            )

    previous_tick: int | None = None
    for position, event in enumerate(pdus):
        if event.length != len(event.payload):
            _record_failure(
                failures, "framing",
                f"PDU {position}: declared {event.length}, has {len(event.payload)} bytes",
            )
            continue
        source = by_index.get(event.sample_index)
        if source is None:
            _record_failure(
                failures, "framing",
                f"PDU {position}: missing source sample {event.sample_index}",
            )
            continue
        tick, elapsed = _tick_and_elapsed(event)
        if tick != source.tick:
            _record_failure(
                failures, "timeline",
                f"PDU {position}: wire tick {tick} != source tick {source.tick}",
            )
        if previous_tick is not None:
            delta = (tick - previous_tick) & 0x0FFF
            if delta != elapsed:
                _record_failure(
                    failures, "timeline",
                    f"PDU {position}: elapsed {elapsed} != predecessor delta {delta}",
                )
        previous_tick = tick

        if event.length == 0x1E:
            orientation = R.decode_motion30_orientation(event.payload)
            carrier_error = _q_delta_degrees(
                orientation.quaternion_wxyz,
                _xyzw_to_wxyz(source.quaternion_xyzw),
            )
            metrics["max_carrier_deg"] = max(
                float(metrics["max_carrier_deg"]), carrier_error
            )
            if carrier_error > MAX_CARRIER_DEG:
                _record_failure(
                    failures, "carrier_truth",
                    f"PDU {position}: carrier error {carrier_error:.6f} deg",
                )
            accel = tuple(
                struct.unpack_from("<i", event.payload, 16 + axis * 4)[0] /
                65536.0
                for axis in range(3)
            )
            accel_error = max(
                abs(actual - expected * accel_gain)
                for actual, expected in zip(accel, source.accel)
            )
            metrics["max_accel_counts"] = max(
                float(metrics["max_accel_counts"]), accel_error
            )
            if accel_error > MAX_ACCEL_COUNTS:
                _record_failure(
                    failures, "accel_source",
                    f"PDU {position}: 0x1E accel error {accel_error:.6f} counts",
                )
            continue

        if event.length != 0x28:
            _record_failure(
                failures, "framing",
                f"PDU {position}: unsupported length {event.length}",
            )
            continue
        decoded = R.decode_motion40(event.payload, None)
        if decoded.layout != "high_rate" or decoded.packing_mode != 3 or \
                decoded.sensor_status != K.STATUS_HIGH_RATE:
            _record_failure(
                failures, "layout",
                f"PDU {position}: {decoded.layout}, mode {decoded.packing_mode}, "
                f"status 0x{decoded.sensor_status:02X}",
            )
        if position == 0 or position + 1 >= len(pdus) or \
                pdus[position - 1].length != 0x1E or \
                pdus[position + 1].length != 0x1E:
            _record_failure(
                failures, "closed_loop",
                f"PDU {position}: not bracketed by 0x1E carriers",
            )
            continue
        metrics["closed_loops"] = int(metrics["closed_loops"]) + 1
        preceding = pdus[position - 1]
        following = pdus[position + 1]
        previous_source = by_index[preceding.sample_index]

        reference = R.decode_motion30_orientation(preceding.payload)
        prefix = R.decode_motion40_prefix_orientation(decoded, reference)
        epoch_tick = (tick - elapsed + C.PREFIX_EPOCH_OFFSET) & 0x0FFF
        epoch_candidates = [
            sample for sample in samples
            if preceding.sample_index <= sample.index <= event.sample_index and
            sample.tick == epoch_tick
        ]
        if len(epoch_candidates) != 1:
            _record_failure(
                failures, "prefix_epoch",
                f"PDU {position}: epoch tick {epoch_tick} has "
                f"{len(epoch_candidates)} source candidates",
            )
        else:
            epoch_source = epoch_candidates[0]
            prefix_error = _q_delta_degrees(
                prefix.quaternion_wxyz,
                _xyzw_to_wxyz(epoch_source.quaternion_xyzw),
            )
            metrics["max_prefix_deg"] = max(
                float(metrics["max_prefix_deg"]), prefix_error
            )
            if prefix_error > MAX_PREFIX_DEG:
                _record_failure(
                    failures, "prefix_epoch",
                    f"PDU {position}: prefix error {prefix_error:.6f} deg "
                    f"at tick {epoch_tick}",
                )

        window = [
            sample for sample in samples
            if preceding.sample_index < sample.index <= event.sample_index
        ]
        weighted = [0, 0, 0]
        integrated_ticks = 0
        cursor = previous_source.tick
        for sample in window:
            weight = (sample.tick - cursor) & 0x0FFF
            cursor = sample.tick
            integrated_ticks += weight
            for axis in range(3):
                weighted[axis] += sample.gyro[axis] * weight
        if integrated_ticks != elapsed or not window:
            _record_failure(
                failures, "timeline",
                f"PDU {position}: source window is {integrated_ticks} ticks, "
                f"wire says {elapsed}",
            )
            continue
        expected_gyro = tuple(value / integrated_ticks for value in weighted)
        wire_gyro = R.normalized_vectors(decoded, "gyro")[0]
        gyro_error = max(
            abs(actual - expected)
            for actual, expected in zip(wire_gyro, expected_gyro)
        )
        metrics["max_gyro_counts"] = max(
            float(metrics["max_gyro_counts"]), gyro_error
        )
        if gyro_error > MAX_GYRO_COUNTS:
            _record_failure(
                failures, "gyro_source",
                f"PDU {position}: gyro error {gyro_error:.3f} counts; "
                f"wire={wire_gyro}, source={expected_gyro}",
            )

        source_rotation = _q_delta_degrees(
            analytic[preceding.sample_index], analytic[event.sample_index]
        )
        wire_rotation = (
            _vector_norm(wire_gyro) / COUNTS_PER_DPS * elapsed / TICK_HZ
        )
        gyro_area_error = abs(wire_rotation - source_rotation)
        metrics["max_gyro_area_deg"] = max(
            float(metrics["max_gyro_area_deg"]), gyro_area_error
        )
        if gyro_area_error > MAX_GYRO_AREA_DEG:
            _record_failure(
                failures, "gyro_area",
                f"PDU {position}: gyro predicts {wire_rotation:.6f} deg, "
                f"carrier trajectory is {source_rotation:.6f} deg",
            )

        wire_accel = R.normalized_vectors(decoded, "accel")
        expected_slots = (window[0], window[-1])
        for slot, (actual, expected_sample) in enumerate(
            zip(wire_accel, expected_slots)
        ):
            accel_error = max(
                abs(value - expected * accel_gain)
                for value, expected in zip(actual, expected_sample.accel)
            )
            metrics["max_accel_counts"] = max(
                float(metrics["max_accel_counts"]), accel_error
            )
            if accel_error > MAX_ACCEL_COUNTS:
                _record_failure(
                    failures, "accel_source",
                    f"PDU {position} slot {slot}: accel error "
                    f"{accel_error:.6f} counts",
                )
            expected_gravity = tuple(
                value * accel_gain
                for value in _gravity_body(analytic[expected_sample.index])
            )
            gravity_direction = _vector_angle_degrees(actual, expected_gravity)
            gravity_magnitude = abs(
                _vector_norm(actual) - COUNTS_PER_G * abs(accel_gain)
            )
            if gravity_direction > MAX_GRAVITY_DIRECTION_DEG or \
                    gravity_magnitude > 1.5:
                _record_failure(
                    failures, "accel_gravity",
                    f"PDU {position} slot {slot}: direction "
                    f"{gravity_direction:.6f} deg, magnitude error "
                    f"{gravity_magnitude:.3f} counts",
                )

        # Close the other half of the transition: the following 0x1E must be
        # the same analytic trajectory, not merely a syntactically valid chart.
        next_orientation = R.decode_motion30_orientation(following.payload)
        next_error = _q_delta_degrees(
            next_orientation.quaternion_wxyz,
            _xyzw_to_wxyz(by_index[following.sample_index].quaternion_xyzw),
        )
        if next_error > MAX_CARRIER_DEG:
            _record_failure(
                failures, "carrier_truth",
                f"PDU {position} following carrier error {next_error:.6f} deg",
            )

    if int(metrics["batches"]) == 0 or int(metrics["closed_loops"]) != int(
        metrics["batches"]
    ):
        _record_failure(
            failures, "closed_loop",
            f"closed {metrics['closed_loops']} of {metrics['batches']} batches",
        )
    return Validation(failures, metrics)


def _rebuild40(
    payload: bytes,
    *,
    tick: int | None = None,
    elapsed: int | None = None,
    carrier: Sequence[int] | None = None,
    accel: Sequence[Sequence[int]] | None = None,
    gyro: Sequence[Sequence[int]] | None = None,
) -> bytes:
    decoded = R.decode_motion40(payload, None)
    return K.build_motion40(K.MotionPacketFields(
        tick=decoded.tick if tick is None else tick,
        elapsed_ticks=decoded.elapsed_ticks if elapsed is None else elapsed,
        carrier=decoded.prefix_carrier if carrier is None else tuple(carrier),
        accel=decoded.accel if accel is None else tuple(tuple(v) for v in accel),
        gyro=decoded.gyro if gyro is None else tuple(tuple(v) for v in gyro),
        tail_value=decoded.tail_value,
        packing_mode=decoded.packing_mode,
        status=decoded.sensor_status,
    ))


def _moving_batch(
    samples: Sequence[SourceSample], pdus: Sequence[PduEvent]
) -> int:
    by_index = {sample.index: sample for sample in samples}
    for position, event in enumerate(pdus):
        if event.length != 0x28:
            continue
        decoded = R.decode_motion40(event.payload, None)
        gyro = R.normalized_vectors(decoded, "gyro")[0]
        if _vector_norm(gyro) > 100.0 and position + 1 < len(pdus):
            return position
    raise ValueError("fixture has no moving 0x28 batch")


def mutation_cases(
    samples: Sequence[SourceSample], pdus: Sequence[PduEvent]
) -> Iterable[tuple[str, list[PduEvent], str]]:
    by_index = {sample.index: sample for sample in samples}
    position = _moving_batch(samples, pdus)
    event = pdus[position]
    decoded = R.decode_motion40(event.payload, None)

    current_orientation = R.decode_motion30_orientation(
        by_index[event.sample_index].carrier
    )
    mutated = list(pdus)
    mutated[position] = replace(
        event,
        payload=_rebuild40(
            event.payload,
            carrier=C.encode_prefix(current_orientation.carrier_raw, True),
        ),
    )
    yield "current carrier used as past prefix", mutated, "prefix_epoch"

    mutated = list(pdus)
    mutated[position] = replace(
        event,
        payload=_rebuild40(
            event.payload,
            accel=tuple(
                tuple(int(value / 2) for value in vector)
                for vector in decoded.accel
            ),
        ),
    )
    yield "acceleration divided by two", mutated, "accel_source"

    mutated = list(pdus)
    mutated[position] = replace(
        event,
        payload=_rebuild40(
            event.payload,
            gyro=tuple(
                tuple(int(value / 2) for value in vector)
                for vector in decoded.gyro
            ),
        ),
    )
    yield "gyro fixed-point divided by two", mutated, "gyro_source"

    mutated = list(pdus)
    swapped = tuple((vector[1], vector[0], vector[2]) for vector in decoded.gyro)
    mutated[position] = replace(
        event, payload=_rebuild40(event.payload, gyro=swapped)
    )
    yield "gyro X/Y swapped", mutated, "gyro_source"

    mutated = list(pdus)
    mutated[position] = replace(
        event,
        payload=_rebuild40(
            event.payload, elapsed=decoded.elapsed_ticks + 1
        ),
    )
    yield "elapsed detached from shared tick", mutated, "timeline"

    # Keep the following 0x1E packet's timing/acceleration but replace only its
    # orientation carrier with identity. This is the explicit 0x1E->0x28->0x1E
    # composition failure that per-packet validation cannot see.
    following = pdus[position + 1]
    identity = R.decode_motion30_orientation(samples[0].carrier)
    old = bytearray(following.payload)
    replacement = K.build_motion30(
        identity.state,
        identity.carrier_raw,
        header=old[:5],
        byte12_flag=1 if old[12] & K.MOTION30_BYTE12_FLAG else 0,
    )
    old[4:16] = replacement[4:16]
    mutated = list(pdus)
    mutated[position + 1] = replace(following, payload=bytes(old))
    yield "following carrier detached from trajectory", mutated, "carrier_truth"


def _print_validation(label: str, result: Validation) -> None:
    status = "PASS" if result.ok else "FAIL"
    print(f"{label}: {status}")
    print(json.dumps(result.metrics, indent=2, sort_keys=True))
    for failure in result.failures:
        print(f"  {failure.code}: {failure.detail}")


def generate_fixture_text() -> str:
    """Build the current C sources and return their deterministic JSONL."""

    return _compile_and_run()


def evaluate_fixture_text(
    text: str, *, run_mutations: bool = True
) -> tuple[Validation, list[dict], bool]:
    """Validate one fixture and return baseline, mutation report, and escape."""

    samples, pdus, summary = _load_fixture(text)
    baseline = validate(samples, pdus, summary)
    mutation_report: list[dict] = []
    escaped = False
    if run_mutations:
        for name, mutated, expected_code in mutation_cases(samples, pdus):
            result = validate(samples, mutated, summary)
            caught = expected_code in result.codes
            escaped |= not caught
            mutation_report.append({
                "name": name,
                "expected": expected_code,
                "caught": caught,
                "codes": sorted(result.codes),
            })
    return baseline, mutation_report, escaped


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fixture", type=Path,
        help="validate an existing fixture JSONL instead of compiling C",
    )
    parser.add_argument(
        "--write-fixture", type=Path,
        help="save the generated JSONL for inspection",
    )
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--no-mutations", action="store_true")
    args = parser.parse_args(argv)

    try:
        text = args.fixture.read_text(encoding="utf-8") if args.fixture \
            else generate_fixture_text()
        if args.write_fixture:
            args.write_fixture.parent.mkdir(parents=True, exist_ok=True)
            args.write_fixture.write_text(text, encoding="utf-8")
        baseline, mutation_report, escaped = evaluate_fixture_text(
            text, run_mutations=not args.no_mutations
        )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"coherence fixture error: {error}", file=sys.stderr)
        return 2

    report = {
        "baseline": {
            "ok": baseline.ok,
            "metrics": baseline.metrics,
            "failures": [failure.__dict__ for failure in baseline.failures],
        },
        "mutations": mutation_report,
        "all_mutations_caught": not escaped,
        "evidence_boundary": (
            "proves generated sequence coherence against a known physical "
            "trajectory; does not prove Nintendo private filtering or console semantics"
        ),
    }
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        _print_validation("motion40 closed-loop baseline", baseline)
        for mutation in mutation_report:
            print(
                f"mutation {'PASS' if mutation['caught'] else 'FAIL'}: "
                f"{mutation['name']} -> expected {mutation['expected']}; "
                f"observed {', '.join(mutation['codes']) or 'no failure'}"
            )
        print("Evidence boundary: " + report["evidence_boundary"] + ".")
    return 0 if baseline.ok and not escaped else 1


if __name__ == "__main__":
    raise SystemExit(main())
