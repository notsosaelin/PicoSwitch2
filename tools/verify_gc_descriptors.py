#!/usr/bin/env python3
"""Verify src/switch_gc/switch_gc.c's descriptor byte arrays against the
independently-captured evidence, so future edits to that file cannot silently
drift from what was actually confirmed on real hardware.

Expected bytes below are transcribed independently of switch_gc.c's C source
-- from the raw USB capture this project made itself
(docs/experiments/nso-gc-captures/genuine-controller-descriptors-2026-07-13.pcap,
decoded in docs/switch2-gc/protocol.md) for the device+config descriptors, and
from ndeadly/switch2_controller_research's descriptors.md (cloned at commit
d1c5a7f7ba298f83017fae84952a4e6d2ef8fc92) for the HID report descriptor. This
script re-derives the same numbers a second, independent way rather than
re-stating switch_gc.c's own literals back at itself.

Usage: python tools/verify_gc_descriptors.py
Exit code 0 = all checks passed, nonzero = a mismatch was found (prints which).
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SWITCH_GC_C = REPO_ROOT / "src" / "switch_gc" / "switch_gc.c"

# --- Independently-transcribed expected bytes -------------------------------
#
# NOTE on bcdDevice (offset 12-13): the raw captured value from genuine
# hardware is 0x01,0x01 (BCD 1.01). This tool intentionally expects 0x11,0x01
# instead -- switch_gc.c deliberately deviates from the raw capture here,
# mirroring switch_pro2.c's own already-shipped precedent (its bcdDevice is
# 2.10, not the real 2.00), because using the real value made this project's
# emulated controller and a genuine one indistinguishable to Windows' WinUSB
# driver cache (keyed on VID+PID+bcdDevice) when both are connected in the
# same session -- confirmed via real hardware testing 2026-07-13. See
# switch_gc.c's own comment on this exact byte pair for the full account.

EXPECTED_DEVICE_DESC = bytes([
    0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40,
    0x7E, 0x05, 0x73, 0x20, 0x11, 0x01, 0x01, 0x02, 0x03, 0x01,
])

EXPECTED_CONFIG_DESC = bytes([
    0x09, 0x02, 0x50, 0x00, 0x02, 0x01, 0x04, 0xC0, 0xFA,
    0x08, 0x0B, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x05,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x61, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04,
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04,
    0x08, 0x0B, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x06,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
])

EXPECTED_REPORT_DESC = bytes([
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x85, 0x05, 0x05, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x95, 0x3F, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x0A, 0x09, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x15, 0x25, 0x01, 0x95, 0x15, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x35,
    0x26, 0xFF, 0x0F, 0x95, 0x04, 0x75, 0x0C, 0x81, 0x02, 0xC0,
    0x05, 0xFF, 0x09, 0x02, 0x26, 0xFF, 0x00, 0x95, 0x34, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x03, 0x09, 0x01, 0x95, 0x3F, 0x91, 0x02,
    0xC0,
])


def extract_macro_int(source: str, name: str) -> int:
    m = re.search(rf"#define\s+{re.escape(name)}\s+(\d+)", source)
    if not m:
        raise SystemExit(f"FAIL: could not find '#define {name}' in {SWITCH_GC_C}")
    return int(m.group(1))


def extract_c_array(source: str, name: str, macros: dict) -> bytes:
    """Pull a `static const uint8_t <name>[] = { ... };` array's byte values
    out of switch_gc.c, tolerant of the //-comments and the small set of
    preprocessor byte-splitting expressions (e.g. `(X & 0xFF)`) it uses for
    its own wTotalLength field -- evaluated with Python's eval() against a
    restricted namespace containing only the macro constants extracted above,
    not arbitrary code execution."""
    m = re.search(rf"{re.escape(name)}\[\]\s*=\s*\{{(.*?)\}};", source, re.DOTALL)
    if not m:
        raise SystemExit(f"FAIL: could not find array '{name}' in {SWITCH_GC_C}")
    body = m.group(1)
    body = re.sub(r"//.*", "", body)  # strip line comments
    tokens = [t.strip() for t in body.split(",")]
    values = []
    for t in tokens:
        if not t:
            continue
        if re.fullmatch(r"[0-9xXA-Fa-f]+", t):
            values.append(int(t, 0))
        else:
            # A small C integer expression, e.g. "(SWITCH_GC_CONFIG_LEN & 0xFF)".
            # C's '&'/'>>' are also valid Python operators, so eval() with only
            # the known macro names in scope is sufficient and safe here.
            values.append(eval(t, {"__builtins__": {}}, macros) & 0xFF)
    return bytes(values)


def check(name: str, actual: bytes, expected: bytes) -> bool:
    if actual == expected:
        print(f"OK   {name}: {len(actual)} bytes, matches independently-transcribed evidence")
        return True
    print(f"FAIL {name}: MISMATCH")
    print(f"     switch_gc.c has {len(actual)} bytes, expected {len(expected)} bytes")
    n = min(len(actual), len(expected))
    for i in range(n):
        if actual[i] != expected[i]:
            print(f"     first differing byte at offset {i}: "
                  f"switch_gc.c=0x{actual[i]:02X} expected=0x{expected[i]:02X}")
            break
    return False


def main() -> int:
    source = SWITCH_GC_C.read_text(encoding="utf-8")
    macros = {"SWITCH_GC_CONFIG_LEN": extract_macro_int(source, "SWITCH_GC_CONFIG_LEN")}

    device = extract_c_array(source, "switch_gc_device_desc", macros)
    config = extract_c_array(source, "switch_gc_config_desc", macros)
    report = extract_c_array(source, "switch_gc_report_desc", macros)

    ok = True
    ok &= check("device descriptor", device, EXPECTED_DEVICE_DESC)
    ok &= check("configuration descriptor", config, EXPECTED_CONFIG_DESC)
    ok &= check("HID report descriptor", report, EXPECTED_REPORT_DESC)

    # Structural checks independent of the byte-for-byte comparison above --
    # these catch a class of error a pure byte-diff might not make obvious.
    if len(report) != 97:
        print(f"FAIL: HID report descriptor is {len(report)} bytes, must be exactly 97 "
              "(this project's own Confirmed live capture of wDescriptorLength)")
        ok = False
    else:
        print("OK   HID report descriptor is exactly 97 bytes")

    # wTotalLength (config[2:4], little-endian) must equal the array's own length.
    wtotal = config[2] | (config[3] << 8)
    if wtotal != len(config):
        print(f"FAIL: config descriptor wTotalLength={wtotal} does not match actual "
              f"array length {len(config)}")
        ok = False
    else:
        print(f"OK   config descriptor wTotalLength ({wtotal}) matches actual array length")

    # bNumInterfaces (config[4]) must be 2 (IF0 HID, IF1 vendor).
    if config[4] != 2:
        print(f"FAIL: config descriptor bNumInterfaces={config[4]}, expected 2")
        ok = False
    else:
        print("OK   config descriptor declares 2 interfaces (HID + vendor)")

    # Declared report IDs inside the HID report descriptor: 5 (shared vendor
    # array), 0x0A (GameCube input), 3 (GameCube rumble output). Cross-checks
    # against this project's own live decode of real report 0x0A traffic
    # (docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md) and real
    # report 0x03 rumble samples from the same capture -- both used report
    # IDs that must appear as 0x85,<id> (Report ID tag) somewhere in this array.
    report_id_tag = 0x85
    declared_ids = {report[i + 1] for i in range(len(report) - 1) if report[i] == report_id_tag}
    expected_ids = {0x05, 0x0A, 0x03}
    if not expected_ids.issubset(declared_ids):
        print(f"FAIL: HID report descriptor declares report IDs {sorted(declared_ids)}, "
              f"missing {sorted(expected_ids - declared_ids)}")
        ok = False
    else:
        print(f"OK   HID report descriptor declares report IDs {sorted(declared_ids)} "
              "(5 shared-vendor, 0x0A input, 0x03 rumble output -- all Confirmed live)")

    print()
    if ok:
        print("All checks passed.")
        return 0
    else:
        print("One or more checks FAILED -- see above.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
