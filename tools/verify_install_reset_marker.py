#!/usr/bin/env python3
"""Verify the one-shot install-reset marker is present and safely placed."""

from __future__ import annotations

import argparse
from pathlib import Path


MARKER = b"PS2-INSTALL-RESET-1"
FLASH_PAGE_SIZE = 256
PERSISTENT_SIZE = 6 * 4096


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "--flash-size",
        type=lambda value: int(value, 0),
        required=True,
        help="target flash size in bytes (for example 0x400000)",
    )
    args = parser.parse_args()

    data = args.binary.read_bytes()
    positions: list[int] = []
    offset = data.find(MARKER)
    while offset >= 0:
        positions.append(offset)
        offset = data.find(MARKER, offset + 1)

    if len(positions) != 1:
        raise SystemExit(
            f"{args.binary}: expected exactly one install marker, found {len(positions)}"
        )

    marker_offset = positions[0]
    persistent_start = args.flash_size - PERSISTENT_SIZE
    if marker_offset % FLASH_PAGE_SIZE:
        raise SystemExit(
            f"{args.binary}: marker offset 0x{marker_offset:X} is not page aligned"
        )
    if marker_offset + FLASH_PAGE_SIZE > persistent_start:
        raise SystemExit(
            f"{args.binary}: marker overlaps persistence at 0x{persistent_start:X}"
        )

    print(
        f"{args.binary}: install marker at 0x{marker_offset:X}; "
        f"persistent region starts at 0x{persistent_start:X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
