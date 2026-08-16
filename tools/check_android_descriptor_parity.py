#!/usr/bin/env python3
"""Verify the Android bridge HID descriptor is byte-identical on both sides.

The firmware identifies the companion bridge by an EXACT match against the
canonical descriptor (see bthid_android_bridge.c), so a one-sided edit would not
fail to compile or fail a unit test -- it would silently stop the handheld's
motion/battery/rumble/LED from ever being recognized. This check makes that
failure loud and cheap.

Sources compared:
  tools/fixtures/android_controller_hid.h  -> ANDROID_CONTROLLER_V2_HID_DESCRIPTOR
  android/companion/bridge-core/.../protocol/BridgeHidDescriptor.kt -> BridgeHidDescriptor.bytes

The Kotlin side lives in Bridge Core, the platform-neutral module, because the
descriptor is a BRIDGE artifact rather than an Android one -- every platform
backend registers these same bytes.

Usage: python tools/check_android_descriptor_parity.py
Exit code 0 when identical, 1 otherwise.
"""
from __future__ import annotations

import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
C_HEADER = ROOT / "tools" / "fixtures" / "android_controller_hid.h"
KOTLIN = (ROOT / "android" / "companion" / "bridge-core" / "src" / "main" / "kotlin" /
          "dev" / "picoswitch" / "bridge" / "protocol" / "BridgeHidDescriptor.kt")

BYTE_RE = re.compile(r"0[xX][0-9a-fA-F]{1,2}")


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _extract_block(text: str, start_pattern: str) -> str:
    """Return the text between the first '{' after start_pattern and its '}'."""
    match = re.search(start_pattern, text)
    if not match:
        raise SystemExit(f"could not locate {start_pattern!r}")
    open_index = text.index("{", match.end())
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:index]
    raise SystemExit(f"unterminated block for {start_pattern!r}")


def _c_bytes() -> list[int]:
    text = _strip_comments(C_HEADER.read_text(encoding="utf-8"))
    body = _extract_block(text, r"ANDROID_CONTROLLER_V2_HID_DESCRIPTOR\s*\[\s*\]\s*=")
    # The C source references the report-ID macros symbolically.
    body = body.replace("ANDROID_CONTROLLER_REPORT_ID", "0x01")
    body = body.replace("ANDROID_CONTROLLER_OUTPUT_REPORT_ID", "0x02")
    return [int(value, 16) for value in BYTE_RE.findall(body)]


def _kotlin_bytes() -> list[int]:
    text = _strip_comments(KOTLIN.read_text(encoding="utf-8"))
    body = _extract_block(text, r"object\s+BridgeHidDescriptor")
    body = body[body.index("byteArrayOf("):]
    body = body[body.index("(") + 1: body.rindex(")")]
    return [int(value, 16) for value in BYTE_RE.findall(body)]


CONTRACT_KOTLIN = (ROOT / "android" / "companion" / "bridge-core" / "src" / "main" /
                   "kotlin" / "dev" / "picoswitch" / "bridge" / "protocol" /
                   "BridgeContract.kt")


def _c_contract() -> int:
    text = C_HEADER.read_text(encoding="utf-8")
    match = re.search(r"#define\s+ANDROID_BRIDGE_CONTRACT_VERSION\s+(\d+)", text)
    if not match:
        raise SystemExit("ANDROID_BRIDGE_CONTRACT_VERSION not found in the C fixture")
    return int(match.group(1))


def _kotlin_contract() -> int:
    text = _strip_comments(CONTRACT_KOTLIN.read_text(encoding="utf-8"))
    match = re.search(r"const\s+val\s+VERSION\s*=\s*(\d+)", text)
    if not match:
        raise SystemExit("BridgeContract.VERSION not found in the Kotlin source")
    return int(match.group(1))


def _registered_digest(version: int) -> str | None:
    """The digest BridgeContract registers for `version`, or None."""
    text = _strip_comments(CONTRACT_KOTLIN.read_text(encoding="utf-8"))
    block = re.search(r"DESCRIPTOR_DIGESTS[^=]*=\s*mapOf\((.*?)\)", text, re.S)
    if not block:
        raise SystemExit("BridgeContract.DESCRIPTOR_DIGESTS not found")
    for entry_version, digest in re.findall(r"(\d+)\s+to\s+\"([0-9a-fA-F]{64})\"", block.group(1)):
        if int(entry_version) == version:
            return digest.lower()
    return None


def main() -> int:
    # Contract version first: a descriptor change without a version bump is the
    # failure this check exists to prevent, and reporting it before the byte diff
    # makes the required fix obvious.
    c_contract = _c_contract()
    kt_contract = _kotlin_contract()
    if c_contract != kt_contract:
        print("android bridge CONTRACT MISMATCH", file=sys.stderr)
        print(f"  C  : ANDROID_BRIDGE_CONTRACT_VERSION = {c_contract}", file=sys.stderr)
        print(f"  Kt : BridgeContract.VERSION          = {kt_contract}", file=sys.stderr)
        return 1

    c = _c_bytes()
    kt = _kotlin_bytes()

    # Whole-descriptor digest, pinned per contract version. Byte-for-byte parity
    # between the two languages is not enough on its own: a coordinated edit to
    # BOTH sides keeps them equal while silently changing what goes on the wire.
    # This is the check that forces a deliberate version bump for ANY byte.
    digest = hashlib.sha256(bytes(c)).hexdigest()
    registered = _registered_digest(c_contract)
    if registered is None:
        print(f"android bridge contract {c_contract} has NO registered descriptor digest",
              file=sys.stderr)
        print(f'  add to BridgeContract.DESCRIPTOR_DIGESTS: {c_contract} to "{digest}",',
              file=sys.stderr)
        return 1
    if digest != registered:
        print("android descriptor CHANGED WITHOUT A CONTRACT BUMP", file=sys.stderr)
        print(f"  contract {c_contract} registers sha256 {registered}", file=sys.stderr)
        print(f"  the descriptor now hashes to  {digest}", file=sys.stderr)
        print("  If the change is intentional and observable by a peer:", file=sys.stderr)
        print("    1. bump ANDROID_BRIDGE_CONTRACT_VERSION and BridgeContract.VERSION",
              file=sys.stderr)
        print(f'    2. register {c_contract + 1} to "{digest}",', file=sys.stderr)
        print("    3. reflash the adapter before testing a new APK.", file=sys.stderr)
        print("  If not, revert the descriptor.", file=sys.stderr)
        return 1

    if c == kt:
        print(f"android descriptor parity OK ({len(c)} bytes identical, "
              f"bridge contract {c_contract}, sha256 {digest[:16]}...)")
        return 0

    print("android descriptor MISMATCH", file=sys.stderr)
    print(f"  C  : {len(c)} bytes", file=sys.stderr)
    print(f"  Kt : {len(kt)} bytes", file=sys.stderr)
    for index in range(max(len(c), len(kt))):
        left = f"{c[index]:#04x}" if index < len(c) else "--"
        right = f"{kt[index]:#04x}" if index < len(kt) else "--"
        if left != right:
            print(f"  first difference at index {index}: C={left} Kotlin={right}",
                  file=sys.stderr)
            break
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
