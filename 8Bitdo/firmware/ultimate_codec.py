#!/usr/bin/env python3
"""Decode and encode first-generation 8BitDo Ultimate Bluetooth firmware."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


HEADER_SIZE = 0x1C
BLOCK_SIZE = 0x200
TABLE_FILE_OFFSET = 0x1ACF68
TABLE_ENTRIES = 256
ADDRESS_DELTA = 0xEE97FAFA


def ror32(value: int, bits: int) -> int:
    return ((value >> bits) | (value << (32 - bits))) & 0xFFFFFFFF


def load_key_table(updater_dll: Path) -> tuple[int, ...]:
    data = updater_dll.read_bytes()
    return struct.unpack_from(
        f"<{TABLE_ENTRIES}H",
        data,
        TABLE_FILE_OFFSET,
    )


def transform_payload(
    payload: bytes,
    table: tuple[int, ...],
    *,
    decode: bool,
) -> bytes:
    output = bytearray(payload)

    for block_offset in range(0, len(output), BLOCK_SIZE):
        relative_address = block_offset
        address_sum = (
            relative_address
            + (relative_address >> 8)
            + (relative_address >> 16)
            + (relative_address >> 24)
        ) ^ 0xB645
        key = (
            table[(address_sum >> 8) & 0xFF] << 16
            | table[address_sum & 0xFF]
        )
        address_state = (
            relative_address + ror32(relative_address, 20)
        ) & 0xFFFFFFFF
        address_state = (
            address_state + ror32(address_state, 17)
        ) & 0xFFFFFFFF
        previous_ciphertext = 0

        block_length = min(BLOCK_SIZE, len(output) - block_offset)
        word_length = block_length & ~3
        for word_offset in range(0, word_length, 4):
            absolute_offset = block_offset + word_offset
            input_word = struct.unpack_from("<I", output, absolute_offset)[0]

            if decode:
                ciphertext = input_word
                output_word = (
                    ciphertext
                    ^ previous_ciphertext
                    ^ key
                    ^ address_state
                ) & 0xFFFFFFFF
            else:
                output_word = (
                    input_word
                    ^ previous_ciphertext
                    ^ key
                    ^ address_state
                ) & 0xFFFFFFFF
                ciphertext = output_word

            struct.pack_into("<I", output, absolute_offset, output_word)
            previous_ciphertext = ror32(ciphertext, 18)
            key = ror32(key, 16)
            address_state = (
                address_state + ADDRESS_DELTA
            ) & 0xFFFFFFFF

    return bytes(output)


def decode_image(image: bytes, table: tuple[int, ...]) -> bytes:
    if len(image) < HEADER_SIZE:
        raise ValueError("firmware image is shorter than its 28-byte header")
    return image[:HEADER_SIZE] + transform_payload(
        image[HEADER_SIZE:],
        table,
        decode=True,
    )


def encode_image(image: bytes, table: tuple[int, ...]) -> bytes:
    if len(image) < HEADER_SIZE:
        raise ValueError("firmware image is shorter than its 28-byte header")
    return image[:HEADER_SIZE] + transform_payload(
        image[HEADER_SIZE:],
        table,
        decode=False,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("decode", "encode", "roundtrip"))
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument(
        "--updater-dll",
        type=Path,
        required=True,
        help="official 8BitDoFirmwareUpdaterTools.dll containing the key table",
    )
    parser.add_argument(
        "--payload-only",
        action="store_true",
        help="write only the decoded/encoded payload, without the 28-byte header",
    )
    args = parser.parse_args()

    table = load_key_table(args.updater_dll)
    source = args.input.read_bytes()

    if args.operation == "decode":
        result = decode_image(source, table)
    elif args.operation == "encode":
        result = encode_image(source, table)
    else:
        decoded = decode_image(source, table)
        result = encode_image(decoded, table)
        if result != source:
            raise SystemExit("round-trip mismatch")
        print(f"round-trip exact: {len(source)} bytes")
        return

    if args.output is None:
        raise SystemExit("output path is required for decode and encode")
    if args.payload_only:
        result = result[HEADER_SIZE:]
    args.output.write_bytes(result)


if __name__ == "__main__":
    main()
