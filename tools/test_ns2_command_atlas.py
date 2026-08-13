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


def ble_record(kind: str, handle: str, payload: str, *, length: int | None = None) -> dict:
    captured = len(payload) // 2
    return {
        "blecap": "record",
        "t_us": 1,
        "kind": kind,
        "handle": handle,
        "length": captured if length is None else length,
        "captured": captured,
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
            self.assertEqual(audio["inferred_name"], "audio_candidate")
            self.assertEqual(audio["request"]["lengths"], [15])
            self.assertEqual(audio["response"]["count"], 1)
            self.assertTrue(audio["request"]["complete"])
            self.assertEqual(audio["request"]["capture_boundaries"], ["console_side"])
            self.assertEqual(audio["request"]["header_transports"][0]["name"], "usb")

    def test_aggregates_controller_ble_with_handle_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ble.jsonl"
            items = [
                ble_record("cmd_out", "0x0014", "0C9101020004000027000000"),
                ble_record("ack", "0x001A", "0C0101021078000000000000"),
                # A non-command report-rate characteristic write remains outside the atlas.
                ble_record("cmd_out", "0x000C", "0B00"),
                {"blecap": "end", "records": 3, "dropped": 0},
            ]
            path.write_text(
                "\n".join(json.dumps(item) for item in items) + "\n",
                encoding="utf-8",
            )
            atlas = ns2_command_atlas.build([path])
            self.assertEqual(atlas["captures_by_schema"], {"blecap": 1})
            self.assertEqual(atlas["skipped_non_command_records"], {"blecap:cmd_out": 1})
            self.assertEqual(len(atlas["entries"]), 1)
            entry = atlas["entries"][0]
            self.assertEqual(entry["request"]["handles"], ["0x0014"])
            self.assertEqual(entry["response"]["handles"], ["0x001A"])
            self.assertEqual(entry["request"]["capture_boundaries"], ["controller_side"])
            self.assertEqual(entry["request"]["link_transports"], ["bluetooth_gatt"])
            self.assertEqual(entry["request"]["header_transports"][0]["name"], "bluetooth")

    def test_truncated_controller_response_is_retained_as_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ble.jsonl"
            items = [
                ble_record(
                    "ack",
                    "0x001A",
                    "02010104107800004000000000300100",
                    length=80,
                ),
                {"blecap": "end", "records": 1, "dropped": 0},
            ]
            path.write_text(
                "\n".join(json.dumps(item) for item in items) + "\n",
                encoding="utf-8",
            )
            atlas = ns2_command_atlas.build([path])
            self.assertFalse(atlas["entries"][0]["response"]["complete"])
            self.assertEqual(atlas["entries"][0]["response"]["lengths"], [80])

    def test_overwrite_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trace.jsonl"
            path.write_text(
                json.dumps({"trace": "end", "records": 0, "overwritten": 1}) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ns2_command_atlas.AtlasError, "lost"):
                ns2_command_atlas._load(path)

    def test_missing_loss_metadata_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ble.jsonl"
            path.write_text(
                json.dumps({"blecap": "end", "records": 0}) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ns2_command_atlas.AtlasError, "loss metadata"):
                ns2_command_atlas._load(path)


if __name__ == "__main__":
    unittest.main()
