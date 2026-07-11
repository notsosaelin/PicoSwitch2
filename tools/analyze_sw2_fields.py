#!/usr/bin/env python3
"""Follow-up field-decode test for sw2_capture NDJSON logs.

Motivated by analyze_sw2_capture.py's finding that only offsets
{0,1,2,10,11,13,14,31} ever vary across four captures (STILL + 3 fixed
orientations), and that offsets 16-59 -- where switch2_input_viewer.py's
"Common" (handle 0x000A) format places a 14-byte motion block at 0x2E:0x3C --
are constant zero throughout.

This script tests concrete hypotheses about offsets 10-15 (where the
reference tool's format-0 places stick1/stick2) instead of assuming its
mapping: does a 12-bit stick unpack produce plausible joystick coordinates,
or does a raw int16 interpretation look more like accelerometer/orientation
data? Also tests +/-1 byte alignment shifts in case of an off-by-one framing
difference between this capture and the reference tool's bleak-based reads.

Read-only. Never modifies the source NDJSON.
"""
import sys
import os
import json
import glob
import statistics

DUMPS_DIR = sys.argv[1] if len(sys.argv) > 1 else "dumps"


def load_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("kind") == "input" and r.get("handle") == "0x000A":
                try:
                    rows.append(bytes.fromhex(r["bytes"]))
                except ValueError:
                    pass
    return rows


def unpack_12bit_triplet(b0, b1, b2):
    a = b0 | ((b1 & 0x0F) << 8)
    b = (b1 >> 4) | (b2 << 4)
    return a, b


def summarize(label, values):
    if not values:
        print(f"    {label}: (no data)")
        return
    print(f"    {label}: n={len(values)} min={min(values)} max={max(values)} "
          f"mean={statistics.mean(values):.1f} stdev={statistics.pstdev(values):.2f}")


def field_decode_report(rows, filelabel):
    print(f"\n=== {filelabel} (n={len(rows)}) ===")

    # --- reference-tool format-0 hypothesis: buttons @4:8, stick1 @A:D (12-bit), stick2 @D:10 ---
    buttons = [int.from_bytes(b[4:8], "little") for b in rows]
    nonzero_buttons = sum(1 for v in buttons if v != 0)
    print(f"  buttons (report[4:8] LE u32): nonzero in {nonzero_buttons}/{len(rows)} records "
          f"(sanity check -- should be 0 if no buttons pressed during a motion-only hold)")

    s1x, s1y = [], []
    s2x, s2y = [], []
    for b in rows:
        x, y = unpack_12bit_triplet(b[10], b[11], b[12])
        s1x.append(x); s1y.append(y)
        x2, y2 = unpack_12bit_triplet(b[13], b[14], b[15])
        s2x.append(x2); s2y.append(y2)
    print("  12-bit-stick hypothesis (reference tool's stick1/stick2 unpack):")
    summarize("stick1 X (report[0xA],[0xB] low nibble)", s1x)
    summarize("stick1 Y (report[0xB] high nibble,[0xC])", s1y)
    summarize("stick2 X (report[0xD],[0xE] low nibble)", s2x)
    summarize("stick2 Y (report[0xE] high nibble,[0xF])", s2y)
    print("    -- a resting analog stick should read near mid-scale (~0x800=2048) for a 12-bit "
          "axis regardless of controller ORIENTATION (only hand pressure moves a stick); a value "
          "that tracks orientation instead is evidence against the stick hypothesis for this field.")

    # --- alternative hypothesis: raw signed int16 LE pairs directly over the same byte range ---
    print("  raw-int16-LE hypothesis (same byte range, no 12-bit packing assumed):")
    for off in (10, 12, 14):
        vals = [int.from_bytes(b[off:off + 2], "little", signed=True) for b in rows]
        summarize(f"int16_le @ offset 0x{off:02x}", vals)

    # --- offset 2: small-range flag/counter byte ---
    o2 = [b[2] for b in rows]
    summarize("offset 0x02 (raw byte)", o2)

    # --- offset 31: the other field that varies a little ---
    o31 = [b[31] for b in rows]
    summarize("offset 0x1f (raw byte)", o31)

    # --- offsets 0,1: confirm counter/timestamp-like behavior (already known to be near-full-entropy) ---
    o0 = [b[0] for b in rows]
    o1 = [b[1] for b in rows]
    # Check if (byte0, byte1) as a little-endian uint16 is monotonically non-decreasing mod 65536
    pairs = [b[0] | (b[1] << 8) for b in rows]
    wraps = 0
    increasing_steps = 0
    for i in range(1, len(pairs)):
        d = (pairs[i] - pairs[i - 1]) % 65536
        if d == 0:
            continue
        increasing_steps += 1
        if pairs[i] < pairs[i - 1]:
            wraps += 1
    print(f"  offsets 0-1 as LE u16 'counter': {increasing_steps} changing steps, "
          f"{wraps} wrap-arounds over {len(pairs)} records "
          f"(consistent with a free-running counter/tick if wraps are rare and steps are small)")
    step_deltas = [(pairs[i] - pairs[i - 1]) % 65536 for i in range(1, len(pairs))]
    if step_deltas:
        summarize("per-record delta of the u16 @ offset 0", step_deltas)


def alignment_shift_test(rows, filelabel):
    """Check whether shifting the whole 63-byte record by +/-1 reveals a DIFFERENT
    non-zero region in what analyze_sw2_capture.py found to be an all-zero span
    (offsets 16-59) -- i.e. test for an off-by-one framing mismatch."""
    print(f"\n  --- alignment shift test: {filelabel} ---")
    for shift in (-1, 0, 1):
        nonzero_offsets_in_1659 = set()
        for b in rows:
            if shift == 0:
                view = b
            elif shift == 1:
                view = b[1:]
            else:
                view = b"\x00" + b[:-1]
            for off in range(16, min(60, len(view))):
                if view[off] != 0:
                    nonzero_offsets_in_1659.add(off)
        print(f"    shift={shift:+d}: offsets 16-59 with any nonzero byte after shifting: "
              f"{sorted(nonzero_offsets_in_1659) if nonzero_offsets_in_1659 else '(none)'}")


def main():
    files = sorted(glob.glob(os.path.join(DUMPS_DIR, "sw2_capture_*.ndjson")))
    for path in files:
        rows = load_rows(path)
        label = os.path.basename(path)
        field_decode_report(rows, label)
        alignment_shift_test(rows, label)


if __name__ == "__main__":
    main()
