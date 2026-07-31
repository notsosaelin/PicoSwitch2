#!/usr/bin/env python3
"""Audit a candidate flash region for a capture-only firmware-update sink.

This proves only that the current binary leaves enough address space. It does
not reserve that space in the linker and therefore cannot authorize an
on-device writer by itself.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


SECTOR_SIZE = 4096
PERSISTENT_SECTORS = 6


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def audit(
    image_size: int,
    flash_size: int,
    capture_size: int,
    guard_sectors: int,
) -> dict[str, int | str | bool]:
    if image_size <= 0 or flash_size <= 0 or capture_size <= 0:
        raise ValueError("image, flash, and capture sizes must be positive")
    if capture_size % SECTOR_SIZE:
        raise ValueError("capture size must be flash-sector aligned")
    if guard_sectors < 1:
        raise ValueError("at least one guard sector is required")

    persistent_start = flash_size - PERSISTENT_SECTORS * SECTOR_SIZE
    capture_end = persistent_start
    capture_start = capture_end - capture_size
    program_end_aligned = align_up(image_size, SECTOR_SIZE)
    required_program_end = program_end_aligned + guard_sectors * SECTOR_SIZE
    fits = capture_start >= required_program_end

    return {
        "schema": "picoswitch2-firmware-capture-space/v1",
        "candidate_only": True,
        "fits_current_binary": fits,
        "flash_size": flash_size,
        "image_size": image_size,
        "program_end_aligned": program_end_aligned,
        "guard_size": guard_sectors * SECTOR_SIZE,
        "capture_start": capture_start,
        "capture_end": capture_end,
        "capture_size": capture_size,
        "persistent_start": persistent_start,
        "free_before_capture": max(0, capture_start - program_end_aligned),
        "safety": (
            "Current-image space audit only. A research sink still requires a "
            "linker-reserved region and compile-time overlap assertions."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--flash-size", type=lambda value: int(value, 0), required=True)
    parser.add_argument(
        "--capture-size",
        type=lambda value: int(value, 0),
        default=0x100000,
        help="candidate capture allocation; default 1 MiB",
    )
    parser.add_argument("--guard-sectors", type=int, default=1)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    result = audit(
        args.binary.stat().st_size,
        args.flash_size,
        args.capture_size,
        args.guard_sectors,
    )
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(
            f"{args.binary}: current image ends at 0x{result['image_size']:X}; "
            f"candidate 0x{result['capture_start']:X}-"
            f"0x{result['capture_end']:X} "
            f"({'fits' if result['fits_current_binary'] else 'DOES NOT FIT'})"
        )
        print(result["safety"])
    return 0 if result["fits_current_binary"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
