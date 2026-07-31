#!/usr/bin/env python3
"""Analyze genuine Switch 2 controller raw and packed motion PCAPNG captures.

The reference capture set exposes two useful Pro Controller 2 paths:

* handle 0x000A / report 0x05: one ordinary 18-byte raw IMU sample;
* handle 0x000E / report 0x09: the variable-length Nintendo motion PDU.

This tool deliberately keeps PCAP extraction separate from decoding. TShark is
used only to recover ATT notification values; all report parsing is pure Python
and covered by host tests.
"""

from __future__ import annotations

import argparse
import bisect
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


class MotionReferenceError(RuntimeError):
    """Raised for malformed captures, unavailable tools, or invalid reports."""


@dataclass(frozen=True)
class Notification:
    time_seconds: float
    value: bytes


@dataclass(frozen=True)
class RawImuSample:
    timestamp_us: int
    temperature_raw: int
    accel: tuple[int, int, int]
    gyro: tuple[int, int, int]

    @property
    def temperature_c(self) -> float:
        # ICM-42670-P register-data conversion.
        return self.temperature_raw / 128.0 + 25.0


@dataclass(frozen=True)
class Motion40Sample:
    tick: int
    elapsed_nibble: int
    elapsed_ticks: int
    sensor_status: int
    tick_delta: int | None
    layout: str
    accel: tuple[tuple[int, int, int], ...]
    gyro: tuple[tuple[int, int, int], ...]
    packing_mode: int
    prefix_lane2_low2: int
    prefix_carrier: tuple[int, int, int]
    prefix_widths: tuple[int, int, int]
    prefix_precision_shift: int
    tail_value: int
    tail_width: int


@dataclass(frozen=True)
class Motion30Orientation:
    """Decoded length-0x1E carrier and legacy quaternion candidate.

    Genuine chart-transition captures prove that ``state`` is not a strict
    smallest-three omitted-component selector.  ``quaternion_wxyz`` remains
    available for compatibility with the stable-state diagnostic and the
    validated production approximation, but callers must check
    ``strict_unit_valid`` before treating it as a literal genuine quaternion.

    ``canonical_state0_carrier`` is a legacy name for state-0-boundary
    projections observed at ``0<->1`` and ``0<->3`` seams. A later rapid
    ``3->1->0`` capture proved that these projections are not one globally
    composable unsigned chart map. State 2 remains intentionally unresolved.
    """

    state: int
    carrier_raw: tuple[int, int, int]
    retained: tuple[float, float, float]
    quaternion_wxyz: tuple[float, float, float, float]
    canonical_state0_carrier: tuple[float, float, float] | None
    retained_energy: float
    strict_unit_valid: bool


@dataclass(frozen=True)
class Motion40PrefixOrientation:
    """History-unwrapped orientation carried by a length-0x28 prefix."""

    reference_state: int
    retained: tuple[float, float, float]
    quaternion_wxyz: tuple[float, float, float, float]
    modular_windows: tuple[int, int, int]


@dataclass(frozen=True)
class Tail16Fields:
    """Two Q3 temperature samples packed into a normal/high-rate 0x28 tail."""

    temperature_integer_raw: int
    temperature_a_fraction3: int
    temperature_b_fraction3: int
    temperature_a_q3: int
    temperature_b_q3: int

    @property
    def fractions_equal(self) -> bool:
        return self.temperature_a_fraction3 == self.temperature_b_fraction3

    @property
    def temperature_a_raw(self) -> float:
        return self.temperature_a_q3 / 8.0

    @property
    def temperature_b_raw(self) -> float:
        return self.temperature_b_q3 / 8.0

    @property
    def temperature_a_c(self) -> float:
        return 25.0 + self.temperature_a_q3 / 1024.0

    @property
    def temperature_b_c(self) -> float:
        return 25.0 + self.temperature_b_q3 / 1024.0


def _signed_bits(data: bytes, offset: int, width: int) -> int:
    if offset < 0 or width <= 0 or offset + width > len(data) * 8:
        raise MotionReferenceError(
            f"bit field [{offset}, {offset + width}) exceeds {len(data) * 8}-bit payload"
        )
    value = (int.from_bytes(data, "little") >> offset) & ((1 << width) - 1)
    sign = 1 << (width - 1)
    return value - (1 << width) if value & sign else value


def _unsigned_bits(data: bytes, offset: int, width: int) -> int:
    if offset < 0 or width <= 0 or offset + width > len(data) * 8:
        raise MotionReferenceError(
            f"bit field [{offset}, {offset + width}) exceeds {len(data) * 8}-bit payload"
        )
    return (int.from_bytes(data, "little") >> offset) & ((1 << width) - 1)


def _vector(data: bytes, offset: int, width: int) -> tuple[int, int, int]:
    return tuple(
        _signed_bits(data, offset + axis * width, width) for axis in range(3)
    )


def decode_temperature_tail16_value(tail_value: int) -> Tail16Fields:
    """Decode two Q3 ICM temperature samples sharing a signed integer part."""

    if tail_value < 0 or tail_value > 0xFFFF:
        raise MotionReferenceError(
            f"tail16 value must fit in 16 bits, got {tail_value}"
        )
    integer_unsigned = tail_value >> 6
    integer_raw = (
        integer_unsigned - (1 << 10)
        if integer_unsigned & (1 << 9)
        else integer_unsigned
    )
    fraction_a = tail_value & 0x07
    fraction_b = (tail_value >> 3) & 0x07
    return Tail16Fields(
        temperature_integer_raw=integer_raw,
        temperature_a_fraction3=fraction_a,
        temperature_b_fraction3=fraction_b,
        temperature_a_q3=integer_raw * 8 + fraction_a,
        temperature_b_q3=integer_raw * 8 + fraction_b,
    )


def decode_tail16_fields(sample: Motion40Sample) -> Tail16Fields:
    """Decode the temperature pair in a normal/high-rate length-0x28 tail."""

    if sample.tail_width != 16:
        raise MotionReferenceError(
            f"tail16 temperature requires a 16-bit tail, got {sample.tail_width}"
        )
    return decode_temperature_tail16_value(sample.tail_value)


def decode_report05_raw_imu(report: bytes) -> RawImuSample:
    """Decode the report-0x05 raw IMU sample at offsets 0x2A..0x3B."""

    if len(report) < 0x3C:
        raise MotionReferenceError(
            f"report 0x05 requires at least 60 bytes, got {len(report)}"
        )
    motion = report[0x2A:0x3C]
    return RawImuSample(
        timestamp_us=int.from_bytes(motion[0:4], "little"),
        temperature_raw=int.from_bytes(motion[4:6], "little", signed=True),
        accel=tuple(
            int.from_bytes(motion[offset:offset + 2], "little", signed=True)
            for offset in (6, 8, 10)
        ),
        gyro=tuple(
            int.from_bytes(motion[offset:offset + 2], "little", signed=True)
            for offset in (12, 14, 16)
        ),
    )


def decode_motion40(pdu: bytes, previous_tick: int | None) -> Motion40Sample:
    """Decode a 40-byte native PDU using the reference multi-sample map.

    The controller changes packing at exact cadence boundaries:

    * 0--10 ticks: high-rate 22-bit fixed-point form;
    * 11--14 ticks: normal mixed 13/14-bit form;
    * 15+ ticks: catch-up mixed 13/14/16-bit form.

    The prefix begins with packing mode 3. High-rate then uses
    ``carrier0 s24, carrier1 s23, carrier2 s25``; the other layouts use
    ``carrier0 s22, carrier1 s21, carrier2 s23``. Carrier 2 is split on the
    wire: its low two bits precede its signed high 23 or 21 bits. Those two
    bits were formerly misidentified as a separate state field.

    Byte 1's high nibble carries elapsed bits 0..3 and byte 2 carries elapsed
    bits 4..11. This self-contained 12-bit elapsed count selects the layout
    even when the capture dropped a preceding notification. ``tick_delta`` is
    retained as an independent capture-integrity check.
    """

    if len(pdu) != 0x28:
        raise MotionReferenceError(f"motion PDU must be 40 bytes, got {len(pdu)}")

    tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
    elapsed_nibble = pdu[1] >> 4
    elapsed_ticks = (pdu[2] << 4) | elapsed_nibble
    sensor_status = pdu[3]
    tick_delta = None if previous_tick is None else (tick - previous_tick) & 0x0FFF
    payload = pdu[4:40]
    if elapsed_ticks >= 15:
        # Catch-up layout: accel14, gyro16, accel13, gyro16, accel14.
        # The middle acceleration sample is half-resolution. Both gyros are
        # stored at four times the ordinary 16.4-count/dps scale. Bit 287 is
        # the byte-alignment remainder and is zero throughout the corpus.
        return Motion40Sample(
            tick=tick,
            elapsed_nibble=elapsed_nibble,
            elapsed_ticks=elapsed_ticks,
            sensor_status=sensor_status,
            tick_delta=tick_delta,
            layout="catchup",
            accel=(
                _vector(payload, 68, 14),
                _vector(payload, 158, 13),
                _vector(payload, 245, 14),
            ),
            gyro=(
                _vector(payload, 110, 16),
                _vector(payload, 197, 16),
            ),
            packing_mode=_unsigned_bits(payload, 0, 2),
            prefix_lane2_low2=_unsigned_bits(payload, 45, 2),
            prefix_carrier=(
                _signed_bits(payload, 2, 22),
                _signed_bits(payload, 24, 21),
                _signed_bits(payload, 45, 23),
            ),
            prefix_widths=(22, 21, 23),
            prefix_precision_shift=0,
            tail_value=_unsigned_bits(payload, 287, 1),
            tail_width=1,
        )

    if elapsed_ticks < 11:
        # High-rate layout: accel22, gyro22, accel22. All vectors carry eight
        # fractional bits, so divide wire values by 256 to recover ordinary
        # ICM counts. Every high-rate carrier lane has two more fractional
        # bits than its normal/catch-up counterpart. The 16-bit tail carries
        # two Q3 temperature samples.
        return Motion40Sample(
            tick=tick,
            elapsed_nibble=elapsed_nibble,
            elapsed_ticks=elapsed_ticks,
            sensor_status=sensor_status,
            tick_delta=tick_delta,
            layout="high_rate",
            accel=(
                _vector(payload, 74, 22),
                _vector(payload, 206, 22),
            ),
            gyro=(_vector(payload, 140, 22),),
            packing_mode=_unsigned_bits(payload, 0, 2),
            prefix_lane2_low2=_unsigned_bits(payload, 49, 2),
            prefix_carrier=(
                _signed_bits(payload, 2, 24),
                _signed_bits(payload, 26, 23),
                _signed_bits(payload, 49, 25),
            ),
            prefix_widths=(24, 23, 25),
            prefix_precision_shift=2,
            tail_value=_unsigned_bits(payload, 272, 16),
            tail_width=16,
        )

    # Normal layout: accel14, gyro13, accel13, gyro14, accel14.
    # The middle gyro and accel samples are half-resolution.
    return Motion40Sample(
        tick=tick,
        elapsed_nibble=elapsed_nibble,
        elapsed_ticks=elapsed_ticks,
        sensor_status=sensor_status,
        tick_delta=tick_delta,
        layout="normal",
        accel=(
            _vector(payload, 68, 14),
            _vector(payload, 149, 13),
            _vector(payload, 230, 14),
        ),
        gyro=(
            _vector(payload, 110, 13),
            _vector(payload, 188, 14),
        ),
        packing_mode=_unsigned_bits(payload, 0, 2),
        prefix_lane2_low2=_unsigned_bits(payload, 45, 2),
        prefix_carrier=(
            _signed_bits(payload, 2, 22),
            _signed_bits(payload, 24, 21),
            _signed_bits(payload, 45, 23),
        ),
        prefix_widths=(22, 21, 23),
        prefix_precision_shift=0,
        tail_value=_unsigned_bits(payload, 272, 16),
        tail_width=16,
    )


def icm_fifo_header_audit(pdu: bytes) -> dict[str, object]:
    """Test exact ICM-42670-P Packet-3/Packet-4 header placements.

    Packet 4 is 20 bytes and requires header high nibble 0x7 (accel, gyro,
    20-bit extension). Packet 3 is 16 bytes and requires high nibble 0x6
    (accel and gyro, no 20-bit extension).
    """

    if len(pdu) != 0x28:
        raise MotionReferenceError(f"motion PDU must be 40 bytes, got {len(pdu)}")
    packet4 = (pdu[0] & 0xF0) == 0x70 and (pdu[20] & 0xF0) == 0x70
    packet3_offsets = tuple(
        offset
        for offset in range(9)
        if (pdu[offset] & 0xF0) == 0x60
        and (pdu[offset + 16] & 0xF0) == 0x60
    )
    return {
        "two_packet4_at_0_20": packet4,
        "two_packet3_offsets": packet3_offsets,
    }


def legacy_g678_source_ranges() -> tuple[tuple[int, int], ...]:
    """Return source bit ranges for the historical G6/G7/G8 aliases.

    Ranges are inclusive and relative to the 288-bit payload at PDU offset 4.
    They demonstrate that the aliases cross packed gyro/accel fields rather
    than occupying independent sensor lanes.
    """

    return ((204, 225), (228, 249), (252, 271))


def _find_tshark(explicit: str | None) -> str:
    candidates = [
        explicit,
        os.environ.get("TSHARK"),
        shutil.which("tshark"),
        r"C:\Program Files\Wireshark\tshark.exe",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise MotionReferenceError(
        "TShark not found; install Wireshark, add tshark to PATH, or pass --tshark"
    )


def read_att_notifications(
    capture: Path, handle: int, tshark: str | None = None
) -> list[Notification]:
    executable = _find_tshark(tshark)
    if not capture.is_file():
        raise MotionReferenceError(f"capture not found: {capture}")
    display_filter = (
        f"btatt.opcode == 0x1b && btatt.handle == 0x{handle:04x}"
    )
    command = [
        executable,
        "-r",
        str(capture),
        "-Y",
        display_filter,
        "-T",
        "fields",
        "-E",
        "separator=|",
        "-e",
        "frame.time_relative",
        "-e",
        "btatt.value",
    ]
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if completed.returncode:
        raise MotionReferenceError(
            f"TShark failed ({completed.returncode}): {completed.stderr.strip()}"
        )
    notifications: list[Notification] = []
    for line_number, line in enumerate(completed.stdout.splitlines(), 1):
        if not line:
            continue
        try:
            time_text, value_text = line.split("|", 1)
            notifications.append(
                Notification(float(time_text), bytes.fromhex(value_text))
            )
        except (ValueError, TypeError) as exc:
            raise MotionReferenceError(
                f"malformed TShark row {line_number}: {line!r}"
            ) from exc
    if not notifications:
        raise MotionReferenceError(
            f"capture has no notifications for handle 0x{handle:04X}: {capture}"
        )
    return notifications


def read_blecap_jsonl(
    capture: Path,
) -> tuple[dict[int, list[Notification]], int]:
    """Read a UART ``blecap dump`` JSONL file without external dependencies."""

    if not capture.is_file():
        raise MotionReferenceError(f"capture not found: {capture}")
    streams: dict[int, list[Notification]] = {}
    end: dict[str, object] | None = None
    previous_raw: int | None = None
    elapsed_us = 0
    with capture.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            text = line.strip()
            if not text:
                continue
            try:
                item = json.loads(text)
            except json.JSONDecodeError as exc:
                raise MotionReferenceError(
                    f"{capture}:{line_number}: invalid JSON: {exc.msg}"
                ) from exc
            if not isinstance(item, dict):
                raise MotionReferenceError(
                    f"{capture}:{line_number}: JSON value must be an object"
                )
            if item.get("blecap") == "end":
                end = item
                continue
            if item.get("blecap") != "record":
                raise MotionReferenceError(
                    f"{capture}:{line_number}: expected blecap record or end"
                )
            if item.get("kind") != "input":
                continue
            try:
                handle = int(str(item.get("handle")), 16)
                timestamp_raw = int(item.get("t_us"))
                report_length = int(item.get("length"))
                captured_length = int(item.get("captured"))
                value = bytes.fromhex(str(item.get("payload")))
            except (TypeError, ValueError) as exc:
                raise MotionReferenceError(
                    f"{capture}:{line_number}: malformed input record"
                ) from exc
            if len(value) != captured_length:
                raise MotionReferenceError(
                    f"{capture}:{line_number}: captured length/payload mismatch"
                )
            if report_length > captured_length:
                continue
            if previous_raw is None:
                elapsed_us = 0
            else:
                elapsed_us += (timestamp_raw - previous_raw) & 0xFFFFFFFF
            previous_raw = timestamp_raw
            streams.setdefault(handle, []).append(
                Notification(elapsed_us / 1_000_000.0, value)
            )
    if end is None:
        raise MotionReferenceError(f"{capture}: blecap end record is missing")
    try:
        dropped = int(end.get("dropped", 0))
    except (TypeError, ValueError) as exc:
        raise MotionReferenceError(f"{capture}: invalid dropped count") from exc
    return streams, dropped


def read_motionpair_jsonl(capture: Path) -> tuple[list[Notification], int]:
    """Read UART ``motionpair dump`` JSONL as native-PDU notifications.

    Unlike ``blecap``, motionpair records contain the PDU directly instead of
    the surrounding report-0x09 bytes.  Timestamp wrap is handled identically
    so both capture formats can feed the same native analyzer.
    """

    if not capture.is_file():
        raise MotionReferenceError(f"capture not found: {capture}")
    notifications: list[Notification] = []
    end: dict[str, object] | None = None
    previous_raw: int | None = None
    elapsed_us = 0
    with capture.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            text = line.strip()
            if not text:
                continue
            try:
                item = json.loads(text)
            except json.JSONDecodeError as exc:
                raise MotionReferenceError(
                    f"{capture}:{line_number}: invalid JSON: {exc.msg}"
                ) from exc
            if not isinstance(item, dict):
                raise MotionReferenceError(
                    f"{capture}:{line_number}: JSON value must be an object"
                )
            if item.get("motionpair") == "end":
                end = item
                continue
            if item.get("motionpair") != "record":
                raise MotionReferenceError(
                    f"{capture}:{line_number}: expected motionpair record or end"
                )
            try:
                timestamp_raw = int(item.get("t_us"))
                native_length = int(item.get("native_len"))
                pdu = bytes.fromhex(str(item.get("native")))
            except (TypeError, ValueError) as exc:
                raise MotionReferenceError(
                    f"{capture}:{line_number}: malformed motionpair record"
                ) from exc
            if len(pdu) != native_length:
                raise MotionReferenceError(
                    f"{capture}:{line_number}: native length/payload mismatch"
                )
            if native_length not in (0x1E, 0x28):
                continue
            if previous_raw is None:
                elapsed_us = 0
            else:
                elapsed_us += (timestamp_raw - previous_raw) & 0xFFFFFFFF
            previous_raw = timestamp_raw
            report = bytes(0x0E) + bytes([native_length]) + pdu
            notifications.append(
                Notification(elapsed_us / 1_000_000.0, report)
            )
    if end is None:
        raise MotionReferenceError(f"{capture}: motionpair end record is missing")
    try:
        dropped = int(end.get("dropped", 0))
    except (TypeError, ValueError) as exc:
        raise MotionReferenceError(f"{capture}: invalid dropped count") from exc
    if not notifications:
        raise MotionReferenceError(f"{capture}: no length-0x1E/0x28 records")
    return notifications, dropped


def _norm(vector: Sequence[int]) -> float:
    return math.sqrt(sum(value * value for value in vector))


def _five(values: Iterable[float]) -> dict[str, float]:
    materialized = list(values)
    if not materialized:
        return {}
    return {
        "min": min(materialized),
        "max": max(materialized),
        "median": statistics.median(materialized),
        "mean": statistics.mean(materialized),
        "stdev": statistics.pstdev(materialized),
    }


def _correlation(first: Sequence[float], second: Sequence[float]) -> float | None:
    if len(first) != len(second) or len(first) < 2:
        return None
    first_mean = statistics.mean(first)
    second_mean = statistics.mean(second)
    numerator = sum(
        (left - first_mean) * (right - second_mean)
        for left, right in zip(first, second)
    )
    first_energy = sum((value - first_mean) ** 2 for value in first)
    second_energy = sum((value - second_mean) ** 2 for value in second)
    denominator = math.sqrt(first_energy * second_energy)
    return numerator / denominator if denominator else None


def analyze_raw(notifications: Sequence[Notification]) -> dict[str, object]:
    decoded = [
        (notification.time_seconds, decode_report05_raw_imu(notification.value))
        for notification in notifications
    ]
    active = [
        item for item in decoded
        if item[1].timestamp_us or any(item[1].accel) or any(item[1].gyro)
    ]
    if len(active) < 2:
        raise MotionReferenceError("raw capture has fewer than two active IMU samples")
    timestamp_delta = (
        active[-1][1].timestamp_us - active[0][1].timestamp_us
    ) & 0xFFFFFFFF
    wall_us = (active[-1][0] - active[0][0]) * 1_000_000.0
    timestamp_steps = [
        (current[1].timestamp_us - previous[1].timestamp_us) & 0xFFFFFFFF
        for previous, current in zip(active, active[1:])
    ]
    accel_norms = [
        _norm(sample.accel)
        for _, sample in active
        if _norm(sample.accel) > 512.0
    ]
    return {
        "notifications": len(notifications),
        "active_samples": len(active),
        "duration_seconds": active[-1][0] - active[0][0],
        "timestamp_first_us": active[0][1].timestamp_us,
        "timestamp_last_us": active[-1][1].timestamp_us,
        "timestamp_wall_ratio": timestamp_delta / wall_us if wall_us else 0.0,
        "timestamp_step_us": _five(timestamp_steps),
        "temperature_raw": _five(sample.temperature_raw for _, sample in active),
        "temperature_c": _five(sample.temperature_c for _, sample in active),
        "accel_norm_counts": _five(accel_norms),
        "accel_axes": [
            _five(sample.accel[axis] for _, sample in active) for axis in range(3)
        ],
        "gyro_axes": [
            _five(sample.gyro[axis] for _, sample in active) for axis in range(3)
        ],
    }


def decode_motion30_orientation(pdu: bytes) -> Motion30Orientation:
    """Decode a length-0x1E carrier without hiding its genuine wire state."""

    if len(pdu) != 0x1E:
        raise MotionReferenceError(
            f"motion orientation PDU must be 30 bytes, got {len(pdu)}"
        )
    g0 = pdu[5] | (pdu[6] << 8) | (pdu[7] << 16) | ((pdu[8] & 3) << 24)
    g1 = pdu[9] | (pdu[10] << 8) | (pdu[11] << 16) | ((pdu[12] & 3) << 24)
    g2 = pdu[13] | (pdu[14] << 8) | (pdu[15] << 16) | ((pdu[4] & 3) << 24)
    omitted = g2 >> 24
    retained = (
        ((g0 / float(1 << 26)) - 0.5) * math.sqrt(2.0),
        ((g1 / float(1 << 25)) - 0.5) * math.sqrt(2.0),
        ((((g2 & 0x00FFFFFF) / float(1 << 24)) - 0.5) * math.sqrt(2.0)),
    )
    retained_energy = sum(value * value for value in retained)
    # Direct state-0 chart-transition captures establish these local boundary
    # projections. They are useful for legacy seam diagnostics but are not a
    # global chart atlas: a later 3->1->0 capture refuted unsigned composition.
    # The stateful cyclic transition helpers below now cover state 2 without
    # pretending these unsigned state-0 projections compose globally.
    canonical_state0_carrier = {
        0: retained,
        1: (retained[2], retained[0], retained[1]),
        3: (retained[1], retained[2], retained[0]),
    }.get(omitted)
    quaternion = [0.0, 0.0, 0.0, 0.0]
    quaternion[omitted] = math.sqrt(
        max(0.0, 1.0 - retained_energy)
    )
    for index, value in enumerate(retained, 1):
        quaternion[(omitted + index) & 3] = value
    norm = math.sqrt(sum(value * value for value in quaternion))
    if norm == 0.0:
        raise MotionReferenceError("decoded zero length-0x1E quaternion")
    normalized = tuple(
        value / norm for value in quaternion
    )  # type: ignore[assignment]
    return Motion30Orientation(
        state=omitted,
        carrier_raw=(g0, g1, g2 & 0x00FFFFFF),
        retained=retained,
        quaternion_wxyz=normalized,
        canonical_state0_carrier=canonical_state0_carrier,
        retained_energy=retained_energy,
        strict_unit_valid=retained_energy <= 1.0 + 1e-12,
    )


def _decode_motion30_orientation(
    pdu: bytes,
) -> tuple[int, tuple[float, float, float, float]]:
    """Compatibility tuple used by the existing report summary."""

    orientation = decode_motion30_orientation(pdu)
    return orientation.state, orientation.quaternion_wxyz


def decode_motion30_quaternion(
    pdu: bytes,
) -> tuple[float, float, float, float]:
    """Return the legacy strict-smallest-three quaternion candidate as WXYZ."""

    return decode_motion30_orientation(pdu).quaternion_wxyz


def decode_motion40_prefix_orientation(
    sample: Motion40Sample,
    reference: Motion30Orientation,
) -> Motion40PrefixOrientation:
    """Unwrap a truncated length-0x28 carrier using prior 0x1E history.

    The prefix does not carry an omitted-component/chart selector. Each signed
    lane is also a modular window rather than a complete standalone carrier.
    A recent length-0x1E orientation therefore supplies both the chart and the
    nearest valid modular window. Captures establish this as a diagnostic
    decoder; callers must not silently bridge a chart transition.
    """

    normalized = _normalized_prefix_carrier(sample)
    quarter_sqrt2 = math.sqrt(2.0) / 4.0
    component_limit = math.sqrt(0.5)
    retained: list[float] = []
    modular_windows: list[int] = []
    for axis, exponent in enumerate((24, 23, 23)):
        base = normalized[axis] * math.sqrt(2.0) / float(1 << exponent)
        candidates = [
            (
                abs(base + window * quarter_sqrt2 - reference.retained[axis]),
                base + window * quarter_sqrt2,
                window,
            )
            for window in range(-4, 5)
            if (
                -component_limit - 1e-12
                <= base + window * quarter_sqrt2
                <= component_limit + 1e-12
            )
        ]
        if not candidates:
            raise MotionReferenceError(
                f"prefix axis {axis} has no valid modular carrier window"
            )
        _, value, window = min(candidates)
        retained.append(value)
        modular_windows.append(window)

    quaternion = [0.0, 0.0, 0.0, 0.0]
    quaternion[reference.state] = math.sqrt(
        max(0.0, 1.0 - sum(value * value for value in retained))
    )
    for index, value in enumerate(retained, 1):
        quaternion[(reference.state + index) & 3] = value
    norm = math.sqrt(sum(value * value for value in quaternion))
    if norm == 0.0:
        raise MotionReferenceError("decoded zero length-0x28 prefix quaternion")
    normalized_quaternion = tuple(
        value / norm for value in quaternion
    )
    return Motion40PrefixOrientation(
        reference_state=reference.state,
        retained=tuple(retained),  # type: ignore[arg-type]
        quaternion_wxyz=normalized_quaternion,  # type: ignore[arg-type]
        modular_windows=tuple(modular_windows),  # type: ignore[arg-type]
    )


def _q_slerp(
    left: tuple[float, float, float, float],
    right: tuple[float, float, float, float],
    fraction: float,
) -> tuple[float, float, float, float]:
    dot = sum(a * b for a, b in zip(left, right))
    if dot < 0.0:
        right = tuple(-value for value in right)
        dot = -dot
    dot = max(-1.0, min(1.0, dot))
    if dot > 0.9995:
        mixed = tuple(
            a + fraction * (b - a) for a, b in zip(left, right)
        )
        norm = math.sqrt(sum(value * value for value in mixed))
        return tuple(value / norm for value in mixed)  # type: ignore[return-value]
    angle = math.acos(dot)
    sine = math.sin(angle)
    left_weight = math.sin((1.0 - fraction) * angle) / sine
    right_weight = math.sin(fraction * angle) / sine
    return tuple(
        left_weight * a + right_weight * b for a, b in zip(left, right)
    )  # type: ignore[return-value]


def _q_delta_degrees(
    left: tuple[float, float, float, float],
    right: tuple[float, float, float, float],
) -> float:
    dot = abs(sum(a * b for a, b in zip(left, right)))
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, dot))))


def _interpolate_orientation(
    coordinate: float,
    samples: Sequence[
        tuple[float, tuple[float, float, float, float]]
    ],
    max_span: float = 0.050,
) -> tuple[float, float, float, float] | None:
    times = [item[0] for item in samples]
    insertion = bisect.bisect_left(times, coordinate)
    if insertion <= 0 or insertion >= len(samples):
        return None
    before_time, before = samples[insertion - 1]
    after_time, after = samples[insertion]
    span = after_time - before_time
    if span <= 0.0 or span > max_span:
        return None
    fraction = (coordinate - before_time) / span
    if not 0.0 <= fraction <= 1.0:
        return None
    return _q_slerp(before, after, fraction)


def _normalized_prefix_carrier(
    sample: Motion40Sample,
) -> tuple[float, float, float]:
    divisor = float(1 << sample.prefix_precision_shift)
    return tuple(
        value / divisor for value in sample.prefix_carrier
    )  # type: ignore[return-value]


def _linear_fit(
    independent: Sequence[float], dependent: Sequence[float]
) -> dict[str, float] | None:
    if len(independent) != len(dependent) or len(independent) < 3:
        return None
    mean_x = statistics.mean(independent)
    mean_y = statistics.mean(dependent)
    denominator = sum((value - mean_x) ** 2 for value in independent)
    dependent_stdev = statistics.pstdev(dependent)
    if denominator <= 1e-18 or dependent_stdev <= 1e-12:
        return None
    slope = sum(
        (x_value - mean_x) * (y_value - mean_y)
        for x_value, y_value in zip(independent, dependent)
    ) / denominator
    intercept = mean_y - slope * mean_x
    residuals = [
        y_value - (slope * x_value + intercept)
        for x_value, y_value in zip(independent, dependent)
    ]
    rmse = math.sqrt(
        sum(value * value for value in residuals) / len(residuals)
    )
    exponent = (
        round(math.log2(math.sqrt(2.0) / abs(slope)))
        if slope != 0.0 else 0
    )
    power_of_two_scale = math.sqrt(2.0) / float(1 << exponent)
    if slope < 0.0:
        power_of_two_scale = -power_of_two_scale
    return {
        "slope": slope,
        "intercept": intercept,
        "correlation": _correlation(independent, dependent) or 0.0,
        "rmse": rmse,
        "normalized_rmse": rmse / dependent_stdev,
        "nearest_sqrt2_power_exponent": float(exponent),
        "slope_ratio_to_nearest_power": slope / power_of_two_scale,
    }


def _fixed_scale_fit(
    independent: Sequence[float],
    dependent: Sequence[float],
    exponent: int,
) -> dict[str, float] | None:
    if len(independent) != len(dependent) or len(independent) < 3:
        return None
    dependent_stdev = statistics.pstdev(dependent)
    if dependent_stdev <= 1e-12:
        return None
    slope = math.sqrt(2.0) / float(1 << exponent)
    intercept = statistics.mean(
        y_value - slope * x_value
        for x_value, y_value in zip(independent, dependent)
    )
    residuals = [
        y_value - (slope * x_value + intercept)
        for x_value, y_value in zip(independent, dependent)
    ]
    rmse = math.sqrt(
        sum(value * value for value in residuals) / len(residuals)
    )
    return {
        "sqrt2_power_exponent": float(exponent),
        "slope": slope,
        "intercept": intercept,
        "rmse": rmse,
        "normalized_rmse": rmse / dependent_stdev,
        "correlation": _correlation(independent, dependent) or 0.0,
    }


def _interpolate_motion30_carrier(
    coordinate: float,
    samples: Sequence[tuple[float, Motion30Orientation]],
    max_span: float = 50.0,
) -> tuple[
    int,
    tuple[float, float, float],
    tuple[float, float, float, float],
] | None:
    times = [item[0] for item in samples]
    insertion = bisect.bisect_left(times, coordinate)
    if insertion <= 0 or insertion >= len(samples):
        return None
    before_time, before = samples[insertion - 1]
    after_time, after = samples[insertion]
    span = after_time - before_time
    if (
        span <= 0.0
        or span > max_span
        or before.state != after.state
    ):
        return None
    fraction = (coordinate - before_time) / span
    if not 0.0 <= fraction <= 1.0:
        return None
    retained = tuple(
        left + fraction * (right - left)
        for left, right in zip(before.retained, after.retained)
    )
    quaternion = _q_slerp(
        before.quaternion_wxyz, after.quaternion_wxyz, fraction
    )
    return before.state, retained, quaternion


def _interpolate_motion30_canonical_carrier(
    coordinate: float,
    samples: Sequence[tuple[float, Motion30Orientation]],
    max_span: float = 50.0,
) -> tuple[
    tuple[float, float, float],
    Motion30Orientation,
    Motion30Orientation,
] | None:
    """Interpolate within one chart or across a proven state-0 seam.

    The legacy field name reflects the original state-0 projection experiment.
    It is not a globally composable chart frame. Cross-chart interpolation is
    permitted only in directions directly validated by zero-drop captures.
    """

    times = [item[0] for item in samples]
    insertion = bisect.bisect_left(times, coordinate)
    if insertion <= 0 or insertion >= len(samples):
        return None
    before_time, before = samples[insertion - 1]
    after_time, after = samples[insertion]
    if (
        before.state != after.state
        and (before.state, after.state) not in {(3, 0), (0, 3), (0, 1)}
    ):
        return None
    span = after_time - before_time
    if (
        span <= 0.0
        or span > max_span
        or before.canonical_state0_carrier is None
        or after.canonical_state0_carrier is None
    ):
        return None
    fraction = (coordinate - before_time) / span
    if not 0.0 <= fraction <= 1.0:
        return None
    carrier = tuple(
        left + fraction * (right - left)
        for left, right in zip(
            before.canonical_state0_carrier,
            after.canonical_state0_carrier,
        )
    )
    return carrier, before, after  # type: ignore[return-value]


def _canonical_carrier_to_wire(
    carrier: tuple[float, float, float],
    state: int,
) -> tuple[float, float, float] | None:
    """Map a state-0-boundary projection back to a captured wire chart."""

    if state == 0:
        return carrier
    if state == 1:
        return carrier[1], carrier[2], carrier[0]
    if state == 3:
        return carrier[2], carrier[0], carrier[1]
    return None


def _wire_carrier_to_canonical(
    carrier: tuple[float, float, float],
    state: int,
) -> tuple[float, float, float] | None:
    """Map a captured wire chart into its state-0-boundary projection."""

    if state == 0:
        return carrier
    if state == 1:
        return carrier[2], carrier[0], carrier[1]
    if state == 3:
        return carrier[1], carrier[2], carrier[0]
    return None


def _cyclic_chart_transform(
    low_state: int,
    high_state: int,
) -> tuple[tuple[int, int, int], int]:
    """Return the higher-to-lower cyclic lane map and boundary lane."""

    low_components = tuple((low_state + index) & 3 for index in (1, 2, 3))
    high_components = tuple((high_state + index) & 3 for index in (1, 2, 3))
    permutation: list[int] = []
    for component in low_components:
        source_component = (
            low_state if component == high_state else component
        )
        permutation.append(high_components.index(source_component))
    return (
        tuple(permutation),  # type: ignore[return-value]
        low_components.index(high_state),
    )


def _apply_cyclic_chart_transform(
    high_wire: tuple[float, float, float],
    permutation: tuple[int, int, int],
    signs: tuple[int, int, int],
) -> tuple[float, float, float]:
    return tuple(
        high_wire[permutation[lane]] * signs[lane]
        for lane in range(3)
    )  # type: ignore[return-value]


def _invert_cyclic_chart_transform(
    low_wire: tuple[float, float, float],
    permutation: tuple[int, int, int],
    signs: tuple[int, int, int],
) -> tuple[float, float, float]:
    high_wire = [0.0, 0.0, 0.0]
    for low_lane, high_lane in enumerate(permutation):
        high_wire[high_lane] = low_wire[low_lane] * signs[low_lane]
    return tuple(high_wire)  # type: ignore[return-value]


def _cyclic_transition_fit(
    before: Motion30Orientation,
    after: Motion30Orientation,
) -> dict[str, object] | None:
    """Fit one adjacent chart boundary in the lower state's local frame."""

    if before.state == after.state:
        return None
    low_state = min(before.state, after.state)
    high_state = max(before.state, after.state)
    low = before if before.state == low_state else after
    high = after if after.state == high_state else before
    permutation, boundary_lane = _cyclic_chart_transform(
        low_state, high_state
    )
    same_signs = (1, 1, 1)
    opposite_signs = tuple(
        1 if lane == boundary_lane else -1 for lane in range(3)
    )
    candidates: list[dict[str, object]] = []
    for branch, signs in (
        ("same_omitted_sign", same_signs),
        ("opposite_omitted_sign", opposite_signs),
    ):
        projected = _apply_cyclic_chart_transform(
            high.retained, permutation, signs
        )
        candidates.append(
            {
                "branch": branch,
                "signs": signs,
                "projected_high": projected,
                "residual": _norm(
                    tuple(
                        left - right
                        for left, right in zip(low.retained, projected)
                    )
                ),
            }
        )
    candidates.sort(key=lambda candidate: float(candidate["residual"]))
    selected, rejected = candidates
    projected_high = selected["projected_high"]
    before_local = (
        before.retained
        if before.state == low_state
        else projected_high
    )
    after_local = (
        after.retained
        if after.state == low_state
        else projected_high
    )
    return {
        "low_state": low_state,
        "high_state": high_state,
        "topology_permutation": permutation,
        "boundary_lane": boundary_lane,
        "branch": selected["branch"],
        "branch_signs": selected["signs"],
        "residual": selected["residual"],
        "other_branch_residual": rejected["residual"],
        "branch_margin": (
            float(rejected["residual"]) - float(selected["residual"])
        ),
        "before_local": before_local,
        "after_local": after_local,
    }


def _prefix_wire_candidate(
    sample: Motion40Sample,
    reference_wire: tuple[float, float, float],
) -> tuple[tuple[float, float, float], tuple[int, int, int]]:
    """Unwrap a prefix near one candidate wire chart without unit constraints."""

    normalized = _normalized_prefix_carrier(sample)
    quarter_sqrt2 = math.sqrt(2.0) / 4.0
    values: list[float] = []
    windows: list[int] = []
    for axis, exponent in enumerate((24, 23, 23)):
        base = normalized[axis] * math.sqrt(2.0) / float(1 << exponent)
        candidates = [
            (
                abs(base + window * quarter_sqrt2 - reference_wire[axis]),
                base + window * quarter_sqrt2,
                window,
            )
            for window in range(-4, 5)
        ]
        _, value, window = min(candidates)
        values.append(value)
        windows.append(window)
    return (
        tuple(values),  # type: ignore[return-value]
        tuple(windows),  # type: ignore[return-value]
    )


def _motion30_chart_audit(
    orientation30: Sequence[tuple[float, Motion30Orientation]],
    pdu40: Sequence[tuple[float, Motion40Sample]],
    predecessor_offset_ticks: float = 4.0,
) -> dict[str, object]:
    """Report genuine carrier chart transitions and prefix seam behavior."""

    state_counts = Counter(sample.state for _, sample in orientation30)
    unit_violations = [
        (tick, sample)
        for tick, sample in orientation30
        if not sample.strict_unit_valid
    ]
    transitions: list[dict[str, object]] = []
    for (before_tick, before), (after_tick, after) in zip(
        orientation30, orientation30[1:]
    ):
        if before.state == after.state:
            continue
        record: dict[str, object] = {
            "before_tick": before_tick,
            "after_tick": after_tick,
            "before_state": before.state,
            "after_state": after.state,
            "before_wire": before.retained,
            "after_wire": after.retained,
            "before_energy": before.retained_energy,
            "after_energy": after.retained_energy,
        }
        cyclic_fit = _cyclic_transition_fit(before, after)
        if cyclic_fit is not None:
            before_local = cyclic_fit["before_local"]
            after_local = cyclic_fit["after_local"]
            cyclic_delta = tuple(
                right - left
                for left, right in zip(before_local, after_local)
            )
            record.update(
                {
                    "cyclic_frame": (
                        f"state{cyclic_fit['low_state']}_local_projection"
                    ),
                    "cyclic_topology_permutation": cyclic_fit[
                        "topology_permutation"
                    ],
                    "cyclic_branch": cyclic_fit["branch"],
                    "cyclic_branch_signs": cyclic_fit["branch_signs"],
                    "cyclic_other_branch_residual": cyclic_fit[
                        "other_branch_residual"
                    ],
                    "cyclic_branch_margin": cyclic_fit["branch_margin"],
                    "cyclic_delta": cyclic_delta,
                    "cyclic_delta_norm": _norm(cyclic_delta),
                }
            )
        # These are state-0-boundary projections, not globally composable
        # chart maps. Never compare state 1 directly with state 3 through
        # their separately observed state-0 seam projections.
        if (
            0 in (before.state, after.state)
            and
            before.canonical_state0_carrier is not None
            and after.canonical_state0_carrier is not None
        ):
            delta = tuple(
                right - left
                for left, right in zip(
                    before.canonical_state0_carrier,
                    after.canonical_state0_carrier,
                )
            )
            record.update(
                {
                    "canonical_frame": "state0_boundary_projection",
                    "before_canonical": before.canonical_state0_carrier,
                    "after_canonical": after.canonical_state0_carrier,
                    "canonical_delta": delta,
                    "canonical_delta_norm": _norm(delta),
                }
            )
        transitions.append(record)

    prefix_seams: list[dict[str, object]] = []
    orientation_times = [item[0] for item in orientation30]
    for tick, sample in pdu40:
        coordinate = tick - sample.elapsed_ticks + predecessor_offset_ticks
        insertion = bisect.bisect_left(orientation_times, coordinate)
        if insertion <= 0 or insertion >= len(orientation30):
            continue
        before_tick, before = orientation30[insertion - 1]
        after_tick, after = orientation30[insertion]
        if before.state == after.state:
            continue
        span = after_tick - before_tick
        if span <= 0.0 or span > 50.0:
            continue
        fraction = (coordinate - before_tick) / span
        if not 0.0 <= fraction <= 1.0:
            continue
        cyclic_fit = _cyclic_transition_fit(before, after)
        if cyclic_fit is None:
            continue
        before_local = cyclic_fit["before_local"]
        after_local = cyclic_fit["after_local"]
        reference = tuple(
            left + fraction * (right - left)
            for left, right in zip(before_local, after_local)
        )
        candidates: list[dict[str, object]] = []
        for state in dict.fromkeys((before.state, after.state)):
            if state == cyclic_fit["low_state"]:
                reference_wire = reference
            else:
                reference_wire = _invert_cyclic_chart_transform(
                    reference,
                    cyclic_fit["topology_permutation"],
                    cyclic_fit["branch_signs"],
                )
            wire, windows = _prefix_wire_candidate(
                sample, reference_wire
            )
            if state == cyclic_fit["low_state"]:
                local = wire
            else:
                local = _apply_cyclic_chart_transform(
                    wire,
                    cyclic_fit["topology_permutation"],
                    cyclic_fit["branch_signs"],
                )
            delta = tuple(
                value - expected
                for value, expected in zip(local, reference)
            )
            candidates.append(
                {
                    "state": state,
                    "modular_windows": windows,
                    "canonical_delta": delta,
                    "canonical_delta_norm": _norm(delta),
                }
            )
        if not candidates:
            continue
        selected = min(
            candidates,
            key=lambda candidate: candidate["canonical_delta_norm"],
        )
        prefix_seams.append(
            {
                "packet_tick": tick,
                "carrier_epoch_tick": coordinate,
                "before_state": before.state,
                "after_state": after.state,
                "local_frame_state": cyclic_fit["low_state"],
                "cyclic_branch": cyclic_fit["branch"],
                "cyclic_branch_signs": cyclic_fit["branch_signs"],
                "selected_state": selected["state"],
                "modular_windows": selected["modular_windows"],
                "canonical_delta": selected["canonical_delta"],
                "canonical_delta_norm": selected["canonical_delta_norm"],
                "candidates": candidates,
            }
        )

    return {
        "interpretation": (
            "stateful_cyclic_chart_transitions_observed_for_all_states;"
            "global_unsigned_chart_composition_refuted_by_3_1_0_capture;"
            "strict_smallest_three_is_not_exact_genuine_behavior"
        ),
        "state_counts": dict(sorted(state_counts.items())),
        "strict_unit_constraint": {
            "records": len(orientation30),
            "violations": len(unit_violations),
            "maximum_retained_energy": max(
                (
                    sample.retained_energy
                    for _, sample in orientation30
                ),
                default=0.0,
            ),
            "first_violation_tick": (
                unit_violations[0][0] if unit_violations else None
            ),
        },
        "transitions": transitions,
        "prefix_transition_epochs": prefix_seams,
    }


def _evaluate_prefix_epoch(
    offset_ticks: float,
    pdu40: Sequence[tuple[float, Motion40Sample]],
    orientation30: Sequence[tuple[float, Motion30Orientation]],
    predecessor_offset_ticks: float | None = None,
) -> dict[str, object] | None:
    groups: dict[
        int,
        list[
            tuple[
                tuple[float, float, float],
                tuple[float, float, float],
                tuple[float, float, float, float],
            ]
        ],
    ] = {}
    for tick, sample in pdu40:
        coordinate = (
            tick + offset_ticks
            if predecessor_offset_ticks is None
            else tick - sample.elapsed_ticks + predecessor_offset_ticks
        )
        reference = _interpolate_motion30_carrier(
            coordinate, orientation30
        )
        if reference is None:
            continue
        reference_state, retained, quaternion = reference
        groups.setdefault(reference_state, []).append(
            (_normalized_prefix_carrier(sample), retained, quaternion)
        )

    group_reports: list[dict[str, object]] = []
    objectives: list[float] = []
    fixed_objectives: list[float] = []
    correlations: list[float] = []
    aligned_quaternions: list[tuple[float, float, float, float]] = []
    aligned_records = 0
    for reference_state, records in sorted(groups.items()):
        if len(records) < 5:
            continue
        axes: list[dict[str, float] | None] = []
        fixed_axes: list[dict[str, float] | None] = []
        for axis in range(3):
            independent = [record[0][axis] for record in records]
            dependent = [record[1][axis] for record in records]
            fit = _linear_fit(
                independent, dependent
            )
            fixed = _fixed_scale_fit(
                independent, dependent, (24, 23, 23)[axis]
            )
            if fixed is not None:
                quarter_sqrt2 = math.sqrt(2.0) / 4.0
                intercept_units = fixed["intercept"] / quarter_sqrt2
                nearest_units = round(intercept_units)
                fixed["intercept_quarter_sqrt2_units"] = intercept_units
                fixed["nearest_quarter_sqrt2_units"] = float(nearest_units)
                fixed["intercept_quantization_error"] = (
                    intercept_units - nearest_units
                )
            axes.append(fit)
            fixed_axes.append(fixed)
            if fit is not None:
                objectives.append(fit["normalized_rmse"])
                correlations.append(abs(fit["correlation"]))
            if fixed is not None:
                fixed_objectives.append(fixed["normalized_rmse"])
        if sum(fit is not None for fit in axes) < 2:
            continue
        aligned_records += len(records)
        aligned_quaternions.extend(record[2] for record in records)
        group_reports.append(
            {
                "length30_state": reference_state,
                "records": len(records),
                "axes": axes,
                "fixed_scale_axes": fixed_axes,
            }
        )

    if not objectives or not fixed_objectives or not group_reports:
        return None
    motion_span = max(
        _q_delta_degrees(aligned_quaternions[0], quaternion)
        for quaternion in aligned_quaternions
    )
    result: dict[str, object] = {
        "aligned_records": aligned_records,
        "groups": group_reports,
        "mean_normalized_rmse": statistics.mean(objectives),
        "mean_fixed_scale_normalized_rmse": statistics.mean(
            fixed_objectives
        ),
        "mean_abs_correlation": statistics.mean(correlations),
        "reference_span_degrees": motion_span,
    }
    if predecessor_offset_ticks is None:
        result["offset_ticks"] = offset_ticks
        result["epoch_model"] = "current_tick_plus_constant"
    else:
        result["predecessor_offset_ticks"] = predecessor_offset_ticks
        result["epoch_model"] = "current_tick_minus_encoded_elapsed_plus_offset"
    return result


def _evaluate_prefix_history_decode(
    pdu40: Sequence[tuple[float, Motion40Sample]],
    orientation30: Sequence[tuple[float, Motion30Orientation]],
    predecessor_offset_ticks: float = 4.0,
) -> dict[str, object]:
    """Compare the causal history decoder with an interpolated 0x1E reference."""

    orientation_times = [item[0] for item in orientation30]
    angular_errors: list[float] = []
    state_mismatches = 0
    reference_records = 0
    window_counts: Counter[tuple[int, int, int]] = Counter()
    for tick, sample in pdu40:
        coordinate = tick - sample.elapsed_ticks + predecessor_offset_ticks
        truth = _interpolate_motion30_carrier(coordinate, orientation30)
        if truth is None:
            continue
        insertion = bisect.bisect_right(orientation_times, coordinate)
        if insertion <= 0:
            continue
        prior = orientation30[insertion - 1][1]
        reference_records += 1
        if prior.state != truth[0]:
            state_mismatches += 1
            continue
        decoded = decode_motion40_prefix_orientation(sample, prior)
        angular_errors.append(
            _q_delta_degrees(decoded.quaternion_wxyz, truth[2])
        )
        window_counts[decoded.modular_windows] += 1

    if not angular_errors:
        return {
            "status": "insufficient_state_stable_records",
            "reference_records": reference_records,
            "state_mismatches": state_mismatches,
        }
    return {
        "status": "diagnostic_against_interpolated_length30_reference",
        "epoch_model": "current_tick_minus_encoded_elapsed_plus_offset",
        "predecessor_offset_ticks": predecessor_offset_ticks,
        "reference_records": reference_records,
        "decoded_records": len(angular_errors),
        "state_mismatches": state_mismatches,
        "angular_error_degrees": _five(angular_errors),
        "modular_windows": {
            str(windows): count for windows, count in sorted(window_counts.items())
        },
    }


def _fit_prefix_carrier_epoch(
    pdu40: Sequence[tuple[float, Motion40Sample]],
    orientation30: Sequence[tuple[float, Motion30Orientation]],
) -> dict[str, object]:
    """Compare the mode-3 carrier lanes to the established 0x1E carrier.

    This is a diagnostic epoch/window model, not a production decoder. The
    carrier-2 low bits are part of that carrier, not a separate state. Affine
    grouping is therefore keyed only by the length-0x1E carrier state, whose
    chart changes create exact modular window offsets.
    """

    candidates = [
        -20.0 + index * 0.25
        for index in range(161)
    ]
    evaluations = [
        evaluation
        for offset in candidates
        if (
            evaluation := _evaluate_prefix_epoch(
                offset, pdu40, orientation30
            )
        ) is not None
    ]
    if not evaluations:
        return {
            "status": "insufficient_state_aligned_records",
            "pdu40_records": len(pdu40),
            "length30_records": len(orientation30),
        }
    best_constant = min(
        evaluations,
        key=lambda item: (
            item["mean_fixed_scale_normalized_rmse"],
            item["mean_normalized_rmse"],
            -item["mean_abs_correlation"],
            abs(item["offset_ticks"]),
        ),
    )
    zero = next(
        (
            evaluation for evaluation in evaluations
            if evaluation["offset_ticks"] == 0.0
        ),
        None,
    )
    predecessor_plus_four = _evaluate_prefix_epoch(
        0.0,
        pdu40,
        orientation30,
        predecessor_offset_ticks=4.0,
    )
    best = predecessor_plus_four or best_constant
    result = dict(best)
    result["status"] = (
        "diagnostic_predecessor_plus_four_truncated_carrier_fit"
        if best["reference_span_degrees"] >= 1.0
        else "stationary_state_model_underconstrained"
    )
    result["best_constant_offset_ticks"] = best_constant["offset_ticks"]
    result["best_constant_mean_fixed_scale_normalized_rmse"] = best_constant[
        "mean_fixed_scale_normalized_rmse"
    ]
    if "offset_ticks" in best:
        result["epoch_offset_milliseconds"] = (
            float(best["offset_ticks"]) * 1.25
        )
    else:
        result["predecessor_offset_milliseconds"] = (
            float(best["predecessor_offset_ticks"]) * 1.25
        )
    if zero is not None:
        result["zero_offset_mean_normalized_rmse"] = zero[
            "mean_normalized_rmse"
        ]
        result["zero_offset_mean_fixed_scale_normalized_rmse"] = zero[
            "mean_fixed_scale_normalized_rmse"
        ]
        result["epoch_fit_improvement"] = (
            zero["mean_fixed_scale_normalized_rmse"]
            / best["mean_fixed_scale_normalized_rmse"]
            if best["mean_fixed_scale_normalized_rmse"] > 0.0 else 0.0
        )
    return result


def analyze_native(
    notifications: Sequence[Notification], dropped_records: int = 0
) -> dict[str, object]:
    length_counts: Counter[int] = Counter()
    pdus: list[tuple[float, bytes]] = []
    for notification in notifications:
        report = notification.value
        if len(report) <= 0x0E:
            continue
        length = report[0x0E]
        length_counts[length] += 1
        end = 0x0F + length
        if length in (0x1E, 0x28) and len(report) >= end:
            pdus.append((notification.time_seconds, report[0x0F:end]))

    pdu40 = [(time_value, pdu) for time_value, pdu in pdus if len(pdu) == 0x28]
    if len(pdu40) < 2:
        raise MotionReferenceError("native capture has fewer than two 40-byte PDUs")

    timeline: list[tuple[float, bytes, int]] = []
    previous_timeline_tick: int | None = None
    unwrapped_tick = 0
    for time_value, pdu in pdus:
        tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
        if previous_timeline_tick is None:
            unwrapped_tick = tick
        else:
            unwrapped_tick += (tick - previous_timeline_tick) & 0x0FFF
        timeline.append((time_value, pdu, unwrapped_tick))
        previous_timeline_tick = tick

    decoded: list[Motion40Sample] = []
    previous_tick: int | None = None
    packet4_matches = 0
    packet3_offsets: Counter[int] = Counter()
    # Production native motion interleaves 0x1E and 0x28 carriers. Preserve the
    # immediately preceding carrier tick as an integrity check. Layout itself
    # comes from the 0x28 packet's complete encoded elapsed count; comparing
    # only adjacent 0x28 ticks folds intervening 0x1E time into a false delta.
    for _, pdu in pdus:
        tick = pdu[0] | ((pdu[1] & 0x0F) << 8)
        if len(pdu) == 0x28:
            sample = decode_motion40(pdu, previous_tick)
            decoded.append(sample)
            audit = icm_fifo_header_audit(pdu)
            packet4_matches += int(audit["two_packet4_at_0_20"])
            packet3_offsets.update(audit["two_packet3_offsets"])
        previous_tick = tick

    catchup = [sample for sample in decoded if sample.layout == "catchup"]
    normal = [sample for sample in decoded if sample.layout == "normal"]
    high_rate = [sample for sample in decoded if sample.layout == "high_rate"]
    unknown = [sample for sample in decoded if sample.layout == "unknown"]
    classified = [sample for sample in decoded if sample.layout != "unknown"]
    elapsed_tick_comparisons = sum(
        sample.tick_delta is not None for sample in decoded
    )
    elapsed_tick_matches = sum(
        sample.tick_delta is not None
        and sample.elapsed_ticks == sample.tick_delta
        for sample in decoded
    )
    local_elapsed_mismatches = elapsed_tick_comparisons - elapsed_tick_matches

    def accel_norms(
        samples: Sequence[Motion40Sample], sample_index: int, scale: float
    ) -> dict[str, float]:
        return _five(_norm(sample.accel[sample_index]) / scale for sample in samples)

    def vector_axes(
        samples: Sequence[Motion40Sample],
        field: str,
        sample_index: int,
        scale: float,
    ) -> list[dict[str, float]]:
        return [
            _five(
                getattr(sample, field)[sample_index][axis] / scale
                for sample in samples
            )
            for axis in range(3)
        ]

    def accel_correlations(
        samples: Sequence[Motion40Sample], first_index: int, second_index: int
    ) -> list[float | None]:
        return [
            _correlation(
                [sample.accel[first_index][axis] for sample in samples],
                [sample.accel[second_index][axis] for sample in samples],
            )
            for axis in range(3)
        ]

    orientation30_meta = [
        (
            float(tick_value),
            decode_motion30_orientation(pdu),
        )
        for _, pdu, tick_value in timeline
        if len(pdu) == 0x1E
    ]
    pdu40_items = [
        item for item in timeline if len(item[1]) == 0x28
    ]
    pdu40_timeline = [
        (float(item[2]), sample)
        for item, sample in zip(pdu40_items, decoded)
        if sample.layout != "unknown"
    ]
    carrier_chart_audit = _motion30_chart_audit(
        orientation30_meta, pdu40_timeline
    )

    prefix_stats = {}
    if classified:
        prefix_stats = {
            "packing_mode": dict(
                Counter(sample.packing_mode for sample in classified)
            ),
            "lane2_low2": dict(
                Counter(sample.prefix_lane2_low2 for sample in classified)
            ),
            "widths": dict(
                Counter(str(sample.prefix_widths) for sample in classified)
            ),
            "axes_common_precision_units": [
                _five(
                    _normalized_prefix_carrier(sample)[axis]
                    for sample in classified
                )
                for axis in range(3)
            ],
            "tails": {
                layout: {
                    "width": samples[0].tail_width,
                    "values": dict(Counter(sample.tail_value for sample in samples)),
                    **(
                        {
                            "temperature_integer_raw": dict(
                                Counter(
                                    decode_tail16_fields(sample).temperature_integer_raw
                                    for sample in samples
                                )
                            ),
                            "fractions_equal": sum(
                                decode_tail16_fields(sample).fractions_equal
                                for sample in samples
                            ),
                            "fractions_different": sum(
                                not decode_tail16_fields(sample).fractions_equal
                                for sample in samples
                            ),
                            "temperature_a_q3": _five(
                                decode_tail16_fields(sample).temperature_a_q3
                                for sample in samples
                            ),
                            "temperature_b_q3": _five(
                                decode_tail16_fields(sample).temperature_b_q3
                                for sample in samples
                            ),
                            "temperature_a_raw": _five(
                                decode_tail16_fields(sample).temperature_a_raw
                                for sample in samples
                            ),
                            "temperature_b_raw": _five(
                                decode_tail16_fields(sample).temperature_b_raw
                                for sample in samples
                            ),
                            "temperature_a_c": _five(
                                decode_tail16_fields(sample).temperature_a_c
                                for sample in samples
                            ),
                            "temperature_b_c": _five(
                                decode_tail16_fields(sample).temperature_b_c
                                for sample in samples
                            ),
                        }
                        if samples[0].tail_width == 16 else
                        {"bit_287_ones": sum(sample.tail_value for sample in samples)}
                    ),
                }
                for layout, samples in (
                    ("high_rate", high_rate),
                    ("normal", normal),
                    ("catchup", catchup),
                )
                if samples
            },
            "semantics": (
                "mode3_truncated_orientation_carrier_candidate;"
                "not_a_standalone_smallest_three_quaternion"
            ),
        }
        if local_elapsed_mismatches:
            prefix_stats["carrier_epoch_fit"] = {
                "status": "skipped_retained_sequence_has_tick_gaps",
                "dropped_records": dropped_records,
                "local_elapsed_mismatches": local_elapsed_mismatches,
            }
        else:
            prefix_stats["carrier_epoch_fit"] = (
                _fit_prefix_carrier_epoch(
                    pdu40_timeline, orientation30_meta
                )
            )
            prefix_stats["history_orientation_decode"] = (
                _evaluate_prefix_history_decode(
                    pdu40_timeline, orientation30_meta
                )
            )
            if dropped_records:
                prefix_stats["carrier_epoch_fit"][
                    "capture_integrity"
                ] = "global_drops_but_retained_window_is_locally_contiguous"
                prefix_stats["carrier_epoch_fit"][
                    "dropped_records"
                ] = dropped_records

    high_rate_stats = {}
    if high_rate:
        high_rate_stats = {
            "count": len(high_rate),
            "accel_1_norm_g": accel_norms(high_rate, 0, 256.0 * 4096.0),
            "accel_2_norm_g": accel_norms(high_rate, 1, 256.0 * 4096.0),
            "accel_1_axes_counts": vector_axes(high_rate, "accel", 0, 256.0),
            "gyro_axes_counts": vector_axes(high_rate, "gyro", 0, 256.0),
            "accel_2_axes_counts": vector_axes(high_rate, "accel", 1, 256.0),
            "accel_1_2_axis_correlation": accel_correlations(high_rate, 0, 1),
            "gyro_norm_counts": _five(
                _norm(sample.gyro[0]) / 256.0 for sample in high_rate
            ),
        }
    normal_stats = {}
    if normal:
        normal_stats = {
            "count": len(normal),
            "accel_a_norm_g": accel_norms(normal, 0, 4096.0),
            "accel_b_norm_g": accel_norms(normal, 1, 2048.0),
            "accel_c_norm_g": accel_norms(normal, 2, 4096.0),
            "accel_a_axes_counts": vector_axes(normal, "accel", 0, 1.0),
            "gyro_a_axes_counts": vector_axes(normal, "gyro", 0, 0.5),
            "accel_b_axes_counts": vector_axes(normal, "accel", 1, 0.5),
            "gyro_b_axes_counts": vector_axes(normal, "gyro", 1, 1.0),
            "accel_c_axes_counts": vector_axes(normal, "accel", 2, 1.0),
            "accel_a_c_axis_correlation": accel_correlations(normal, 0, 2),
        }
    catchup_stats = {}
    if catchup:
        catchup_stats = {
            "count": len(catchup),
            "accel_1_norm_g": accel_norms(catchup, 0, 4096.0),
            "accel_2_norm_g": accel_norms(catchup, 1, 2048.0),
            "accel_3_norm_g": accel_norms(catchup, 2, 4096.0),
            "accel_1_axes_counts": vector_axes(catchup, "accel", 0, 1.0),
            "gyro_1_axes_counts": vector_axes(catchup, "gyro", 0, 4.0),
            "accel_2_axes_counts": vector_axes(catchup, "accel", 1, 0.5),
            "gyro_2_axes_counts": vector_axes(catchup, "gyro", 1, 4.0),
            "accel_3_axes_counts": vector_axes(catchup, "accel", 2, 1.0),
            "accel_1_3_axis_correlation": accel_correlations(catchup, 0, 2),
            "gyro_1_norm_counts": _five(
                _norm(sample.gyro[0]) / 4.0 for sample in catchup
            ),
            "gyro_2_norm_counts": _five(
                _norm(sample.gyro[1]) / 4.0 for sample in catchup
            ),
        }

    return {
        "notifications": len(notifications),
        "capture_integrity": {
            "dropped_records": dropped_records,
            "layout_classification": "self_contained_12_bit_elapsed_field",
            "preceding_carrier_sequence": (
                "has_local_tick_gaps"
                if local_elapsed_mismatches
                else (
                    "retained_window_contiguous_global_drops_elsewhere"
                    if dropped_records
                    else "complete_zero_drop_stream"
                )
            ),
            "local_elapsed_mismatches": local_elapsed_mismatches,
        },
        "motion_lengths": {str(key): value for key, value in sorted(length_counts.items())},
        "pdu40_count": len(pdu40),
        "duration_seconds": pdu40[-1][0] - pdu40[0][0],
        "tick_delta": _five(
            sample.tick_delta
            for sample in decoded
            if sample.tick_delta is not None
        ),
        "tick_delta_histogram": dict(
            Counter(
                sample.tick_delta
                for sample in decoded
                if sample.tick_delta is not None
            ).most_common()
        ),
        "elapsed_ticks": _five(sample.elapsed_ticks for sample in decoded),
        "elapsed_ticks_histogram": dict(
            Counter(sample.elapsed_ticks for sample in decoded).most_common()
        ),
        "elapsed_ticks_match_preceding_delta": elapsed_tick_matches,
        "elapsed_ticks_comparisons": elapsed_tick_comparisons,
        "elapsed_ticks_mismatches": {
            f"encoded_{encoded}_observed_{observed}": count
            for (encoded, observed), count in Counter(
                (sample.elapsed_ticks, sample.tick_delta)
                for sample in decoded
                if sample.tick_delta is not None
                and sample.elapsed_ticks != sample.tick_delta
            ).most_common()
        },
        "sensor_status": {
            f"0x{sensor:02X}": count
            for sensor, count in sorted(
                Counter(sample.sensor_status for sample in decoded).items()
            )
        },
        "sensor_status_profiles": {
            f"0x{sensor:02X}": {
                "layouts": dict(
                    Counter(
                        sample.layout
                        for sample in decoded
                        if sample.sensor_status == sensor
                    )
                ),
                "prefix_lane2_low2": dict(
                    Counter(
                        sample.prefix_lane2_low2
                        for sample in decoded
                        if sample.sensor_status == sensor
                        and sample.layout != "unknown"
                    )
                ),
                "tail_widths": dict(
                    Counter(
                        sample.tail_width
                        for sample in decoded
                        if sample.sensor_status == sensor
                        and sample.layout != "unknown"
                    )
                ),
            }
            for sensor in sorted({sample.sensor_status for sample in decoded})
        },
        "carrier_chart": carrier_chart_audit,
        "prefix": prefix_stats,
        "high_rate": high_rate_stats,
        "unclassified": len(unknown),
        "normal": normal_stats,
        "catchup": catchup_stats,
        "icm_fifo_audit": {
            "two_packet4_at_0_20": packet4_matches,
            "two_packet3_offsets": {
                str(key): value for key, value in sorted(packet3_offsets.items())
            },
        },
        "legacy_g678_payload_bit_ranges": legacy_g678_source_ranges(),
    }


def _rounded(value: object) -> object:
    if isinstance(value, float):
        if value != 0.0 and abs(value) < 1e-6:
            return float(f"{value:.9g}")
        return round(value, 6)
    if isinstance(value, dict):
        return {key: _rounded(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_rounded(item) for item in value]
    return value


def _print_text(summary: dict[str, object]) -> None:
    raw = summary.get("raw")
    if isinstance(raw, dict):
        print("Report 0x05 / handle 0x000A raw IMU")
        print(
            f"  notifications={raw['notifications']} active={raw['active_samples']} "
            f"duration={raw['duration_seconds']:.3f}s"
        )
        print(
            f"  timestamp/wall={raw['timestamp_wall_ratio']:.6f} "
            f"temperature={raw['temperature_c']['mean']:.3f} C "
            f"accel norm median={raw['accel_norm_counts']['median']:.2f} counts"
        )

    native = summary.get("native")
    if isinstance(native, dict):
        print("Report 0x09 / handle 0x000E native motion")
        print(
            f"  notifications={native['notifications']} lengths={native['motion_lengths']} "
            f"pdu40={native['pdu40_count']}"
        )
        print(
            f"  tick delta median={native['tick_delta']['median']:.1f}; "
            f"high-rate={native.get('high_rate', {}).get('count', 0)} "
            f"normal={native.get('normal', {}).get('count', 0)} "
            f"catchup={native.get('catchup', {}).get('count', 0)} "
            f"unclassified={native['unclassified']}"
        )
        print(
            "  encoded elapsed == preceding carrier delta: "
            f"{native['elapsed_ticks_match_preceding_delta']}/"
            f"{native['elapsed_ticks_comparisons']}"
        )
        print(f"  sensor status={native['sensor_status']}")
        high_rate = native.get("high_rate", {})
        if high_rate:
            print(
                "  high-rate accel norm medians="
                f"{high_rate['accel_1_norm_g']['median']:.3f}, "
                f"{high_rate['accel_2_norm_g']['median']:.3f} g"
            )
        normal = native.get("normal", {})
        if normal:
            print(
                "  normal accel norm medians="
                f"{normal['accel_a_norm_g']['median']:.3f}, "
                f"{normal['accel_b_norm_g']['median']:.3f}, "
                f"{normal['accel_c_norm_g']['median']:.3f} g"
            )
        catchup = native.get("catchup", {})
        if catchup:
            print(
                "  catch-up accel norm medians="
                f"{catchup['accel_1_norm_g']['median']:.3f}, "
                f"{catchup['accel_2_norm_g']['median']:.3f}, "
                f"{catchup['accel_3_norm_g']['median']:.3f} g"
            )
        audit = native["icm_fifo_audit"]
        print(
            "  exact ICM FIFO matches: "
            f"Packet4={audit['two_packet4_at_0_20']}/{native['pdu40_count']} "
            f"Packet3={audit['two_packet3_offsets']}"
        )
        print(
            "  legacy G6/G7/G8 aliases source payload bits "
            f"{native['legacy_g678_payload_bit_ranges']} (packed gyro/accel overlap)"
        )
        chart = native.get("carrier_chart", {})
        if chart:
            constraint = chart["strict_unit_constraint"]
            print(
                "  length-0x1E carrier states="
                f"{chart['state_counts']} transitions={len(chart['transitions'])} "
                "strict-unit violations="
                f"{constraint['violations']}/{constraint['records']}"
            )
            for transition in chart["transitions"]:
                delta = transition.get("cyclic_delta_norm")
                detail = (
                    f" cyclic-delta={delta:.6f}"
                    if isinstance(delta, float) else ""
                )
                print(
                    "    chart "
                    f"{transition['before_state']}->{transition['after_state']}"
                    f" at {transition['before_tick']:.0f}->"
                    f"{transition['after_tick']:.0f}{detail}"
                )
            for seam in chart["prefix_transition_epochs"]:
                print(
                    "    prefix seam epoch="
                    f"{seam['carrier_epoch_tick']:.0f} selects chart "
                    f"{seam['selected_state']} windows="
                    f"{seam['modular_windows']} carrier-error="
                    f"{seam['canonical_delta_norm']:.6f}"
                )
        prefix = native.get("prefix", {})
        if prefix:
            fit = prefix.get("carrier_epoch_fit", {})
            if fit:
                line = f"  prefix carrier fit: {fit.get('status')}"
                if "predecessor_offset_ticks" in fit:
                    line += (
                        " epoch=previous+"
                        f"{fit['predecessor_offset_ticks']:.2f} ticks"
                        f" corr={fit['mean_abs_correlation']:.6f}"
                        " fixed-nrmse="
                        f"{fit['mean_fixed_scale_normalized_rmse']:.6f}"
                    )
                elif "offset_ticks" in fit:
                    line += (
                        f" epoch={fit['offset_ticks']:+.2f} ticks"
                        f" corr={fit['mean_abs_correlation']:.6f}"
                        " fixed-nrmse="
                        f"{fit['mean_fixed_scale_normalized_rmse']:.6f}"
                    )
                print(line)
            history = prefix.get("history_orientation_decode", {})
            if history.get("angular_error_degrees"):
                errors = history["angular_error_degrees"]
                print(
                    "  prefix history decode error="
                    f"{errors['median']:.6f} deg median, "
                    f"{errors['max']:.6f} deg max "
                    f"({history['decoded_records']} records)"
                )
            for layout, tail in prefix.get("tails", {}).items():
                if tail.get("width") == 16:
                    total = tail["fractions_equal"] + tail["fractions_different"]
                    print(
                        f"  {layout} tail16 temperature="
                        f"{tail['temperature_a_c']['mean']:.4f}/"
                        f"{tail['temperature_b_c']['mean']:.4f} C "
                        "fraction-pair-equal="
                        f"{tail['fractions_equal']}/{total}"
                    )
    corpus = summary.get("motionpair_corpus")
    if isinstance(corpus, dict):
        for source, native_summary in corpus.items():
            print(f"\nMotionpair capture: {source}")
            _print_text({"native": native_summary})


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, help="report-0x05 / handle-0x000A PCAPNG")
    parser.add_argument("--native", type=Path, help="report-0x09 / handle-0x000E PCAPNG")
    parser.add_argument(
        "--blecap",
        type=Path,
        help="UART blecap JSONL; analyzes every retained 0x000A/0x000E input stream",
    )
    parser.add_argument(
        "--motionpair",
        type=Path,
        action="append",
        help=(
            "UART motionpair JSONL; repeat for a per-capture normalized corpus "
            "comparison"
        ),
    )
    parser.add_argument("--tshark", help="explicit path to tshark executable")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of text")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.raw and not args.native and not args.blecap and not args.motionpair:
        raise MotionReferenceError(
            "pass --raw, --native, --blecap, --motionpair, or a combination"
        )
    summary: dict[str, object] = {}
    if args.raw:
        summary["raw"] = analyze_raw(
            read_att_notifications(args.raw, 0x000A, args.tshark)
        )
    if args.native:
        summary["native"] = analyze_native(
            read_att_notifications(args.native, 0x000E, args.tshark)
        )
    if args.blecap:
        streams, dropped = read_blecap_jsonl(args.blecap)
        summary["blecap"] = {
            "dropped": dropped,
            "handles": {
                f"0x{handle:04X}": len(notifications)
                for handle, notifications in sorted(streams.items())
            },
        }
        if 0x000A in streams:
            summary["raw"] = analyze_raw(streams[0x000A])
        if 0x000E in streams:
            summary["native"] = analyze_native(
                streams[0x000E], dropped_records=dropped
            )
        if 0x000A not in streams and 0x000E not in streams:
            raise MotionReferenceError(
                f"{args.blecap}: no handle-0x000A or handle-0x000E input records"
            )
    if args.motionpair:
        corpus: dict[str, object] = {}
        for path in args.motionpair:
            notifications, dropped = read_motionpair_jsonl(path)
            corpus[str(path)] = analyze_native(
                notifications, dropped_records=dropped
            )
        if len(corpus) == 1 and not any(
            (args.raw, args.native, args.blecap)
        ):
            summary["native"] = next(iter(corpus.values()))
        else:
            summary["motionpair_corpus"] = corpus
    if args.json:
        print(json.dumps(_rounded(summary), indent=2))
    else:
        _print_text(summary)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MotionReferenceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
