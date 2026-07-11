#!/usr/bin/env python3
"""Analyze the v2 experiment-matrix hardware captures (genuine Switch 2 BLE traffic).

Reproducible RE tool -- see docs/switch2/ble-controller-protocol-inventory.md §2.7/§3.7/§3.8.
Reads dumps/sw2_capture_2026-07-10_{BASE-NO-VARIANT,VARIANT-1..6,
NO-VARIANT-GATT-DISCOVERY}.ndjson.

Three passes, run in order (see main()):
  1. Integrity + full ordered timeline reconstruction for every file (kinds, handles, lengths,
     timestamp monotonicity/duration, variant marker, non-input event sequence with cmd/subcmd
     decoded).
  2. GATT-discovery-capture-only: reconstructs the live service -> characteristic -> descriptor
     hierarchy BTstack itself discovered, purely from the captured gatt_svc/gatt_char/gatt_desc
     entries (see sw2_capture.h for the exact byte layout each kind uses). This is the ground
     truth this repo's earlier paper-only handle-numbering analysis was explicitly built to be
     replaced by once available.
  3. Base-vs-variant byte-level comparison: per-offset stats for every (file, handle) input
     stream, then cross-file/cross-handle equality checks (including byte-shifted alignment,
     since 0x000A vs 0x000E turned out to be a shifted duplicate in the v1 result) to test
     whether any variant's stream contains data that is NOT explained by a counter, a shifted
     duplicate of another handle's payload, or simple constant-zero padding.

Read-only. Never modifies the source NDJSON. All numeric conclusions in the accompanying report
should trace back to a specific table this script prints -- re-run it to verify any claim.

Usage:
    python tools/analyze_sw2_v2_captures.py [dumps_dir]
"""
import sys
import os
import json
import math
import glob
import statistics
from collections import Counter, defaultdict

DUMPS_DIR = sys.argv[1] if len(sys.argv) > 1 else "dumps"

BASE_FILE = "sw2_capture_2026-07-10_BASE-NO-VARIANT.ndjson"
GATT_DISC_FILE = "sw2_capture_2026-07-10_NO-VARIANT-GATT-DISCOVERY.ndjson"
VARIANT_FILES = {n: f"sw2_capture_2026-07-10_VARIANT-{n}.ndjson" for n in range(1, 7)}

VARIANT_NAMES = {
    1: "control", 2: "mask_ff", 3: "handle_write_only", 4: "mask_ff_handle_write",
    5: "calibration_seq", 6: "full_sequence",
}
# Expected design per variant (must match SW2_V2_VARIANTS[] in btstack_host.c exactly).
VARIANT_DESIGN = {
    1: dict(configure_flags=0x07, enable_flags=0x07, cal=False, write=False, defer_ccc=False),
    2: dict(configure_flags=0xFF, enable_flags=0xFF, cal=False, write=False, defer_ccc=False),
    3: dict(configure_flags=0x07, enable_flags=0x07, cal=False, write=True,  defer_ccc=False),
    4: dict(configure_flags=0xFF, enable_flags=0xFF, cal=False, write=True,  defer_ccc=False),
    5: dict(configure_flags=0x07, enable_flags=0x07, cal=True,  write=False, defer_ccc=False),
    6: dict(configure_flags=0xFF, enable_flags=0x07, cal=True,  write=True,  defer_ccc=True),
}
CAL_READS = [(0x13080, 0x40), (0x130C0, 0x40), (0x1FC040, 0x40),
             (0x13040, 0x10), (0x13100, 0x18), (0x1FA000, 0x40)]
REPORT_RATE_HANDLE_HYPOTHESIS = "0x000C"
MOTION_CCC_HANDLE = "0x000F"
CMD_HANDLE = "0x0014"


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_records(path):
    records, errors = [], []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            s = line.strip()
            if not s:
                continue
            try:
                r = json.loads(s)
                r["_bytes"] = bytes.fromhex(r.get("bytes", "")) if r.get("bytes") else b""
                records.append(r)
            except (json.JSONDecodeError, ValueError) as e:
                errors.append((lineno, str(e)))
    return records, errors


def entropy_bits(counter, total):
    if total == 0:
        return 0.0
    h = 0.0
    for c in counter.values():
        p = c / total
        h -= p * math.log2(p)
    return h


# ---------------------------------------------------------------------------
# Pass 1: integrity + ordered timeline
# ---------------------------------------------------------------------------

def cmd_subcmd(rec):
    b = rec["_bytes"]
    if len(b) >= 4:
        return b[0], b[3]
    return None, None


def describe_event(rec, t0):
    kind = rec["kind"]
    handle = rec.get("handle")
    b = rec["_bytes"]
    dt_ms = (rec["us"] - t0) / 1000.0
    if kind == "cmd_out":
        cmd, sub = cmd_subcmd(rec)
        tag = f"cmd=0x{cmd:02X} subcmd=0x{sub:02X}" if cmd is not None else "?"
        return f"t={dt_ms:9.2f}ms cmd_out  handle={handle} len={rec['len']:3d} {tag} bytes={rec['bytes']}"
    if kind == "ack":
        cmd, sub = cmd_subcmd(rec)
        tag = f"cmd=0x{cmd:02X} subcmd=0x{sub:02X}" if cmd is not None else "?"
        return f"t={dt_ms:9.2f}ms ack      handle={handle} len={rec['len']:3d} {tag} bytes={rec['bytes'][:40]}"
    if kind == "ccc_write":
        return f"t={dt_ms:9.2f}ms ccc_write handle={handle} bytes={rec['bytes']}"
    if kind == "state":
        return f"t={dt_ms:9.2f}ms state    value={b[0] if b else '?'}"
    if kind == "variant":
        return f"t={dt_ms:9.2f}ms VARIANT  id={b[0] if b else '?'}"
    if kind in ("gatt_svc", "gatt_char", "gatt_desc"):
        return f"t={dt_ms:9.2f}ms {kind} handle={handle} bytes={rec['bytes']}"
    return f"t={dt_ms:9.2f}ms {kind} handle={handle} len={rec.get('len')}"


def analyze_integrity(path, label):
    print(f"\n{'=' * 100}\nFILE: {label}  ({os.path.basename(path)})\n{'=' * 100}")
    records, errors = load_records(path)
    print(f"  records parsed: {len(records)}   parse errors: {len(errors)}")
    for lineno, msg in errors[:10]:
        print(f"    line {lineno}: {msg}")
    if not records:
        return records

    kinds = Counter(r["kind"] for r in records)
    handles = Counter(r.get("handle") for r in records)
    lens = Counter(r.get("len") for r in records)
    print(f"  kinds: {dict(kinds)}")
    print(f"  handles: {dict(handles)}")
    print(f"  len values: {dict(sorted((k, v) for k, v in lens.items() if k is not None))}")

    ts = [r["us"] for r in records]
    non_mono = sum(1 for i in range(1, len(ts)) if ts[i] < ts[i - 1])
    dur_s = (ts[-1] - ts[0]) / 1e6
    print(f"  timestamp range: {ts[0]} .. {ts[-1]}  (duration {dur_s:.2f}s)  non-monotonic steps: {non_mono}")

    # Reconnect boundaries: a large gap (>2s) between consecutive records, OR a repeated
    # ccc_write to the same handle (0x001B is written once per connection attempt at the very
    # start of init) both indicate a new connection attempt started mid-file.
    gaps = [(ts[i] - ts[i - 1]) for i in range(1, len(ts))]
    big_gaps = [(i, g) for i, g in enumerate(gaps) if g > 2_000_000]
    if big_gaps:
        print(f"  gaps > 2s (possible reconnect boundaries): {len(big_gaps)}")
        for i, g in big_gaps[:10]:
            print(f"    after record {i}: gap={g/1e6:.2f}s  "
                  f"(record {i}: {records[i]['kind']}@{records[i].get('handle')}  "
                  f"record {i+1}: {records[i+1]['kind']}@{records[i+1].get('handle')})")
    ack_ccc = [i for i, r in enumerate(records) if r["kind"] == "ccc_write" and r.get("handle") == "0x001B"]
    print(f"  ccc_write to 0x001B (ACK-notify enable -- one per connection attempt): {len(ack_ccc)} "
          f"at record indices {ack_ccc}")

    variant_recs = [r for r in records if r["kind"] == "variant"]
    if variant_recs:
        ids = [r["_bytes"][0] if r["_bytes"] else None for r in variant_recs]
        print(f"  VARIANT markers found: {len(variant_recs)}  ids={ids}")
    else:
        print(f"  VARIANT markers found: 0")

    dropped_fields = [r for r in records if "dropped" in r]
    print(f"  records carrying a 'dropped' field: {len(dropped_fields)} "
          f"(dropped-ring-overrun count is a live sw2cap-stat/-drain field, not stored per capture "
          f"entry -- not recoverable retroactively from this file; must be read from the session's "
          f"own log/UI at capture time)")

    non_input = [r for r in records if r["kind"] != "input"]
    print(f"\n  --- full non-input ordered timeline ({len(non_input)} events) ---")
    t0 = records[0]["us"]
    for r in non_input:
        print("   ", describe_event(r, t0))

    return records


# ---------------------------------------------------------------------------
# Pass 2: GATT discovery hierarchy reconstruction
# ---------------------------------------------------------------------------

def u16le(b, off):
    return b[off] | (b[off + 1] << 8)


def analyze_gatt_discovery(records):
    print(f"\n{'=' * 100}\nGATT DISCOVERY HIERARCHY (ground truth from BTstack's own live discovery)\n{'=' * 100}")

    svcs = [r for r in records if r["kind"] == "gatt_svc"]
    chars = [r for r in records if r["kind"] == "gatt_char"]
    descs = [r for r in records if r["kind"] == "gatt_desc"]
    print(f"  {len(svcs)} services, {len(chars)} characteristics, {len(descs)} descriptors discovered")

    svc_list = []
    for r in svcs:
        b = r["_bytes"]
        start = int(r["handle"], 16)
        end = u16le(b, 0)
        uuid16 = u16le(b, 2)
        uuid128 = b[4:20].hex()
        svc_list.append((start, end, uuid16, uuid128))
    svc_list.sort()
    print("\n  Services:")
    for start, end, uuid16, uuid128 in svc_list:
        u = f"0x{uuid16:04X}" if uuid16 else f"128-bit {uuid128}"
        print(f"    [0x{start:04X}..0x{end:04X}]  uuid={u}")

    char_list = []
    for r in chars:
        b = r["_bytes"]
        value_h = int(r["handle"], 16)
        decl_h = u16le(b, 0)
        end_h = u16le(b, 2)
        props = u16le(b, 4)
        uuid16 = u16le(b, 6)
        uuid128 = b[8:24].hex()
        char_list.append(dict(decl=decl_h, value=value_h, end=end_h, props=props,
                               uuid16=uuid16, uuid128=uuid128, descs=[]))
    char_list.sort(key=lambda c: c["value"])

    # Associate each descriptor with the characteristic whose [value, end] range contains it.
    # (end_handle is the characteristic's own extent, i.e. up through the last of its own
    # descriptors / right before the next characteristic's declaration -- exactly what's needed
    # to place a descriptor unambiguously without relying on capture order.)
    unassigned = []
    for r in descs:
        dh = int(r["handle"], 16)
        b = r["_bytes"]
        uuid16 = u16le(b, 0)
        uuid128 = b[2:18].hex()
        owner = None
        for c in char_list:
            if c["value"] < dh <= c["end"]:
                owner = c
                break
        if owner:
            owner["descs"].append((dh, uuid16, uuid128))
        else:
            unassigned.append((dh, uuid16, uuid128))

    print("\n  Characteristics (+ their descriptors):")
    target = int(REPORT_RATE_HANDLE_HYPOTHESIS, 16)
    resolved = None
    for c in char_list:
        props_str = describe_properties(c["props"])
        u = f"0x{c['uuid16']:04X}" if c["uuid16"] else f"128-bit {c['uuid128']}"
        print(f"    decl=0x{c['decl']:04X}  value=0x{c['value']:04X}  end=0x{c['end']:04X}  "
              f"props=0x{c['props']:02X} ({props_str})  uuid={u}")
        for dh, duuid16, duuid128 in sorted(c["descs"]):
            du = f"0x{duuid16:04X}" if duuid16 else f"128-bit {duuid128}"
            marker = "  <=== SW2_REPORT_RATE_HANDLE_HYPOTHESIS TARGET" if dh == target else ""
            print(f"        desc=0x{dh:04X}  uuid={du}{marker}")
            if dh == target:
                resolved = ("descriptor of char value=0x%04X (decl=0x%04X)" % (c["value"], c["decl"]), du)
        if c["value"] == target or c["decl"] == target:
            marker_kind = "value handle" if c["value"] == target else "declaration handle"
            resolved = (f"IS ITSELF a characteristic {marker_kind}", u)

    if unassigned:
        print("\n  Descriptors NOT falling inside any discovered characteristic's [value, end] range:")
        for dh, duuid16, duuid128 in sorted(unassigned):
            du = f"0x{duuid16:04X}" if duuid16 else f"128-bit {duuid128}"
            marker = "  <=== SW2_REPORT_RATE_HANDLE_HYPOTHESIS TARGET" if dh == target else ""
            print(f"    desc=0x{dh:04X}  uuid={du}{marker}")
            if dh == target:
                resolved = ("unassigned descriptor (outside every discovered characteristic's range)", du)

    print(f"\n  --- Resolution for {REPORT_RATE_HANDLE_HYPOTHESIS} ---")
    if resolved:
        print(f"    {REPORT_RATE_HANDLE_HYPOTHESIS} is: {resolved[0]}, uuid={resolved[1]}")
    else:
        print(f"    {REPORT_RATE_HANDLE_HYPOTHESIS} does not appear anywhere in the discovered table at all "
              f"(neither as a characteristic decl/value handle nor as any characteristic's descriptor) "
              f"-- the write in variants 3/4/6 would target a handle this device's live GATT server "
              f"never actually allocated.")

    print(f"\n  --- Cross-check: this repo's other confirmed/assumed handles ---")
    for label, hx in [("0x000A (input, Common)", 0x000A), ("0x000E (input, Pro/GCN)", 0x000E),
                       ("0x0014 (cmd)", 0x0014), ("0x0016 (cmd, secondary)", 0x0016),
                       ("0x001A (ack)", 0x001A), ("0x001E (ack, secondary)", 0x001E)]:
        found = None
        for c in char_list:
            if c["value"] == hx:
                found = f"characteristic value (decl=0x{c['decl']:04X}, end=0x{c['end']:04X})"
            for dh, duuid16, duuid128 in c["descs"]:
                if dh == hx:
                    found = f"descriptor of char value=0x{c['value']:04X}"
        for dh, duuid16, duuid128 in unassigned:
            if dh == hx:
                found = "unassigned descriptor"
        print(f"    {label}: {'FOUND -- ' + found if found else 'not present in this discovery capture'}")

    return char_list, unassigned


def describe_properties(props):
    # BTstack ATT_PROPERTY_* bit values (see gatt_client.h / bluetooth.h)
    bits = []
    if props & 0x02: bits.append("READ")
    if props & 0x08: bits.append("WRITE")
    if props & 0x04: bits.append("WRITE_NO_RESPONSE")
    if props & 0x10: bits.append("NOTIFY")
    if props & 0x20: bits.append("INDICATE")
    if props & 0x01: bits.append("BROADCAST")
    if props & 0x40: bits.append("AUTH_SIGNED_WRITE")
    if props & 0x80: bits.append("EXTENDED")
    return "|".join(bits) if bits else "none"


# ---------------------------------------------------------------------------
# Pass 3: byte-level analysis + cross-file/cross-handle comparison
# ---------------------------------------------------------------------------

def byte_matrix(records, handle):
    rows = []
    for r in records:
        if r["kind"] != "input" or r.get("handle") != handle:
            continue
        if r["_bytes"]:
            rows.append(r["_bytes"])
    if not rows:
        return [], 0
    lens = Counter(len(b) for b in rows)
    common_len = lens.most_common(1)[0][0]
    return [b for b in rows if len(b) == common_len], common_len


def per_offset_stats(rows, common_len):
    n = len(rows)
    stats = []
    for off in range(common_len):
        vals = [b[off] for b in rows]
        uniq = len(set(vals))
        cnt = Counter(vals)
        stats.append({
            "offset": off, "unique": uniq, "min": min(vals), "max": max(vals),
            "variance": statistics.pvariance(vals) if n > 1 else 0.0,
            "entropy": entropy_bits(cnt, n),
            "trans_rate": (sum(1 for i in range(1, n) if vals[i] != vals[i - 1]) / (n - 1)) if n > 1 else 0.0,
        })
    return stats


def summarize_stream(label, records, handle):
    rows, common_len = byte_matrix(records, handle)
    if not rows:
        return None
    stats = per_offset_stats(rows, common_len)
    varying = [s for s in stats if s["unique"] > 1]
    print(f"\n  [{label}] handle={handle}: {len(rows)} records, common_len={common_len}, "
          f"{len(varying)}/{common_len} offsets vary")
    if varying:
        print(f"    {'off':>4} {'uniq':>5} {'min':>4} {'max':>4} {'var':>9} {'entropy':>8} {'trans%':>7}")
        for s in varying:
            print(f"    {s['offset']:>4} {s['unique']:>5} {s['min']:>4} {s['max']:>4} "
                  f"{s['variance']:>9.2f} {s['entropy']:>8.3f} {s['trans_rate']*100:>6.1f}%")
    return dict(rows=rows, common_len=common_len, stats=stats, varying=[s["offset"] for s in varying])


def shifted_equal(rows_a, rows_b, max_shift=8, sample=200):
    """Check whether rows_a (at some shift) is byte-identical to rows_b, sampled -- returns the
    best (shift, match_fraction) pair. Used to test "is this handle's payload just a
    shifted duplicate of another handle's payload" without assuming any particular offset."""
    na, nb = min(len(rows_a), sample), min(len(rows_b), sample)
    if na == 0 or nb == 0:
        return None
    best = (None, 0.0)
    for shift in range(-max_shift, max_shift + 1):
        matches = 0
        checked = 0
        for i in range(min(na, nb)):
            a, b = rows_a[i], rows_b[i]
            if shift >= 0:
                av, bv = a[shift:], b[:len(a) - shift] if shift else b
            else:
                av, bv = a[:len(a) + shift], b[-shift:]
            n = min(len(av), len(bv))
            if n <= 0:
                continue
            checked += 1
            if av[:n] == bv[:n]:
                matches += 1
        frac = matches / checked if checked else 0.0
        if frac > best[1]:
            best = (shift, frac)
    return best


def main():
    print("Loading and validating all capture files (pass 1: integrity + timeline)...")
    all_records = {}
    all_records["BASE"] = analyze_integrity(os.path.join(DUMPS_DIR, BASE_FILE), "BASE-NO-VARIANT")
    all_records["GATT_DISC"] = analyze_integrity(os.path.join(DUMPS_DIR, GATT_DISC_FILE), "NO-VARIANT-GATT-DISCOVERY")
    for n in range(1, 7):
        all_records[n] = analyze_integrity(os.path.join(DUMPS_DIR, VARIANT_FILES[n]), f"VARIANT-{n} ({VARIANT_NAMES[n]})")

    print("\n\n" + "#" * 100)
    print("# PASS 1B: variant design verification -- does each file's cmd_out sequence match its design?")
    print("#" * 100)
    for n in range(1, 7):
        verify_variant_design(all_records[n], n)

    print("\n\n" + "#" * 100)
    print("# PASS 2: GATT discovery hierarchy")
    print("#" * 100)
    char_list, unassigned = analyze_gatt_discovery(all_records["GATT_DISC"])

    print("\n\n" + "#" * 100)
    print("# PASS 3: byte-level analysis per (file, handle)")
    print("#" * 100)
    summaries = {}
    summaries["BASE_0x000A"] = summarize_stream("BASE", all_records["BASE"], "0x000A")
    for n in range(1, 7):
        summaries[f"V{n}_0x000A"] = summarize_stream(f"VARIANT-{n}", all_records[n], "0x000A")
        summaries[f"V{n}_0x000E"] = summarize_stream(f"VARIANT-{n}", all_records[n], "0x000E")

    print("\n\n" + "#" * 100)
    print("# PASS 3B: cross-handle / cross-variant equality checks (shifted-duplicate test)")
    print("#" * 100)
    for n in range(1, 7):
        a = summaries.get(f"V{n}_0x000A")
        e = summaries.get(f"V{n}_0x000E")
        if a and e:
            best = shifted_equal(a["rows"], e["rows"])
            print(f"  VARIANT-{n}: 0x000A vs 0x000E best alignment: shift={best[0]}, "
                  f"match_fraction={best[1]:.3f} (1.0 = byte-identical at that shift over the sample)")
        elif e and not a:
            print(f"  VARIANT-{n}: only 0x000E present (no 0x000A input records in this file) -- "
                  f"comparing against VARIANT-1's 0x000A instead")
            a1 = summaries.get("V1_0x000A")
            if a1:
                best = shifted_equal(a1["rows"], e["rows"])
                print(f"    0x000A(variant 1) vs 0x000E(variant {n}) best alignment: shift={best[0]}, "
                      f"match_fraction={best[1]:.3f}")

    print("\n  --- Cross-variant 0x000E comparison (is variant N's 0x000E stream different from variant 1's?) ---")
    v1e = summaries.get("V1_0x000E")
    for n in range(2, 7):
        vne = summaries.get(f"V{n}_0x000E")
        if v1e and vne:
            v1_varying = set(v1e["varying"])
            vn_varying = set(vne["varying"])
            new_offsets = vn_varying - v1_varying
            print(f"  VARIANT-{n} vs VARIANT-1: varying offsets V1={sorted(v1_varying)} "
                  f"V{n}={sorted(vn_varying)}  NEW-IN-V{n}={sorted(new_offsets)}")

    print("\nDone. See the accompanying experiment report for the causal table and conclusions.")


def verify_variant_design(records, n):
    design = VARIANT_DESIGN[n]
    cmd_outs = [r for r in records if r["kind"] == "cmd_out"]
    ccc_writes = [r for r in records if r["kind"] == "ccc_write" and r.get("handle") == MOTION_CCC_HANDLE]
    variant_marker = [r for r in records if r["kind"] == "variant"]

    print(f"\n  --- VARIANT-{n} ({VARIANT_NAMES[n]}) design verification ---")
    if not variant_marker:
        print(f"    MISSING variant marker entirely!")
    else:
        vid = variant_marker[0]["_bytes"][0] if variant_marker[0]["_bytes"] else None
        status = "OK" if vid == n else f"MISMATCH (expected {n}, got {vid})"
        print(f"    variant marker: id={vid}  [{status}]")

    # Find the configure/enable commands (cmd=0x0C on CMD_HANDLE) among cmd_out entries.
    fam_0c = [r for r in cmd_outs if r.get("handle") == CMD_HANDLE and len(r["_bytes"]) >= 9 and r["_bytes"][0] == 0x0c]
    configures = [r for r in fam_0c if r["_bytes"][3] == 0x02]
    enables = [r for r in fam_0c if r["_bytes"][3] == 0x04]
    print(f"    configure(0x0C/0x02) commands sent: {len(configures)}", end="")
    if configures:
        flags = configures[0]["_bytes"][8]
        ok = "OK" if flags == design["configure_flags"] else f"MISMATCH (expected 0x{design['configure_flags']:02X})"
        print(f"  flags=0x{flags:02X} [{ok}]")
    else:
        print("  -- MISSING")
    print(f"    enable(0x0C/0x04) commands sent: {len(enables)}", end="")
    if enables:
        flags = enables[0]["_bytes"][8]
        ok = "OK" if flags == design["enable_flags"] else f"MISMATCH (expected 0x{design['enable_flags']:02X})"
        print(f"  flags=0x{flags:02X} [{ok}]")
    else:
        print("  -- MISSING")

    # Calibration reads: cmd=0x02, subcmd=0x04, addresses/sizes should match CAL_READS in order.
    # NOTE: the *normal* init sequence's READ_INFO step uses this exact same cmd/subcmd (address
    # 0x13000, size 0x40) once per connection, before SW2_INIT_DONE -- exclude it explicitly by
    # address rather than by timestamp, since address is the more direct/robust discriminator
    # (READ_INFO's address, 0x13000, does not appear anywhere in CAL_READS).
    cal_cmds = [r for r in cmd_outs if r.get("handle") == CMD_HANDLE and len(r["_bytes"]) >= 16
                and r["_bytes"][0] == 0x02 and r["_bytes"][3] == 0x04
                and not (r["_bytes"][12] | (r["_bytes"][13] << 8) | (r["_bytes"][14] << 16)
                         | (r["_bytes"][15] << 24)) == 0x13000]
    if design["cal"]:
        print(f"    calibration reads: expected {len(CAL_READS)}, found {len(cal_cmds)}")
        for i, r in enumerate(cal_cmds):
            b = r["_bytes"]
            size = b[8]
            addr = b[12] | (b[13] << 8) | (b[14] << 16) | (b[15] << 24)
            if i < len(CAL_READS):
                exp_addr, exp_size = CAL_READS[i]
                ok = "OK" if (addr, size) == (exp_addr, exp_size) else "MISMATCH"
            else:
                ok = "UNEXPECTED EXTRA"
            print(f"      #{i+1}: addr=0x{addr:X} size=0x{size:X} [{ok}]")
    else:
        print(f"    calibration reads: expected 0, found {len(cal_cmds)}"
              + ("  MISMATCH" if cal_cmds else "  OK"))

    # Handle write: a cmd_out to REPORT_RATE_HANDLE_HYPOTHESIS.
    hw = [r for r in cmd_outs if r.get("handle") == REPORT_RATE_HANDLE_HYPOTHESIS]
    if design["write"]:
        print(f"    handle-write (to {REPORT_RATE_HANDLE_HYPOTHESIS}): expected 1, found {len(hw)}", end="")
        if hw:
            print(f"  bytes={hw[0]['bytes']}")
        else:
            print()
    else:
        print(f"    handle-write: expected 0, found {len(hw)}" + ("  MISMATCH" if hw else "  OK"))

    print(f"    ccc_write to {MOTION_CCC_HANDLE} (0x000E subscribe): {len(ccc_writes)} occurrence(s)")
    if ccc_writes and configures:
        # "Deferred" means the CCC write is the LAST v2 operation (after configure, all cal
        # reads, enable, AND the handle write) -- not merely "after configure," which variants
        # 1-5 also technically satisfy trivially if compared only to configure. Compare against
        # every other v2 operation this variant sent, not just configure, to state it correctly.
        other_ops = [x["us"] for x in (configures + cal_cmds + enables + hw)]
        is_last = ccc_writes[0]["us"] > max(other_ops) if other_ops else False
        observed = "LAST (after every other v2 operation)" if is_last else "NOT last"
        expected = "LAST (deferred)" if design["defer_ccc"] else "FIRST (before configure)"
        match = "OK" if (is_last == design["defer_ccc"]) else "MISMATCH"
        print(f"      observed: CCC write is {observed}  |  design expects: {expected}  [{match}]")

    # Notification/ACK results for each: how many ack entries followed, and any error statuses
    # (ATT status is the whole ack payload's semantics per this repo's existing convention --
    # print raw bytes here rather than assume a fixed status-byte offset, since that's exactly
    # the kind of unverified-format assumption this task warns against).
    acks = [r for r in records if r["kind"] == "ack"]
    print(f"    total ack entries in file: {len(acks)}")


if __name__ == "__main__":
    main()
