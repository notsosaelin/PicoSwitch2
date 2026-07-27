#!/usr/bin/env python3
"""Decode and semantically compare PicoSwitch2 UART protocol traces.

The input format is the JSON-lines output from::

    ./tools/read_uart_diag.ps1 -Port COM11 -Command 'trace dump'

Only Python's standard library is required. Pairing challenges, controller
addresses, and identity serials are not printed unless --show-payload is used.
"""

from __future__ import annotations

import argparse
import difflib
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, TextIO


TRACE_KINDS = {
    "ep0_setup",
    "ep0_response",
    "bulk_command",
    "bulk_response",
    "hid_output",
}
TRACE_DIRECTIONS = {"console_to_device", "device_to_console"}
PERSONALITIES = {"pro2", "gc", "joycon_l", "joycon_r", "config"}

COMMAND_NAMES = {
    0x01: "nfc",
    0x02: "flash_memory",
    0x03: "initialization",
    0x04: "unused_04",
    0x05: "unknown_05",
    0x06: "power",
    0x07: "first_init",
    0x08: "charging_grip",
    0x09: "player_leds",
    0x0A: "vibration",
    0x0B: "battery",
    0x0C: "features",
    0x0D: "firmware_update",
    0x0E: "unused_0e",
    0x0F: "unknown_0f",
    0x10: "firmware_info",
    0x11: "unknown_parameters",
    0x12: "unused_12",
    0x13: "unknown_joycon",
    0x14: "unknown_14",
    0x15: "bluetooth_pairing",
    0x16: "unknown_status",
    0x17: "unknown_audio_config",
    0x18: "unknown_18",
}

SUBCOMMAND_NAMES = {
    (0x01, 0x01): "nfc_start",
    (0x01, 0x0C): "nfc_identity",
    (0x02, 0x01): "read_0x40",
    (0x02, 0x03): "erase_sector",
    (0x02, 0x04): "read",
    (0x02, 0x05): "write",
    (0x03, 0x01): "bt_wake",
    (0x03, 0x02): "bt_wake_cancel",
    (0x03, 0x03): "enable_hid",
    (0x03, 0x07): "pairing_info_send",
    (0x03, 0x08): "pairing_info_clear",
    (0x03, 0x09): "pairing_info_store",
    (0x03, 0x0A): "select_report",
    (0x03, 0x0C): "unknown_0c",
    (0x03, 0x0D): "init_usb",
    (0x03, 0x0F): "unknown_0f",
    (0x06, 0x02): "shutdown",
    (0x06, 0x03): "reboot",
    (0x07, 0x01): "first_init",
    (0x08, 0x02): "enable_grip_buttons",
    (0x09, 0x07): "set_player_leds",
    (0x0A, 0x02): "play_sample",
    (0x0A, 0x08): "vibration_data",
    (0x0B, 0x03): "voltage",
    (0x0B, 0x04): "charge_state",
    (0x0C, 0x01): "get_info",
    (0x0C, 0x02): "set_mask",
    (0x0C, 0x03): "clear_mask",
    (0x0C, 0x04): "enable",
    (0x0C, 0x05): "disable",
    (0x0C, 0x06): "configure",
    (0x10, 0x01): "get_version",
    (0x11, 0x01): "unknown_01",
    (0x11, 0x03): "unknown_03",
    (0x15, 0x01): "exchange_addresses",
    (0x15, 0x02): "confirm_ltk",
    (0x15, 0x03): "finalize",
    (0x15, 0x04): "exchange_keys",
    (0x16, 0x01): "unknown_01",
    (0x18, 0x01): "unknown_01",
    (0x18, 0x03): "unknown_03",
}

EP0_NAMES = {0x02: "firmware_info", 0x03: "identity", 0x04: "identity_ack"}
FEATURE_BITS = {
    0x01: "buttons",
    0x02: "sticks",
    0x04: "imu",
    0x10: "mouse",
    0x20: "rumble",
    0x80: "magnetometer",
}


class TraceError(ValueError):
    """A trace is malformed or internally inconsistent."""


@dataclass
class TraceCapture:
    records: list[dict[str, Any]]
    overwritten: int = 0


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TraceError(f"{name} must be an integer")
    if not minimum <= value <= maximum:
        raise TraceError(f"{name} is outside {minimum}..{maximum}: {value}")
    return value


def _validate_record(record: dict[str, Any], source: str, line_number: int) -> None:
    required = {
        "seq", "t_us", "personality", "kind", "dir", "id", "sub",
        "length", "captured", "payload",
    }
    missing = sorted(required - record.keys())
    if missing:
        raise TraceError(f"{source}:{line_number}: missing fields: {', '.join(missing)}")
    if record.get("trace") != "record":
        raise TraceError(f"{source}:{line_number}: expected trace record")
    if record["personality"] not in PERSONALITIES:
        raise TraceError(f"{source}:{line_number}: unknown personality {record['personality']!r}")
    if record["kind"] not in TRACE_KINDS:
        raise TraceError(f"{source}:{line_number}: unknown kind {record['kind']!r}")
    if record["dir"] not in TRACE_DIRECTIONS:
        raise TraceError(f"{source}:{line_number}: unknown direction {record['dir']!r}")
    _integer(record["seq"], "seq", 0, 0xFFFFFFFF)
    _integer(record["t_us"], "t_us", 0, 0xFFFFFFFF)
    _integer(record["id"], "id", 0, 0xFF)
    _integer(record["sub"], "sub", 0, 0xFF)
    length = _integer(record["length"], "length", 0, 0xFFFF)
    captured = _integer(record["captured"], "captured", 0, 72)
    if captured > length:
        raise TraceError(f"{source}:{line_number}: captured length exceeds total length")
    payload = record["payload"]
    if not isinstance(payload, str) or len(payload) != captured * 2:
        raise TraceError(f"{source}:{line_number}: payload length does not match captured")
    try:
        raw = bytes.fromhex(payload)
    except ValueError as exc:
        raise TraceError(f"{source}:{line_number}: payload is not hexadecimal") from exc

    expected_direction = {
        "ep0_setup": "console_to_device",
        "ep0_response": "device_to_console",
        "bulk_command": "console_to_device",
        "bulk_response": "device_to_console",
        "hid_output": "console_to_device",
    }[record["kind"]]
    if record["dir"] != expected_direction:
        raise TraceError(f"{source}:{line_number}: direction disagrees with event kind")
    if record["kind"] == "ep0_setup":
        if length != 8 or captured != 8 or raw[1] != record["id"]:
            raise TraceError(f"{source}:{line_number}: malformed EP0 setup summary")
    if record["kind"] in ("bulk_command", "bulk_response"):
        if length < 8 or captured < 8:
            raise TraceError(f"{source}:{line_number}: bulk command header is incomplete")
        if raw[0] != record["id"] or raw[3] != record["sub"]:
            raise TraceError(f"{source}:{line_number}: bulk summary disagrees with payload header")
        expected_header_direction = 0x91 if record["kind"] == "bulk_command" else (0x01, 0x04)
        if isinstance(expected_header_direction, tuple):
            direction_valid = raw[1] in expected_header_direction
        else:
            direction_valid = raw[1] == expected_header_direction
        if not direction_valid:
            raise TraceError(f"{source}:{line_number}: invalid bulk direction byte 0x{raw[1]:02X}")


def read_trace(stream: TextIO, source: str = "<stream>") -> TraceCapture:
    records: list[dict[str, Any]] = []
    end: dict[str, Any] | None = None
    previous_sequence: int | None = None
    for line_number, line in enumerate(stream, 1):
        text = line.strip()
        if not text:
            continue
        if end is not None:
            raise TraceError(f"{source}:{line_number}: data appears after trace end")
        try:
            item = json.loads(text)
        except json.JSONDecodeError as exc:
            raise TraceError(f"{source}:{line_number}: invalid JSON: {exc.msg}") from exc
        if not isinstance(item, dict):
            raise TraceError(f"{source}:{line_number}: JSON value must be an object")
        if item.get("trace") == "end":
            if end is not None:
                raise TraceError(f"{source}:{line_number}: duplicate trace end record")
            end = item
            continue
        _validate_record(item, source, line_number)
        sequence = item["seq"]
        if previous_sequence is not None and sequence != previous_sequence + 1:
            raise TraceError(
                f"{source}:{line_number}: sequence {sequence} does not follow {previous_sequence}"
            )
        previous_sequence = sequence
        records.append(item)

    if end is None:
        raise TraceError(f"{source}: trace end record is missing")
    count = _integer(end.get("records"), "end.records", 0, 0xFFFF)
    overwritten = _integer(end.get("overwritten", 0), "end.overwritten", 0, 0xFFFFFFFF)
    if count != len(records):
        raise TraceError(f"{source}: end count {count} does not match {len(records)} records")
    return TraceCapture(records, overwritten)


def load_trace(path: str) -> TraceCapture:
    if path == "-":
        return read_trace(sys.stdin)
    with Path(path).open("r", encoding="utf-8") as stream:
        return read_trace(stream, path)


def payload(record: dict[str, Any]) -> bytes:
    return bytes.fromhex(record["payload"])


def command_name(command: int, subcommand: int) -> str:
    command_text = COMMAND_NAMES.get(command, f"unknown_{command:02x}")
    subcommand_text = SUBCOMMAND_NAMES.get((command, subcommand), f"sub_{subcommand:02x}")
    return f"{command_text}.{subcommand_text}"


def feature_mask_text(mask: int) -> str:
    names = [name for bit, name in FEATURE_BITS.items() if mask & bit]
    unknown = mask & ~sum(FEATURE_BITS)
    if unknown:
        names.append(f"unknown_0x{unknown:02X}")
    return "+".join(names) if names else "none"


def _version(data: bytes) -> str:
    return ".".join(str(value) for value in data)


def _bulk_fields(record: dict[str, Any], strict: bool = False) -> dict[str, Any]:
    raw = payload(record)
    fields: dict[str, Any] = {}
    if len(raw) < 8:
        fields["framing"] = "truncated_header"
        fields["payload"] = raw.hex().upper()
        return fields

    command, direction, transport, subcommand = raw[0:4]
    fields.update({
        "command": command,
        "command_name": command_name(command, subcommand),
        "direction_byte": direction,
        "transport": transport,
        "subcommand": subcommand,
    })
    if record["kind"] == "bulk_command":
        fields["declared_data_length"] = raw[5]
    else:
        fields["ack"] = raw[4:8].hex().upper()
    data = raw[8:]

    if command == 0x02 and subcommand in (0x01, 0x04, 0x05) and len(data) >= 8:
        fields["memory_length"] = data[0]
        fields["memory_address"] = f"0x{int.from_bytes(data[4:8], 'little'):08X}"
        if record["kind"] == "bulk_response" and len(data) > 8:
            fields["memory_data_prefix"] = data[8:].hex().upper()
    elif command == 0x10 and subcommand == 0x01 and record["kind"] == "bulk_response" and len(data) >= 12:
        fields["controller_version"] = _version(data[0:3])
        fields["controller_type"] = data[3]
        fields["bluetooth_version"] = _version(data[4:7])
        fields["dsp_version"] = _version(data[8:11])
    elif command == 0x0C and record["kind"] == "bulk_command" and data:
        fields["feature_mask"] = f"0x{data[0]:02X}"
        fields["features"] = feature_mask_text(data[0])
        if subcommand == 0x06 and len(data) >= 5:
            fields["feature_id"] = data[4]
    elif command == 0x09 and subcommand == 0x07 and record["kind"] == "bulk_command" and data:
        fields["player_led_mask"] = f"0x{data[0]:02X}"
    elif command == 0x03 and subcommand == 0x0A and record["kind"] == "bulk_command" and data:
        fields["input_report"] = f"0x{data[0]:02X}"
    elif command == 0x0A and subcommand == 0x02 and record["kind"] == "bulk_command" and data:
        fields["sample"] = data[0]
    elif command == 0x15:
        fields["sensitive_payload"] = "redacted"
    elif command == 0x03 and subcommand == 0x0D and record["kind"] == "bulk_command":
        fields["sensitive_payload"] = "host address redacted"
    else:
        fields["data"] = data.hex().upper()

    if strict:
        fields["raw_payload"] = raw.hex().upper()
    return fields


def semantic_record(record: dict[str, Any], strict: bool = False) -> dict[str, Any]:
    raw = payload(record)
    fields: dict[str, Any] = {
        "kind": record["kind"],
        "direction": record["dir"],
        "id": record["id"],
        "sub": record["sub"],
        "length": record["length"],
    }
    kind = record["kind"]
    if kind == "ep0_setup" and len(raw) == 8:
        fields.update({
            "request_type": f"0x{raw[0]:02X}",
            "request": f"0x{raw[1]:02X}",
            "request_name": EP0_NAMES.get(raw[1], "unknown"),
            "value": f"0x{int.from_bytes(raw[2:4], 'little'):04X}",
            "index": f"0x{int.from_bytes(raw[4:6], 'little'):04X}",
            "requested_length": int.from_bytes(raw[6:8], "little"),
        })
    elif kind == "ep0_response" and record["id"] == 0x02 and len(raw) >= 10:
        fields.update({
            "response_name": "firmware_info",
            "controller_version": _version(raw[0:3]),
            "bluetooth_version": f"{raw[6]}.0.0",
            "unit_address": "redacted",
        })
        if strict:
            fields["raw_payload"] = raw.hex().upper()
    elif kind == "ep0_response" and record["id"] == 0x03:
        fields["response_name"] = "identity"
        fields["serial"] = "redacted"
        if len(raw) >= 22:
            fields["vid"] = f"0x{int.from_bytes(raw[18:20], 'little'):04X}"
            fields["pid"] = f"0x{int.from_bytes(raw[20:22], 'little'):04X}"
        if strict:
            fields["raw_payload"] = raw.hex().upper()
    elif kind == "ep0_response":
        fields["response_name"] = EP0_NAMES.get(record["id"], "unknown")
        fields["data"] = raw.hex().upper()
    elif kind in ("bulk_command", "bulk_response"):
        fields.update(_bulk_fields(record, strict))
    elif kind == "hid_output":
        fields["report_id"] = f"0x{record['id']:02X}"
        fields["instance"] = record["sub"]
        fields["data_prefix"] = raw.hex().upper()
    return fields


def describe_record(record: dict[str, Any], show_payload: bool = False) -> str:
    fields = semantic_record(record, strict=show_payload)
    kind = record["kind"]
    if kind == "ep0_setup":
        summary = (
            f"EP0 SETUP {fields['request_name']} req={fields['request']} "
            f"value={fields['value']} index={fields['index']} len={fields['requested_length']}"
        )
    elif kind == "ep0_response":
        summary = f"EP0 RESPONSE {fields.get('response_name', 'unknown')} len={record['length']}"
        for name in ("controller_version", "bluetooth_version", "vid", "pid"):
            if name in fields:
                summary += f" {name}={fields[name]}"
    elif kind in ("bulk_command", "bulk_response"):
        arrow = "REQUEST" if kind == "bulk_command" else "RESPONSE"
        summary = f"BULK {arrow} 0x{record['id']:02X}/0x{record['sub']:02X} {fields['command_name']}"
        for name in (
            "memory_address", "memory_length", "controller_version", "controller_type",
            "bluetooth_version", "dsp_version", "feature_mask", "features", "feature_id",
            "player_led_mask", "input_report", "sample",
        ):
            if name in fields:
                summary += f" {name}={fields[name]}"
    else:
        summary = f"HID OUTPUT report=0x{record['id']:02X} instance={record['sub']} len={record['length']}"
    if show_payload:
        summary += f" payload={record['payload']}"
    return summary


def _relative_times(records: Iterable[dict[str, Any]]) -> Iterable[tuple[int, int]]:
    first: int | None = None
    previous: int | None = None
    for record in records:
        timestamp = record["t_us"]
        if first is None:
            first = timestamp
            previous = timestamp
        elapsed = (timestamp - first) & 0xFFFFFFFF
        delta = (timestamp - previous) & 0xFFFFFFFF
        previous = timestamp
        yield elapsed, delta


def render_decode(capture: TraceCapture, show_payload: bool = False) -> str:
    lines = [
        f"records={len(capture.records)} overwritten={capture.overwritten}",
        " seq   elapsed    delta  direction personality event",
    ]
    for record, (elapsed, delta) in zip(capture.records, _relative_times(capture.records)):
        direction = "C->D" if record["dir"] == "console_to_device" else "D->C"
        lines.append(
            f"{record['seq']:4d} {elapsed / 1000:9.3f} {delta / 1000:8.3f} "
            f"{direction:>9} {record['personality']:<11} "
            f"{describe_record(record, show_payload)}"
        )
    return "\n".join(lines)


def record_signature(record: dict[str, Any]) -> tuple[Any, ...]:
    signature: tuple[Any, ...] = (
        record["kind"], record["dir"], record["id"], record["sub"], record["length"]
    )
    raw = payload(record)
    if record["kind"] in ("bulk_command", "bulk_response") and record["id"] == 0x02 and len(raw) >= 16:
        signature += (int.from_bytes(raw[12:16], "little"),)
    return signature


def _flatten(value: Any, prefix: str = "") -> dict[str, Any]:
    if not isinstance(value, dict):
        return {prefix: value}
    result: dict[str, Any] = {}
    for key, child in value.items():
        path = f"{prefix}.{key}" if prefix else key
        result.update(_flatten(child, path))
    return result


def _field_differences(left: dict[str, Any], right: dict[str, Any]) -> list[str]:
    left_flat = _flatten(left)
    right_flat = _flatten(right)
    differences = []
    for key in sorted(left_flat.keys() | right_flat.keys()):
        if left_flat.get(key) != right_flat.get(key):
            differences.append(f"{key}: {left_flat.get(key)!r} -> {right_flat.get(key)!r}")
    return differences


def render_diff(
    left: TraceCapture,
    right: TraceCapture,
    left_name: str,
    right_name: str,
    strict: bool = False,
    show_equal: bool = False,
) -> tuple[str, bool]:
    left_signatures = [record_signature(record) for record in left.records]
    right_signatures = [record_signature(record) for record in right.records]
    matcher = difflib.SequenceMatcher(None, left_signatures, right_signatures, autojunk=False)
    lines = [f"left={left_name} ({len(left.records)} records)",
             f"right={right_name} ({len(right.records)} records)"]
    equal = changed = inserted = deleted = 0

    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            for offset in range(i2 - i1):
                left_record = left.records[i1 + offset]
                right_record = right.records[j1 + offset]
                differences = _field_differences(
                    semantic_record(left_record, strict), semantic_record(right_record, strict)
                )
                label = describe_record(left_record)
                if differences:
                    changed += 1
                    lines.append(f"~ L{left_record['seq']} R{right_record['seq']} {label}")
                    lines.extend(f"    {difference}" for difference in differences)
                else:
                    equal += 1
                    if show_equal:
                        lines.append(f"= L{left_record['seq']} R{right_record['seq']} {label}")
        else:
            if tag in ("replace", "delete"):
                for record in left.records[i1:i2]:
                    deleted += 1
                    lines.append(f"- L{record['seq']} {describe_record(record)}")
            if tag in ("replace", "insert"):
                for record in right.records[j1:j2]:
                    inserted += 1
                    lines.append(f"+ R{record['seq']} {describe_record(record)}")

    lines.append(
        f"summary equal={equal} changed={changed} inserted={inserted} deleted={deleted} "
        f"left_overwritten={left.overwritten} right_overwritten={right.overwritten}"
    )
    return "\n".join(lines), not (changed or inserted or deleted)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)

    decode = subparsers.add_parser("decode", help="render a readable trace timeline")
    decode.add_argument("trace", help="trace JSONL path, or - for stdin")
    decode.add_argument(
        "--show-payload", action="store_true",
        help="show raw prefixes, including per-unit and pairing material",
    )

    diff = subparsers.add_parser("diff", help="semantically compare two trace timelines")
    diff.add_argument("left")
    diff.add_argument("right")
    diff.add_argument("--strict", action="store_true", help="include every captured payload byte")
    diff.add_argument("--show-equal", action="store_true", help="also print matching records")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.operation == "decode":
            print(render_decode(load_trace(args.trace), args.show_payload))
            return 0
        left = load_trace(args.left)
        right = load_trace(args.right)
        result, identical = render_diff(
            left, right, args.left, args.right, args.strict, args.show_equal
        )
        print(result)
        return 0 if identical else 1
    except (OSError, TraceError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
