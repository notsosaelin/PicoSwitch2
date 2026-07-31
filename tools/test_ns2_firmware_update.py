#!/usr/bin/env python3

from __future__ import annotations

import binascii
import struct
import tempfile
import unittest
from pathlib import Path

import ns2_firmware_update as firmware


def sequence(image: bytes) -> list[tuple[int, bytes]]:
    region = 2
    address = 0x75000
    base = 0
    crc = binascii.crc32(image) & 0xFFFFFFFF
    frames = [
        (1, b""),
        (2, bytes([region]) + struct.pack("<I", address)),
        (3, bytes([region]) + struct.pack("<II", base, len(image))),
    ]
    for offset in range(0, len(image), 0x4C):
        chunk = image[offset : offset + 0x4C]
        frames.append((4, struct.pack("<I", len(chunk)) + chunk))
    frames.extend(
        [
            (5, b""),
            (6, bytes([region]) + struct.pack("<III", base, len(image), crc)),
            (7, b""),
        ]
    )
    return frames


class FirmwareUpdateTests(unittest.TestCase):
    def test_decodes_documented_usb_envelope(self) -> None:
        frame = bytes.fromhex("0d910102000500000200500700")
        subcommand, body = firmware._decode_frame(frame, "documented example")
        self.assertEqual(subcommand, 2)
        self.assertEqual(body, bytes.fromhex("0200500700"))

    def test_complete_sequence(self) -> None:
        image = bytes(range(200))
        capture = firmware.FirmwareUpdateCapture()
        for subcommand, body in sequence(image):
            capture.feed(subcommand, body)
        metadata = capture.metadata()
        self.assertTrue(metadata["complete"])
        self.assertEqual(capture.image, image)
        self.assertEqual(metadata["chunk_count"], 3)

    def test_rejects_unsafe_address(self) -> None:
        capture = firmware.FirmwareUpdateCapture()
        capture.feed(1, b"")
        with self.assertRaisesRegex(firmware.FirmwareCaptureError, "unsafe"):
            capture.feed(2, b"\x02" + struct.pack("<I", 0x123456))

    def test_rejects_bad_chunk_length(self) -> None:
        capture = firmware.FirmwareUpdateCapture()
        for subcommand, body in sequence(b"abc")[:3]:
            capture.feed(subcommand, body)
        with self.assertRaisesRegex(firmware.FirmwareCaptureError, "says 4"):
            capture.feed(4, struct.pack("<I", 4) + b"abc")

    def test_rejects_bad_crc(self) -> None:
        frames = sequence(b"firmware")
        subcommand, body = frames[-2]
        frames[-2] = (subcommand, body[:-4] + struct.pack("<I", 0))
        capture = firmware.FirmwareUpdateCapture()
        with self.assertRaisesRegex(firmware.FirmwareCaptureError, "CRC32"):
            for subcommand, body in frames:
                capture.feed(subcommand, body)

    def test_trace_truncation_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trace.jsonl"
            path.write_text(
                '{"trace":"record","kind":"bulk_command",'
                '"dir":"console_to_device","id":13,"sub":4,'
                '"length":84,"captured":24,"payload":"' + "00" * 24 + '"}\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                firmware.FirmwareCaptureError, "dedicated firmware sink"
            ):
                list(firmware.frames_from_trace(path))

    def test_rejects_wrong_protocol_header(self) -> None:
        frame = bytes.fromhex("0d910002000500000200500700")
        with self.assertRaisesRegex(
            firmware.FirmwareCaptureError, "not a console"
        ):
            firmware._decode_frame(frame, "bad protocol byte")


if __name__ == "__main__":
    unittest.main()
