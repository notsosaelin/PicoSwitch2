#!/usr/bin/env python3
"""Structural-decomposition tests for the 40-byte block on Switch 2 handle 0x000E (raw report
offsets 15-54). Tests whether the block matches a repeating native IMU FIFO packet structure
(e.g. ICM-42670-P style 20-byte high-resolution packets) rather than assuming byte-aligned scalar
fields at arbitrary offsets.

See docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md §14 for how this tool's output
was interpreted, the confidence-qualified conclusions, and what remains unresolved.

Tests implemented:
  1. check_framing()           -- confirms the length-prefix byte and block/tail boundaries.
  2. entropy_profile()          -- per-offset Shannon entropy across the whole 63-byte report.
  3. periodicity_test()         -- correlation of the entropy profile against itself shifted by
                                    each candidate packet period (2,4,5,8,10,16,20) -- a genuine
                                    repeating packet structure with a header byte would produce a
                                    strong, position-consistent low-entropy recurrence at its own
                                    period; this checks for that signature without assuming it.
  4. subbyte_header_scan()      -- checks the TOP 2/3 bits of each block byte for low entropy,
                                    in case header flags share a byte with otherwise-busy data
                                    (a real pattern in some FIFO header conventions).
  5. magnitude_stability_scan() -- for every 3-consecutive-int16 window (accelerometer-triplet
                                    candidate) and both endiannesses, compares vector magnitude
                                    mean/CV between confirmed-stationary phases and fixed-tilt
                                    hold phases -- a genuine accelerometer's magnitude should be
                                    orientation-invariant (~constant regardless of tilt), which is
                                    a physically-grounded test independent of any assumed byte map.

Read-only. Never modifies source NDJSON. Requires a capture with `marker` entries for phase-aware
tests (magnitude_stability_scan); framing/entropy/periodicity tests work on any 0x000E-bearing
capture.

Usage:
    python tools/analyze_sw2_block_structure.py <path-to-ndjson> [--markers]
"""
import sys
import os
import json
import math
import statistics
from collections import Counter

BLOCK_START = 15   # raw report offset where the 40-byte block begins (after the length-prefix
                    # byte at offset 14, which is 0x28=40 once active)
BLOCK_LEN = 40
TAIL_START = 55     # constant-zero tail begins here


def load_rows(path, handle="0x000E"):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("kind") == "input" and r.get("handle") == handle:
                out.append((r["us"], bytes.fromhex(r["bytes"])))
    return out


def load_markers(path):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("kind") == "marker":
                out.append((r["us"], bytes.fromhex(r["bytes"]).decode("ascii", errors="replace")))
    return out


def entropy_bits(counter, total):
    if total == 0:
        return 0.0
    h = 0.0
    for c in counter.values():
        p = c / total
        h -= p * math.log2(p)
    return h


def check_framing(rows):
    print(f"\n{'=' * 100}\nFRAMING CHECK\n{'=' * 100}")
    vals14 = Counter(b[14] for _, b in rows)
    print(f"  offset 14 (length-prefix candidate) value distribution: {dict(vals14)}")
    print(f"    (expect overwhelmingly {{40}}; a handful of 0s at the very start of a session is"
          f" the known pre-activation transient, not a mid-session validity toggle -- see §14.1)")
    tails = Counter(bytes(b[TAIL_START:63]) for _, b in rows)
    print(f"  tail (offset {TAIL_START}-62) distinct patterns: {len(tails)} (expect 1, all-zero)")


def entropy_profile(rows, start, length):
    n = len(rows)
    profile = []
    for off in range(start, start + length):
        vals = [b[off] for _, b in rows]
        cnt = Counter(vals)
        profile.append(entropy_bits(cnt, n))
    return profile


def corr(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return 0.0
    return sxy / math.sqrt(sxx * syy)


def periodicity_test(rows):
    print(f"\n{'=' * 100}\nPACKET-PERIODICITY TEST (entropy-profile self-correlation)\n{'=' * 100}")
    profile = entropy_profile(rows, BLOCK_START, BLOCK_LEN)
    print("  Block entropy profile (payload-relative 0-39):")
    for i in range(0, BLOCK_LEN, 10):
        print("   ", [f"{v:.1f}" for v in profile[i:i + 10]])
    print(f"\n  corr(entropy[i], entropy[i+P]) for each candidate packet period P -- a genuine")
    print(f"  repeating packet with a stable header/field structure would show a notably positive")
    print(f"  correlation at its own true period; near-zero/negative means no such structure found:")
    for P in (2, 4, 5, 8, 10, 16, 20):
        xs = profile[:BLOCK_LEN - P]
        ys = profile[P:BLOCK_LEN]
        c = corr(xs, ys)
        print(f"    P={P:>3}: corr={c:+.3f}  (n={len(xs)} pairs)")


def subbyte_header_scan(rows, low_entropy_threshold=1.5):
    print(f"\n{'=' * 100}\nSUB-BYTE HEADER-BIT SCAN (top 2/3 bits of each block byte)\n{'=' * 100}")
    n = len(rows)
    print(f"  Offsets where top-2-bit or top-3-bit entropy < {low_entropy_threshold} "
          f"(candidate header-bit positions):")
    found = False
    for off in range(BLOCK_START, BLOCK_START + BLOCK_LEN):
        vals = [b[off] for _, b in rows]
        top2 = Counter(v >> 6 for v in vals)
        top3 = Counter(v >> 5 for v in vals)
        e2, e3 = entropy_bits(top2, n), entropy_bits(top3, n)
        if e2 < low_entropy_threshold or e3 < 2.0:
            found = True
            print(f"    raw 0x{off:02x}: top2_entropy={e2:.3f} (uniq={len(top2)})  "
                  f"top3_entropy={e3:.3f} (uniq={len(top3)})")
    if not found:
        print("    (none found)")
    print("  Note: isolated low-entropy candidates that don't recur at a consistent packet-period")
    print("  spacing are weak evidence at best -- cross-check against periodicity_test() above.")


def s16(b, off, big_endian=False):
    v = (b[off] << 8 | b[off + 1]) if big_endian else (b[off] | (b[off + 1] << 8))
    return v - 65536 if v >= 32768 else v


def build_phase_index(rows, markers):
    t0 = rows[0][0]
    ts = [(us - t0) / 1e6 for us, _ in rows]
    mk = [((us - t0) / 1e6, label) for us, label in markers]
    stack, pairs = {}, []
    for mt, label in mk:
        if label.endswith("_start"):
            stack[label[:-6]] = mt
        elif label.endswith("_end"):
            name = label[:-4]
            if name in stack:
                pairs.append((name, stack.pop(name), mt))
    return ts, pairs


def magnitude_stability_scan(rows, markers, exclude_offsets=(15,), top_n=15):
    print(f"\n{'=' * 100}\nMAGNITUDE-STABILITY SCAN (orientation-invariance test)\n{'=' * 100}")
    print("  A genuine accelerometer triplet's vector magnitude should stay roughly constant")
    print("  regardless of orientation (only the per-axis distribution should change). Tests")
    print("  every 3-consecutive-int16 window against baseline vs. fixed-tilt hold phases.")
    ts, pairs = build_phase_index(rows, markers)

    def phase_idxs(pred):
        out = []
        for name, tstart, tend in pairs:
            if pred(name):
                out.extend(i for i, t in enumerate(ts) if tstart <= t <= tend)
        return out

    baseline = phase_idxs(lambda n: n.startswith("baseline"))
    hold_names = set(name for name, _, _ in pairs if name.endswith("_hold"))
    holds = {name: phase_idxs(lambda n, name=name: n == name) for name in hold_names}
    if not baseline or not holds:
        print("  (no baseline/*_hold marker pairs found in this capture -- skipping)")
        return

    def mag_stats(idxs, off, be):
        mags = []
        for i in idxs:
            b = rows[i][1]
            x, y, z = s16(b, off, be), s16(b, off + 2, be), s16(b, off + 4, be)
            mags.append(math.sqrt(x * x + y * y + z * z))
        if len(mags) < 5:
            return None
        mean = statistics.mean(mags)
        sd = statistics.pstdev(mags)
        return mean, (sd / mean if mean > 0 else float("inf"))

    results = []
    for off in range(BLOCK_START, BLOCK_START + BLOCK_LEN - 4):
        if off in exclude_offsets or off + 1 in exclude_offsets:
            continue
        for be in (False, True):
            b_stats = mag_stats(baseline, off, be)
            if not b_stats or b_stats[0] == 0:
                continue
            hold_ratios, hold_cvs = [], []
            ok = True
            for name, idxs in holds.items():
                h_stats = mag_stats(idxs, off, be)
                if not h_stats:
                    ok = False
                    break
                hold_ratios.append(h_stats[0] / b_stats[0])
                hold_cvs.append(h_stats[1])
            if not ok:
                continue
            score = sum(abs(r - 1) for r in hold_ratios) + b_stats[1] + sum(hold_cvs)
            results.append((score, off, be, b_stats, hold_ratios, hold_cvs))

    results.sort(key=lambda r: r[0])
    print(f"\n  Top {top_n} candidates by orientation-invariance score (lower = better):")
    for score, off, be, b_stats, hold_ratios, hold_cvs in results[:top_n]:
        print(f"    0x{off:02x} {'BE' if be else 'LE'}  baseline=({b_stats[0]:8.1f},cv={b_stats[1]:.2f})  "
              f"hold_ratios={['%.2f' % r for r in hold_ratios]}  hold_cvs={['%.2f' % c for c in hold_cvs]}  "
              f"score={score:.2f}")
    print(f"\n  CAUTION: candidates overlapping known accumulator/counter bytes (raw 0x0f, 0x13-0x14,")
    print(f"  0x19-0x1a -- see the parent report §8) can show spuriously stable-looking magnitude")
    print(f"  because those bytes' typical value range is similar across any two multi-second")
    print(f"  windows regardless of physical orientation -- a statistical artifact, not physics.")
    print(f"  Manually cross-check top candidates against that list before trusting a match.")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    with_markers = "--markers" in sys.argv

    rows = load_rows(path)
    print(f"Loaded {len(rows)} 0x000E records from {os.path.basename(path)}")
    check_framing(rows)
    periodicity_test(rows)
    subbyte_header_scan(rows)

    if with_markers:
        markers = load_markers(path)
        print(f"\nLoaded {len(markers)} markers")
        magnitude_stability_scan(rows, markers)
    else:
        print("\n(pass --markers to also run the phase-aware magnitude-stability scan, requires "
              "a capture with marker entries)")


if __name__ == "__main__":
    main()
