#!/usr/bin/env python3
"""Exhaustive, layout-agnostic analysis of the 40-byte block observed on handle 0x000E
(raw report offsets 14-54, payload-relative offsets 0-39) across the v2 experiment captures.

See docs/experiments/sw2-v2-motion-block-discovery-2026-07-10.md for how this block was found,
and docs/switch2/ble-controller-protocol-inventory.md §3.8/§3.9 for how this script's output was
interpreted. This script assigns NO semantics -- it only computes objective statistical
properties of every plausible byte-alignment/width/endianness/signedness interpretation and ranks
them, per the explicit instruction not to pick an interpretation because its numbers "look
physically plausible."

Framing (established in the prior pass, re-verified here in check_framing()):
  raw offset 14      = a constant byte (0x28 = 40 decimal) -- a self-describing length prefix.
  raw offsets 15-54   = the 40-byte payload this script analyzes, called PAYLOAD[0..39] below.
  raw offsets 0-13    = counter(0-1) + tag(1) + 2 unknown + tag(1) + shifted stick duplicate
                        (5-10) + tag(1) -- already characterized, not re-analyzed here.
  raw offsets 55-62   = constant zero tail -- already characterized, not re-analyzed here.

Read-only. Never modifies source NDJSON.

Usage:
    python tools/analyze_sw2_motion_block.py [dumps_dir]
"""
import sys
import os
import json
import math
import statistics
from collections import Counter, defaultdict

DUMPS_DIR = sys.argv[1] if len(sys.argv) > 1 else "dumps"
VARIANT_FILES = {n: os.path.join(DUMPS_DIR, f"sw2_capture_2026-07-10_VARIANT-{n}.ndjson") for n in range(1, 7)}
PAYLOAD_START = 15   # raw report offset
PAYLOAD_LEN = 40


def load_rows(path, handle="0x000E"):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("kind") == "input" and r.get("handle") == handle:
                try:
                    out.append((r["us"], bytes.fromhex(r["bytes"])))
                except ValueError:
                    pass
    return out


def active_rows(rows):
    """Drop the brief pre-activation all-zero-payload prefix (see the prior pass's report)."""
    for i, (us, b) in enumerate(rows):
        if any(v != 0 for v in b[PAYLOAD_START:PAYLOAD_START + PAYLOAD_LEN]):
            return rows[i:]
    return []


def entropy_bits(counter, total):
    if total == 0:
        return 0.0
    h = 0.0
    for c in counter.values():
        p = c / total
        h -= p * math.log2(p)
    return h


# ---------------------------------------------------------------------------
# Framing check
# ---------------------------------------------------------------------------

def check_framing(all_active):
    print(f"\n{'=' * 100}\nFRAMING CHECK\n{'=' * 100}")
    for n, rows in all_active.items():
        vals14 = set(b[14] for _, b in rows)
        tail = set(tuple(b[55:63]) for _, b in rows)
        print(f"  V{n}: offset14 values={vals14}  (expect {{40}} -- self-describing length prefix)  "
              f"tail(55-62) distinct patterns={len(tail)} (expect 1, all-zero)")


# ---------------------------------------------------------------------------
# Per-byte stats, payload-relative
# ---------------------------------------------------------------------------

def per_byte_stats(rows):
    n = len(rows)
    stats = []
    for off in range(PAYLOAD_LEN):
        raw_off = PAYLOAD_START + off
        vals = [b[raw_off] for _, b in rows]
        cnt = Counter(vals)
        stats.append(dict(
            payload_off=off, raw_off=raw_off, unique=len(cnt), min=min(vals), max=max(vals),
            variance=statistics.pvariance(vals) if n > 1 else 0.0,
            entropy=entropy_bits(cnt, n),
            trans_rate=(sum(1 for i in range(1, n) if vals[i] != vals[i - 1]) / (n - 1)) if n > 1 else 0.0,
        ))
    return stats


def print_per_byte(label, stats):
    print(f"\n  --- per-byte (payload-relative), {label} ---")
    print(f"  {'p_off':>5} {'raw':>5} {'uniq':>5} {'min':>4} {'max':>4} {'var':>9} {'entropy':>8} {'trans%':>7}")
    for s in stats:
        print(f"  {s['payload_off']:>5} {s['raw_off']:#05x} {s['unique']:>5} {s['min']:>4} {s['max']:>4} "
              f"{s['variance']:>9.1f} {s['entropy']:>8.3f} {s['trans_rate']*100:>6.1f}%")


# ---------------------------------------------------------------------------
# Multi-width/alignment/endian/sign field scanner
# ---------------------------------------------------------------------------

def decode_field(b, raw_off, width, big_endian, signed):
    chunk = b[raw_off:raw_off + width]
    if len(chunk) < width:
        return None
    v = int.from_bytes(chunk, "big" if big_endian else "little", signed=signed)
    return v


def linreg_r2(xs, ys):
    n = len(xs)
    if n < 3:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return 0.0
    slope = sxy / sxx
    ss_res = sum((y - (my + slope * (x - mx))) ** 2 for x, y in zip(xs, ys))
    return 1 - ss_res / syy if syy > 0 else 0.0


def scan_fields(rows, sample_cap=1200):
    """Every (start, width, endian, signed) combination fully inside the 40-byte payload.
    Returns a list of candidate dicts with objective metrics: linear-fit R^2 against sample
    index (drift-likeness), monotonic-step fraction, range, entropy, and a rough
    classification (drift-like / stable / other) purely from those numbers."""
    rows = rows[:sample_cap] if len(rows) > sample_cap else rows
    idxs = list(range(len(rows)))
    candidates = []
    for width in (1, 2, 3, 4):
        endians = (False,) if width == 1 else (False, True)
        signs = (False, True)
        for start in range(0, PAYLOAD_LEN - width + 1):
            raw_off = PAYLOAD_START + start
            for big_endian in endians:
                for signed in signs:
                    if width == 1 and signed and False:
                        continue
                    vals = [decode_field(b, raw_off, width, big_endian, signed) for _, b in rows]
                    if any(v is None for v in vals):
                        continue
                    uniq = len(set(vals))
                    if uniq <= 1:
                        continue  # constant -- not interesting for this scan (already in per-byte stats)
                    r2 = linreg_r2(idxs, vals)
                    steps = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
                    nonzero_steps = [s for s in steps if s != 0]
                    mono_frac = (sum(1 for s in nonzero_steps if s > 0) / len(nonzero_steps)
                                 if nonzero_steps else 0.0)
                    mono_frac = max(mono_frac, 1 - mono_frac)  # symmetric: consistently +ve OR -ve
                    vrange = max(vals) - min(vals)
                    cnt = Counter(vals)
                    ent = entropy_bits(cnt, len(vals))
                    candidates.append(dict(
                        start=start, raw_off=raw_off, width=width,
                        endian="BE" if big_endian else "LE", signed=signed,
                        r2=r2, mono_frac=mono_frac, range=vrange, entropy=ent,
                        vmin=min(vals), vmax=max(vals), uniq=uniq,
                    ))
    return candidates


def classify(c):
    if c["r2"] > 0.85 and c["mono_frac"] > 0.9:
        return "DRIFT-LIKE (near-linear, consistently monotonic)"
    if c["r2"] < 0.15 and c["range"] < 32 and c["entropy"] < 3.0:
        return "STABLE (low range, low entropy -- near-constant)"
    if c["r2"] < 0.15 and c["range"] >= 32:
        return "BOUNDED/OSCILLATING (varies but no net drift)"
    return "OTHER"


def print_ranked_fields(label, candidates, top_n=20):
    print(f"\n  --- top drift-like candidates, {label} (ranked by R^2 * mono_frac) ---")
    ranked = sorted(candidates, key=lambda c: -(c["r2"] * c["mono_frac"]))
    print(f"  {'raw_off':>7} {'w':>2} {'end':>3} {'sgn':>4} {'R^2':>6} {'mono%':>6} "
          f"{'range':>8} {'entropy':>8} {'class':<45}")
    for c in ranked[:top_n]:
        print(f"  {c['raw_off']:#07x} {c['width']:>2} {c['endian']:>3} {str(c['signed']):>4} "
              f"{c['r2']:>6.3f} {c['mono_frac']*100:>5.1f}% {c['range']:>8d} {c['entropy']:>8.3f} "
              f"{classify(c):<45}")


def print_stable_fields(label, candidates, top_n=15):
    stable = [c for c in candidates if classify(c).startswith("STABLE")]
    stable.sort(key=lambda c: c["entropy"])
    print(f"\n  --- stable/near-constant candidates, {label} (excluding truly-constant bytes) ---")
    print(f"  {'raw_off':>7} {'w':>2} {'end':>3} {'sgn':>4} {'range':>8} {'entropy':>8} {'min':>7} {'max':>7}")
    for c in stable[:top_n]:
        print(f"  {c['raw_off']:#07x} {c['width']:>2} {c['endian']:>3} {str(c['signed']):>4} "
              f"{c['range']:>8d} {c['entropy']:>8.3f} {c['vmin']:>7d} {c['vmax']:>7d}")


# ---------------------------------------------------------------------------
# Derivative / linearity / piecewise / saturation / wraparound analysis
# ---------------------------------------------------------------------------

def analyze_derivative(rows, raw_off, width=2, big_endian=False, signed=True, label=""):
    vals = [decode_field(b, raw_off, width, big_endian, signed) for _, b in rows]
    if any(v is None for v in vals):
        print(f"    {label}: decode failed")
        return
    deltas = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    mean_d = statistics.mean(deltas) if deltas else 0
    stdev_d = statistics.pstdev(deltas) if len(deltas) > 1 else 0
    # crude piecewise-linearity check: split into 4 chunks, compare per-chunk mean delta
    chunk_size = max(1, len(deltas) // 4)
    chunk_means = [statistics.mean(deltas[i:i + chunk_size]) for i in range(0, len(deltas), chunk_size)
                   if deltas[i:i + chunk_size]]
    # wraparound check: for a signed N-bit field, a "wrap" looks like a huge single-step delta
    # (near the full range) surrounded by small steps.
    max_abs_step = max(abs(d) for d in deltas) if deltas else 0
    full_range = (1 << (8 * width))
    big_steps = [d for d in deltas if abs(d) > full_range * 0.4]
    print(f"    {label}: n={len(vals)} start={vals[0]} end={vals[-1]} "
          f"mean_step={mean_d:+.2f} stdev_step={stdev_d:.2f} "
          f"per-quarter mean_step={[round(m,2) for m in chunk_means]} "
          f"max|step|={max_abs_step} big-steps(>40%FS)={len(big_steps)} "
          f"saturated(min/max==field bound)={vals[0]==-(1<<(8*width-1)) or vals[-1]==(1<<(8*width-1))-1}")


# ---------------------------------------------------------------------------
# Cross-field correlation
# ---------------------------------------------------------------------------

def pearson(xs, ys):
    n = len(xs)
    if n < 3:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return 0.0
    return sxy / math.sqrt(sxx * syy)


def cross_field_correlation(rows, offsets, label=""):
    print(f"\n  --- cross-field correlation (int16 LE signed), {label} ---")
    series = {}
    for off in offsets:
        series[off] = [decode_field(b, off, 2, False, True) for _, b in rows]
    print("        " + "  ".join(f"0x{o:02x}" for o in offsets))
    for oa in offsets:
        row = []
        for ob in offsets:
            r = pearson(series[oa], series[ob])
            row.append(f"{r:+.2f}")
        print(f"  0x{oa:02x}  " + "  ".join(f"{v:>6}" for v in row))


# ---------------------------------------------------------------------------
# Repeated-lane / interleave check
# ---------------------------------------------------------------------------

def lane_check(rows, label=""):
    print(f"\n  --- repeated-lane / interleave check, {label} ---")
    for lanes in (2, 4, 5, 8):
        if PAYLOAD_LEN % lanes != 0:
            continue
        lane_len = PAYLOAD_LEN // lanes
        # Compare lane 0 vs lane 1 (etc) via correlation of their byte sequences over time
        us, b0 = rows[0]
        lane_bytes = [[b[PAYLOAD_START + l * lane_len: PAYLOAD_START + (l + 1) * lane_len]
                       for _, b in rows] for l in range(lanes)]
        # crude similarity: for each pair of lanes, fraction of byte positions with correlated
        # first-16-bit-LE-signed trend
        vals_per_lane = []
        for l in range(lanes):
            vals_per_lane.append([int.from_bytes(lb[:2], "little", signed=True) for lb in lane_bytes[l]])
        corrs = []
        for l in range(1, lanes):
            corrs.append(pearson(vals_per_lane[0], vals_per_lane[l]))
        print(f"    lanes={lanes} (each {lane_len}B): lane0-vs-others first-int16 correlation={['%.2f'%c for c in corrs]}")


# ---------------------------------------------------------------------------
# Timing relationship
# ---------------------------------------------------------------------------

def timing_relationship(rows, raw_off, label=""):
    print(f"\n  --- timing relationship, {label} (offset 0x{raw_off:02x}, int16 LE signed) ---")
    vals = [decode_field(b, raw_off, 2, False, True) for _, b in rows]
    ts = [us for us, _ in rows]
    dts = [(ts[i + 1] - ts[i]) for i in range(len(ts) - 1)]
    steps = [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]
    r_dt_step = pearson(dts, steps)
    idxs = list(range(len(vals)))
    r2_time = linreg_r2(ts, vals)
    r2_index = linreg_r2(idxs, vals)
    print(f"    corr(inter-sample dt, per-step delta) = {r_dt_step:+.3f}  "
          f"(near 0 => fixed per-notification increment, not blank scaled-by-elapsed-time)")
    print(f"    R^2(value vs wall-clock us)  = {r2_time:.4f}")
    print(f"    R^2(value vs notification index) = {r2_index:.4f}")


def main():
    print("Loading v2 variant captures, extracting 0x000E rows, trimming pre-activation prefix...")
    all_rows = {n: load_rows(VARIANT_FILES[n]) for n in range(1, 7)}
    all_active = {n: active_rows(all_rows[n]) for n in range(1, 7)}
    for n in range(1, 7):
        print(f"  V{n}: {len(all_rows[n])} total 0x000E records, {len(all_active[n])} active (post-activation)")

    check_framing(all_active)

    print(f"\n{'=' * 100}\nPER-BYTE STATS (payload-relative, all 6 variants pooled)\n{'=' * 100}")
    pooled = []
    for n in range(1, 7):
        pooled.extend(all_active[n])
    stats = per_byte_stats(pooled)
    print_per_byte("all variants pooled", stats)

    print(f"\n{'=' * 100}\nMULTI-WIDTH/ALIGNMENT/ENDIAN/SIGN FIELD SCAN\n{'=' * 100}")
    all_candidates = {}
    for n in range(1, 7):
        cands = scan_fields(all_active[n])
        all_candidates[n] = cands
        print_ranked_fields(f"V{n}", cands)
        print_stable_fields(f"V{n}", cands)

    print(f"\n{'=' * 100}\nDERIVATIVE / LINEARITY / PIECEWISE / SATURATION ANALYSIS\n{'=' * 100}")
    # Focus on the offsets the prior pass's manual spot-check flagged as drift-like: 19,22,25 (int16 LE signed)
    for n in range(1, 7):
        print(f"\n  V{n}:")
        for off in (19, 22, 25, 16):
            analyze_derivative(all_active[n], off, label=f"offset 0x{off:02x}")

    print(f"\n{'=' * 100}\nCROSS-FIELD CORRELATION\n{'=' * 100}")
    candidate_offsets = [16, 19, 22, 25, 32, 34, 36, 38, 40, 42, 45, 47, 49]
    for n in range(1, 7):
        cross_field_correlation(all_active[n], candidate_offsets, label=f"V{n}")

    print(f"\n{'=' * 100}\nREPEATED-LANE / INTERLEAVE CHECK\n{'=' * 100}")
    for n in range(1, 7):
        lane_check(all_active[n], label=f"V{n}")

    print(f"\n{'=' * 100}\nTIMING RELATIONSHIP\n{'=' * 100}")
    for n in range(1, 7):
        for off in (19, 25):
            timing_relationship(all_active[n], off, label=f"V{n}")

    print("\nDone. See the accompanying report for ranked-candidate interpretation and the controlled-motion experiment design.")


if __name__ == "__main__":
    main()
