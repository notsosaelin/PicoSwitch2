#!/usr/bin/env python3
"""Convert validated PicoSwitch2 UART JSONL into deterministic test fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


class FixtureError(ValueError):
    pass


def _load(path: Path) -> tuple[str, list[dict[str, Any]], dict[str, Any]]:
    records: list[dict[str, Any]] = []
    end: dict[str, Any] | None = None
    domain: str | None = None
    for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise FixtureError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
        if not isinstance(item, dict):
            raise FixtureError(f"{path}:{line_number}: expected a JSON object")
        current = next(
            (
                name
                for name in ("trace", "motionpair", "motionhybrid", "blecap")
                if name in item
            ),
            None,
        )
        if current is None:
            raise FixtureError(f"{path}:{line_number}: unknown capture record")
        if domain is None:
            domain = current
        elif current != domain:
            raise FixtureError(
                f"{path}:{line_number}: mixed {domain}/{current} capture"
            )
        if item[current] == "end":
            if end is not None:
                raise FixtureError(f"{path}:{line_number}: duplicate end record")
            end = item
        elif item[current] == "record":
            if end is not None:
                raise FixtureError(f"{path}:{line_number}: data after end record")
            records.append(item)
        else:
            raise FixtureError(
                f"{path}:{line_number}: unexpected {current} event {item[current]!r}"
            )
    if domain is None or end is None:
        raise FixtureError(f"{path}: capture is empty or lacks an end record")
    if int(end.get("records", -1)) != len(records):
        raise FixtureError(
            f"{path}: end count {end.get('records')} != {len(records)} records"
        )
    loss = int(end.get("dropped", end.get("overwritten", 0)))
    if loss:
        raise FixtureError(f"{path}: capture reports {loss} lost record(s)")
    return domain, records, end


def _payload(domain: str, record: dict[str, Any]) -> bytes:
    if domain == "motionhybrid":
        base_text = record.get("base")
        xor_text = record.get("output_xor")
        if (
            not isinstance(base_text, str)
            or not isinstance(xor_text, str)
            or not re.fullmatch(r"[0-9A-Fa-f]*", base_text)
            or not re.fullmatch(r"[0-9A-Fa-f]*", xor_text)
        ):
            raise FixtureError("hybrid record has invalid base/output_xor hex")
        base = bytes.fromhex(base_text)
        delta = bytes.fromhex(xor_text)
        declared = int(record.get("native_len", len(base)))
        if len(base) != declared or len(delta) != declared:
            raise FixtureError(
                "hybrid base/output_xor lengths do not match native_len"
            )
        return bytes(left ^ right for left, right in zip(base, delta))
    field = "native" if domain == "motionpair" else "payload"
    text = record.get(field)
    if not isinstance(text, str) or not re.fullmatch(r"[0-9A-Fa-f]*", text):
        raise FixtureError(f"record has invalid {field} hex")
    data = bytes.fromhex(text)
    declared = int(
        record.get("native_len" if domain == "motionpair" else "captured", len(data))
    )
    if declared != len(data):
        raise FixtureError(
            f"record {field} has {len(data)} bytes but declares {declared}"
        )
    return data


def _select(
    domain: str,
    records: list[dict[str, Any]],
    command: int | None,
    subcommand: int | None,
    record_kind: str | None,
) -> list[dict[str, Any]]:
    selected = records
    if command is not None:
        if domain != "trace":
            raise FixtureError("--command is valid only for trace captures")
        selected = [record for record in selected if int(record.get("id", -1)) == command]
    if subcommand is not None:
        if domain != "trace":
            raise FixtureError("--subcommand is valid only for trace captures")
        selected = [
            record for record in selected if int(record.get("sub", -1)) == subcommand
        ]
    if record_kind is not None:
        selected = [record for record in selected if record.get("kind") == record_kind]
    if not selected:
        raise FixtureError("selection contains no records")
    return selected


def _canonical_record(domain: str, record: dict[str, Any]) -> dict[str, Any]:
    data = _payload(domain, record)
    if domain == "trace":
        return {
            key: record[key]
            for key in (
                "seq",
                "t_us",
                "personality",
                "kind",
                "dir",
                "id",
                "sub",
                "length",
                "captured",
                "payload",
            )
        }
    if domain == "motionpair":
        keys = (
            "t_us",
            "native_len",
            "native",
            "ds5_valid",
            "ds5_seq",
            "ds5_t_us",
            "ds5_age_us",
            "ds5_sensor",
            "cal_state",
            "raw_g",
            "raw_a",
            "cal_g",
            "cal_a",
        )
        return {key: record[key] for key in keys}
    if domain == "motionhybrid":
        keys = (
            "t_us",
            "native_len",
            "mode",
            "reason",
            "requested_groups",
            "changed_bits",
            "ds5_age_us",
            "ds5_seq",
            "cal_state",
            "pose_aligned",
            "base",
            "output_xor",
        )
        return {key: record[key] for key in keys} | {
            "output": data.hex().upper()
        }
    return {
        key: record[key]
        for key in ("t_us", "kind", "handle", "length", "captured", "payload")
    } | {"data_bytes": len(data)}


def _c_identifier(value: str) -> str:
    identifier = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not identifier or identifier[0].isdigit():
        identifier = f"fixture_{identifier}"
    return identifier


def _write_c(path: Path, name: str, domain: str, records: list[dict[str, Any]]) -> None:
    symbol = _c_identifier(name)
    guard = f"{symbol.upper()}_H"
    lines = [
        "/* Generated by tools/capture_to_fixture.py. Do not hand-edit. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint64_t t_us;",
        "    uint16_t declared_length;",
        "    uint16_t data_length;",
        "    uint8_t id;",
        "    uint8_t sub;",
        "    const uint8_t *data;",
        "} picoswitch2_capture_fixture_record_t;",
        "",
    ]
    rows: list[str] = []
    for index, record in enumerate(records):
        data = _payload(domain, record)
        array = f"{symbol}_data_{index}"
        # Standard C does not define a zero-length array initializer. Keep one
        # unused byte while the fixture's data_length remains authoritative.
        encoded = ", ".join(f"0x{byte:02X}" for byte in data) or "0x00"
        lines.append(f"static const uint8_t {array}[] = {{{encoded}}};")
        if domain in ("motionpair", "motionhybrid"):
            declared = int(record["native_len"])
            command = sub = 0
        else:
            declared = int(record["length"])
            command = int(record.get("id", 0))
            sub = int(record.get("sub", 0))
        rows.append(
            "    {"
            f"{int(record['t_us'])}ULL, {declared}, {len(data)}, "
            f"{command}, {sub}, {array}"
            "},"
        )
    lines.extend(
        [
            "",
            f"static const picoswitch2_capture_fixture_record_t {symbol}[] = {{",
            *rows,
            "};",
            f"static const size_t {symbol}_count =",
            f"    sizeof({symbol}) / sizeof({symbol}[0]);",
            "",
            f"#endif /* {guard} */",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def convert(args: argparse.Namespace) -> dict[str, Any]:
    source = Path(args.input)
    domain, records, end = _load(source)
    selected = _select(domain, records, args.command, args.subcommand, args.kind)
    source_bytes = source.read_bytes()
    fixture = {
        "schema": "picoswitch2-capture-fixture/v1",
        "name": args.name,
        "domain": domain,
        "source": {
            "name": source.name,
            "sha256": hashlib.sha256(source_bytes).hexdigest(),
            "records": len(records),
            "selected_records": len(selected),
            "end": end,
        },
        "selection": {
            "command": args.command,
            "subcommand": args.subcommand,
            "kind": args.kind,
        },
        "records": [_canonical_record(domain, record) for record in selected],
    }
    output_json = Path(args.output_json)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(
        json.dumps(fixture, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    if args.output_c:
        output_c = Path(args.output_c)
        output_c.parent.mkdir(parents=True, exist_ok=True)
        _write_c(output_c, args.name, domain, selected)
    return fixture


def _integer(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {text}") from exc
    if not 0 <= value <= 255:
        raise argparse.ArgumentTypeError("integer must be from 0 through 255")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="validated UART JSONL capture")
    parser.add_argument("--name", required=True, help="fixture and C symbol name")
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-c")
    parser.add_argument("--command", type=_integer)
    parser.add_argument("--subcommand", type=_integer)
    parser.add_argument("--kind", help="record kind filter")
    args = parser.parse_args(argv)
    try:
        fixture = convert(args)
    except (OSError, FixtureError) as exc:
        print(f"capture_to_fixture: {exc}", file=sys.stderr)
        return 2
    print(
        f"wrote {len(fixture['records'])} {fixture['domain']} record(s) "
        f"to {args.output_json}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
