#!/usr/bin/env python3
"""Analyze sw2_capture NDJSON logs (genuine Switch 2 BLE traffic captures).

Reproducible RE tool -- see docs/switch2/ble-controller-protocol-inventory.md.
Reads dumps/sw2_capture_*.ndjson, validates integrity, and produces byte-level
statistics (per-offset entropy/variance/range/transition-frequency) without
assuming any field semantics. Read-only: never modifies the source NDJSON.

Usage:
    python tools/analyze_sw2_capture.py [dumps_dir]
"""
import sys
import os
import json
import math
import glob
import statistics
from collections import Counter, defaultdict

DUMPS_DIR = sys.argv[1] if len(sys.argv) > 1 else "dumps"


def load_ndjson(path):
    """Parse one NDJSON file. Returns (records, parse_errors, blank_lines)."""
    records = []
    errors = []
    blanks = 0
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            s = line.strip()
            if not s:
                blanks += 1
                continue
            try:
                records.append(json.loads(s))
            except json.JSONDecodeError as e:
                errors.append((lineno, str(e), s[:80]))
    return records, errors, blanks


def entropy_bits(counter, total):
    if total == 0:
        return 0.0
    h = 0.0
    for c in counter.values():
        p = c / total
        h -= p * math.log2(p)
    return h


def analyze_file(path):
    name = os.path.basename(path)
    print(f"\n{'=' * 100}")
    print(f"FILE: {name}")
    print("=" * 100)

    records, errors, blanks = load_ndjson(path)
    print(f"  records parsed: {len(records)}   parse errors: {len(errors)}   blank lines: {blanks}")
    for lineno, msg, snippet in errors[:10]:
        print(f"    line {lineno}: {msg} :: {snippet!r}")

    if not records:
        print("  NO RECORDS -- nothing further to analyze.")
        return name, records

    # ---- schema sanity ----
    required = {"us", "kind", "handle", "len", "orig_len", "bytes"}
    missing_field_count = sum(1 for r in records if not required.issubset(r.keys()))
    print(f"  records missing an expected field: {missing_field_count}")

    # ---- kinds / handles / lengths ----
    kinds = Counter(r.get("kind") for r in records)
    handles = Counter(r.get("handle") for r in records)
    lens = Counter(r.get("len") for r in records)
    orig_lens = Counter(r.get("orig_len") for r in records)
    print(f"  kinds: {dict(kinds)}")
    print(f"  handles: {dict(handles)}")
    print(f"  len values: {dict(sorted(lens.items()))}")
    print(f"  orig_len values: {dict(sorted(orig_lens.items()))}")
    truncated = sum(1 for r in records if r.get("len") != r.get("orig_len"))
    print(f"  records where len != orig_len (truncated): {truncated}")

    # ---- timestamp monotonicity / cadence / duration ----
    ts = [r["us"] for r in records if "us" in r]
    non_monotonic = sum(1 for i in range(1, len(ts)) if ts[i] < ts[i - 1])
    deltas = [ts[i] - ts[i - 1] for i in range(1, len(ts))]
    if deltas:
        dur_s = (ts[-1] - ts[0]) / 1e6
        print(f"  timestamp range: {ts[0]} .. {ts[-1]}  (duration {dur_s:.2f} s)")
        print(f"  non-monotonic (ts[i] < ts[i-1]) steps: {non_monotonic}")
        print(f"  inter-record delta (us): min={min(deltas)} max={max(deltas)} "
              f"median={statistics.median(deltas):.1f} mean={statistics.mean(deltas):.1f}")
        implied_hz = 1e6 / statistics.median(deltas) if statistics.median(deltas) else 0
        print(f"  implied median notification rate: {implied_hz:.2f} Hz")
        # Histogram of delta buckets (rounded to nearest ms) to spot bimodal cadence
        bucket = Counter(round(d / 1000) for d in deltas)
        top_buckets = bucket.most_common(8)
        print(f"  top delta buckets (ms : count): {top_buckets}")

    # ---- duplicates ----
    exact_dupe_counter = Counter((r.get("us"), r.get("bytes")) for r in records)
    exact_dupes = sum(c - 1 for c in exact_dupe_counter.values() if c > 1)
    bytes_only_counter = Counter(r.get("bytes") for r in records if r.get("kind") == "input")
    repeated_payload_records = sum(c for c in bytes_only_counter.values() if c > 1)
    print(f"  exact duplicate (us+bytes) records: {exact_dupes}")
    print(f"  input records whose payload repeats verbatim at least once: {repeated_payload_records} "
          f"of {sum(bytes_only_counter.values())} input records "
          f"({len(bytes_only_counter)} distinct payloads)")

    # ---- non-input kinds: print them in full (should be few) ----
    non_input = [r for r in records if r.get("kind") != "input"]
    print(f"  non-input records: {len(non_input)}")
    for r in non_input[:40]:
        print(f"    us={r.get('us')} kind={r.get('kind')} handle={r.get('handle')} "
              f"len={r.get('len')} bytes={r.get('bytes')}")
    if len(non_input) > 40:
        print(f"    ... and {len(non_input) - 40} more non-input records")

    return name, records


def byte_matrix(records, kind="input", handle=None):
    """Return (list_of_byte_arrays, common_len) for records matching kind/handle."""
    rows = []
    for r in records:
        if r.get("kind") != kind:
            continue
        if handle is not None and r.get("handle") != handle:
            continue
        hexs = r.get("bytes", "")
        try:
            b = bytes.fromhex(hexs)
        except ValueError:
            continue
        rows.append(b)
    if not rows:
        return [], 0
    lens = Counter(len(b) for b in rows)
    common_len = lens.most_common(1)[0][0]
    rows = [b for b in rows if len(b) == common_len]
    return rows, common_len


def per_offset_stats(rows, common_len):
    """Per-offset: unique count, min, max, variance, entropy, transition frequency."""
    n = len(rows)
    stats = []
    for off in range(common_len):
        vals = [b[off] for b in rows]
        uniq = len(set(vals))
        vmin, vmax = min(vals), max(vals)
        var = statistics.pvariance(vals) if n > 1 else 0.0
        cnt = Counter(vals)
        ent = entropy_bits(cnt, n)
        transitions = sum(1 for i in range(1, n) if vals[i] != vals[i - 1])
        trans_rate = transitions / (n - 1) if n > 1 else 0.0
        stats.append({
            "offset": off, "unique": uniq, "min": vmin, "max": vmax,
            "variance": var, "entropy": ent, "trans_rate": trans_rate,
        })
    return stats


def print_offset_table(stats, label, top_n=64):
    print(f"\n  --- per-offset byte statistics: {label} ({len(stats)} offsets) ---")
    print(f"  {'off':>4} {'hex':>4} {'uniq':>5} {'min':>4} {'max':>4} {'var':>8} {'entropy':>8} {'trans%':>7}")
    for s in stats[:top_n]:
        print(f"  {s['offset']:>4} {s['offset']:#04x} {s['unique']:>5} {s['min']:>4} {s['max']:>4} "
              f"{s['variance']:>8.1f} {s['entropy']:>8.3f} {s['trans_rate'] * 100:>6.1f}%")


def compare_offsets(stats_a, stats_b, label_a, label_b, threshold=0.15):
    """Flag offsets whose mean byte value differs meaningfully between two datasets."""
    print(f"\n  --- offsets differing between {label_a} and {label_b} (mean-shift heuristic) ---")
    n = min(len(stats_a), len(stats_b))
    flagged = []
    for i in range(n):
        a, b = stats_a[i], stats_b[i]
        # Only compare offsets that vary at all in at least one dataset (skip pure-constant-both)
        if a["unique"] <= 1 and b["unique"] <= 1:
            if a["min"] != b["min"]:
                flagged.append((i, "constant-but-different", a["min"], b["min"]))
            continue
    for off, kind, va, vb in flagged:
        print(f"    offset {off:#04x} ({off:3d}): {kind}: {label_a}={va} {label_b}={vb}")
    if not flagged:
        print("    (none using the constant-value heuristic; see mean-based comparison below)")


def mean_by_offset(rows, common_len):
    n = len(rows)
    means = []
    for off in range(common_len):
        vals = [b[off] for b in rows]
        means.append(sum(vals) / n if n else 0.0)
    return means


def compare_means(means_a, means_b, label_a, label_b, min_diff=2.0):
    print(f"\n  --- offsets where mean byte value shifts by >= {min_diff} between {label_a} and {label_b} ---")
    n = min(len(means_a), len(means_b))
    any_flagged = False
    for i in range(n):
        d = means_b[i] - means_a[i]
        if abs(d) >= min_diff:
            any_flagged = True
            print(f"    offset {i:#04x} ({i:3d}): {label_a} mean={means_a[i]:7.2f}  "
                  f"{label_b} mean={means_b[i]:7.2f}  delta={d:+7.2f}")
    if not any_flagged:
        print("    (none)")


def main():
    files = sorted(glob.glob(os.path.join(DUMPS_DIR, "sw2_capture_*.ndjson")))
    if not files:
        print(f"No sw2_capture_*.ndjson files found in {DUMPS_DIR}")
        return

    all_data = {}
    for path in files:
        name, records = analyze_file(path)
        all_data[name] = records

    # ---- byte-level analysis: input records on handle 0x000A ----
    print(f"\n{'=' * 100}")
    print("BYTE-LEVEL ANALYSIS (kind=input, handle=0x000A)")
    print("=" * 100)

    per_file_stats = {}
    per_file_means = {}
    per_file_len = {}
    for name, records in all_data.items():
        rows, common_len = byte_matrix(records, kind="input", handle="0x000A")
        print(f"\n[{name}] input rows on 0x000A with common length {common_len}: {len(rows)}")
        if not rows:
            continue
        stats = per_offset_stats(rows, common_len)
        print_offset_table(stats, name, top_n=common_len)
        per_file_stats[name] = stats
        per_file_means[name] = mean_by_offset(rows, common_len)
        per_file_len[name] = common_len

    # ---- within-STILL_CAPTURE variation summary ----
    still_key = next((k for k in all_data if "STILL_CAPTURE" in k), None)
    if still_key and still_key in per_file_stats:
        print(f"\n{'=' * 100}")
        print(f"WITHIN-STATIONARY VARIATION SUMMARY ({still_key})")
        print("=" * 100)
        stats = per_file_stats[still_key]
        varying = [s for s in stats if s["unique"] > 1]
        constant = [s for s in stats if s["unique"] <= 1]
        print(f"  offsets that vary at all while stationary: {len(varying)} / {len(stats)}")
        print(f"  offsets that are perfectly constant while stationary: {len(constant)} / {len(stats)}")
        print("  varying offsets, sorted by entropy (highest first):")
        for s in sorted(varying, key=lambda s: -s["entropy"])[:30]:
            print(f"    offset {s['offset']:#04x} ({s['offset']:3d}): entropy={s['entropy']:.3f} "
                  f"unique={s['unique']} range=[{s['min']},{s['max']}] trans%={s['trans_rate'] * 100:.1f}")

    # ---- between-capture comparison: STILL vs each ANGLE ----
    print(f"\n{'=' * 100}")
    print("BETWEEN-CAPTURE COMPARISON (STILL_CAPTURE vs each ANGLE*)")
    print("=" * 100)
    if still_key:
        for name in sorted(all_data):
            if name == still_key or name not in per_file_means:
                continue
            if per_file_len.get(still_key) != per_file_len.get(name):
                print(f"\n[{still_key} vs {name}] SKIPPED -- different common record length "
                      f"({per_file_len.get(still_key)} vs {per_file_len.get(name)})")
                continue
            print(f"\n[{still_key} vs {name}]")
            compare_means(per_file_means[still_key], per_file_means[name], still_key, name)


if __name__ == "__main__":
    main()
