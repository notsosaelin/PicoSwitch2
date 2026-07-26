#!/usr/bin/env python3
"""Generate a raw-format, identity-only 540-byte amiibo image for the
signature-less acceptance experiment.

Motivation (see docs/switch2/amiibo-identity-and-generation.md): community
generators such as hax0kartik/amiibo-generator emit amiitool DECRYPTED-layout
templates (identity at 0x1DC) that are not directly serveable by the
PicoSwitch2 virtual reader, which speaks the raw NTAG215 layout. This tool
builds the raw-layout equivalent: correct NTAG215 structure plus the 8-byte
amiibo identity block, with all cryptographic regions (tag/data HMACs,
encrypted settings, originality signature) intentionally zeroed.

The output is deliberately an experiment artifact. Whether a real Switch 2
accepts it through the virtual reader path is an open question this file
exists to answer; a genuine dump remains the only Confirmed-tier input.

Usage:
    python tools/generate_test_amiibo.py <amiibo-id> [-o OUT.bin] [--uid HEX14]

<amiibo-id> is the 16-hex-digit AmiiboAPI identity (head+tail, e.g.
0100000000040002 for Zelda-series Link variants — check amiiboapi.com).
"""

import argparse
import secrets
import sys

RAW_SIZE = 540


def build_image(amiibo_id: bytes, uid: bytes) -> bytes:
    assert len(amiibo_id) == 8
    assert len(uid) == 7 and uid[0] == 0x04
    img = bytearray(RAW_SIZE)

    # Pages 0-2: UID with both check bytes, NTAG "internal" byte, and the
    # static lock bytes every retail amiibo carries (0x0F 0xE0).
    img[0:3] = uid[0:3]
    img[3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2]  # BCC0 includes cascade tag
    img[4:8] = uid[3:7]
    img[8] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6]  # BCC1
    img[9] = 0x48  # internal
    img[10] = 0x0F
    img[11] = 0xE0

    # Page 3: amiibo capability container.
    img[0x0C:0x10] = bytes.fromhex("F110FFEE")

    # Pages 0x15-0x16: plaintext amiibo identity block. This is the only
    # content-bearing field; AmiiboAPI's head+tail concatenation maps here
    # byte for byte.
    # AmiiboAPI ids already carry the trailing 0x02 format-version byte.
    img[0x54:0x5C] = amiibo_id

    # Page 0x82: dynamic lock bytes + RFUI as seen on retail tags.
    img[0x208:0x20C] = bytes.fromhex("01000FBD")
    # Pages 0x83-0x84: CFG0/CFG1 (AUTH0=4, ACCESS=0x5F) per retail amiibo.
    img[0x20C:0x210] = bytes.fromhex("00000004")
    img[0x210:0x214] = bytes.fromhex("5F000000")
    # PWD/PACK stay zero: genuine NTAG reads never return them, so dumps
    # normally carry zeros there and the firmware treats zeros as canonical.

    return bytes(img)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("amiibo_id",
                        help="16 hex digits: AmiiboAPI head+tail")
    parser.add_argument("-o", "--output", default=None,
                        help="output path (default amiibo-<id>-test.bin)")
    parser.add_argument("--uid", default=None,
                        help="explicit 14-hex-digit UID starting with 04; "
                             "random when omitted")
    args = parser.parse_args()

    identity = args.amiibo_id.lower().removeprefix("0x")
    if len(identity) != 16 or any(c not in "0123456789abcdef"
                                  for c in identity):
        print("amiibo-id must be exactly 16 hex digits", file=sys.stderr)
        return 1
    amiibo_id = bytes.fromhex(identity)

    if args.uid:
        uid = bytes.fromhex(args.uid)
        if len(uid) != 7 or uid[0] != 0x04:
            print("--uid must be 14 hex digits beginning with 04",
                  file=sys.stderr)
            return 1
    else:
        uid = bytes([0x04]) + secrets.token_bytes(6)

    image = build_image(amiibo_id, uid)
    out = args.output or f"amiibo-{identity}-test.bin"
    with open(out, "wb") as handle:
        handle.write(image)
    print(f"wrote {out}: identity {identity.upper()}, "
          f"UID {uid.hex().upper()}, {len(image)} bytes, "
          "crypto regions zeroed (experiment artifact)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
