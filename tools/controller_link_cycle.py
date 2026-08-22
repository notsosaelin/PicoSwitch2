#!/usr/bin/env python3
"""Drive the Controller Link / Touch Gamepad lifecycle over ADB and classify failures.

This is the harness that produced the 2026-08-22 baseline (10 failures in 25
cycles, 8 of them Type C). It exists as a committed tool because the physical
acceptance campaign for the Controller Link candidate has to run the *same*
workload the baseline was measured on -- otherwise the before/after comparison
means nothing.

    python tools/controller_link_cycle.py --cycles 30

Requirements: `adb` on PATH with the tablet attached, the companion app
installed, the tablet unlocked with its screen on, and the adapter powered. UART
is optional; when `--port` is given the tool reads `btstate` between cycles and
reports the `enc.*` acceptance counters.

Pacing is deliberately human-scale. Do not lower it to "stress test" the stack:
the failure being measured is a race during connection establishment, and a
zero-delay loop measures something else.

See docs/experiments/controller-link-cycling-failure-2026-08-22.md for the
failure taxonomy and the accept/fail criteria.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time

PKG = "dev.picoswitch.companion.debug"
ACTIVITY = PKG + "/dev.picoswitch.companion.MainActivity"

# Signatures that classify a failed cycle. Order matters: Type C is checked
# first because an Android abort can follow it in the same log window.
SIGNATURES = (
    ("TypeC", ("Encryption failure 35", "LMP_ERR_TRANS_COLLISION")),
    ("Mode1", ("reason:PAGE_TIMEOUT",)),
    ("TypeA", ("Process com.android.bluetooth", "DeadObjectException")),
)


def adb(*args: str, timeout: int = 90) -> str:
    try:
        done = subprocess.run(
            ("adb",) + args, capture_output=True, text=True, timeout=timeout
        )
        return done.stdout
    except (subprocess.TimeoutExpired, OSError):
        return ""


def ui_dump() -> str:
    return adb("exec-out", "uiautomator", "dump", "/dev/tty")


def find_text(xml: str, label: str):
    """Centre point of the node whose text is exactly `label`, or None."""
    for match in re.finditer(r"<node [^>]*>", xml):
        node = match.group(0)
        text = (re.search(r'text="([^"]*)"', node) or [None, ""])[1]
        if text != label:
            continue
        bounds = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', node)
        if bounds:
            x1, y1, x2, y2 = (int(g) for g in bounds.groups())
            return (x1 + x2) // 2, (y1 + y2) // 2
    return None


def tap_text(xml: str, label: str) -> bool:
    point = find_text(xml, label)
    if point is None:
        return False
    adb("shell", "input", "tap", str(point[0]), str(point[1]))
    return True


def uart(port: str, command: str) -> dict:
    if not port:
        return {}
    try:
        done = subprocess.run(
            [
                "pwsh", "-NoProfile", "-File", "tools/uart_query.ps1",
                "-Port", port, "-Command", command,
            ],
            capture_output=True, text=True, timeout=90,
        )
    except (subprocess.TimeoutExpired, OSError):
        return {}
    match = re.search(r"(\{.*\})", done.stdout)
    return json.loads(match.group(1)) if match else {}


def classify(log: str) -> str:
    for name, needles in SIGNATURES:
        if any(needle in log for needle in needles):
            return name
    return "unknown"


def run_cycle(port: str, settle: float) -> tuple[str, float]:
    """One full lifecycle. Returns (result, seconds_to_link)."""
    adb("shell", "am", "force-stop", PKG)
    time.sleep(settle)
    adb("shell", "am", "start", "-n", ACTIVITY)
    time.sleep(settle + 3.0)

    xml = ui_dump()
    if find_text(xml, "Reconnect"):
        tap_text(xml, "Reconnect")
        time.sleep(settle + 2.0)
        xml = ui_dump()
    if tap_text(xml, "Gamepad"):
        time.sleep(settle - 1.0)
        xml = ui_dump()

    adb("logcat", "-c")
    if not tap_text(xml, "Touch Gamepad"):
        return "skipped", -1.0

    started = time.time()
    linked = False
    while time.time() - started < 26.0:
        time.sleep(2.0)
        if "bridge/link up" in adb("logcat", "-d", "-v", "time", "-t", "400"):
            linked = True
            break
    elapsed = round(time.time() - started, 1)

    result = "ok"
    if not linked:
        result = classify(adb("logcat", "-d", "-v", "time", "-t", "4000"))

    time.sleep(settle)
    adb("shell", "input", "keyevent", "KEYCODE_BACK")
    time.sleep(settle - 1.0)
    tap_text(ui_dump(), "Stop playing")
    time.sleep(settle)
    return result, elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--port", default="", help="UART port, e.g. COM11")
    parser.add_argument("--settle", type=float, default=4.0,
                        help="seconds between UI steps; approximates human pacing")
    args = parser.parse_args()

    if not adb("devices").strip():
        print("no adb device", file=sys.stderr)
        return 2

    before = uart(args.port, "btstate")
    if before:
        print("baseline enc:", before.get("enc"), "admission:", before.get("admission"))

    tally: dict[str, int] = {}
    print("cycle | result  | link_s")
    for index in range(1, args.cycles + 1):
        result, elapsed = run_cycle(args.port, args.settle)
        tally[result] = tally.get(result, 0) + 1
        print(f"{index:5} | {result:<7} | {elapsed}")

    after = uart(args.port, "btstate")
    print("\n=== summary ===")
    for name in sorted(tally):
        print(f"  {name:<8} {tally[name]}")
    if after:
        enc = after.get("enc", {})
        adm = after.get("admission", {})
        print("  enc:", enc)
        print("  admission:", adm)
        verdict = []
        if enc.get("deferrals", 0) == 0:
            verdict.append("MECHANISM FALSIFIED: enc.deferrals stayed 0")
        if enc.get("collisions", 0):
            verdict.append("RESIDUAL COLLISION: enc.collisions > 0")
        if enc.get("unencrypted_active", 0):
            verdict.append("SECURITY FAILURE: enc.unencrypted_active > 0")
        if tally.get("TypeC"):
            verdict.append(f"TYPE C STILL PRESENT: {tally['TypeC']} cycle(s)")
        if before and adm.get("reject_window") != before.get("admission", {}).get("reject_window"):
            verdict.append("MODE 2 REGRESSION: reject_window moved")
        print("  verdict:", "; ".join(verdict) if verdict else "ACCEPT (see doc for the full list)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
