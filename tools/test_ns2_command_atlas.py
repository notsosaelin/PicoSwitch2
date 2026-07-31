#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import ns2_command_atlas


def record(seq: int, kind: str, command: int, subcommand: int, payload: str) -> dict:
    return {
        "trace": "record",
        "seq": seq,
        "t_us": seq,
        "personality": "pro2",
        "kind": kind,
        "dir": "console_to_device" if kind == "bulk_command" else "device_to_console",
        "id": command,
        "sub": subcommand,
        "length": len(payload) // 2,
        "captured": len(payload) // 2,
        "payload": payload,
    }


class CommandAtlasTests(unittest.TestCase):
    def test_aggregates_command_shapes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trace.jsonl"
            items = [
                record(0, "bulk_command", 0x17, 2, "179100020007000080BB000002F000"),
                record(1, "bulk_response", 0x17, 2, "1701000200F80000"),
                record(2, "bulk_command", 0x11, 3, "1191000300000000"),
                {"trace": "end", "records": 3, "overwritten": 0},
            ]
            path.write_text(
                "\n".join(json.dumps(item) for item in items) + "\n",
                encoding="utf-8",
            )
            atlas = ns2_command_atlas.build([path])
            self.assertEqual(len(atlas["entries"]), 2)
            audio = atlas["entries"][1]
            self.assertEqual(audio["command"], 0x17)
            self.assertEqual(audio["request"]["lengths"], [15])
            self.assertEqual(audio["response"]["count"], 1)
            self.assertTrue(audio["request"]["complete"])

    def test_overwrite_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trace.jsonl"
            path.write_text(
                json.dumps({"trace": "end", "records": 0, "overwritten": 1}) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ns2_command_atlas.AtlasError, "overwrote"):
                ns2_command_atlas._load(path)


if __name__ == "__main__":
    unittest.main()
