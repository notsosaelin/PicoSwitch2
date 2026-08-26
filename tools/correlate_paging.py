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
# The phone never transmitted a page, so the adapter is not implicated and the
# absence of page_rx is expected rather than evidence. Confirmed 2026-08-23,
# cycle 3: OnConnectFail reason:CONNECTION_ALREADY_EXISTS(0x0b).
PHONE_NEVER_PAGED = "PHONE_NEVER_PAGED"
A_SCAN_OFF = "A_PAGE_SCAN_DISABLED"
A_OR_B = "A_OR_B_NO_PAGE_RX"
C_AFTER_RESPONSE = "C_RESPONDED_THEN_FAILED"
D_REFUSED = "D_REFUSED_BY_ADMISSION"
NO_DATA = "NO_BTLIFE_COVERAGE"

# Post-page phase outcomes. The 40-cycle run showed page_rx + page_accept
# followed by 20.3 s of silence and then acl_fail 0x08, which the earlier events
# could locate only by the size of the gap. These name the boundary.
C1_ACL_NEVER_COMPLETED = "C1_PAGE_ACCEPTED_ACL_NEVER_COMPLETED"
C2_AUTH_OR_ENC_FAILED = "C2_ACL_UP_SECURITY_FAILED"
C3_ACL_UP_THEN_DROPPED = "C3_ACL_UP_THEN_DISCONNECTED"
C4_STALLED_BETWEEN_PHASES = "C4_STALLED_BETWEEN_PHASES"

# Ordered establishment phases, for locating the last one reached and the gap
# that followed it.
PHASE_ORDER = ("page_rx", "page_accept", "acl_up", "link_key_req",
               "auth_start", "auth_done", "enc_change", "hid_ready")


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


def align(timelines: list[dict], btlife: list[dict]) -> tuple[float, float, int, str]:
    """Offset such that wall = t_ms/1000 + offset, from acl_up <-> ACL up.

    Matched in order: both sides see the same successful establishments in the
    same sequence, so the k-th of each is the same event.
    """
    android = [wall_seconds(row["at"])
               for r in timelines if r["result"] == "ok"
               for row in r["timeline"] if row["event"] == "classic.acl_up"]
    android = [a for a in android if a is not None]
    adapter = [e["t_ms"] / 1000.0 for e in btlife if e["code"] == "acl_up"]
    alignment_name = "Android classic.acl_up <-> Pico acl_up"
    # Some current Fluoride log levels omit BTA_DM_LINK_UP_EVT even though the
    # app callback and Pico HID-ready event remain visible. Those two boundaries
    # are the same successful establishment observed from opposite hosts and are
    # a valid fallback when the direct ACL marker is absent.
    if len(android) < 5 or len(adapter) < 5:
        android = [wall_seconds(row["at"])
                   for r in timelines if r["result"] == "ok"
                   for row in r["timeline"] if row["event"] == "app.link_up"]
        android = [a for a in android if a is not None]
        adapter = [e["t_ms"] / 1000.0 for e in btlife
                   if e["code"] == "hid_ready"]
        alignment_name = "Android app.link_up <-> Pico hid_ready"
    if len(android) < 5 or len(adapter) < 5:
        sys.exit(f"only {min(len(android), len(adapter))} acl_up events; "
                 "cannot align clocks confidently")

    # Best-offset search rather than positional pairing. The two series are NOT
    # guaranteed to correspond element for element: the ring may have evicted
    # early cycles, and a FAILED cycle can still produce an adapter-side acl_up
    # (a link that comes up and then dies) with no matching Android success. Any
    # such asymmetry shifts a positional pairing and silently corrupts every
    # verdict downstream, so the offset is chosen by how many events it actually
    # reconciles.
    TOL = 0.5
    best = (0, 0.0, [])
    for a in android:
        for b in adapter:
            candidate = a - b
            matched = []
            for x in android:
                near = min((abs(x - (y + candidate)), x - (y + candidate))
                           for y in adapter)
                if near[0] <= TOL:
                    matched.append(near[1])
            if len(matched) > best[0]:
                best = (len(matched), candidate, matched)
    count, offset, residuals = best
    if count < 5:
        sys.exit(f"only {count} acl_up events reconcile at any offset; "
                 "cannot align clocks confidently")
    # Refine on the matched set, and report its spread as the quality measure.
    offset += statistics.median(residuals)
    return (offset,
            statistics.pstdev(residuals) if len(residuals) > 1 else 0.0,
            count, alignment_name)


def _phase_verdict(events: list[dict], codes: list[str],
                   detail: dict) -> tuple[str, dict]:
    """The page was answered. Say which phase it died in, and after how long.

    Every boundary is timestamped, so the longest gap is computed rather than
    inferred from which events happen to be missing. No timer exists in the
    firmware for this; the arithmetic lives here.
    """
    at = {}
    for e in events:
        at.setdefault(e["code"], e["t_ms"] / 1000.0)
    reached = [p for p in PHASE_ORDER if p in at]
    detail["phases_reached"] = reached
    detail["last_phase"] = reached[-1] if reached else None

    # Largest gap between consecutive boundaries: where it stalled.
    gaps = []
    for a, b in zip(reached, reached[1:]):
        gaps.append((round(at[b] - at[a], 3), f"{a}->{b}"))
    terminal = next((e["code"] for e in events
                     if e["code"] in ("acl_fail", "hci_disconnect", "hid_fail")), None)
    if terminal and reached:
        gaps.append((round(at.get(terminal, at[reached[-1]]) - at[reached[-1]], 3),
                     f"{reached[-1]}->{terminal}"))
    if gaps:
        gaps.sort(reverse=True)
        detail["longest_gap_s"], detail["longest_gap"] = gaps[0]

    if "acl_up" not in codes:
        # A: answered the page, Connection Complete never succeeded.
        return C1_ACL_NEVER_COMPLETED, detail
    if "hid_ready" in codes and "hci_disconnect" in codes:
        # C: it came all the way up and then went away.
        return C3_ACL_UP_THEN_DROPPED, detail
    if any(at.get(k) is not None for k in ("auth_done", "enc_change")) and             "hid_ready" not in codes:
        # B: security ran and HID never became usable.
        return C2_AUTH_OR_ENC_FAILED, detail
    if "hid_ready" not in codes:
        # D: stopped between phases with no failure event of its own.
        return C4_STALLED_BETWEEN_PHASES, detail
    return C_AFTER_RESPONSE, detail


def classify_failure(events: list[dict],
                     harness_detail: dict | None = None) -> tuple[str, dict]:
    codes = [e["code"] for e in events]
    detail = {"btlife_events": codes}

    # Before asking anything of the adapter, check whether the phone actually
    # paged. If its own reason is not PAGE_TIMEOUT it never put a page on the
    # air, and a missing page_rx says nothing about this adapter.
    reason = (harness_detail or {}).get("connect_fail_reason")
    phone_state = ("CONNECTION_ALREADY_EXISTS", "COMMAND_DISALLOWED",
                   "CONTROLLER_BUSY", "MEMORY_FULL", "REPEATED_ATTEMPTS")
    if reason and any(r in reason for r in phone_state):
        detail["phone_reason"] = reason
        detail["adapter_saw_page"] = "page_rx" in codes
        return PHONE_NEVER_PAGED, detail

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
        return _phase_verdict(events, codes, detail)
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
    offset, spread, pairs, alignment_name = align(timelines, btlife)

    print(f"clock alignment: {pairs} pairs via {alignment_name}, "
          f"offset {offset:.3f}s, "
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
        # Two window widths on purpose. "Did a page arrive at all" is answered
        # inside Android's 5.12 s page timeout, so a narrow window keeps a
        # neighbouring cycle from leaking in. But once a page HAS been answered,
        # the terminal event can be far later -- cycle 3 of the 40-cycle run
        # failed with acl_fail 24.4 s after the request -- so the window is
        # widened only in that case, where there is no ambiguity about ownership.
        lo, hi = opened - 1.0, opened + 8.0
        window = [e for e in btlife if lo <= e["t_ms"] / 1000.0 + offset <= hi]
        if any(e["code"] == "page_rx" for e in window):
            hi = opened + 30.0
            window = [e for e in btlife if lo <= e["t_ms"] / 1000.0 + offset <= hi]
        verdict, detail = classify_failure(window, r.get("detail"))
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
            reason = (r.get("detail") or {}).get("connect_fail_reason")
            if reason and any(x in reason for x in
                              ("CONNECTION_ALREADY_EXISTS", "COMMAND_DISALLOWED",
                               "CONTROLLER_BUSY", "MEMORY_FULL",
                               "REPEATED_ATTEMPTS")):
                continue  # the phone never paged; not a paging sample
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

    report_establishment_windows(btlife)
    return 0


def establishment_windows(btlife: list[dict]) -> tuple[list[dict], int]:
    """Every page_accept -> Connection Complete window, adapter-side only.

    This isolates CLASSIC_ACL_TIMEOUT, and it is deliberately built from the
    firmware event stream alone: no Android timestamps, so no clock alignment,
    so an alignment residual cannot move a latency figure. The window is exactly
    the state in which the 20.2-20.6 s stall occurs --
    ACCEPTED_CONNECTION_REQUEST, from the accept callback returning 1 to
    HCI_EVENT_CONNECTION_COMPLETE arriving with any status. See
    CYW43439_Bluetooth_Investigation.md.

    Returns the closed windows plus a count of ones that never closed, which are
    reported rather than dropped silently -- an unterminated window is either a
    ring that wrapped mid-establishment or a Connection Complete that genuinely
    never arrived, and those must not be averaged in as if they were fast.
    """
    windows: list[dict] = []
    unterminated = 0
    open_at: float | None = None
    inquiries = 0
    for e in btlife:
        code = e["code"]
        t = e["t_ms"] / 1000.0
        if code == "page_accept":
            if open_at is not None:
                unterminated += 1
            open_at, inquiries = t, 0
        elif open_at is None:
            continue
        elif code == "inquiry_start":
            inquiries += 1
        elif code in ("acl_up", "acl_fail"):
            windows.append({
                "latency_s": round(t - open_at, 3),
                "outcome": code,
                "reason": e.get("a") if code == "acl_fail" else 0,
                "inquiry_starts": inquiries,
            })
            open_at = None
    if open_at is not None:
        unterminated += 1
    return windows, unterminated


def report_establishment_windows(btlife: list[dict]) -> None:
    windows, unterminated = establishment_windows(btlife)
    print("\n=== page_accept -> connection complete (ACCEPTED_CONNECTION_REQUEST) ===")
    if not windows:
        print("  no closed establishment windows in this ring")
        return
    ups = sorted(w["latency_s"] for w in windows if w["outcome"] == "acl_up")
    fails = [w for w in windows if w["outcome"] == "acl_fail"]
    # 0x08 is the CYW43 controller's own supervision of the accepted request;
    # it is the failure class this experiment exists to move.
    timeouts = [w for w in fails if w["reason"] == 0x08]

    def pct(values: list[float], p: float) -> float:
        return values[min(len(values) - 1, int(len(values) * p))]

    print(f"  windows {len(windows)}  acl_up {len(ups)}  "
          f"acl_fail {len(fails)}  unterminated {unterminated}")
    if ups:
        print(f"  acl_up latency  n={len(ups)}  min={ups[0]:.3f}  "
              f"p50={pct(ups, 0.5):.3f}  p90={pct(ups, 0.9):.3f}  "
              f"p99={pct(ups, 0.99):.3f}  max={ups[-1]:.3f}")
    print(f"  CLASSIC_ACL_TIMEOUT (0x08): {len(timeouts)}")
    for w in timeouts:
        print(f"    stalled {w['latency_s']:.3f}s, "
              f"inquiry restarts inside window: {w['inquiry_starts']}")
    other = [w for w in fails if w["reason"] != 0x08]
    if other:
        # A new failure class appearing is itself a result of the experiment.
        print("  other acl_fail reasons (a new class here is itself a result):")
        for w in other:
            print(f"    reason 0x{w['reason']:02X} after {w['latency_s']:.3f}s")

    # Inquiry occupancy of the window. Read this against window DURATION, never
    # on its own: a ~0.63 s success cannot contain a restart when rounds are
    # 6.4 s apart, while a 20 s stall will contain two or three regardless of
    # cause. That confound made restarts look causal until an interventional
    # test removed them and the stall rate did not move
    # (CYW43439_Bluetooth_Investigation.md).
    with_inq = sum(1 for w in windows if w["inquiry_starts"] > 0)
    total_inq = sum(w["inquiry_starts"] for w in windows)
    print(f"  inquiry restarts during establishment: {total_inq} across "
          f"{with_inq}/{len(windows)} windows")


if __name__ == "__main__":
    raise SystemExit(main())
