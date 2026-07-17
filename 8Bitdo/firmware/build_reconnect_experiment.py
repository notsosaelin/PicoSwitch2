#!/usr/bin/env python3
"""Build a guarded Ultimate Bluetooth 1.11 reconnect-timing experiment."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from build_paddle_patch import (
    EXECUTION_BASE,
    EXPECTED_HEADER,
    STOCK_SHA256,
    build_patch as build_paddle_patch,
    require_equal,
)
from ultimate_codec import HEADER_SIZE, decode_image, encode_image, load_key_table


# FUN_00025d04 sends HCI_CMD_BT_RECONNECT to the controller's Bluetooth
# coprocessor. The stock timeout is 4,800 Bluetooth baseband slots:
#
#     4,800 * 0.625 ms = 3,000 ms
#
# A controller-side callback is scheduled 6,000 ms later. This experiment
# reduces both values by one third while preserving their exact 2:1 real-time
# relationship:
#
#     3,200 * 0.625 ms = 2,000 ms; callback = 4,000 ms
#
# This only changes failed bonded reconnect attempts. Pairing/discovery uses
# separate firmware paths.
RECONNECT_TIMEOUT_ADDRESS = 0x00025D6E
RECONNECT_TIMEOUT_EXPECTED = bytes.fromhex("4b 21 89 01")
RECONNECT_TIMEOUT_PATCH = bytes.fromhex("32 21 89 01")

RECONNECT_WATCHDOG_ADDRESS = 0x00025E3C
RECONNECT_WATCHDOG_EXPECTED = bytes.fromhex("70 17 00 00")
RECONNECT_WATCHDOG_PATCH = bytes.fromhex("a0 0f 00 00")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def image_offset(address: int) -> int:
    return HEADER_SIZE + address - EXECUTION_BASE


def build_experiment(
    stock: bytes,
    updater_dll: Path,
) -> tuple[bytes, dict[str, object]]:
    require_equal(sha256(stock), STOCK_SHA256, "official 1.11 SHA-256")

    paddle_image, paddle_manifest = build_paddle_patch(stock, updater_dll)
    table = load_key_table(updater_dll)
    decoded = bytearray(decode_image(paddle_image, table))

    timeout_offset = image_offset(RECONNECT_TIMEOUT_ADDRESS)
    watchdog_offset = image_offset(RECONNECT_WATCHDOG_ADDRESS)
    require_equal(
        bytes(
            decoded[
                timeout_offset:timeout_offset + len(RECONNECT_TIMEOUT_EXPECTED)
            ]
        ),
        RECONNECT_TIMEOUT_EXPECTED,
        "reconnect timeout instructions",
    )
    require_equal(
        bytes(
            decoded[
                watchdog_offset:watchdog_offset
                + len(RECONNECT_WATCHDOG_EXPECTED)
            ]
        ),
        RECONNECT_WATCHDOG_EXPECTED,
        "reconnect watchdog literal",
    )

    decoded[
        timeout_offset:timeout_offset + len(RECONNECT_TIMEOUT_PATCH)
    ] = RECONNECT_TIMEOUT_PATCH
    decoded[
        watchdog_offset:watchdog_offset + len(RECONNECT_WATCHDOG_PATCH)
    ] = RECONNECT_WATCHDOG_PATCH

    patched_decoded = bytes(decoded)
    patched = encode_image(patched_decoded, table)
    require_equal(
        decode_image(patched, table),
        patched_decoded,
        "experimental encode/decode round-trip",
    )
    require_equal(
        encode_image(decode_image(patched, table), table),
        patched,
        "experimental decode/encode round-trip",
    )
    require_equal(
        tuple(int(value) for value in EXPECTED_HEADER),
        tuple(
            int.from_bytes(patched[offset:offset + 4], "little")
            for offset in range(0, HEADER_SIZE, 4)
        ),
        "experimental header",
    )

    paddle_decoded = decode_image(paddle_image, table)
    changed_from_paddles = {
        index
        for index, (before, after) in enumerate(
            zip(paddle_decoded, patched_decoded)
        )
        if before != after
    }
    intended = {
        timeout_offset + index
        for index, (before, after) in enumerate(
            zip(RECONNECT_TIMEOUT_EXPECTED, RECONNECT_TIMEOUT_PATCH)
        )
        if before != after
    }
    intended.update(
        watchdog_offset + index
        for index, (before, after) in enumerate(
            zip(RECONNECT_WATCHDOG_EXPECTED, RECONNECT_WATCHDOG_PATCH)
        )
        if before != after
    )
    require_equal(
        changed_from_paddles,
        intended,
        "decoded changed-byte set relative to known-good paddle image",
    )

    manifest: dict[str, object] = {
        "base_firmware": "8BitDo Ultimate Bluetooth 1.11 (type 41)",
        "base_image_sha256": sha256(stock),
        "known_good_paddle_image_sha256": sha256(paddle_image),
        "experimental_image_sha256": sha256(patched),
        "decoded_payload_sha256": sha256(patched_decoded[HEADER_SIZE:]),
        "inherits_paddle_manifest": paddle_manifest,
        "experiment": {
            "scope": "bonded Bluetooth reconnect failure timing only",
            "pairing_discovery_changed": False,
            "radio_timeout_before_slots": 4800,
            "radio_timeout_after_slots": 3200,
            "radio_timeout_before_ms": 3000,
            "radio_timeout_after_ms": 2000,
            "watchdog_before_ms": 6000,
            "watchdog_after_ms": 4000,
            "decoded_changed_bytes_from_known_good_paddles": len(
                changed_from_paddles
            ),
        },
        "patches": [
            {
                "execution_address": f"0x{RECONNECT_TIMEOUT_ADDRESS:08X}",
                "before": RECONNECT_TIMEOUT_EXPECTED.hex(" "),
                "after": RECONNECT_TIMEOUT_PATCH.hex(" "),
                "purpose": "reduce BT reconnect timeout from 4,800 to 3,200 slots",
            },
            {
                "execution_address": f"0x{RECONNECT_WATCHDOG_ADDRESS:08X}",
                "before": RECONNECT_WATCHDOG_EXPECTED.hex(" "),
                "after": RECONNECT_WATCHDOG_PATCH.hex(" "),
                "purpose": "reduce matching callback watchdog from 6,000 to 4,000 ms",
            },
        ],
    }
    return patched, manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("stock", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--updater-dll", type=Path, required=True)
    args = parser.parse_args()

    patched, manifest = build_experiment(
        args.stock.read_bytes(),
        args.updater_dll,
    )
    args.output.write_bytes(patched)
    manifest_path = args.output.with_suffix(args.output.suffix + ".json")
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )

    print(f"wrote {args.output} ({len(patched)} bytes)")
    print(f"SHA-256 {manifest['experimental_image_sha256']}")
    print(f"wrote {manifest_path}")


if __name__ == "__main__":
    main()
