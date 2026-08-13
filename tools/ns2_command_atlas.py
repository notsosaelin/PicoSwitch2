#!/usr/bin/env python3
"""Aggregate lossless Switch 2 trace/blecap files into an observed command atlas."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any


COMMAND_NAMES = {
    0x01: "nfc",
    0x02: "flash",
    0x03: "initialization",
    0x04: "unused_or_unknown",
    0x05: "unknown",
    0x06: "shutdown_reboot",
    0x07: "first_init",
    0x08: "charging_grip",
    0x09: "player_leds",
    0x0A: "vibration",
    0x0B: "battery",
    0x0C: "feature_select",
    0x0D: "firmware_update",
    0x0E: "unused_or_unknown",
    0x0F: "unknown",
    0x10: "firmware_info",
    0x11: "unknown",
    0x12: "unused_or_unknown",
    0x13: "joycon_unknown",
    0x14: "unknown",
    0x15: "bluetooth_pairing",
    0x16: "unknown",
    0x17: "audio_candidate",
    0x18: "audio_or_analog_candidate",
}

HEADER_TRANSPORT_NAMES = {0x00: "usb", 0x01: "bluetooth"}
HEX_RE = re.compile(r"[0-9A-Fa-f]*\Z")


class AtlasError(ValueError):
    pass


@dataclass(frozen=True)
class Capture:
    schema: str
    boundary: str
    link_transport: str
    records: list[dict[str, Any]]


def _integer(value: Any, label: str, minimum: int = 0) -> int:
    try:
        result = int(value)
    except (TypeError, ValueError) as exc:
        raise AtlasError(f"invalid {label}") from exc
    if result < minimum:
        raise AtlasError(f"invalid {label}")
    return result


def _load(path: Path) -> Capture:
    records: list[dict[str, Any]] = []
    end: dict[str, Any] | None = None
    schema: str | None = None
    for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise AtlasError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
        if not isinstance(item, dict):
            raise AtlasError(f"{path}:{line_number}: expected a JSON object")
        current = next((name for name in ("trace", "blecap") if name in item), None)
        if current is None:
            raise AtlasError(f"{path}:{line_number}: expected trace or blecap record")
        if schema is None:
            schema = current
        elif schema != current:
            raise AtlasError(f"{path}:{line_number}: mixed {schema}/{current} capture")
        event = item[current]
        if event == "end":
            if end is not None:
                raise AtlasError(f"{path}:{line_number}: duplicate end record")
            end = item
        elif event == "record":
            if end is not None:
                raise AtlasError(f"{path}:{line_number}: data after end record")
            records.append(item)
        else:
            raise AtlasError(f"{path}:{line_number}: unexpected {current} event {event!r}")

    if schema is None or end is None:
        raise AtlasError(f"{path}: capture is empty or missing its end record")
    if _integer(end.get("records"), "end record count") != len(records):
        raise AtlasError(f"{path}: record count mismatch")
    loss_field = "overwritten" if schema == "trace" else "dropped"
    if loss_field not in end:
        raise AtlasError(f"{path}: end record lacks {loss_field} loss metadata")
    loss = _integer(end[loss_field], f"end {loss_field}")
    if loss:
        raise AtlasError(f"{path}: capture reports {loss} {loss_field}/lost record(s)")
    return Capture(
        schema=schema,
        boundary="console_side" if schema == "trace" else "controller_side",
        link_transport="usb_vendor_bulk" if schema == "trace" else "bluetooth_gatt",
        records=records,
    )


def _payload(record: dict[str, Any], schema: str) -> tuple[bytes, int, int]:
    if schema == "trace":
        text = record.get("payload")
        length_value = record.get("length")
        captured_value = record.get("captured")
    else:
        text = record.get("payload", record.get("bytes"))
        length_value = record.get("length", record.get("orig_len"))
        captured_value = record.get("captured", record.get("len"))
    if not isinstance(text, str) or not HEX_RE.fullmatch(text) or len(text) % 2:
        raise AtlasError("record payload is not even-length hexadecimal")
    raw = bytes.fromhex(text)
    length = _integer(length_value, "record length")
    captured = _integer(captured_value, "record captured length")
    if captured != len(raw) or captured > length:
        raise AtlasError("record payload length disagrees with capture metadata")
    return raw, length, captured


def _command_record(
    record: dict[str, Any], capture: Capture
) -> tuple[str, int, int, bytes, int, int, str | None] | None:
    kind = record.get("kind")
    if capture.schema == "trace":
        if kind not in ("bulk_command", "bulk_response"):
            return None
        direction = "request" if kind == "bulk_command" else "response"
    else:
        if kind not in ("cmd_out", "ack"):
            return None
        direction = "request" if kind == "cmd_out" else "response"

    raw, length, captured = _payload(record, capture.schema)
    expected_direction = (0x91,) if direction == "request" else (0x01, 0x04)
    # Controller-side cmd_out also records non-command characteristic writes
    # (report-rate writes and audio frames). They are not vendor command evidence.
    if len(raw) < 8 or raw[1] not in expected_direction:
        if capture.schema == "blecap":
            return None
        raise AtlasError("trace bulk record has an invalid or incomplete command header")
    if capture.schema == "trace":
        if _integer(record.get("id"), "trace command id") != raw[0]:
            raise AtlasError("trace command summary disagrees with payload")
        if _integer(record.get("sub"), "trace subcommand id") != raw[3]:
            raise AtlasError("trace subcommand summary disagrees with payload")
    handle = None
    if capture.schema == "blecap":
        handle_value = record.get("handle")
        if not isinstance(handle_value, str) or not re.fullmatch(r"0x[0-9A-Fa-f]{4}", handle_value):
            raise AtlasError("blecap command record has an invalid handle")
        handle = handle_value.upper().replace("X", "x")
    return direction, raw[0], raw[3], raw, length, captured, handle


def _new_side() -> dict[str, Any]:
    return {
        "count": 0,
        "lengths": set(),
        "captured_lengths": set(),
        "complete": True,
        "payload_sha256": set(),
        "handles": set(),
        "header_transports": set(),
        "capture_boundaries": set(),
        "link_transports": set(),
        "capture_schemas": set(),
    }


def build(paths: list[Path]) -> dict[str, Any]:
    entries: dict[tuple[int, int], dict[str, Any]] = {}
    capture_counts: Counter[str] = Counter()
    skipped: Counter[str] = Counter()
    for path in paths:
        capture = _load(path)
        capture_counts[capture.schema] += 1
        source_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        source_name = path.as_posix()
        for record in capture.records:
            decoded = _command_record(record, capture)
            if decoded is None:
                if record.get("kind") in ("bulk_command", "bulk_response", "cmd_out", "ack"):
                    skipped[f"{capture.schema}:{record.get('kind')}"] += 1
                continue
            direction, command, subcommand, raw, length, captured, handle = decoded
            key = (command, subcommand)
            entry = entries.setdefault(
                key,
                {
                    "command": command,
                    "command_hex": f"0x{command:02X}",
                    "inferred_name": COMMAND_NAMES.get(command, "unassigned"),
                    "subcommand": subcommand,
                    "subcommand_hex": f"0x{subcommand:02X}",
                    "personalities": set(),
                    "sources": {},
                    "request": _new_side(),
                    "response": _new_side(),
                },
            )
            personality = record.get("personality")
            if isinstance(personality, str):
                entry["personalities"].add(personality)
            entry["sources"][(source_name, capture.schema)] = {
                "path": source_name,
                "sha256": source_hash,
                "schema": capture.schema,
                "capture_boundary": capture.boundary,
                "link_transport": capture.link_transport,
            }
            side = entry[direction]
            side["count"] += 1
            side["lengths"].add(length)
            side["captured_lengths"].add(captured)
            side["complete"] &= captured == length
            side["payload_sha256"].add(hashlib.sha256(raw).hexdigest())
            if handle is not None:
                side["handles"].add(handle)
            side["header_transports"].add(raw[2])
            side["capture_boundaries"].add(capture.boundary)
            side["link_transports"].add(capture.link_transport)
            side["capture_schemas"].add(capture.schema)

    canonical_entries = []
    for key in sorted(entries):
        entry = entries[key]
        entry["personalities"] = sorted(entry["personalities"])
        entry["sources"] = [entry["sources"][source] for source in sorted(entry["sources"])]
        for direction in ("request", "response"):
            side = entry[direction]
            for field in (
                "lengths",
                "captured_lengths",
                "payload_sha256",
                "handles",
                "capture_boundaries",
                "link_transports",
                "capture_schemas",
            ):
                side[field] = sorted(side[field])
            side["header_transports"] = [
                {"value": value, "hex": f"0x{value:02X}", "name": HEADER_TRANSPORT_NAMES.get(value, "unknown")}
                for value in sorted(side["header_transports"])
            ]
        canonical_entries.append(entry)
    return {
        "schema": "picoswitch2-command-atlas/v2",
        "captures": len(paths),
        "captures_by_schema": dict(sorted(capture_counts.items())),
        "skipped_non_command_records": dict(sorted(skipped.items())),
        "entries": canonical_entries,
        "evidence_boundary": (
            "Observed wire shapes only. inferred_name comes from the current command audit. "
            "capture_boundary and link_transport identify where bytes were observed; neither "
            "field proves that a response was generated by genuine Nintendo hardware."
        ),
    }


def _shape(side: dict[str, Any]) -> str:
    return f"{side['count']} / {side['lengths']}"


def markdown(atlas: dict[str, Any]) -> str:
    capture_summary = ", ".join(
        f"{name}={count}" for name, count in atlas["captures_by_schema"].items()
    )
    lines = [
        "# Observed Switch 2 command atlas",
        "",
        f"Captures: {atlas['captures']} ({capture_summary})",
        "",
        "| Command | Sub | Inferred name | Req count/length | Resp count/length | Boundaries | BLE handles | Complete |",
        "|---:|---:|---|---|---|---|---|---|",
    ]
    for entry in atlas["entries"]:
        request = entry["request"]
        response = entry["response"]
        complete = request["complete"] and response["complete"]
        boundaries = sorted(set(request["capture_boundaries"] + response["capture_boundaries"]))
        handles = sorted(set(request["handles"] + response["handles"]))
        lines.append(
            f"| `{entry['command_hex']}` | `{entry['subcommand_hex']}` | "
            f"{entry['inferred_name']} | {_shape(request)} | {_shape(response)} | "
            f"{', '.join(boundaries) or '-'} | {', '.join(handles) or '-'} | "
            f"{'yes' if complete else 'no'} |"
        )
    lines.extend(["", f"> {atlas['evidence_boundary']}", ""])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args(argv)
    try:
        atlas = build([Path(path) for path in args.captures])
        output = json.dumps(atlas, indent=2, sort_keys=True) if args.json else markdown(atlas)
        if args.output:
            Path(args.output).write_text(output + "\n", encoding="utf-8", newline="\n")
        else:
            print(output)
    except (OSError, AtlasError, ValueError) as exc:
        print(f"ns2_command_atlas: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
