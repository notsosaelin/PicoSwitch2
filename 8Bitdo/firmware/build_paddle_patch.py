#!/usr/bin/env python3
"""Build and verify the Ultimate Bluetooth 1.11 independent-paddle patch."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from ultimate_codec import HEADER_SIZE, decode_image, encode_image, load_key_table


STOCK_SHA256 = "1030145fec364aceb55ceaed221396131dcf02eaaeeb8bd9ad4044ba5596074d"
STOCK_DECODED_PAYLOAD_SHA256 = (
    "fa653c10bba23ec346ea92b85493a37b0b6dda53534c7257214c4d41f80ada8d"
)

EXECUTION_BASE = 0x00018000
EXPECTED_HEADER = (111, 0x01018000, 0x00019A00, 0x00006007, 0, 0, 0)

# FUN_00020d40's full-report path ends with:
#   00020fd0  ldrb r0, [sp, #0x14]
#   00020fd2  strb r0, [r4, #2]
#   00020fd4  b     0x00021232
# Replace the latter two instructions with an ARMv6-M-supported BL. The helper
# exits through the function's saved return address, so its link value is
# intentionally unused:
#   00020fd2  bl     0x00031838
HOOK_ADDRESS = 0x00020FD2
HOOK_EXPECTED = bytes.fromhex("a0 70 2d e1")
HOOK_PATCH = bytes.fromhex("10 f0 31 fc")

# This region is zero padding in official 1.11, has no Ghidra references, and
# lies within the firmware payload. The helper restores the displaced STRB,
# maps raw physical P2/P1 bits 25/26 to reserved report-system bits 6/7, then
# executes the original stack epilogue.
CAVE_ADDRESS = 0x00031838
CAVE_PATCH = bytes.fromhex(
    "a0 70"        # strb r0, [r4, #2]
    " 01 20"       # movs r0, #1
    " 40 06"       # lsls r0, r0, #25       ; 0x02000000 (physical P2)
    " 05 42"       # tst r5, r0
    " 03 d0"       # beq p2
    " 61 79"       # ldrb r1, [r4, #5]
    " 40 22"       # movs r2, #0x40
    " 11 43"       # orrs r1, r2
    " 61 71"       # strb r1, [r4, #5]
    " 40 00"       # p1: lsls r0, r0, #1    ; 0x04000000 (physical P1)
    " 05 42"       # tst r5, r0
    " 03 d0"       # beq done
    " 61 79"       # ldrb r1, [r4, #5]
    " 80 22"       # movs r2, #0x80
    " 11 43"       # orrs r1, r2
    " 61 71"       # strb r1, [r4, #5]
    " 09 b0"       # done: add sp, #0x24
    " f0 bd"       # pop {r4, r5, r6, r7, pc}
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def image_offset(address: int) -> int:
    return HEADER_SIZE + address - EXECUTION_BASE


def require_equal(actual: object, expected: object, description: str) -> None:
    if actual != expected:
        raise ValueError(
            f"{description} mismatch: expected {expected!r}, got {actual!r}"
        )


def build_patch(stock: bytes, updater_dll: Path) -> tuple[bytes, dict[str, object]]:
    require_equal(sha256(stock), STOCK_SHA256, "official 1.11 SHA-256")
    require_equal(
        struct.unpack_from("<7I", stock),
        EXPECTED_HEADER,
        "official 1.11 header",
    )

    table = load_key_table(updater_dll)
    decoded = bytearray(decode_image(stock, table))
    require_equal(
        sha256(decoded[HEADER_SIZE:]),
        STOCK_DECODED_PAYLOAD_SHA256,
        "decoded official 1.11 payload SHA-256",
    )

    hook_offset = image_offset(HOOK_ADDRESS)
    cave_offset = image_offset(CAVE_ADDRESS)
    require_equal(
        bytes(decoded[hook_offset:hook_offset + len(HOOK_EXPECTED)]),
        HOOK_EXPECTED,
        "hook-site instructions",
    )
    require_equal(
        bytes(decoded[cave_offset:cave_offset + len(CAVE_PATCH)]),
        bytes(len(CAVE_PATCH)),
        "code-cave padding",
    )

    decoded[hook_offset:hook_offset + len(HOOK_PATCH)] = HOOK_PATCH
    decoded[cave_offset:cave_offset + len(CAVE_PATCH)] = CAVE_PATCH
    patched_decoded = bytes(decoded)
    patched = encode_image(patched_decoded, table)

    require_equal(
        decode_image(patched, table),
        patched_decoded,
        "patched encode/decode round-trip",
    )
    require_equal(
        encode_image(decode_image(patched, table), table),
        patched,
        "patched decode/encode round-trip",
    )
    require_equal(
        struct.unpack_from("<7I", patched),
        EXPECTED_HEADER,
        "patched header",
    )

    changed_decoded = [
        index
        for index, (before, after) in enumerate(
            zip(decode_image(stock, table), patched_decoded)
        )
        if before != after
    ]
    intended = {
        hook_offset + index
        for index, (before, after) in enumerate(zip(HOOK_EXPECTED, HOOK_PATCH))
        if before != after
    }
    intended.update(
        cave_offset + index
        for index, value in enumerate(CAVE_PATCH)
        if value != 0
    )
    require_equal(set(changed_decoded), intended, "decoded changed-byte set")

    manifest: dict[str, object] = {
        "base_firmware": "8BitDo Ultimate Bluetooth 1.11 (type 41)",
        "base_image_sha256": sha256(stock),
        "patched_image_sha256": sha256(patched),
        "decoded_payload_sha256": sha256(patched_decoded[HEADER_SIZE:]),
        "header": {
            "version": EXPECTED_HEADER[0],
            "flash_destination": f"0x{EXPECTED_HEADER[1]:08X}",
            "payload_length": EXPECTED_HEADER[2],
            "product_id": f"0x{EXPECTED_HEADER[3]:04X}",
        },
        "patches": [
            {
                "execution_address": f"0x{HOOK_ADDRESS:08X}",
                "before": HOOK_EXPECTED.hex(" "),
                "after": HOOK_PATCH.hex(" "),
                "purpose": f"branch to 0x{CAVE_ADDRESS:08X}",
            },
            {
                "execution_address": f"0x{CAVE_ADDRESS:08X}",
                "before": bytes(len(CAVE_PATCH)).hex(" "),
                "after": CAVE_PATCH.hex(" "),
                "purpose": (
                    "restore displaced report write; expose P1/P2 as full-report "
                    "system byte bits 6/7; restore original epilogue"
                ),
            },
        ],
        "decoded_changed_bytes": len(changed_decoded),
    }
    return patched, manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("stock", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--updater-dll", type=Path, required=True)
    args = parser.parse_args()

    patched, manifest = build_patch(args.stock.read_bytes(), args.updater_dll)
    args.output.write_bytes(patched)
    manifest_path = args.output.with_suffix(args.output.suffix + ".json")
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )

    print(f"wrote {args.output} ({len(patched)} bytes)")
    print(f"SHA-256 {manifest['patched_image_sha256']}")
    print(f"wrote {manifest_path}")


if __name__ == "__main__":
    main()
