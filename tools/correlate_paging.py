#!/usr/bin/env python3
"""Answer, for every failed Classic page, which of A/B/C/D actually happened.

    python tools/correlate_paging.py <run-folder>

Joins two independent records of the same events:

  harness  evidence/timelines.jsonl  -- Android's view: when the connection was
           requested, when PAGE_TIMEOUT arrived, how the cycle was classified
  firmware 96-btlife.jsonl           -- the adapter's view: page scan state,
           inquiry rounds, page_rx, page_accept/reject, acl_up/acl_fail

CLOCK ALIGNMENT. btlife timestamps are milliseconds since the adapter booted;
logcat is wall clock. They are aligned on the one event both sides record
independently for every successful cycle -- the adapter's `acl_up` and Android's
`BTA_DM_LINK_UP_EVT`. With ~90 matched pairs in a 100-cycle run the offset is
over-determined, and the residual spread is reported so a bad alignment cannot
be mistaken for a finding.

WHAT THIS CAN AND CANNOT DECIDE. Page scan is a baseband function: the
controller answers a page and only then raises HCI_Connection_Request, which is
what `page_rx` records. So a host can separate

  C  page_rx present, no acl_up      -> answered, then establishment failed
  D  page_reject present             -> this adapter refused it on purpose
  A' page scan disabled              -> we were not listening

but it CANNOT separate "no page arrived" from "a page arrived and the controller
did not answer it". Both are simply the absence of page_rx, and both are
reported here as A_OR_B. Splitting those needs an air sniffer or controller-level
tracing, not HCI.
"""

from __future__ import annotations

import json
import pathlib
import re
import statistics
import sys

# Verdicts, kept deliberately blunt.
A_SCAN_OFF = "A_PAGE_SCAN_DISABLED"
A_OR_B = "A_OR_B_NO_PAGE_RX"
C_AFTER_RESPONSE = "C_RESPONDED_THEN_FAILED"
D_REFUSED = "D_REFUSED_BY_ADMISSION"
NO_DATA = "NO_BTLIFE_COVERAGE"


def load_timelines(run: pathlib.Path) -> list[dict]:
    path = run / "evidence" / "timelines.jsonl"
    if not path.exists():
        sys.exit(f"missing {path}; run the soak with --evidence-dir")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def load_btlife(run: pathlib.Path) -> list[dict]:
    """Read the drained ring. Accepts the raw `btlife dump` JSON lines."""
    path = run / "96-btlife.jsonl"
    if not path.exists():
        sys.exit(f"missing {path}; drain the ring with `btlife dump N` after the run")
    events: list[dict] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.search(r"(\{.*\})", line)
        if not match:
            continue
        try:
            blob = json.loads(match.group(1))
        except json.JSONDecodeError:
            continue
        for row in blob.get("events", []):
            t_ms, code, a, handle, radio, addr = row
            events.append({"t_ms": t_ms, "code": code, "a": a,
                           "handle": handle, "radio": radio, "addr": addr})
    # The ring may be drained in overlapping pages; keep one of each.
    seen, unique = set(), []
    for e in events:
        key = (e["t_ms"], e["code"], e["a"], e["handle"], e["addr"])
        if key in seen:
            continue
        seen.add(key)
        unique.append(e)
    unique.sort(key=lambda e: e["t_ms"])
    return unique


def wall_seconds(stamp: str | None) -> float | None:
    if not stamp:
        return None
    match = re.match(r"\d{2}-\d{2} (\d{2}):(\d{2}):(\d{2}\.\d+)", stamp)
    if not match:
        return None
    return (int(match.group(1)) * 3600 + int(match.group(2)) * 60
            + float(match.group(3)))


def align(timelines: list[dict], btlife: list[dict]) -> tuple[float, float, int]:
    """Offset such that wall = t_ms/1000 + offset, from acl_up <-> ACL up.

    Matched in order: both sides see the same successful establishments in the
    same sequence, so the k-th of each is the same event.
    """
    android = [wall_seconds(row["at"])
               for r in timelines if r["result"] == "ok"
               for row in r["timeline"] if row["event"] == "classic.acl_up"]
    android = [a for a in android if a is not None]
    adapter = [e["t_ms"] / 1000.0 for e in btlife if e["code"] == "acl_up"]
    pairs = min(len(android), len(adapter))
    if pairs < 5:
        sys.exit(f"only {pairs} acl_up pairs; cannot align clocks confidently")
    # Trailing alignment: the ring may have evicted the earliest cycles, so the
    # LAST n of each series are the ones that certainly correspond.
    offsets = [a - b for a, b in zip(android[-pairs:], adapter[-pairs:])]
    return statistics.median(offsets), statistics.pstdev(offsets), pairs


def classify_failure(events: list[dict]) -> tuple[str, dict]:
    codes = [e["code"] for e in events]
    detail = {"btlife_events": codes}
    if not events:
        return NO_DATA, detail
    if "page_reject" in codes:
        detail["reject_cause"] = next(
            e["a"] for e in events if e["code"] == "page_reject")
        return D_REFUSED, detail
    if "page_rx" in codes:
        detail["accepted"] = "page_accept" in codes
        detail["acl_fail_reason"] = next(
            (e["a"] for e in events if e["code"] == "acl_fail"), None)
        return C_AFTER_RESPONSE, detail
    # No page reached the host. Was the adapter even listening?
    scan_state = None
    for e in events:
        if e["code"] in ("page_scan_on", "page_scan_off"):
            scan_state = e["code"]
    detail["page_scan_state_in_window"] = scan_state
    detail["inquiry_overlap"] = sum(1 for e in events
                                    if e["code"] == "inquiry_start")
    detail["radio_at_window"] = [e["radio"] for e in events][:4]
    if scan_state == "page_scan_off":
        return A_SCAN_OFF, detail
    return A_OR_B, detail


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    run = pathlib.Path(sys.argv[1])
    timelines = load_timelines(run)
    btlife = load_btlife(run)
    offset, spread, pairs = align(timelines, btlife)

    print(f"clock alignment: {pairs} acl_up pairs, offset {offset:.3f}s, "
          f"residual sd {spread:.3f}s")
    if spread > 0.5:
        print("  WARNING: residual spread is large; treat verdicts as suspect")
    print(f"btlife events: {len(btlife)}   cycles: {len(timelines)}\n")

    verdicts: dict[str, int] = {}
    print("cycle  result                          verdict")
    for r in timelines:
        if r["result"] == "ok":
            continue
        opened = next((wall_seconds(row["at"]) for row in r["timeline"]
                       if row["event"] == "app.touch_opened"), None)
        if opened is None:
            verdicts[NO_DATA] = verdicts.get(NO_DATA, 0) + 1
            continue
        # The attempt window: from the connection request to a little past the
        # 5.12 s Android page timeout.
        lo, hi = opened - 1.0, opened + 8.0
        window = [e for e in btlife if lo <= e["t_ms"] / 1000.0 + offset <= hi]
        verdict, detail = classify_failure(window)
        verdicts[verdict] = verdicts.get(verdict, 0) + 1
        print(f"{r['cycle']:5}  {r['result']:<30}  {verdict}")
        print(f"       {json.dumps(detail)}")

    print("\n=== failure classes ===")
    for name in sorted(verdicts):
        print(f"  {name:<28} {verdicts[name]}")

    # Inquiry overlap across ALL attempts, so the occupancy hypothesis is tested
    # rather than illustrated with the failures alone.
    print("\n=== inquiry overlap, successes vs failures ===")
    for label, want_ok in (("success", True), ("failure", False)):
        overlaps, total = 0, 0
        for r in timelines:
            if (r["result"] == "ok") != want_ok:
                continue
            opened = next((wall_seconds(row["at"]) for row in r["timeline"]
                           if row["event"] == "app.touch_opened"), None)
            if opened is None:
                continue
            total += 1
            window = [e for e in btlife
                      if opened - 1.0 <= e["t_ms"] / 1000.0 + offset <= opened + 8.0]
            if any(e["code"] == "inquiry_start" for e in window) or \
               any("i" in e["radio"] for e in window):
                overlaps += 1
        if total:
            print(f"  {label:<8} inquiry active during attempt: "
                  f"{overlaps}/{total} ({100.0*overlaps/total:.0f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
