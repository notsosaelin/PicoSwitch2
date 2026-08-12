#!/usr/bin/env python3
"""Inspect and splice native Switch 2 motion PDUs by semantic group.

This is the offline half of the genuine/generated hybrid harness.  It never
guesses byte boundaries: every bit in a length-0x1E carrier or mode-3
length-0x28 IMU packet belongs to exactly one named group.  A splice copies
only the selected bit masks from a donor and proves every unselected bit stayed
identical to the genuine base packet.

The tool is deliberately structural.  A packet can fit every field and still
be rejected by the console because two sources describe different physical
instants or orientations.  Live source/epoch alignment belongs to the firmware
harness and must fail closed independently.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ns2_motion_packet as K
import ns2_motion_reference as R


class HybridError(ValueError):
    """Two packets cannot be compared or spliced safely."""


GROUP_BITS = {
    "timing": 1 << 0,
    "status": 1 << 1,
    "packing": 1 << 2,
    "prefix": 1 << 3,
    "accel": 1 << 4,
    "gyro": 1 << 5,
    "tail": 1 << 6,
    "temperature": 1 << 7,
    "flags_reserved": 1 << 8,
}

MODE_GROUPS = {
    "genuine": 0,
    "accel": GROUP_BITS["accel"],
    "gyro": GROUP_BITS["gyro"],
    "prefix": GROUP_BITS["prefix"],
    "imu": GROUP_BITS["accel"] | GROUP_BITS["gyro"],
    "all": (
        GROUP_BITS["prefix"] | GROUP_BITS["accel"] | GROUP_BITS["gyro"]
    ),
}


@dataclass(frozen=True)
class BitRange:
    offset: int
    width: int

    @property
    def stop(self) -> int:
        return self.offset + self.width


MOTION30_GROUPS: dict[str, tuple[BitRange, ...]] = {
    "timing": (BitRange(0, 16),),
    "temperature": (BitRange(16, 16),),
    # Chart state plus the three unequal-width carrier lanes.  The unused and
    # still-unexplained flag bits in bytes 4/8/12 are intentionally separate.
    "prefix": (
        BitRange(32, 2), BitRange(40, 24), BitRange(64, 2),
        BitRange(72, 24), BitRange(96, 2), BitRange(104, 24),
    ),
    "flags_reserved": (
        BitRange(34, 6), BitRange(66, 6), BitRange(98, 6),
    ),
    "accel": (BitRange(128, 96),),
    "tail": (BitRange(224, 16),),
}


def _range(offset: int, width: int) -> BitRange:
    return BitRange(offset, width)


def _motion40_groups(layout: str) -> dict[str, tuple[BitRange, ...]]:
    try:
        spec = K._LAYOUT_FIELDS[layout]  # one authoritative inverse-codec map
    except KeyError as error:
        raise HybridError(f"unsupported 0x28 layout: {layout}") from error
    payload = 32  # four-byte tick/elapsed/status preamble
    return {
        "timing": (_range(0, 24),),
        "status": (_range(24, 8),),
        "packing": (_range(payload, 2),),
        "prefix": tuple(
            _range(payload + offset, width)
            for offset, width in spec["carrier"]
        ),
        "accel": tuple(
            _range(payload + offset, width * 3)
            for offset, width in spec["accel"]
        ),
        "gyro": tuple(
            _range(payload + offset, width * 3)
            for offset, width in spec["gyro"]
        ),
        "tail": (_range(payload + spec["tail"][0], spec["tail"][1]),),
    }


def _check_partition(
    groups: dict[str, tuple[BitRange, ...]], total_bits: int
) -> None:
    owner: list[str | None] = [None] * total_bits
    for name, ranges in groups.items():
        for span in ranges:
            if span.offset < 0 or span.width <= 0 or span.stop > total_bits:
                raise HybridError(
                    f"{name} range [{span.offset},{span.stop}) exceeds {total_bits} bits"
                )
            for bit in range(span.offset, span.stop):
                if owner[bit] is not None:
                    raise HybridError(
                        f"bit {bit} overlaps {owner[bit]} and {name}"
                    )
                owner[bit] = name
    missing = [bit for bit, name in enumerate(owner) if name is None]
    if missing:
        raise HybridError(
            f"field partition leaves {len(missing)} bits uncovered; first={missing[0]}"
        )


def packet_groups(pdu: bytes) -> dict[str, tuple[BitRange, ...]]:
    """Return a complete, non-overlapping semantic partition for one PDU."""

    if len(pdu) == 0x1E:
        groups = MOTION30_GROUPS
    elif len(pdu) == 0x28:
        decoded = R.decode_motion40(pdu, None)
        if decoded.packing_mode != 3:
            raise HybridError(
                f"0x28 packing mode {decoded.packing_mode} is not spliceable"
            )
        groups = _motion40_groups(decoded.layout)
    else:
        raise HybridError(f"motion PDU must be 30 or 40 bytes, got {len(pdu)}")
    _check_partition(groups, len(pdu) * 8)
    return groups


def _packet_value(pdu: bytes) -> int:
    return int.from_bytes(pdu, "little")


def _mask_for(ranges: Iterable[BitRange]) -> int:
    mask = 0
    for span in ranges:
        mask |= ((1 << span.width) - 1) << span.offset
    return mask


def group_mask(pdu: bytes, group: str) -> int:
    groups = packet_groups(pdu)
    if group not in groups:
        raise HybridError(
            f"group {group!r} is unavailable; choose {', '.join(groups)}"
        )
    return _mask_for(groups[group])


def structural_fit(base: bytes, donor: bytes) -> dict[str, object]:
    """Prove two packets use compatible field partitions.

    This does not claim their source epochs or poses match.
    """

    if len(base) != len(donor):
        raise HybridError(
            f"length mismatch: base={len(base)}, donor={len(donor)}"
        )
    base_groups = packet_groups(base)
    donor_groups = packet_groups(donor)
    if tuple(base_groups) != tuple(donor_groups):
        raise HybridError("semantic group sets differ")
    result: dict[str, object] = {
        "length": len(base),
        "groups": list(base_groups),
        "structural_fit": True,
        "physical_alignment_proven": False,
    }
    if len(base) == 0x28:
        left = R.decode_motion40(base, None)
        right = R.decode_motion40(donor, None)
        if left.layout != right.layout:
            raise HybridError(
                f"layout mismatch: base={left.layout}, donor={right.layout}"
            )
        if left.sensor_status != right.sensor_status:
            raise HybridError(
                f"status mismatch: base=0x{left.sensor_status:02X}, "
                f"donor=0x{right.sensor_status:02X}"
            )
        result.update({
            "layout": left.layout,
            "status": left.sensor_status,
            "base_tick": left.tick,
            "base_elapsed": left.elapsed_ticks,
            "donor_tick": right.tick,
            "donor_elapsed": right.elapsed_ticks,
        })
    return result


def splice(base: bytes, donor: bytes, selected: Sequence[str]) -> bytes:
    """Copy selected semantic groups from donor into a genuine base packet."""

    fit = structural_fit(base, donor)
    del fit  # structural validation is the side effect we need here
    groups = packet_groups(base)
    if not selected:
        raise HybridError("select at least one semantic group")
    unknown = [name for name in selected if name not in groups]
    if unknown:
        raise HybridError(
            f"unavailable groups {unknown}; choose {', '.join(groups)}"
        )
    mask = 0
    for name in selected:
        mask |= _mask_for(groups[name])
    width_mask = (1 << (len(base) * 8)) - 1
    base_value = _packet_value(base)
    donor_value = _packet_value(donor)
    output_value = (base_value & ~mask) | (donor_value & mask)
    output = (output_value & width_mask).to_bytes(len(base), "little")

    if ((output_value ^ base_value) & ~mask & width_mask) != 0:
        raise AssertionError("splice changed an unselected bit")
    # Re-decode to make layout-changing combinations fail closed.
    if len(output) == 0x28:
        decoded = R.decode_motion40(output, None)
        base_decoded = R.decode_motion40(base, None)
        if decoded.packing_mode != 3 or decoded.layout != base_decoded.layout:
            raise HybridError("selected groups changed the packet layout")
        if decoded.sensor_status != base_decoded.sensor_status:
            raise HybridError("selected groups changed the layout status")
    return output


def inspect_packet(pdu: bytes) -> dict[str, object]:
    groups = packet_groups(pdu)
    value = _packet_value(pdu)
    result: dict[str, object] = {
        "length": len(pdu),
        "payload": pdu.hex().upper(),
        "groups": {
            name: {
                "ranges": [[span.offset, span.width] for span in ranges],
                "masked_hex": (
                    value & _mask_for(ranges)
                ).to_bytes(len(pdu), "little").hex().upper(),
            }
            for name, ranges in groups.items()
        },
    }
    if len(pdu) == 0x1E:
        carrier = R.decode_motion30_orientation(pdu)
        result["decoded"] = {
            "kind": "carrier",
            "tick": pdu[0] | ((pdu[1] & 0x0F) << 8),
            "elapsed": pdu[1] >> 4,
            "state": carrier.state,
            "carrier_raw": list(carrier.carrier_raw),
            "quaternion_wxyz": list(carrier.quaternion_wxyz),
            "byte12_flag": bool(pdu[12] & K.MOTION30_BYTE12_FLAG),
        }
    else:
        sample = R.decode_motion40(pdu, None)
        result["decoded"] = {
            "kind": "multi_sample_imu",
            "layout": sample.layout,
            "tick": sample.tick,
            "elapsed": sample.elapsed_ticks,
            "status": sample.sensor_status,
            "packing_mode": sample.packing_mode,
            "prefix_carrier": list(sample.prefix_carrier),
            "accel_wire": [list(vector) for vector in sample.accel],
            "gyro_wire": [list(vector) for vector in sample.gyro],
            "accel_counts": [
                list(vector) for vector in R.normalized_vectors(sample, "accel")
            ],
            "gyro_counts": [
                list(vector) for vector in R.normalized_vectors(sample, "gyro")
            ],
            "tail": sample.tail_value,
        }
    return result


def capture_pdus(path: Path) -> tuple[list[bytes], int]:
    """Load exact native PDUs from motionpair or blecap UART JSONL."""

    errors: list[str] = []
    for reader in (R.read_motionpair_jsonl, R.read_blecap_jsonl):
        try:
            records, dropped = reader(path)
        except Exception as error:
            errors.append(str(error))
            continue
        notifications = records
        if isinstance(records, dict):
            notifications = [
                item for stream in records.values() for item in stream
            ]
        pdus: list[bytes] = []
        for item in notifications:
            report = item.value
            if len(report) < 15:
                continue
            length = report[0x0E]
            end = 0x0F + length
            if length in (0x1E, 0x28) and len(report) >= end:
                pdus.append(bytes(report[0x0F:end]))
        if pdus:
            return pdus, dropped
    raise HybridError(
        f"{path}: no native motion PDUs ({' | '.join(errors)})"
    )


def audit_live_capture(path: Path) -> dict[str, object]:
    """Validate one retained live-hybrid capture and its fail-closed contract."""

    records: list[dict[str, object]] = []
    end: dict[str, object] | None = None
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8-sig").splitlines(), 1
    ):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as error:
            raise HybridError(
                f"{path}:{line_number}: invalid JSON: {error.msg}"
            ) from error
        if not isinstance(item, dict) or "motionhybrid" not in item:
            raise HybridError(f"{path}:{line_number}: not a hybrid record")
        if item["motionhybrid"] == "record":
            if end is not None:
                raise HybridError(f"{path}:{line_number}: data after end")
            records.append(item)
        elif item["motionhybrid"] == "end":
            if end is not None:
                raise HybridError(f"{path}:{line_number}: duplicate end")
            end = item
        else:
            raise HybridError(
                f"{path}:{line_number}: unexpected event "
                f"{item['motionhybrid']!r}"
            )
    if end is None:
        raise HybridError(f"{path}: missing end record")
    if int(end.get("records", -1)) != len(records):
        raise HybridError(
            f"{path}: end count {end.get('records')} != {len(records)} records"
        )
    dropped = int(end.get("dropped", 0))
    if dropped:
        raise HybridError(f"{path}: capture dropped {dropped} record(s)")
    if not records:
        raise HybridError(f"{path}: capture contains no records")

    reasons: dict[str, int] = {}
    modes: dict[str, int] = {}
    applied = 0
    fallbacks = 0
    changed_records = 0
    for index, record in enumerate(records):
        mode = str(record.get("mode", ""))
        reason = str(record.get("reason", ""))
        if mode not in MODE_GROUPS:
            raise HybridError(f"record {index}: unknown mode {mode!r}")
        groups = int(record.get("requested_groups", -1))
        if groups != MODE_GROUPS[mode]:
            raise HybridError(
                f"record {index}: mode {mode} declares groups 0x{groups:X}, "
                f"expected 0x{MODE_GROUPS[mode]:X}"
            )
        length = int(record.get("native_len", 0))
        if length not in (0x1E, 0x28):
            raise HybridError(f"record {index}: invalid native_len {length}")
        try:
            base = bytes.fromhex(str(record["base"]))
            delta = bytes.fromhex(str(record["output_xor"]))
        except (KeyError, ValueError) as error:
            raise HybridError(f"record {index}: invalid base/output XOR") from error
        if len(base) != length or len(delta) != length:
            raise HybridError(
                f"record {index}: base/XOR length does not match {length}"
            )
        output = bytes(left ^ right for left, right in zip(base, delta))
        changed = sum(byte.bit_count() for byte in delta)
        if int(record.get("changed_bits", -1)) != changed:
            raise HybridError(
                f"record {index}: changed_bits disagrees with output XOR"
            )

        if reason == "applied":
            if not bool(record.get("pose_aligned")) or groups == 0:
                raise HybridError(
                    f"record {index}: applied donor lacks pose/group gate"
                )
            allowed = 0
            available = packet_groups(base)
            for name, bit in GROUP_BITS.items():
                if groups & bit:
                    if name not in available:
                        raise HybridError(
                            f"record {index}: selected group {name} is unavailable"
                        )
                    allowed |= group_mask(base, name)
            delta_value = int.from_bytes(delta, "little")
            if delta_value & ~allowed:
                raise HybridError(
                    f"record {index}: output changed an unselected bit"
                )
            # Re-decode 0x28, proving immutable timing/status/layout survived.
            # The mask proof above is the corresponding complete invariant for
            # a 0x1E carrier: timing, temperature, flags, acceleration and tail
            # are all outside the selected prefix ranges.
            if length == 0x28:
                before = R.decode_motion40(base, None)
                after = R.decode_motion40(output, None)
                if (
                    before.tick != after.tick
                    or before.elapsed_ticks != after.elapsed_ticks
                    or before.sensor_status != after.sensor_status
                    or before.layout != after.layout
                ):
                    raise HybridError(
                        f"record {index}: immutable packet identity changed"
                    )
            applied += 1
        else:
            if changed:
                raise HybridError(
                    f"record {index}: non-applied reason {reason!r} changed data"
                )
            fallbacks += int(reason != "genuine_control")
        changed_records += int(changed != 0)
        reasons[reason] = reasons.get(reason, 0) + 1
        modes[mode] = modes.get(mode, 0) + 1

    return {
        "schema": "picoswitch2-motion-hybrid-audit/v1",
        "source": str(path),
        "records": len(records),
        "dropped": dropped,
        "modes": modes,
        "reasons": reasons,
        "applied": applied,
        "fallbacks": fallbacks,
        "changed_records": changed_records,
        "fail_closed": True,
    }


def _hex_pdu(value: str) -> bytes:
    try:
        pdu = bytes.fromhex(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if len(pdu) not in (0x1E, 0x28):
        raise argparse.ArgumentTypeError(
            f"PDU must decode to 30 or 40 bytes, got {len(pdu)}"
        )
    return pdu


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    inspect_cmd = sub.add_parser("inspect", help="explode one PDU into groups")
    inspect_cmd.add_argument("pdu", type=_hex_pdu)

    fit_cmd = sub.add_parser("fit", help="check structural splice compatibility")
    fit_cmd.add_argument("base", type=_hex_pdu)
    fit_cmd.add_argument("donor", type=_hex_pdu)

    splice_cmd = sub.add_parser("splice", help="copy named groups into base")
    splice_cmd.add_argument("base", type=_hex_pdu)
    splice_cmd.add_argument("donor", type=_hex_pdu)
    splice_cmd.add_argument("--group", action="append", required=True)

    capture_cmd = sub.add_parser(
        "capture", help="explode every PDU in a motionpair/blecap JSONL"
    )
    capture_cmd.add_argument("path", type=Path)
    capture_cmd.add_argument("--output", type=Path)

    audit_cmd = sub.add_parser(
        "audit-capture",
        help="validate a live hybrid base/XOR UART capture",
    )
    audit_cmd.add_argument("path", type=Path)
    audit_cmd.add_argument("--output", type=Path)

    args = parser.parse_args(argv)
    try:
        if args.command == "inspect":
            output: object = inspect_packet(args.pdu)
        elif args.command == "fit":
            output = structural_fit(args.base, args.donor)
        elif args.command == "splice":
            result = splice(args.base, args.donor, args.group)
            output = {
                "fit": structural_fit(args.base, args.donor),
                "selected": args.group,
                "payload": result.hex().upper(),
                "inspection": inspect_packet(result),
            }
        elif args.command == "capture":
            pdus, dropped = capture_pdus(args.path)
            if dropped:
                raise HybridError(f"capture dropped {dropped} records")
            output = {
                "schema": "picoswitch2-motion-fitment/v1",
                "source": str(args.path),
                "dropped": dropped,
                "count": len(pdus),
                "records": [inspect_packet(pdu) for pdu in pdus],
            }
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(
                    json.dumps(output, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
        else:
            output = audit_live_capture(args.path)
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(
                    json.dumps(output, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
        print(json.dumps(output, indent=2, sort_keys=True))
    except (OSError, HybridError, R.MotionReferenceError) as error:
        print(f"motion hybrid error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
