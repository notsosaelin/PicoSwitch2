#!/usr/bin/env python3
"""Verify the Android bridge HID descriptor is byte-identical on both sides.

The firmware identifies the companion bridge by an EXACT match against the
canonical descriptor (see bthid_android_bridge.c), so a one-sided edit would not
fail to compile or fail a unit test -- it would silently stop the handheld's
motion/battery/rumble/LED from ever being recognized. This check makes that
failure loud and cheap.

Sources compared:
  tools/fixtures/android_controller_hid.h  -> ANDROID_CONTROLLER_V2_HID_DESCRIPTOR
  android/companion/.../controller/ControllerState.kt -> AndroidControllerDescriptor.bytes

Usage: python tools/check_android_descriptor_parity.py
Exit code 0 when identical, 1 otherwise.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
C_HEADER = ROOT / "tools" / "fixtures" / "android_controller_hid.h"
KOTLIN = (ROOT / "android" / "companion" / "app" / "src" / "main" / "java" / "dev" /
          "picoswitch" / "companion" / "controller" / "ControllerState.kt")

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
    body = _extract_block(text, r"object\s+AndroidControllerDescriptor")
    body = body[body.index("byteArrayOf("):]
    body = body[body.index("(") + 1: body.rindex(")")]
    return [int(value, 16) for value in BYTE_RE.findall(body)]


def main() -> int:
    c = _c_bytes()
    kt = _kotlin_bytes()
    if c == kt:
        print(f"android descriptor parity OK ({len(c)} bytes identical)")
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
