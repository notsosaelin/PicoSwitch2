#!/usr/bin/env python3
"""Aggregate Switch 2 UART traces into an observed command/subcommand atlas."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import defaultdict
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


class AtlasError(ValueError):
    pass


def _load(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    end: dict[str, Any] | None = None
    for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise AtlasError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
        if item.get("trace") == "end":
            end = item
        elif item.get("trace") == "record":
            if end is not None:
                raise AtlasError(f"{path}:{line_number}: data after end record")
            records.append(item)
        else:
            raise AtlasError(f"{path}:{line_number}: expected trace record or end")
    if end is None:
        raise AtlasError(f"{path}: missing trace end record")
    if int(end.get("records", -1)) != len(records):
        raise AtlasError(f"{path}: record count mismatch")
    if int(end.get("overwritten", 0)) != 0:
        raise AtlasError(f"{path}: trace overwrote records")
    return records


def build(paths: list[Path]) -> dict[str, Any]:
    entries: dict[tuple[int, int], dict[str, Any]] = {}
    for path in paths:
        source_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        for record in _load(path):
            if record.get("kind") not in ("bulk_command", "bulk_response"):
                continue
            command = int(record["id"])
            subcommand = int(record["sub"])
            key = (command, subcommand)
            entry = entries.setdefault(
                key,
                {
                    "command": command,
                    "command_hex": f"0x{command:02X}",
                    "name": COMMAND_NAMES.get(command, "unassigned"),
                    "subcommand": subcommand,
                    "subcommand_hex": f"0x{subcommand:02X}",
                    "personalities": set(),
                    "sources": {},
                    "request": {
                        "count": 0,
                        "lengths": set(),
                        "captured_lengths": set(),
                        "complete": True,
                        "payload_sha256": set(),
                    },
                    "response": {
                        "count": 0,
                        "lengths": set(),
                        "captured_lengths": set(),
                        "complete": True,
                        "payload_sha256": set(),
                    },
                },
            )
            entry["personalities"].add(record["personality"])
            entry["sources"][path.name] = source_hash
            direction = "request" if record["kind"] == "bulk_command" else "response"
            side = entry[direction]
            side["count"] += 1
            side["lengths"].add(int(record["length"]))
            side["captured_lengths"].add(int(record["captured"]))
            side["complete"] &= int(record["captured"]) == int(record["length"])
            side["payload_sha256"].add(
                hashlib.sha256(bytes.fromhex(record["payload"])).hexdigest()
            )

    canonical_entries = []
    for key in sorted(entries):
        entry = entries[key]
        entry["personalities"] = sorted(entry["personalities"])
        entry["sources"] = [
            {"name": name, "sha256": digest}
            for name, digest in sorted(entry["sources"].items())
        ]
        for direction in ("request", "response"):
            side = entry[direction]
            side["lengths"] = sorted(side["lengths"])
            side["captured_lengths"] = sorted(side["captured_lengths"])
            side["payload_sha256"] = sorted(side["payload_sha256"])
        canonical_entries.append(entry)
    return {
        "schema": "picoswitch2-command-atlas/v1",
        "captures": len(paths),
        "entries": canonical_entries,
        "evidence_boundary": (
            "Observed shapes only. Names come from the current command audit; "
            "unknown payload semantics remain unknown."
        ),
    }


def markdown(atlas: dict[str, Any]) -> str:
    lines = [
        "# Observed Switch 2 command atlas",
        "",
        f"Captures: {atlas['captures']}",
        "",
        "| Command | Sub | Name | Req count/length | Resp count/length | Personalities | Complete |",
        "|---:|---:|---|---|---|---|---|",
    ]
    for entry in atlas["entries"]:
        request = entry["request"]
        response = entry["response"]
        complete = request["complete"] and response["complete"]
        lines.append(
            f"| `{entry['command_hex']}` | `{entry['subcommand_hex']}` | "
            f"{entry['name']} | {request['count']} / {request['lengths']} | "
            f"{response['count']} / {response['lengths']} | "
            f"{', '.join(entry['personalities'])} | {'yes' if complete else 'no'} |"
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
