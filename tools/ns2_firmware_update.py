#!/usr/bin/env python3
"""Validate and reassemble a captured Switch 2 controller 0x0D update."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Iterable


class FirmwareCaptureError(ValueError):
    pass


class State(str, Enum):
    IDLE = "idle"
    INITIALIZED = "initialized"
    ADDRESSED = "addressed"
    SIZED = "sized"
    TRANSFERRING = "transferring"
    ENDED = "ended"
    VERIFIED = "verified"
    DONE = "done"


@dataclass
class FirmwareUpdateCapture:
    state: State = State.IDLE
    region_id: int | None = None
    failsafe_address: int | None = None
    base_offset: int | None = None
    declared_size: int | None = None
    declared_crc32: int | None = None
    chunks: list[bytes] = field(default_factory=list)
    observed_size: int = 0
    transitions: list[dict[str, Any]] = field(default_factory=list)

    @property
    def image(self) -> bytes:
        return b"".join(self.chunks)

    def _transition(self, subcommand: int, state: State, **fields: Any) -> None:
        self.state = state
        self.transitions.append(
            {"subcommand": subcommand, "state": state.value, **fields}
        )

    def feed(self, subcommand: int, body: bytes) -> None:
        if subcommand == 0x01:
            if self.state is not State.IDLE or body:
                raise FirmwareCaptureError("0x0D/01 must initialize an empty capture")
            self._transition(subcommand, State.INITIALIZED)
            return

        if subcommand == 0x02:
            if self.state is not State.INITIALIZED or len(body) != 5:
                raise FirmwareCaptureError("0x0D/02 requires 5 bytes after initialize")
            self.region_id = body[0]
            self.failsafe_address = struct.unpack_from("<I", body, 1)[0]
            if self.failsafe_address not in (0x15000, 0x75000):
                raise FirmwareCaptureError(
                    f"unsafe/unrecognized failsafe address 0x{self.failsafe_address:08X}"
                )
            self._transition(
                subcommand,
                State.ADDRESSED,
                region_id=self.region_id,
                failsafe_address=self.failsafe_address,
            )
            return

        if subcommand == 0x03:
            if self.state is not State.ADDRESSED or len(body) != 9:
                raise FirmwareCaptureError("0x0D/03 requires 9 bytes after address")
            region_id = body[0]
            if region_id != self.region_id:
                raise FirmwareCaptureError("0x0D/03 region does not match 0x0D/02")
            self.base_offset, self.declared_size = struct.unpack_from("<II", body, 1)
            if not 0 < self.declared_size <= 0x100000:
                raise FirmwareCaptureError(
                    f"declared image size is unreasonable: {self.declared_size}"
                )
            self._transition(
                subcommand,
                State.SIZED,
                region_id=region_id,
                base_offset=self.base_offset,
                declared_size=self.declared_size,
            )
            return

        if subcommand == 0x04:
            if self.state not in (State.SIZED, State.TRANSFERRING):
                raise FirmwareCaptureError("0x0D/04 arrived before image sizing")
            if len(body) < 4:
                raise FirmwareCaptureError("0x0D/04 lacks its uint32 chunk length")
            chunk_length = struct.unpack_from("<I", body, 0)[0]
            chunk = body[4:]
            if chunk_length != len(chunk):
                raise FirmwareCaptureError(
                    f"0x0D/04 says {chunk_length} bytes but carries {len(chunk)}"
                )
            if not 0 < chunk_length <= 0x4C:
                raise FirmwareCaptureError(
                    f"USB 0x0D/04 chunk length is outside 1..0x4C: {chunk_length}"
                )
            if (
                self.declared_size is None
                or self.observed_size + chunk_length > self.declared_size
            ):
                raise FirmwareCaptureError("0x0D/04 exceeds the declared image size")
            offset = self.observed_size
            self.chunks.append(chunk)
            self.observed_size += chunk_length
            self._transition(
                subcommand,
                State.TRANSFERRING,
                offset=offset,
                length=chunk_length,
                observed_size=self.observed_size,
            )
            return

        if subcommand == 0x05:
            if self.state is not State.TRANSFERRING or body:
                raise FirmwareCaptureError("0x0D/05 must end a transfer with no body")
            if self.observed_size != self.declared_size:
                raise FirmwareCaptureError(
                    f"transfer ended at {self.observed_size} of "
                    f"{self.declared_size} bytes"
                )
            self._transition(
                subcommand, State.ENDED, observed_size=self.observed_size
            )
            return

        if subcommand == 0x06:
            if self.state is not State.ENDED or len(body) != 13:
                raise FirmwareCaptureError("0x0D/06 requires 13 bytes after transfer end")
            region_id = body[0]
            base_offset, size, expected_crc = struct.unpack_from("<III", body, 1)
            if region_id != self.region_id:
                raise FirmwareCaptureError("0x0D/06 region does not match")
            if base_offset != self.base_offset:
                raise FirmwareCaptureError("0x0D/06 base offset does not match")
            if size != self.declared_size:
                raise FirmwareCaptureError("0x0D/06 image size does not match")
            computed_crc = binascii.crc32(self.image) & 0xFFFFFFFF
            if expected_crc != computed_crc:
                raise FirmwareCaptureError(
                    f"CRC32 mismatch: declared {expected_crc:08X}, "
                    f"computed {computed_crc:08X}"
                )
            self.declared_crc32 = expected_crc
            self._transition(
                subcommand,
                State.VERIFIED,
                declared_crc32=expected_crc,
                computed_crc32=computed_crc,
            )
            return

        if subcommand == 0x07:
            if self.state is not State.VERIFIED or body:
                raise FirmwareCaptureError("0x0D/07 must follow verification with no body")
            self._transition(subcommand, State.DONE)
            return

        raise FirmwareCaptureError(f"unknown firmware update subcommand 0x{subcommand:02X}")

    def metadata(self) -> dict[str, Any]:
        image = self.image
        computed_crc = binascii.crc32(image) & 0xFFFFFFFF
        return {
            "schema": "picoswitch2-firmware-capture/v1",
            "state": self.state.value,
            "complete": self.state is State.DONE,
            "region_id": self.region_id,
            "failsafe_address": self.failsafe_address,
            "base_offset": self.base_offset,
            "declared_size": self.declared_size,
            "observed_size": self.observed_size,
            "declared_crc32": self.declared_crc32,
            "computed_crc32": computed_crc,
            "sha256": hashlib.sha256(image).hexdigest(),
            "chunk_count": len(self.chunks),
            "transitions": self.transitions,
            "safety": (
                "Capture metadata only. Nintendo target addresses were validated "
                "but never used as host/Pico write addresses."
            ),
        }


def _decode_frame(frame: bytes, source: str) -> tuple[int, bytes]:
    if len(frame) < 8:
        raise FirmwareCaptureError(f"{source}: command frame is shorter than 8 bytes")
    command, direction, protocol, subcommand, unknown, length, reserved = struct.unpack(
        "<BBBBBBH", frame[:8]
    )
    if (
        command != 0x0D
        or direction != 0x91
        or protocol != 0x01
        or unknown != 0
        or reserved != 0
    ):
        raise FirmwareCaptureError(f"{source}: not a console 0x0D request frame")
    body = frame[8:]
    if length != len(body):
        raise FirmwareCaptureError(
            f"{source}: header declares {length} body bytes, captured {len(body)}"
        )
    return subcommand, body


def frames_from_json(path: Path) -> Iterable[tuple[int, bytes]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FirmwareCaptureError(f"{path}: {exc}") from exc
    frames = document.get("frames") if isinstance(document, dict) else None
    if not isinstance(frames, list):
        raise FirmwareCaptureError(f"{path}: expected a frames array")
    for index, item in enumerate(frames):
        if not isinstance(item, dict):
            raise FirmwareCaptureError(f"{path}: frame {index} is not an object")
        try:
            subcommand = int(item["subcommand"])
            body = bytes.fromhex(item.get("body", ""))
        except (KeyError, TypeError, ValueError) as exc:
            raise FirmwareCaptureError(f"{path}: invalid frame {index}") from exc
        yield subcommand, body


def frames_from_trace(path: Path) -> Iterable[tuple[int, bytes]]:
    found = 0
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8-sig").splitlines(), 1
    ):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise FirmwareCaptureError(
                f"{path}:{line_number}: invalid JSON: {exc.msg}"
            ) from exc
        if item.get("trace") != "record":
            continue
        if (
            item.get("kind") != "bulk_command"
            or item.get("dir") != "console_to_device"
            or int(item.get("id", -1)) != 0x0D
        ):
            continue
        found += 1
        declared = int(item.get("length", -1))
        captured = int(item.get("captured", -1))
        if captured != declared:
            raise FirmwareCaptureError(
                f"{path}:{line_number}: 0x0D frame is truncated "
                f"({captured}/{declared} bytes); use the dedicated firmware sink"
            )
        try:
            frame = bytes.fromhex(item["payload"])
        except (KeyError, ValueError) as exc:
            raise FirmwareCaptureError(
                f"{path}:{line_number}: invalid payload hex"
            ) from exc
        subcommand, body = _decode_frame(frame, f"{path}:{line_number}")
        if int(item.get("sub", -1)) != subcommand:
            raise FirmwareCaptureError(
                f"{path}:{line_number}: trace subcommand disagrees with payload"
            )
        yield subcommand, body
    if not found:
        raise FirmwareCaptureError(f"{path}: no console 0x0D request frames found")


def analyze(path: Path, input_format: str) -> FirmwareUpdateCapture:
    capture = FirmwareUpdateCapture()
    frames = frames_from_trace(path) if input_format == "trace" else frames_from_json(path)
    for subcommand, body in frames:
        capture.feed(subcommand, body)
    return capture


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input")
    parser.add_argument(
        "--format", choices=("trace", "frames"), default="trace"
    )
    parser.add_argument("--image", help="write the verified captured image")
    parser.add_argument("--metadata", help="write metadata JSON")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        capture = analyze(Path(args.input), args.format)
        metadata = capture.metadata()
        if not metadata["complete"]:
            raise FirmwareCaptureError(
                f"capture stopped in state {metadata['state']}, not done"
            )
        if args.image:
            Path(args.image).write_bytes(capture.image)
        if args.metadata:
            Path(args.metadata).write_text(
                json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
                newline="\n",
            )
    except (OSError, FirmwareCaptureError) as exc:
        print(f"ns2_firmware_update: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(metadata, indent=2, sort_keys=True))
    else:
        print(
            f"complete update: {metadata['observed_size']} bytes, "
            f"{metadata['chunk_count']} chunks, CRC32 "
            f"{metadata['computed_crc32']:08X}, SHA256 {metadata['sha256']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
