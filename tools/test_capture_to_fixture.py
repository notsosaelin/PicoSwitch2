#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import capture_to_fixture


class CaptureToFixtureTests(unittest.TestCase):
    def _write(self, directory: Path, name: str, records: list[dict]) -> Path:
        path = directory / name
        path.write_text(
            "\n".join(json.dumps(record) for record in records) + "\n",
            encoding="utf-8",
        )
        return path

    def test_motionpair_fixture_and_c_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._write(
                root,
                "motion.jsonl",
                [
                    {
                        "motionpair": "record",
                        "t_us": 7,
                        "native_len": 30,
                        "native": "00" * 30,
                        "ds5_valid": False,
                        "ds5_seq": 0,
                        "ds5_t_us": 0,
                        "ds5_age_us": 0xFFFFFFFF,
                        "ds5_sensor": 0,
                        "cal_state": 0,
                        "raw_g": [0, 0, 0],
                        "raw_a": [0, 0, 0],
                        "cal_g": [0, 0, 0],
                        "cal_a": [0, 0, 0],
                    },
                    {"motionpair": "end", "records": 1, "dropped": 0},
                ],
            )
            output_json = root / "fixture.json"
            output_c = root / "fixture.h"
            args = type(
                "Args",
                (),
                {
                    "input": str(source),
                    "name": "motion golden",
                    "output_json": str(output_json),
                    "output_c": str(output_c),
                    "command": None,
                    "subcommand": None,
                    "kind": None,
                },
            )()
            fixture = capture_to_fixture.convert(args)
            self.assertEqual(fixture["domain"], "motionpair")
            self.assertEqual(len(fixture["records"]), 1)
            self.assertIn("motion_golden_data_0", output_c.read_text())

    def test_trace_command_filter(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = {
                "trace": "record",
                "t_us": 1,
                "personality": "pro2",
                "kind": "bulk_command",
                "dir": "console_to_device",
                "sub": 4,
                "length": 8,
                "captured": 8,
                "payload": "0D91000400000000",
            }
            source = self._write(
                root,
                "trace.jsonl",
                [
                    base | {"seq": 0, "id": 0x0D},
                    base | {"seq": 1, "id": 0x03},
                    {"trace": "end", "records": 2, "overwritten": 0},
                ],
            )
            args = type(
                "Args",
                (),
                {
                    "input": str(source),
                    "name": "fw",
                    "output_json": str(root / "fw.json"),
                    "output_c": None,
                    "command": 0x0D,
                    "subcommand": None,
                    "kind": None,
                },
            )()
            fixture = capture_to_fixture.convert(args)
            self.assertEqual([record["id"] for record in fixture["records"]], [0x0D])

    def test_loss_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self._write(
                root,
                "lost.jsonl",
                [{"motionpair": "end", "records": 0, "dropped": 1}],
            )
            with self.assertRaisesRegex(capture_to_fixture.FixtureError, "lost"):
                capture_to_fixture._load(source)


if __name__ == "__main__":
    unittest.main()
