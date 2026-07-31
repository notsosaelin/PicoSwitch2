#!/usr/bin/env python3
"""Reproduce historical Switch 2 Pro Controller magnet-probe analyses.

Input is the JSON-lines format produced by:

    ./tools/read_uart_diag.ps1 -Port COM11 \
        -Command 'motionpair capture' -OutputPath capture.jsonl

Semantic warning (2026-07-29): G6/G7/G8 are legacy aliases whose source ranges
cross packed gyro and acceleration samples.  Their vector/quaternion statistics
remain useful only for reproducing the completed A/B/A campaign; they are not
independent motion or magnetometer fields.  Its ``sub_index`` and
``secondary_status`` output names are also historical: together they encode one
12-bit elapsed count, and status 0x0F means catch-up layout rather than
escalation. Use ns2_motion_reference.py for the current multi-sample decode and
reference-PCAP audit.

Only Python's standard library is required.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import io
import json
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, TextIO


SQRT2 = math.sqrt(2.0)
UINT32_MASK = 0xFFFFFFFF
NORMAL_28_STATUS = 0x0D
ESCALATION_28_STATUS = 0x0F


class MagprobeError(ValueError):
    """A capture or command is malformed."""


@dataclass(frozen=True)
class MotionRecord:
    t_us: int
    elapsed_us: int
    native: bytes
    source: dict[str, Any]


@dataclass(frozen=True)
class MotionCapture:
    records: list[MotionRecord]
    dropped: int
    source_name: str
    capture_format: str = "motionpair"


@dataclass(frozen=True)
class RawMagRecord:
    t_us: int
    elapsed_us: int
    axes: tuple[int, int, int]
    report: bytes
    source: dict[str, Any]


@dataclass(frozen=True)
class RawMagCapture:
    records: list[RawMagRecord]
    dropped: int
    source_name: str


@dataclass(frozen=True)
class QuaternionSample:
    record: MotionRecord
    tick: int
    sub_index: int
    secondary_status: int
    sensor_status: int
    omitted: int
    carriers: tuple[int, int, int]
    quaternion_wxyz: tuple[float, float, float, float]
    accel: tuple[int, int, int]


@dataclass(frozen=True)
class MagnetSample:
    record: MotionRecord
    tick: int
    sub_index: int
    secondary_status: int
    sensor_status: int
    reference_accel: tuple[float, float, float] | None
    unknown_middle_s32: tuple[int, int, int]
    magnetic_body: tuple[float, float, float] | None
    nearest_quaternion: QuaternionSample | None
    quaternion_age_us: int | None
    orientation_quaternion_wxyz: tuple[float, float, float, float] | None
    orientation_method: str
    orientation_span_us: int | None
    magnetic_world: tuple[float, float, float] | None


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise MagprobeError(f"{name} must be an integer")
    if not minimum <= value <= maximum:
        raise MagprobeError(f"{name} is outside {minimum}..{maximum}: {value}")
    return value


def _unwrap_u32(previous_raw: int | None, previous_elapsed: int, current: int) -> int:
    if previous_raw is None:
        return 0
    return previous_elapsed + ((current - previous_raw) & UINT32_MASK)


def read_capture(stream: TextIO, source: str = "<stream>") -> MotionCapture:
    records: list[MotionRecord] = []
    end: dict[str, Any] | None = None
    previous_raw: int | None = None
    elapsed = 0

    for line_number, line in enumerate(stream, 1):
        text = line.strip()
        if not text:
            continue
        if end is not None:
            raise MagprobeError(f"{source}:{line_number}: data appears after capture end")
        try:
            item = json.loads(text)
        except json.JSONDecodeError as exc:
            raise MagprobeError(
                f"{source}:{line_number}: invalid JSON: {exc.msg}"
            ) from exc
        if not isinstance(item, dict):
            raise MagprobeError(f"{source}:{line_number}: JSON value must be an object")
        if item.get("motionpair") == "end":
            end = item
            continue
        if item.get("motionpair") != "record":
            raise MagprobeError(
                f"{source}:{line_number}: expected motionpair record or end"
            )

        required = {
            "t_us", "native_len", "native", "ds5_valid", "ds5_seq",
            "ds5_t_us", "ds5_age_us", "ds5_sensor", "cal_state",
            "raw_g", "raw_a", "cal_g", "cal_a",
        }
        missing = sorted(required - item.keys())
        if missing:
            raise MagprobeError(
                f"{source}:{line_number}: missing fields: {', '.join(missing)}"
            )
        timestamp = _integer(item["t_us"], "t_us", 0, UINT32_MASK)
        native_length = _integer(item["native_len"], "native_len", 0, 0xFF)
        if native_length not in (0x1E, 0x28):
            raise MagprobeError(
                f"{source}:{line_number}: native length must be 30 or 40"
            )
        native_hex = item["native"]
        if not isinstance(native_hex, str) or len(native_hex) != native_length * 2:
            raise MagprobeError(
                f"{source}:{line_number}: native hex length does not match native_len"
            )
        try:
            native = bytes.fromhex(native_hex)
        except ValueError as exc:
            raise MagprobeError(
                f"{source}:{line_number}: native payload is not hexadecimal"
            ) from exc
        for field in ("raw_g", "raw_a", "cal_g", "cal_a"):
            value = item[field]
            if not isinstance(value, list) or len(value) != 3 or not all(
                isinstance(axis, int) and not isinstance(axis, bool) for axis in value
            ):
                raise MagprobeError(
                    f"{source}:{line_number}: {field} must contain three integers"
                )

        elapsed = _unwrap_u32(previous_raw, elapsed, timestamp)
        previous_raw = timestamp
        records.append(MotionRecord(timestamp, elapsed, native, item))

    if end is None:
        raise MagprobeError(f"{source}: motionpair end record is missing")
    count = _integer(end.get("records"), "end.records", 0, 0xFFFF)
    dropped = _integer(end.get("dropped", 0), "end.dropped", 0, UINT32_MASK)
    if count != len(records):
        raise MagprobeError(
            f"{source}: end count {count} does not match {len(records)} records"
        )
    if not records:
        raise MagprobeError(f"{source}: capture contains no motion records")
    return MotionCapture(records, dropped, source)


def read_ble_capture(stream: TextIO, source: str = "<stream>") -> MotionCapture:
    """Extract native motion PDUs from a UART ``blecap dump`` JSONL file."""
    records: list[MotionRecord] = []
    end: dict[str, Any] | None = None
    previous_raw: int | None = None
    elapsed = 0

    for line_number, line in enumerate(stream, 1):
        text = line.strip()
        if not text:
            continue
        try:
            item = json.loads(text)
        except json.JSONDecodeError as exc:
            raise MagprobeError(
                f"{source}:{line_number}: invalid JSON: {exc.msg}"
            ) from exc
        if not isinstance(item, dict):
            raise MagprobeError(f"{source}:{line_number}: JSON value must be an object")
        if item.get("blecap") == "end":
            end = item
            continue
        if item.get("blecap") != "record":
            raise MagprobeError(
                f"{source}:{line_number}: expected blecap record or end"
            )
        if (
            item.get("kind") != "input"
            or item.get("handle") != "0x000E"
        ):
            continue

        timestamp = _integer(item.get("t_us"), "t_us", 0, UINT32_MASK)
        report_length = _integer(item.get("length"), "length", 0, 0xFFFF)
        captured_length = _integer(
            item.get("captured"), "captured", 0, 0xFFFF
        )
        payload_hex = item.get("payload")
        if not isinstance(payload_hex, str) or len(payload_hex) != captured_length * 2:
            raise MagprobeError(
                f"{source}:{line_number}: payload hex length does not match captured"
            )
        try:
            report = bytes.fromhex(payload_hex)
        except ValueError as exc:
            raise MagprobeError(
                f"{source}:{line_number}: payload is not hexadecimal"
            ) from exc
        if report_length > captured_length:
            # A truncated full report cannot safely expose its embedded PDU.
            continue
        if len(report) <= 0x0E:
            continue
        native_length = report[0x0E]
        if native_length not in (0x1E, 0x28):
            continue
        native_end = 0x0F + native_length
        if native_end > len(report):
            raise MagprobeError(
                f"{source}:{line_number}: embedded native PDU is truncated"
            )

        elapsed = _unwrap_u32(previous_raw, elapsed, timestamp)
        previous_raw = timestamp
        records.append(MotionRecord(
            timestamp,
            elapsed,
            report[0x0F:native_end],
            item,
        ))

    if end is None:
        raise MagprobeError(f"{source}: blecap end record is missing")
    dropped = _integer(end.get("dropped", 0), "end.dropped", 0, UINT32_MASK)
    if not records:
        raise MagprobeError(
            f"{source}: capture contains no complete handle-0x000E motion PDUs"
        )
    return MotionCapture(records, dropped, source, "blecap")


def read_rawmag_ble_capture(
    stream: TextIO, source: str = "<stream>"
) -> RawMagCapture:
    """Extract the separate signed-int16 magnetometer lanes from handle 0x000A.

    This layout is only expected after explicitly selecting feature mask 0x94.
    It is deliberately kept separate from the native handle-0x000E 0x28 decoder:
    the two channels have different wire formats and must not be conflated.
    """
    records: list[RawMagRecord] = []
    end: dict[str, Any] | None = None
    previous_raw: int | None = None
    elapsed = 0

    for line_number, line in enumerate(stream, 1):
        text = line.strip()
        if not text:
            continue
        try:
            item = json.loads(text)
        except json.JSONDecodeError as exc:
            raise MagprobeError(
                f"{source}:{line_number}: invalid JSON: {exc.msg}"
            ) from exc
        if not isinstance(item, dict):
            raise MagprobeError(f"{source}:{line_number}: JSON value must be an object")
        if item.get("blecap") == "end":
            end = item
            continue
        if item.get("blecap") != "record":
            raise MagprobeError(
                f"{source}:{line_number}: expected blecap record or end"
            )
        if item.get("kind") != "input" or item.get("handle") != "0x000A":
            continue

        timestamp = _integer(item.get("t_us"), "t_us", 0, UINT32_MASK)
        report_length = _integer(item.get("length"), "length", 0, 0xFFFF)
        captured_length = _integer(item.get("captured"), "captured", 0, 0xFFFF)
        payload_hex = item.get("payload")
        if not isinstance(payload_hex, str) or len(payload_hex) != captured_length * 2:
            raise MagprobeError(
                f"{source}:{line_number}: payload hex length does not match captured"
            )
        try:
            report = bytes.fromhex(payload_hex)
        except ValueError as exc:
            raise MagprobeError(
                f"{source}:{line_number}: payload is not hexadecimal"
            ) from exc
        if report_length > captured_length or len(report) < 31:
            continue

        axes = tuple(
            int.from_bytes(report[offset:offset + 2], "little", signed=True)
            for offset in (25, 27, 29)
        )
        elapsed = _unwrap_u32(previous_raw, elapsed, timestamp)
        previous_raw = timestamp
        records.append(RawMagRecord(timestamp, elapsed, axes, report, item))

    if end is None:
        raise MagprobeError(f"{source}: blecap end record is missing")
    dropped = _integer(end.get("dropped", 0), "end.dropped", 0, UINT32_MASK)
    if not records:
        raise MagprobeError(
            f"{source}: capture contains no complete handle-0x000A input reports"
        )
    return RawMagCapture(records, dropped, source)


def load_capture(path: str) -> MotionCapture:
    if path == "-":
        text = sys.stdin.read()
        source = "<stdin>"
    else:
        source = path
        text = Path(path).read_text(encoding="utf-8")
    first = next((line.strip() for line in text.splitlines() if line.strip()), "")
    try:
        marker = json.loads(first)
    except json.JSONDecodeError as exc:
        raise MagprobeError(f"{source}: invalid first JSON record") from exc
    if not isinstance(marker, dict):
        raise MagprobeError(f"{source}: first JSON value must be an object")
    if "motionpair" in marker:
        return read_capture(io.StringIO(text), source)
    if "blecap" in marker:
        return read_ble_capture(io.StringIO(text), source)
    raise MagprobeError(
        f"{source}: expected motionpair or blecap UART JSONL"
    )


def _sext(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return value - (1 << bits) if value & sign else value


def _i32le(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 4], "little", signed=True)


def decode_timing(data: bytes) -> tuple[int, int]:
    return data[0] | ((data[1] & 0x0F) << 8), data[1] >> 4


def decode_accel_1e(data: bytes) -> tuple[int, int, int]:
    if len(data) != 0x1E:
        raise MagprobeError("established acceleration lanes require a length-30 PDU")
    return (_i32le(data, 16), _i32le(data, 20), _i32le(data, 24))


def decode_unknown_middle_28(data: bytes) -> tuple[int, int, int]:
    if len(data) != 0x28:
        raise MagprobeError("0x28 middle lanes require a length-40 PDU")
    return (_i32le(data, 16), _i32le(data, 20), _i32le(data, 24))


def decode_orientation_carriers(data: bytes) -> tuple[int, int, int]:
    if len(data) != 0x1E:
        raise MagprobeError("orientation carriers require a length-30 PDU")
    g0 = data[5] | (data[6] << 8) | (data[7] << 16) | ((data[8] & 3) << 24)
    g1 = data[9] | (data[10] << 8) | (data[11] << 16) | ((data[12] & 3) << 24)
    g2 = data[13] | (data[14] << 8) | (data[15] << 16) | ((data[4] & 3) << 24)
    return g0, g1, g2


def decode_quaternion(data: bytes) -> tuple[int, tuple[int, int, int],
                                             tuple[float, float, float, float]]:
    carriers = decode_orientation_carriers(data)
    omitted = carriers[2] >> 24
    components = (
        ((carriers[0] / 67108864.0) - 0.5) * SQRT2,
        ((carriers[1] / 33554432.0) - 0.5) * SQRT2,
        (((carriers[2] & 0x00FFFFFF) / 16777216.0) - 0.5) * SQRT2,
    )
    hidden_sq = max(0.0, 1.0 - sum(value * value for value in components))
    wire = [0.0, 0.0, 0.0, 0.0]  # w, x, y, z
    wire[omitted] = math.sqrt(hidden_sq)
    for index, value in enumerate(components, 1):
        wire[(omitted + index) & 3] = value
    norm = math.sqrt(sum(value * value for value in wire))
    if norm == 0.0:
        raise MagprobeError("decoded zero quaternion")
    return omitted, carriers, tuple(value / norm for value in wire)


def decode_g678_raw(data: bytes) -> tuple[int, int, int]:
    if len(data) != 0x28:
        raise MagprobeError("G6/G7/G8 require a length-40 PDU")
    g6 = _sext(
        ((data[32] & 0x03) << 20)
        | (data[31] << 12)
        | (((data[30] << 8) | data[29]) >> 4),
        22,
    )
    g7 = _sext(
        ((data[35] & 0x03) << 20)
        | (data[34] << 12)
        | (((data[33] << 8) | data[32]) >> 4),
        22,
    )
    g8 = _sext(
        (data[37] << 12) | (((data[36] << 8) | data[35]) >> 4),
        20,
    )
    return g6, g7, g8


def decode_magnetic(data: bytes) -> tuple[float, float, float]:
    """Backward-compatible name for the normalized G6/G7/G8 vector."""
    g6, g7, g8 = decode_g678_raw(data)
    return (
        g6 / ((1 << 21) * SQRT2),
        g7 / ((1 << 21) * SQRT2),
        g8 / ((1 << 19) * SQRT2),
    )


def decode_magneto_quaternion_candidate(
    data: bytes,
) -> tuple[float, float, float, float]:
    """Reconstruct the positive-scalar quaternion candidate from G6/G7/G8.

    The wire carries only three bounded components. Quaternion sign is
    physically equivalent, so choosing the positive square root is sufficient
    for continuity/stability tests. Component semantics and ordering remain an
    evidence boundary rather than being silently asserted here.
    """
    vector = decode_magnetic(data)
    scalar = math.sqrt(max(0.0, 1.0 - sum(value * value for value in vector)))
    return scalar, vector[0], vector[1], vector[2]


def _q_conjugate(q: tuple[float, float, float, float]
                 ) -> tuple[float, float, float, float]:
    return q[0], -q[1], -q[2], -q[3]


def _q_multiply(a: tuple[float, float, float, float],
                b: tuple[float, float, float, float]
                ) -> tuple[float, float, float, float]:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    )


def _q_slerp(a: tuple[float, float, float, float],
             b: tuple[float, float, float, float],
             fraction: float) -> tuple[float, float, float, float]:
    dot = sum(left * right for left, right in zip(a, b))
    if dot < 0.0:
        b = tuple(-value for value in b)
        dot = -dot
    dot = max(-1.0, min(1.0, dot))
    if dot > 0.9995:
        mixed = tuple(
            left + fraction * (right - left) for left, right in zip(a, b)
        )
        norm = math.sqrt(sum(value * value for value in mixed))
        return tuple(value / norm for value in mixed)  # type: ignore[return-value]
    angle = math.acos(dot)
    sine = math.sin(angle)
    left_weight = math.sin((1.0 - fraction) * angle) / sine
    right_weight = math.sin(fraction * angle) / sine
    return tuple(
        left_weight * left + right_weight * right for left, right in zip(a, b)
    )  # type: ignore[return-value]


def rotate_body_to_world(
    q_wxyz: tuple[float, float, float, float],
    vector: tuple[float, float, float],
) -> tuple[float, float, float]:
    pure = (0.0, vector[0], vector[1], vector[2])
    rotated = _q_multiply(_q_multiply(q_wxyz, pure), _q_conjugate(q_wxyz))
    return rotated[1], rotated[2], rotated[3]


def decode_quaternion_samples(capture: MotionCapture) -> list[QuaternionSample]:
    output: list[QuaternionSample] = []
    for record in capture.records:
        if len(record.native) != 0x1E:
            continue
        data = record.native
        tick, sub_index = decode_timing(data)
        omitted, carriers, quaternion = decode_quaternion(data)
        output.append(QuaternionSample(
            record=record,
            tick=tick,
            sub_index=sub_index,
            secondary_status=data[2],
            sensor_status=data[3],
            omitted=omitted,
            carriers=carriers,
            quaternion_wxyz=quaternion,
            accel=decode_accel_1e(data),
        ))
    return output


def decode_magnet_samples(capture: MotionCapture) -> list[MagnetSample]:
    quaternions = decode_quaternion_samples(capture)
    quaternion_times = [sample.record.elapsed_us for sample in quaternions]
    output: list[MagnetSample] = []
    for record in capture.records:
        if len(record.native) != 0x28:
            continue
        data = record.native
        tick, sub_index = decode_timing(data)
        nearest: QuaternionSample | None = None
        age: int | None = None
        insertion = 0
        if quaternions:
            insertion = bisect.bisect_left(quaternion_times, record.elapsed_us)
            candidates = []
            if insertion:
                candidates.append(quaternions[insertion - 1])
            if insertion < len(quaternions):
                candidates.append(quaternions[insertion])
            nearest = min(
                candidates,
                key=lambda sample: abs(sample.record.elapsed_us - record.elapsed_us),
            )
            age = record.elapsed_us - nearest.record.elapsed_us
        orientation = nearest.quaternion_wxyz if nearest is not None else None
        reference_accel = (
            tuple(float(value) for value in nearest.accel)
            if nearest is not None else None
        )
        orientation_method = "nearest" if nearest is not None else "unavailable"
        orientation_span: int | None = None
        if 0 < insertion < len(quaternions):
            before = quaternions[insertion - 1]
            after = quaternions[insertion]
            span = after.record.elapsed_us - before.record.elapsed_us
            if 0 < span <= 50_000:
                fraction = (record.elapsed_us - before.record.elapsed_us) / span
                if 0.0 <= fraction <= 1.0:
                    orientation = _q_slerp(
                        before.quaternion_wxyz, after.quaternion_wxyz, fraction
                    )
                    orientation_method = "slerp"
                    orientation_span = span
                    reference_accel = tuple(
                        before.accel[axis]
                        + fraction * (after.accel[axis] - before.accel[axis])
                        for axis in range(3)
                    )
        normal = data[3] == NORMAL_28_STATUS and data[2] == 0
        magnetic = decode_magnetic(data) if normal else None
        world = (
            rotate_body_to_world(orientation, magnetic)
            if orientation is not None and magnetic is not None else None
        )
        output.append(MagnetSample(
            record=record,
            tick=tick,
            sub_index=sub_index,
            secondary_status=data[2],
            sensor_status=data[3],
            reference_accel=reference_accel,
            unknown_middle_s32=decode_unknown_middle_28(data),
            magnetic_body=magnetic,
            nearest_quaternion=nearest,
            quaternion_age_us=age,
            orientation_quaternion_wxyz=orientation,
            orientation_method=orientation_method,
            orientation_span_us=orientation_span,
            magnetic_world=world,
        ))
    return output


def _mean(values: Iterable[float]) -> float:
    materialized = list(values)
    return statistics.fmean(materialized) if materialized else math.nan


def _stdev(values: Iterable[float]) -> float:
    materialized = list(values)
    return statistics.pstdev(materialized) if len(materialized) > 1 else 0.0


def _vector_mean(vectors: Iterable[tuple[float, float, float]]
                 ) -> tuple[float, float, float]:
    values = list(vectors)
    if not values:
        return math.nan, math.nan, math.nan
    return tuple(statistics.fmean(vector[axis] for vector in values)
                 for axis in range(3))  # type: ignore[return-value]


def _vector_norm(vector: Iterable[float]) -> float:
    return math.sqrt(sum(value * value for value in vector))


def _vector_angle_degrees(a: tuple[float, float, float],
                          b: tuple[float, float, float]) -> float:
    denominator = _vector_norm(a) * _vector_norm(b)
    if denominator == 0.0 or not math.isfinite(denominator):
        return math.nan
    cosine = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b)) / denominator))
    return math.degrees(math.acos(cosine))


def _direction_spread(
    vectors: list[tuple[float, float, float]],
) -> dict[str, float]:
    if not vectors:
        return {"mean_degrees": math.nan, "stdev_degrees": math.nan,
                "maximum_degrees": math.nan}
    center = _vector_mean(vectors)
    angles = [_vector_angle_degrees(center, vector) for vector in vectors]
    return {
        "mean_degrees": _mean(angles),
        "stdev_degrees": _stdev(angles),
        "maximum_degrees": max(angles),
    }


def _mean_quaternion(samples: Iterable[QuaternionSample]
                     ) -> tuple[float, float, float, float] | None:
    quaternions = [sample.quaternion_wxyz for sample in samples]
    if not quaternions:
        return None
    reference = quaternions[0]
    aligned = []
    for quaternion in quaternions:
        dot = sum(a * b for a, b in zip(reference, quaternion))
        aligned.append(tuple(-value for value in quaternion) if dot < 0 else quaternion)
    mean = tuple(statistics.fmean(q[index] for q in aligned) for index in range(4))
    norm = _vector_norm(mean)
    if norm == 0.0:
        return None
    return tuple(value / norm for value in mean)  # type: ignore[return-value]


def _quaternion_angle_degrees(a: tuple[float, float, float, float] | None,
                              b: tuple[float, float, float, float] | None) -> float:
    if a is None or b is None:
        return math.nan
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, dot))))


def _pearson(left: list[float], right: list[float]) -> float | None:
    if len(left) != len(right) or len(left) < 4:
        return None
    mean_left = statistics.fmean(left)
    mean_right = statistics.fmean(right)
    centered_left = [value - mean_left for value in left]
    centered_right = [value - mean_right for value in right]
    sum_left = sum(value * value for value in centered_left)
    sum_right = sum(value * value for value in centered_right)
    if sum_left == 0.0 or sum_right == 0.0:
        return None
    return (
        sum(a * b for a, b in zip(centered_left, centered_right))
        / math.sqrt(sum_left * sum_right)
    )


# Bits not currently assigned to timing/status/G6/G7/G8. Partial masks
# preserve the unused packing bits instead of pretending they are padding.
UNKNOWN_28_MASKS: dict[int, int] = {
    **{offset: 0xFF for offset in range(4, 29)},
    29: 0x0F,
    32: 0x0C,
    35: 0x0C,
    38: 0xFF,
    39: 0xFF,
}


def _unknown_value(data: bytes, offset: int) -> int:
    return data[offset] & UNKNOWN_28_MASKS[offset]


def _unknown_stats(samples: list[MagnetSample]) -> list[dict[str, Any]]:
    output = []
    for offset, mask in UNKNOWN_28_MASKS.items():
        values = [_unknown_value(sample.record.native, offset) for sample in samples]
        output.append({
            "offset": offset,
            "mask": mask,
            "unique": len(set(values)),
            "minimum": min(values) if values else 0,
            "maximum": max(values) if values else 0,
            "mean": _mean(values),
            "stdev": _stdev(values),
            "change_mask": 0,
        })
        if values:
            changed = 0
            reference = values[0]
            for value in values[1:]:
                changed |= reference ^ value
            output[-1]["change_mask"] = changed & mask
    return output


def _correlation_population(
    samples: list[MagnetSample],
) -> tuple[list[MagnetSample], dict[str, list[float]]]:
    normal = [
        sample for sample in samples
        if sample.magnetic_body is not None
        and sample.orientation_quaternion_wxyz is not None
    ]
    if len(normal) < 4:
        return normal, {}
    targets: dict[str, list[float]] = {
        "time": [sample.record.elapsed_us / 1_000_000.0 for sample in normal],
    }
    for axis, name in enumerate(("w", "x", "y", "z")):
        targets[f"q_{name}"] = [
            sample.orientation_quaternion_wxyz[axis] for sample in normal
        ]
    for axis, name in enumerate(("x", "y", "z")):
        targets[f"ref_accel_{name}"] = [
            sample.reference_accel[axis] for sample in normal
        ]
        targets[f"mag_{name}"] = [
            sample.magnetic_body[axis] for sample in normal
        ]
        targets[f"world_mag_{name}"] = [
            sample.magnetic_world[axis] for sample in normal
        ]
    return normal, targets


def _unknown_correlations(samples: list[MagnetSample]) -> list[dict[str, Any]]:
    normal, targets = _correlation_population(samples)
    if not targets:
        return []
    correlations = []
    for offset, mask in UNKNOWN_28_MASKS.items():
        values = [float(_unknown_value(sample.record.native, offset)) for sample in normal]
        if len(set(values)) < 2:
            continue
        for target, target_values in targets.items():
            correlation = _pearson(values, target_values)
            if correlation is not None:
                correlations.append({
                    "offset": offset,
                    "mask": mask,
                    "target": target,
                    "r": correlation,
                    "unique": len(set(values)),
                })
    correlations.sort(key=lambda item: abs(item["r"]), reverse=True)
    return correlations


def _unknown_bit_stats(samples: list[MagnetSample]) -> list[dict[str, Any]]:
    output = []
    for offset, mask in UNKNOWN_28_MASKS.items():
        for bit in range(8):
            bit_mask = 1 << bit
            if not mask & bit_mask:
                continue
            values = [
                1 if sample.record.native[offset] & bit_mask else 0
                for sample in samples
            ]
            transitions = sum(
                values[index] != values[index - 1]
                for index in range(1, len(values))
            )
            output.append({
                "offset": offset,
                "bit": bit,
                "ones": sum(values),
                "samples": len(values),
                "one_fraction": _mean(values),
                "transitions": transitions,
            })
    return output


def _unknown_bit_correlations(samples: list[MagnetSample]) -> list[dict[str, Any]]:
    normal, targets = _correlation_population(samples)
    if not targets:
        return []
    output = []
    for offset, mask in UNKNOWN_28_MASKS.items():
        for bit in range(8):
            bit_mask = 1 << bit
            if not mask & bit_mask:
                continue
            values = [
                1.0 if sample.record.native[offset] & bit_mask else 0.0
                for sample in normal
            ]
            if len(set(values)) < 2:
                continue
            for target, target_values in targets.items():
                correlation = _pearson(values, target_values)
                if correlation is not None:
                    output.append({
                        "offset": offset,
                        "bit": bit,
                        "target": target,
                        "r": correlation,
                        "ones": int(sum(values)),
                        "samples": len(values),
                    })
    output.sort(key=lambda item: abs(item["r"]), reverse=True)
    return output


def summarize(capture: MotionCapture) -> dict[str, Any]:
    quaternions = decode_quaternion_samples(capture)
    magnets = decode_magnet_samples(capture)
    normal = [sample for sample in magnets if sample.magnetic_body is not None]
    escalation = [
        sample for sample in magnets
        if sample.sensor_status == ESCALATION_28_STATUS or sample.secondary_status != 0
    ]
    other = [
        sample for sample in magnets
        if sample.magnetic_body is None and sample not in escalation
    ]
    statuses: dict[str, int] = {}
    for sample in magnets:
        key = f"0x{sample.sensor_status:02X}/0x{sample.secondary_status:02X}"
        statuses[key] = statuses.get(key, 0) + 1

    intervals_ms = [
        (capture.records[index].elapsed_us - capture.records[index - 1].elapsed_us)
        / 1000.0
        for index in range(1, len(capture.records))
    ]
    mag_intervals_ms = [
        (magnets[index].record.elapsed_us - magnets[index - 1].record.elapsed_us)
        / 1000.0
        for index in range(1, len(magnets))
    ]
    body_vectors = [sample.magnetic_body for sample in normal]
    world_vectors = [
        sample.magnetic_world for sample in normal if sample.magnetic_world is not None
    ]
    body_norms = [_vector_norm(vector) for vector in body_vectors]
    world_norms = [_vector_norm(vector) for vector in world_vectors]
    g678_raw = [decode_g678_raw(sample.record.native) for sample in normal]
    hidden_scalars = [
        math.sqrt(max(0.0, 1.0 - norm * norm)) for norm in body_norms
    ]
    implied_angles = [
        math.degrees(2.0 * math.asin(min(1.0, norm))) for norm in body_norms
    ]
    accel_vectors = [
        sample.reference_accel for sample in normal
        if sample.reference_accel is not None
    ]

    return {
        "source": capture.source_name,
        "capture_format": capture.capture_format,
        "records": len(capture.records),
        "dropped": capture.dropped,
        "duration_ms": capture.records[-1].elapsed_us / 1000.0,
        "length_30": len(quaternions),
        "length_40": len(magnets),
        "normal_28": len(normal),
        "escalation_28": len(escalation),
        "other_28": len(other),
        "statuses_28": statuses,
        "combined_interval_ms": {
            "mean": _mean(intervals_ms),
            "stdev": _stdev(intervals_ms),
            "minimum": min(intervals_ms) if intervals_ms else math.nan,
            "maximum": max(intervals_ms) if intervals_ms else math.nan,
        },
        "mag_interval_ms": {
            "mean": _mean(mag_intervals_ms),
            "stdev": _stdev(mag_intervals_ms),
            "minimum": min(mag_intervals_ms) if mag_intervals_ms else math.nan,
            "maximum": max(mag_intervals_ms) if mag_intervals_ms else math.nan,
        },
        "magnetic_body_mean": _vector_mean(body_vectors),
        "magnetic_world_mean": _vector_mean(world_vectors),
        "magnetic_norm": {
            "mean": _mean(body_norms),
            "stdev": _stdev(body_norms),
            "minimum": min(body_norms) if body_norms else math.nan,
            "maximum": max(body_norms) if body_norms else math.nan,
        },
        "world_magnetic_norm": {
            "mean": _mean(world_norms),
            "stdev": _stdev(world_norms),
        },
        "g678_wire": {
            "signed_width_bits": [22, 22, 20],
            "raw_minimum": [
                min((value[axis] for value in g678_raw), default=0)
                for axis in range(3)
            ],
            "raw_maximum": [
                max((value[axis] for value in g678_raw), default=0)
                for axis in range(3)
            ],
            "all_samples_fit_signed_int16": all(
                -32768 <= component <= 32767
                for value in g678_raw for component in value
            ),
        },
        "quaternion_vector_hypothesis": {
            "all_norms_at_most_one": all(norm <= 1.0 for norm in body_norms),
            "reconstructed_scalar": {
                "mean": _mean(hidden_scalars),
                "stdev": _stdev(hidden_scalars),
                "minimum": min(hidden_scalars) if hidden_scalars else math.nan,
                "maximum": max(hidden_scalars) if hidden_scalars else math.nan,
            },
            "implied_rotation_degrees": {
                "mean": _mean(implied_angles),
                "stdev": _stdev(implied_angles),
                "minimum": min(implied_angles) if implied_angles else math.nan,
                "maximum": max(implied_angles) if implied_angles else math.nan,
            },
        },
        "body_direction_spread": _direction_spread(body_vectors),
        "world_direction_spread": _direction_spread(world_vectors),
        "reference_accel_mean": _vector_mean(accel_vectors),
        "mean_quaternion_wxyz": _mean_quaternion(quaternions),
        "nearest_quaternion_age_us": {
            "mean_abs": _mean(
                abs(sample.quaternion_age_us)
                for sample in normal if sample.quaternion_age_us is not None
            ),
            "maximum_abs": max(
                (abs(sample.quaternion_age_us)
                 for sample in normal if sample.quaternion_age_us is not None),
                default=math.nan,
            ),
        },
        "orientation_alignment": {
            "slerp": sum(sample.orientation_method == "slerp" for sample in normal),
            "nearest": sum(sample.orientation_method == "nearest" for sample in normal),
            "unavailable": sum(
                sample.orientation_method == "unavailable" for sample in normal
            ),
            "mean_slerp_span_us": _mean(
                sample.orientation_span_us for sample in normal
                if sample.orientation_span_us is not None
            ),
        },
        "escalation_examples": [
            {
                "secondary_status": sample.secondary_status,
                "sensor_status": sample.sensor_status,
                "native": sample.record.native.hex().upper(),
            }
            for sample in escalation[:4]
        ],
        "unknown_fields": _unknown_stats(normal),
        "unknown_correlations": _unknown_correlations(normal),
        "unknown_bits": _unknown_bit_stats(normal),
        "unknown_bit_correlations": _unknown_bit_correlations(normal),
    }


def _format_vector(vector: Iterable[float]) -> str:
    return "[" + ", ".join(f"{value:+.9f}" for value in vector) + "]"


def render_summary(summary: dict[str, Any], correlation_limit: int = 12) -> str:
    interval = summary["combined_interval_ms"]
    mag_interval = summary["mag_interval_ms"]
    norm = summary["magnetic_norm"]
    age = summary["nearest_quaternion_age_us"]
    alignment = summary["orientation_alignment"]
    wire = summary["g678_wire"]
    q_candidate = summary["quaternion_vector_hypothesis"]
    q_scalar = q_candidate["reconstructed_scalar"]
    q_angle = q_candidate["implied_rotation_degrees"]
    body_spread = summary["body_direction_spread"]
    world_spread = summary["world_direction_spread"]
    lines = [
        f"MAGPROBE: {summary['source']} ({summary['capture_format']})",
        (
            f"records={summary['records']} dropped={summary['dropped']} "
            f"duration={summary['duration_ms']:.3f} ms | "
            f"0x1E={summary['length_30']} 0x28={summary['length_40']}"
        ),
        (
            f"combined cadence: {interval['mean']:.3f} ms mean "
            f"({1000.0 / interval['mean']:.3f} Hz), "
            f"{interval['minimum']:.3f}..{interval['maximum']:.3f} ms"
        ) if math.isfinite(interval["mean"]) else "combined cadence: unavailable",
        (
            f"0x28 cadence: {mag_interval['mean']:.3f} ms mean, "
            f"{mag_interval['minimum']:.3f}..{mag_interval['maximum']:.3f} ms"
        ) if math.isfinite(mag_interval["mean"]) else "0x28 cadence: unavailable",
        (
            f"0x28 forms: normal={summary['normal_28']} "
            f"escalation={summary['escalation_28']} other={summary['other_28']} "
            f"status={summary['statuses_28']}"
        ),
    ]
    if summary["normal_28"]:
        lines.extend([
            "body G6/G7/G8 mean:  "
            + _format_vector(summary["magnetic_body_mean"]),
            "world-rotated G6/G7/G8 mean: "
            + _format_vector(summary["magnetic_world_mean"]),
            (
                f"G6/G7/G8 norm: {norm['mean']:.9f} +/- {norm['stdev']:.9f} "
                f"({norm['minimum']:.9f}..{norm['maximum']:.9f})"
            ),
            (
                "wire integers: signed 22/22/20-bit, ranges "
                f"{wire['raw_minimum']}..{wire['raw_maximum']}; "
                f"all fit signed-int16={wire['all_samples_fit_signed_int16']}"
            ),
            (
                "if interpreted as a quaternion vector part: "
                f"scalar={q_scalar['mean']:.9f} +/- {q_scalar['stdev']:.9f}, "
                f"angle={q_angle['mean']:.4f} +/- {q_angle['stdev']:.4f} deg "
                f"({q_angle['minimum']:.4f}..{q_angle['maximum']:.4f})"
            ),
            (
                "direction spread: "
                f"body mean/max={body_spread['mean_degrees']:.4f}/"
                f"{body_spread['maximum_degrees']:.4f} deg, "
                f"world mean/max={world_spread['mean_degrees']:.4f}/"
                f"{world_spread['maximum_degrees']:.4f} deg"
            ),
            (
                f"nearest 0x1E alignment: {age['mean_abs']:.1f} us mean absolute, "
                f"{age['maximum_abs']:.1f} us maximum"
            ),
            (
                f"orientation alignment: slerp={alignment['slerp']} "
                f"nearest={alignment['nearest']} unavailable={alignment['unavailable']}"
            ),
            "reference 0x1E accel: " +
            _format_vector(summary["reference_accel_mean"]),
        ])
    lines.append("")
    lines.append("Unknown 0x28 lanes (payload-relative offsets):")
    for field in summary["unknown_fields"]:
        lines.append(
            f"  p{field['offset']:02d} mask=0x{field['mask']:02X} "
            f"unique={field['unique']:>3} range={field['minimum']:>3}.."
            f"{field['maximum']:<3} changed=0x{field['change_mask']:02X}"
        )
    correlations = summary["unknown_correlations"][:correlation_limit]
    if correlations:
        lines.extend([
            "",
            "Exploratory strongest unknown-byte correlations "
            "(ranking only; not semantic proof):",
        ])
        for item in correlations:
            lines.append(
                f"  p{item['offset']:02d}&0x{item['mask']:02X} -> "
                f"{item['target']:<12} r={item['r']:+.4f} "
                f"unique={item['unique']}"
            )
    bit_correlations = summary["unknown_bit_correlations"][:correlation_limit]
    if bit_correlations:
        lines.extend([
            "",
            "Exploratory strongest individual-bit correlations "
            "(ranking only; not semantic proof):",
        ])
        for item in bit_correlations:
            lines.append(
                f"  p{item['offset']:02d}.b{item['bit']} -> "
                f"{item['target']:<12} r={item['r']:+.4f} "
                f"ones={item['ones']}/{item['samples']}"
            )
    return "\n".join(lines)


def compare_summaries(baseline: dict[str, Any],
                      stimulus: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    q_angle = _quaternion_angle_degrees(
        baseline["mean_quaternion_wxyz"], stimulus["mean_quaternion_wxyz"]
    )
    accel_angle = _vector_angle_degrees(
        baseline["reference_accel_mean"], stimulus["reference_accel_mean"]
    )
    mag_angle = _vector_angle_degrees(
        baseline["magnetic_body_mean"], stimulus["magnetic_body_mean"]
    )
    world_mag_angle = _vector_angle_degrees(
        baseline["magnetic_world_mean"], stimulus["magnetic_world_mean"]
    )
    mag_delta = tuple(
        right - left for left, right in zip(
            baseline["magnetic_body_mean"], stimulus["magnetic_body_mean"]
        )
    )
    accel_norm_left = _vector_norm(baseline["reference_accel_mean"])
    accel_norm_right = _vector_norm(stimulus["reference_accel_mean"])
    accel_norm_change = (
        (accel_norm_right - accel_norm_left) / accel_norm_left * 100.0
        if accel_norm_left else math.nan
    )
    movement_warning = (
        (math.isfinite(q_angle) and q_angle > 1.0)
        or (math.isfinite(accel_angle) and accel_angle > 2.0)
        or (math.isfinite(accel_norm_change) and abs(accel_norm_change) > 3.0)
    )

    base_unknown = {
        (item["offset"], item["mask"]): item for item in baseline["unknown_fields"]
    }
    stimulus_unknown = {
        (item["offset"], item["mask"]): item for item in stimulus["unknown_fields"]
    }
    unknown_deltas = []
    for key in sorted(base_unknown):
        left = base_unknown[key]
        right = stimulus_unknown[key]
        pooled = math.sqrt((left["stdev"] ** 2 + right["stdev"] ** 2) / 2.0)
        mean_delta = right["mean"] - left["mean"]
        effect = mean_delta / pooled if pooled > 0 else (
            math.inf if mean_delta else 0.0
        )
        unknown_deltas.append({
            "offset": key[0],
            "mask": key[1],
            "mean_delta": mean_delta,
            "effect": effect,
            "baseline_unique": left["unique"],
            "stimulus_unique": right["unique"],
        })
    unknown_deltas.sort(
        key=lambda item: abs(item["effect"]) if math.isfinite(item["effect"])
        else math.inf,
        reverse=True,
    )
    base_bits = {
        (item["offset"], item["bit"]): item for item in baseline["unknown_bits"]
    }
    stimulus_bits = {
        (item["offset"], item["bit"]): item for item in stimulus["unknown_bits"]
    }
    unknown_bit_deltas = []
    for key in sorted(base_bits):
        left = base_bits[key]
        right = stimulus_bits[key]
        unknown_bit_deltas.append({
            "offset": key[0],
            "bit": key[1],
            "one_fraction_delta": right["one_fraction"] - left["one_fraction"],
            "baseline_one_fraction": left["one_fraction"],
            "stimulus_one_fraction": right["one_fraction"],
        })
    unknown_bit_deltas.sort(
        key=lambda item: abs(item["one_fraction_delta"]), reverse=True
    )

    result = {
        "baseline": baseline["source"],
        "stimulus": stimulus["source"],
        "quaternion_angle_degrees": q_angle,
        "accel_angle_degrees": accel_angle,
        "accel_norm_change_percent": accel_norm_change,
        "magnetic_body_angle_degrees": mag_angle,
        "magnetic_world_angle_degrees": world_mag_angle,
        "magnetic_body_delta": mag_delta,
        "movement_warning": movement_warning,
        "unknown_deltas": unknown_deltas,
        "unknown_bit_deltas": unknown_bit_deltas,
    }
    lines = [
        "MAGPROBE A/B",
        f"baseline: {baseline['source']}",
        f"stimulus: {stimulus['source']}",
        f"orientation mean delta: {q_angle:.4f} degrees",
        (
            f"accel mean delta: {accel_angle:.4f} degrees, "
            f"norm {accel_norm_change:+.3f}%"
        ),
        (
            f"body magnetic delta: {_format_vector(mag_delta)}, "
            f"direction {mag_angle:.4f} degrees"
        ),
        f"world magnetic direction delta: {world_mag_angle:.4f} degrees",
        (
            "movement gate: WARNING - controller pose changed enough to confound "
            "a magnetic-only experiment"
            if movement_warning else
            "movement gate: PASS - pose/gravity stayed within conservative limits"
        ),
        "",
        "Largest normalized changes in unexplained lanes:",
    ]
    for item in unknown_deltas[:12]:
        effect_text = (
            f"{item['effect']:+.3f} sigma"
            if math.isfinite(item["effect"]) else
            ("+inf" if item["mean_delta"] > 0 else "-inf")
        )
        lines.append(
            f"  p{item['offset']:02d}&0x{item['mask']:02X}: "
            f"mean delta={item['mean_delta']:+.4f}, effect={effect_text}, "
            f"unique {item['baseline_unique']}->{item['stimulus_unique']}"
        )
    lines.extend(["", "Largest unexplained individual-bit occupancy changes:"])
    for item in unknown_bit_deltas[:12]:
        lines.append(
            f"  p{item['offset']:02d}.b{item['bit']}: "
            f"{item['baseline_one_fraction']:.3f}->"
            f"{item['stimulus_one_fraction']:.3f} "
            f"(delta {item['one_fraction_delta']:+.3f})"
        )
    return "\n".join(lines), result


def compare_aba_summaries(
    baseline: dict[str, Any],
    stimulus: dict[str, Any],
    recovery: dict[str, Any],
    fraction: float = 0.5,
) -> tuple[str, dict[str, Any]]:
    """Compare an A/B/A experiment after subtracting linear session drift."""
    if not 0.0 <= fraction <= 1.0:
        raise MagprobeError("A/B/A interpolation fraction must be between 0 and 1")

    def interpolate(
        left: Iterable[float], right: Iterable[float]
    ) -> tuple[float, ...]:
        return tuple(a + (b - a) * fraction for a, b in zip(left, right))

    expected_q = _q_slerp(
        baseline["mean_quaternion_wxyz"],
        recovery["mean_quaternion_wxyz"],
        fraction,
    )
    q_residual = _quaternion_angle_degrees(
        expected_q, stimulus["mean_quaternion_wxyz"]
    )

    expected_accel = interpolate(
        baseline["reference_accel_mean"], recovery["reference_accel_mean"]
    )
    accel_residual_angle = _vector_angle_degrees(
        expected_accel, stimulus["reference_accel_mean"]
    )
    expected_accel_norm = _vector_norm(expected_accel)
    stimulus_accel_norm = _vector_norm(stimulus["reference_accel_mean"])
    accel_norm_residual = (
        (stimulus_accel_norm - expected_accel_norm)
        / expected_accel_norm * 100.0
        if expected_accel_norm else math.nan
    )

    expected_body = interpolate(
        baseline["magnetic_body_mean"], recovery["magnetic_body_mean"]
    )
    body_residual = tuple(
        observed - expected for observed, expected in zip(
            stimulus["magnetic_body_mean"], expected_body
        )
    )
    body_residual_angle = _vector_angle_degrees(
        expected_body, stimulus["magnetic_body_mean"]
    )
    expected_world = interpolate(
        baseline["magnetic_world_mean"], recovery["magnetic_world_mean"]
    )
    world_residual_angle = _vector_angle_degrees(
        expected_world, stimulus["magnetic_world_mean"]
    )

    summaries = (baseline, stimulus, recovery)
    unknown_maps = [{
        (item["offset"], item["mask"]): item for item in summary["unknown_fields"]
    } for summary in summaries]
    unknown_residuals = []
    for key in sorted(unknown_maps[0]):
        left, observed, right = (mapping[key] for mapping in unknown_maps)
        expected = left["mean"] + (
            right["mean"] - left["mean"]
        ) * fraction
        residual = observed["mean"] - expected
        pooled = math.sqrt(
            (left["stdev"] ** 2 + observed["stdev"] ** 2
             + right["stdev"] ** 2) / 3.0
        )
        effect = residual / pooled if pooled > 0 else (
            math.inf if residual else 0.0
        )
        unknown_residuals.append({
            "offset": key[0],
            "mask": key[1],
            "baseline_mean": left["mean"],
            "stimulus_mean": observed["mean"],
            "recovery_mean": right["mean"],
            "expected_midpoint": expected,
            "residual": residual,
            "effect": effect,
        })
    unknown_residuals.sort(
        key=lambda item: abs(item["effect"]) if math.isfinite(item["effect"])
        else math.inf,
        reverse=True,
    )

    bit_maps = [{
        (item["offset"], item["bit"]): item for item in summary["unknown_bits"]
    } for summary in summaries]
    bit_residuals = []
    for key in sorted(bit_maps[0]):
        left, observed, right = (mapping[key] for mapping in bit_maps)
        expected = left["one_fraction"] + (
            right["one_fraction"] - left["one_fraction"]
        ) * fraction
        residual = observed["one_fraction"] - expected
        bit_residuals.append({
            "offset": key[0],
            "bit": key[1],
            "baseline_fraction": left["one_fraction"],
            "stimulus_fraction": observed["one_fraction"],
            "recovery_fraction": right["one_fraction"],
            "expected_midpoint": expected,
            "residual": residual,
        })
    bit_residuals.sort(key=lambda item: abs(item["residual"]), reverse=True)

    pose_warning = (
        (math.isfinite(accel_residual_angle) and accel_residual_angle > 2.0)
        or (math.isfinite(accel_norm_residual)
            and abs(accel_norm_residual) > 3.0)
    )
    fusion_warning = math.isfinite(q_residual) and q_residual > 1.0
    result = {
        "baseline": baseline["source"],
        "stimulus": stimulus["source"],
        "recovery": recovery["source"],
        "interpolation_fraction": fraction,
        "quaternion_midpoint_residual_degrees": q_residual,
        "accel_midpoint_residual_degrees": accel_residual_angle,
        "accel_norm_midpoint_residual_percent": accel_norm_residual,
        "magnetic_body_midpoint_residual_degrees": body_residual_angle,
        "magnetic_body_midpoint_residual": body_residual,
        "magnetic_world_midpoint_residual_degrees": world_residual_angle,
        "pose_warning": pose_warning,
        "fusion_warning": fusion_warning,
        "unknown_midpoint_residuals": unknown_residuals,
        "unknown_bit_midpoint_residuals": bit_residuals,
    }
    lines = [
        "MAGPROBE A/B/A DRIFT-ADJUSTED",
        f"baseline: {baseline['source']}",
        f"stimulus: {stimulus['source']}",
        f"recovery: {recovery['source']}",
        f"stimulus time fraction between A and A': {fraction:.6f}",
        (
            "quaternion midpoint residual: "
            f"{q_residual:.4f} degrees"
        ),
        (
            "accel midpoint residual: "
            f"{accel_residual_angle:.4f} degrees, "
            f"norm {accel_norm_residual:+.3f}%"
        ),
        (
            "body G6/G7/G8 midpoint residual: "
            f"{_format_vector(body_residual)}, "
            f"direction {body_residual_angle:.4f} degrees"
        ),
        (
            "world G6/G7/G8 midpoint residual: "
            f"{world_residual_angle:.4f} degrees"
        ),
        (
            "pose gate: WARNING - acceleration indicates physical movement"
            if pose_warning else
            "pose gate: PASS - acceleration stayed within conservative limits"
        ),
        (
            "fusion gate: WARNING - native quaternion is not linear across A/B/A"
            if fusion_warning else
            "fusion gate: PASS - native quaternion stayed near the A/A midpoint"
        ),
        "",
        "Largest drift-adjusted unexplained-lane residuals:",
    ]
    for item in unknown_residuals[:12]:
        effect_text = (
            f"{item['effect']:+.3f} sigma"
            if math.isfinite(item["effect"]) else
            ("+inf" if item["residual"] > 0 else "-inf")
        )
        lines.append(
            f"  p{item['offset']:02d}&0x{item['mask']:02X}: "
            f"A={item['baseline_mean']:.3f}, "
            f"B={item['stimulus_mean']:.3f}, "
            f"A'={item['recovery_mean']:.3f}, "
            f"residual={item['residual']:+.4f}, effect={effect_text}"
        )
    lines.extend([
        "",
        "Largest drift-adjusted unexplained-bit residuals:",
    ])
    for item in bit_residuals[:12]:
        lines.append(
            f"  p{item['offset']:02d}.b{item['bit']}: "
            f"A={item['baseline_fraction']:.3f}, "
            f"B={item['stimulus_fraction']:.3f}, "
            f"A'={item['recovery_fraction']:.3f}, "
            f"residual={item['residual']:+.3f}"
        )
    return "\n".join(lines), result


def summarize_corpus(captures: list[MotionCapture]) -> dict[str, Any]:
    if not captures:
        raise MagprobeError("corpus requires at least one capture")
    summaries = [summarize(capture) for capture in captures]
    usable = [summary for summary in summaries if summary["normal_28"]]
    if not usable:
        raise MagprobeError("corpus contains no normal 0x28 PDUs")

    samples = [
        sample
        for capture in captures
        for sample in decode_magnet_samples(capture)
        if sample.magnetic_body is not None
    ]
    norms = [_vector_norm(sample.magnetic_body) for sample in samples]
    raw = [decode_g678_raw(sample.record.native) for sample in samples]
    implied_angles = [
        math.degrees(2.0 * math.asin(min(1.0, norm))) for norm in norms
    ]
    moving = [
        summary for summary in usable
        if summary["body_direction_spread"]["maximum_degrees"] >= 5.0
    ]
    improved = [
        summary for summary in moving
        if (
            summary["world_direction_spread"]["mean_degrees"]
            < summary["body_direction_spread"]["mean_degrees"]
        )
    ]
    result = {
        "captures": len(captures),
        "usable_captures": len(usable),
        "records": sum(summary["records"] for summary in summaries),
        "dropped": sum(summary["dropped"] for summary in summaries),
        "length_30": sum(summary["length_30"] for summary in summaries),
        "length_40": sum(summary["length_40"] for summary in summaries),
        "normal_28": len(samples),
        "escalation_28": sum(summary["escalation_28"] for summary in summaries),
        "other_28": sum(summary["other_28"] for summary in summaries),
        "g678_norm": {
            "mean": _mean(norms),
            "stdev": _stdev(norms),
            "minimum": min(norms),
            "maximum": max(norms),
        },
        "g678_raw_minimum": [
            min(value[axis] for value in raw) for axis in range(3)
        ],
        "g678_raw_maximum": [
            max(value[axis] for value in raw) for axis in range(3)
        ],
        "all_samples_fit_signed_int16": all(
            -32768 <= component <= 32767
            for value in raw for component in value
        ),
        "all_norms_at_most_one": all(norm <= 1.0 for norm in norms),
        "quaternion_vector_implied_angle_degrees": {
            "mean": _mean(implied_angles),
            "stdev": _stdev(implied_angles),
            "minimum": min(implied_angles),
            "maximum": max(implied_angles),
        },
        "moving_captures": len(moving),
        "world_rotation_reduced_mean_spread": len(improved),
        "sessions": [
            {
                "source": summary["source"],
                "capture_format": summary["capture_format"],
                "records": summary["records"],
                "dropped": summary["dropped"],
                "length_30": summary["length_30"],
                "length_40": summary["length_40"],
                "normal_28": summary["normal_28"],
                "g678_norm": summary["magnetic_norm"],
                "body_spread_degrees": summary["body_direction_spread"],
                "world_spread_degrees": summary["world_direction_spread"],
                "implied_angle_degrees": summary[
                    "quaternion_vector_hypothesis"
                ]["implied_rotation_degrees"],
            }
            for summary in summaries
        ],
        "interpretation": {
            "raw_ak09919c_wire_format_rejected": not all(
                -32768 <= component <= 32767
                for value in raw for component in value
            ),
            "quaternion_vector_representation_numerically_possible": all(
                norm <= 1.0 for norm in norms
            ),
            "note": (
                "Legacy metric only: the G6/G7/G8 source ranges cross packed catch-up "
                "gyro and acceleration samples. Their large signed aliases and implied "
                "quaternion do not represent independent wire fields."
            ),
        },
    }
    return result


def render_corpus(summary: dict[str, Any]) -> str:
    norm = summary["g678_norm"]
    angle = summary["quaternion_vector_implied_angle_degrees"]
    lines = [
        "MAGPROBE CORPUS",
        (
            f"captures={summary['captures']} usable={summary['usable_captures']} "
            f"records={summary['records']} dropped={summary['dropped']}"
        ),
        (
            f"0x1E={summary['length_30']} 0x28={summary['length_40']} | "
            f"normal={summary['normal_28']} escalation={summary['escalation_28']} "
            f"other={summary['other_28']}"
        ),
        (
            f"G6/G7/G8 norm: {norm['mean']:.9f} +/- {norm['stdev']:.9f} "
            f"({norm['minimum']:.9f}..{norm['maximum']:.9f})"
        ),
        (
            "raw signed 22/22/20-bit ranges: "
            f"{summary['g678_raw_minimum']}..{summary['g678_raw_maximum']}; "
            f"all fit signed-int16={summary['all_samples_fit_signed_int16']}"
        ),
        (
            "quaternion-vector candidate angle: "
            f"{angle['mean']:.4f} +/- {angle['stdev']:.4f} deg "
            f"({angle['minimum']:.4f}..{angle['maximum']:.4f}); "
            f"all norms <= 1={summary['all_norms_at_most_one']}"
        ),
        (
            "moving-session world-rotation check: reduced mean directional spread in "
            f"{summary['world_rotation_reduced_mean_spread']}/"
            f"{summary['moving_captures']} captures"
        ),
        "",
        "Per capture:",
    ]
    for session in summary["sessions"]:
        session_norm = session["g678_norm"]
        body = session["body_spread_degrees"]
        world = session["world_spread_degrees"]
        lines.append(
            f"  {session['source']} [{session['capture_format']}]: "
            f"0x1E/0x28={session['length_30']}/{session['length_40']}, "
            f"normal={session['normal_28']}, "
            f"norm={session_norm['mean']:.6f}, "
            f"spread body/world={body['mean_degrees']:.3f}/"
            f"{world['mean_degrees']:.3f} deg"
        )
    lines.extend([
        "",
        "Evidence boundary:",
        "  " + summary["interpretation"]["note"],
    ])
    return "\n".join(lines)


def summarize_epochs(captures: list[MotionCapture]) -> dict[str, Any]:
    if len(captures) < 2:
        raise MagprobeError("epochs requires at least two captures")
    summaries = [summarize(capture) for capture in captures]
    if any(not summary["normal_28"] or not summary["length_30"]
           for summary in summaries):
        raise MagprobeError("every epoch needs both 0x1E and normal 0x28 PDUs")

    baseline = summaries[0]
    epochs = []
    for summary in summaries:
        vector = summary["magnetic_body_mean"]
        scalar = math.sqrt(max(0.0, 1.0 - sum(value * value for value in vector)))
        candidate = (scalar, vector[0], vector[1], vector[2])
        base_vector = baseline["magnetic_body_mean"]
        base_scalar = math.sqrt(
            max(0.0, 1.0 - sum(value * value for value in base_vector))
        )
        base_candidate = (
            base_scalar, base_vector[0], base_vector[1], base_vector[2]
        )
        epochs.append({
            "source": summary["source"],
            "records": summary["records"],
            "length_30": summary["length_30"],
            "normal_28": summary["normal_28"],
            "live_quaternion_delta_degrees": _quaternion_angle_degrees(
                baseline["mean_quaternion_wxyz"],
                summary["mean_quaternion_wxyz"],
            ),
            "g678_direction_delta_degrees": _vector_angle_degrees(
                base_vector, vector
            ),
            "magneto_quaternion_candidate_delta_degrees":
                _quaternion_angle_degrees(base_candidate, candidate),
            "g678_norm": summary["magnetic_norm"]["mean"],
            "g678_direction_spread_degrees":
                summary["body_direction_spread"]["mean_degrees"],
            "live_quaternion_wxyz": summary["mean_quaternion_wxyz"],
            "magneto_quaternion_candidate_wxyz": candidate,
        })

    live_delta = epochs[-1]["live_quaternion_delta_degrees"]
    candidate_delta = epochs[-1][
        "magneto_quaternion_candidate_delta_degrees"
    ]
    return {
        "captures": len(captures),
        "records": sum(summary["records"] for summary in summaries),
        "dropped": sum(summary["dropped"] for summary in summaries),
        "epochs": epochs,
        "final_live_delta_degrees": live_delta,
        "final_candidate_delta_degrees": candidate_delta,
        "stability_ratio": (
            live_delta / candidate_delta
            if candidate_delta > 0.0 else math.inf
        ),
        "interpretation": {
            "candidate_more_stable_than_live": candidate_delta < live_delta,
            "note": (
                "A stationary multi-epoch stability split supports G6/G7/G8 "
                "as a corrected/reference quaternion vector, but does not "
                "identify the correction source or physical sensor."
            ),
        },
    }


def render_epochs(summary: dict[str, Any]) -> str:
    lines = [
        "MAGPROBE STATIONARY EPOCHS",
        (
            f"captures={summary['captures']} records={summary['records']} "
            f"dropped={summary['dropped']}"
        ),
        "epoch live-q-delta G678-dir-delta candidate-q-delta norm spread",
    ]
    for index, epoch in enumerate(summary["epochs"]):
        lines.append(
            f"{index:>5} "
            f"{epoch['live_quaternion_delta_degrees']:>12.6f} "
            f"{epoch['g678_direction_delta_degrees']:>15.6f} "
            f"{epoch['magneto_quaternion_candidate_delta_degrees']:>17.6f} "
            f"{epoch['g678_norm']:.9f} "
            f"{epoch['g678_direction_spread_degrees']:.6f}"
        )
    lines.extend([
        "",
        (
            "final stability split: live quaternion "
            f"{summary['final_live_delta_degrees']:.6f} deg vs candidate "
            f"{summary['final_candidate_delta_degrees']:.6f} deg "
            f"({summary['stability_ratio']:.1f}x)"
        ),
        "Evidence boundary:",
        "  " + summary["interpretation"]["note"],
    ])
    return "\n".join(lines)


def summarize_rawmag(capture: RawMagCapture) -> dict[str, Any]:
    axes = [record.axes for record in capture.records]
    norms = [_vector_norm(tuple(float(value) for value in sample)) for sample in axes]
    changes = [
        tuple(current[axis] - previous[axis] for axis in range(3))
        for previous, current in zip(axes, axes[1:])
    ]
    axis_stats = []
    for axis in range(3):
        values = [sample[axis] for sample in axes]
        axis_stats.append({
            "minimum": min(values),
            "maximum": max(values),
            "mean": _mean(values),
            "stdev": _stdev(values),
            "span": max(values) - min(values),
        })
    return {
        "source": capture.source_name,
        "records": len(capture.records),
        "dropped": capture.dropped,
        "duration_us": capture.records[-1].elapsed_us,
        "nonzero_records": sum(any(value != 0 for value in sample) for sample in axes),
        "unique_vectors": len(set(axes)),
        "axes": axis_stats,
        "norm": {
            "minimum": min(norms),
            "maximum": max(norms),
            "mean": _mean(norms),
            "stdev": _stdev(norms),
        },
        "maximum_step": [
            max((abs(change[axis]) for change in changes), default=0)
            for axis in range(3)
        ],
        "interpretation": {
            "channel_present": True,
            "nonzero_data": any(any(value != 0 for value in sample) for sample in axes),
            "varying_data": len(set(axes)) > 1,
            "note": (
                "These are the independent signed-int16 lanes at report offsets "
                "25/27/29 under the experimental 0x94 feature profile. Their presence "
                "does not yet identify the physical sensor or prove calibration."
            ),
        },
    }


def render_rawmag(summary: dict[str, Any]) -> str:
    lines = [
        "RAW MAGNETOMETER CHANNEL",
        (
            f"records={summary['records']} dropped={summary['dropped']} "
            f"duration={summary['duration_us'] / 1_000_000.0:.3f}s "
            f"nonzero={summary['nonzero_records']} "
            f"unique={summary['unique_vectors']}"
        ),
    ]
    for index, stats in enumerate(summary["axes"]):
        lines.append(
            f"axis {index}: mean={stats['mean']:.3f} "
            f"stdev={stats['stdev']:.3f} "
            f"range={stats['minimum']}..{stats['maximum']} "
            f"span={stats['span']} max-step={summary['maximum_step'][index]}"
        )
    norm = summary["norm"]
    lines.extend([
        (
            f"norm: mean={norm['mean']:.3f} stdev={norm['stdev']:.3f} "
            f"range={norm['minimum']:.3f}..{norm['maximum']:.3f}"
        ),
        "",
        "Evidence boundary:",
        "  " + summary["interpretation"]["note"],
    ])
    return "\n".join(lines)


def write_csv(path: str, capture: MotionCapture) -> None:
    samples = decode_magnet_samples(capture)
    destination = Path(path)
    with destination.open("w", encoding="utf-8", newline="") as stream:
        fieldnames = [
            "elapsed_us", "t_us", "tick", "sub_index", "secondary_status",
            "sensor_status", "reference_accel_x", "reference_accel_y",
            "reference_accel_z", "unknown_middle_s32_0",
            "unknown_middle_s32_1", "unknown_middle_s32_2",
            "mag_body_x", "mag_body_y", "mag_body_z",
            "mag_body_norm", "mag_world_x", "mag_world_y", "mag_world_z",
            "orientation_method", "orientation_span_us", "nearest_q_age_us",
            "q_w", "q_x", "q_y", "q_z",
            "unknown_lead_hex", "unknown_p28", "unknown_p29_low",
            "unknown_p32_bits23", "unknown_p35_bits23",
            "unknown_or_escalation_p38", "unknown_p39", "native_hex",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for sample in samples:
            data = sample.record.native
            magnetic = sample.magnetic_body or (math.nan, math.nan, math.nan)
            world = sample.magnetic_world or (math.nan, math.nan, math.nan)
            quaternion = (
                sample.orientation_quaternion_wxyz
                if sample.orientation_quaternion_wxyz is not None
                else (math.nan, math.nan, math.nan, math.nan)
            )
            writer.writerow({
                "elapsed_us": sample.record.elapsed_us,
                "t_us": sample.record.t_us,
                "tick": sample.tick,
                "sub_index": sample.sub_index,
                "secondary_status": f"0x{sample.secondary_status:02X}",
                "sensor_status": f"0x{sample.sensor_status:02X}",
                "reference_accel_x": (
                    sample.reference_accel[0]
                    if sample.reference_accel is not None else math.nan
                ),
                "reference_accel_y": (
                    sample.reference_accel[1]
                    if sample.reference_accel is not None else math.nan
                ),
                "reference_accel_z": (
                    sample.reference_accel[2]
                    if sample.reference_accel is not None else math.nan
                ),
                "unknown_middle_s32_0": sample.unknown_middle_s32[0],
                "unknown_middle_s32_1": sample.unknown_middle_s32[1],
                "unknown_middle_s32_2": sample.unknown_middle_s32[2],
                "mag_body_x": magnetic[0],
                "mag_body_y": magnetic[1],
                "mag_body_z": magnetic[2],
                "mag_body_norm": _vector_norm(magnetic),
                "mag_world_x": world[0],
                "mag_world_y": world[1],
                "mag_world_z": world[2],
                "orientation_method": sample.orientation_method,
                "orientation_span_us": sample.orientation_span_us,
                "nearest_q_age_us": sample.quaternion_age_us,
                "q_w": quaternion[0],
                "q_x": quaternion[1],
                "q_y": quaternion[2],
                "q_z": quaternion[3],
                "unknown_lead_hex": data[4:16].hex().upper(),
                "unknown_p28": f"0x{data[28]:02X}",
                "unknown_p29_low": f"0x{data[29] & 0x0F:02X}",
                "unknown_p32_bits23": f"0x{data[32] & 0x0C:02X}",
                "unknown_p35_bits23": f"0x{data[35] & 0x0C:02X}",
                "unknown_or_escalation_p38": f"0x{data[38]:02X}",
                "unknown_p39": f"0x{data[39]:02X}",
                "native_hex": data.hex().upper(),
            })


def _json_safe(value: Any) -> Any:
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Reproduce legacy Pro Controller 2 magnet-probe statistics; "
            "use ns2_motion_reference.py for current 0x28 field semantics."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze = subparsers.add_parser("analyze", help="summarize one motionpair capture")
    analyze.add_argument("capture", help="motionpair JSONL path, or - for stdin")
    analyze.add_argument("--csv", help="write one decoded row per 0x28 PDU")
    analyze.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    analyze.add_argument(
        "--correlations", type=int, default=12,
        help="number of exploratory unknown-lane correlations to print (default: 12)",
    )

    compare = subparsers.add_parser(
        "compare", help="compare stationary baseline and stimulus captures"
    )
    compare.add_argument("baseline")
    compare.add_argument("stimulus")
    compare.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    aba = subparsers.add_parser(
        "aba",
        help="compare baseline/stimulus/recovery after subtracting linear drift",
    )
    aba.add_argument("baseline")
    aba.add_argument("stimulus")
    aba.add_argument("recovery")
    aba.add_argument(
        "--fraction",
        type=float,
        default=0.5,
        help="stimulus time fraction between baseline=0 and recovery=1",
    )
    aba.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    corpus = subparsers.add_parser(
        "corpus",
        help="aggregate motionpair and blecap captures without comparing session yaw epochs",
    )
    corpus.add_argument("captures", nargs="+")
    corpus.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    rawmag = subparsers.add_parser(
        "rawmag",
        help="summarize handle-0x000A signed-int16 lanes from a 0x94 blecap",
    )
    rawmag.add_argument("capture", help="blecap JSONL path")
    rawmag.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    epochs = subparsers.add_parser(
        "epochs",
        help="compare stationary captures for live-vs-reference quaternion drift",
    )
    epochs.add_argument("captures", nargs="+")
    epochs.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "analyze":
            capture = load_capture(args.capture)
            summary = summarize(capture)
            if args.csv:
                write_csv(args.csv, capture)
            if args.json:
                print(json.dumps(_json_safe(summary), indent=2, sort_keys=True))
            else:
                print(render_summary(summary, max(0, args.correlations)))
                if args.csv:
                    print(f"\nDecoded CSV written to {args.csv}")
            return 0

        if args.command == "corpus":
            corpus = summarize_corpus([
                load_capture(path) for path in args.captures
            ])
            if args.json:
                print(json.dumps(_json_safe(corpus), indent=2, sort_keys=True))
            else:
                print(render_corpus(corpus))
            return 0

        if args.command == "rawmag":
            path = Path(args.capture)
            with path.open("r", encoding="utf-8") as stream:
                rawmag = summarize_rawmag(
                    read_rawmag_ble_capture(stream, str(path))
                )
            if args.json:
                print(json.dumps(_json_safe(rawmag), indent=2, sort_keys=True))
            else:
                print(render_rawmag(rawmag))
            return 0

        if args.command == "epochs":
            epochs = summarize_epochs([
                load_capture(path) for path in args.captures
            ])
            if args.json:
                print(json.dumps(_json_safe(epochs), indent=2, sort_keys=True))
            else:
                print(render_epochs(epochs))
            return 0

        if args.command == "compare":
            baseline = summarize(load_capture(args.baseline))
            stimulus = summarize(load_capture(args.stimulus))
            if not baseline["normal_28"] or not stimulus["normal_28"]:
                raise MagprobeError("both captures need at least one normal 0x28 PDU")
            rendered, comparison = compare_summaries(baseline, stimulus)
            if args.json:
                print(json.dumps(_json_safe(comparison), indent=2, sort_keys=True))
            else:
                print(rendered)
            return 1 if comparison["movement_warning"] else 0

        if args.command == "aba":
            baseline = summarize(load_capture(args.baseline))
            stimulus = summarize(load_capture(args.stimulus))
            recovery = summarize(load_capture(args.recovery))
            if not all(
                summary["normal_28"]
                for summary in (baseline, stimulus, recovery)
            ):
                raise MagprobeError(
                    "baseline, stimulus, and recovery each need a normal 0x28 PDU"
                )
            rendered, comparison = compare_aba_summaries(
                baseline, stimulus, recovery, args.fraction
            )
            if args.json:
                print(json.dumps(_json_safe(comparison), indent=2, sort_keys=True))
            else:
                print(rendered)
            return 1 if (
                comparison["pose_warning"] or comparison["fusion_warning"]
            ) else 0

        raise MagprobeError(f"unsupported command: {args.command}")
    except (MagprobeError, OSError) as exc:
        print(f"magprobe: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
