#!/usr/bin/env python3
"""Drive the Controller Link / Touch Gamepad lifecycle over ADB and classify failures.

This is the harness that produced the 2026-08-22 baseline (10 failures in 25
cycles, 8 of them Type C). It exists as a committed tool because the physical
acceptance campaign for the Controller Link candidate has to run the *same*
workload the baseline was measured on -- otherwise the before/after comparison
means nothing.

    python tools/controller_link_cycle.py --cycles 30              # legacy baseline
    python tools/controller_link_cycle.py --workload A --cycles 500 --port COM11

Requirements: `adb` on PATH with the tablet attached, the companion app
installed, the tablet unlocked with its screen on, and the adapter powered. UART
is optional for the legacy workload; every other workload REQUIRES it, because
the adapter's own counters are the only place the acceptance invariants are
observable.

Pacing is deliberately human-scale. Do not lower it to "stress test" the stack:
the failure being measured is a race during connection establishment, and a
zero-delay loop measures something else.

THE WORKLOADS

Controller Link is not a standalone transport. It is a facility of a live BLE
management relationship: it may be established only while that same peer holds a
connected, bonded, encrypted management session, and it must be torn down when
that session is genuinely lost. The workloads below exist to exercise that
architecture rather than the Classic link in isolation.

  legacy  the 2026-08-22 baseline: force-stop the app each cycle, so management
          and Controller Link come up together. Kept unchanged so the before/
          after comparison against the recorded baseline stays valid.
  A       Controller Link connect/disconnect while management stays connected
          continuously. The main product loop.
  B       full lifecycle: management connect -> Touch connect -> Touch
          disconnect -> management disconnect -> verify Controller Link is fully
          down -> management reconnect -> Touch connect. Exposes lifecycle state
          that leaked across a management generation.
  C       management-only soak with no Controller Link, proving management is
          stable by itself.
  D       forced real management loss while Controller Link is ACTIVE. The loss
          is driven from the adapter (`mgmt off`), which drops only the LE
          management ACL and leaves the phone's Classic HID link untouched --
          that is what isolates the invariant. Verifies clean teardown, then
          management recovery, then a fresh Controller Link.

See docs/experiments/controller-link-cycling-failure-2026-08-22.md for the
failure taxonomy and the accept/fail criteria.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import subprocess
import sys
import time

PKG = "dev.picoswitch.companion.debug"
ACTIVITY = PKG + "/dev.picoswitch.companion.MainActivity"

HANDLE_NONE = "0xFFFF"

# ---------------------------------------------------------------------------
# OUTCOME VOCABULARY
#
# Every cycle ends in exactly one of these, and the domain prefix is the whole
# point: a dependency failure must never be readable as a Bluetooth failure.
#
# The 500-cycle run on 2026-08-23 is why. Cycle 491 was reported as "skipped",
# which looked like a UI automation fault. It was not: UART was healthy, the
# previous cycle had connected cleanly, no Bluetooth operation was attempted at
# all, and the real cause was the wireless ADB transport dropping at 07:54:21.
# adb() discarded stderr, so "no devices/emulators found" became an empty string,
# an empty string produced an empty UI hierarchy, and an empty hierarchy became
# "the Touch Gamepad button is missing". Three layers of silent degradation.
#
# device:*   the adapter or the Bluetooth link failed. These are the only
#            outcomes a verdict may judge.
# app:*      the companion app or its UI was not in the expected state, with ADB
#            proven healthy first.
# harness:*  the measuring apparatus failed. Never a device result.
# EXCLUDED   the cycle was interrupted by a known, recorded event (a management
#            loss) and carries no information about success or failure.
# ---------------------------------------------------------------------------
PNG_MAGIC = bytes((0x89, 0x50, 0x4E, 0x47))

# Set by main() when --evidence-dir is given; None disables capture.
EVIDENCE_DIR = None

OK = "ok"
EXCLUDED = "EXCLUDED"

DEV_MGMT_LE_TIMEOUT = "device:MGMT_LE_TIMEOUT"
DEV_CLASSIC_PAGE_TIMEOUT = "device:CLASSIC_PAGE_TIMEOUT"
DEV_AUTH_FAILURE = "device:AUTH_FAILURE"
DEV_ENCRYPTION_FAILURE = "device:ENCRYPTION_FAILURE"
DEV_HID_TIMEOUT = "device:HID_TIMEOUT"
DEV_UNBOUND_LINK = "device:UNBOUND_LINK"
DEV_STALE_LINK = "device:STALE_LINK"
DEV_UNKNOWN_TIMEOUT = "device:UNKNOWN_TIMEOUT"

APP_TOUCH_GAMEPAD_NOT_FOUND = "app:UI_TOUCH_GAMEPAD_NOT_FOUND"
APP_UI_STATE_UNEXPECTED = "app:UI_STATE_UNEXPECTED"
APP_DUPLICATE_OWNER = "app:DUPLICATE_OWNER"
APP_BT_PROCESS_DIED = "app:ANDROID_BT_PROCESS_DIED"
APP_COD_REJECTED = "app:COD_REJECTED"
# The phone refused or failed the connection for a reason of its own -- local
# connection-state bookkeeping, not the radio. Confirmed on 2026-08-23, cycle 3
# of the 100-cycle soak: OnConnectFail reason:CONNECTION_ALREADY_EXISTS(0x0b),
# 4.6 s after the request, on a link whose previous ACL had been fully down for
# 10 s. No page is transmitted in this case, so the adapter is uninvolved and
# calling it a Classic page failure would be simply wrong.
APP_CONNECTION_STATE_FAILURE = "app:CONNECTION_STATE_FAILURE"
# The app's own 8 s HID callback watchdog fired with no stack-level outcome at
# all -- neither a connect failure nor a connection.
APP_HID_CALLBACK_TIMEOUT = "app:HID_CALLBACK_TIMEOUT"
# The app never got as far as asking for a connection.
APP_NO_CONNECT_ATTEMPT = "app:NO_CONNECT_ATTEMPT"

HARNESS_ADB_DEVICE_LOST = "harness:ADB_DEVICE_LOST"
HARNESS_ADB_COMMAND_FAILED = "harness:ADB_COMMAND_FAILED"
HARNESS_UI_DUMP_FAILED = "harness:UI_DUMP_FAILED"
HARNESS_UART_FAILED = "harness:UART_FAILED"
HARNESS_STATE_UNKNOWN = "harness:STATE_UNKNOWN"


def domain(outcome: str) -> str:
    """device / app / harness / ok / excluded."""
    if outcome == OK:
        return "ok"
    if outcome == EXCLUDED:
        return "excluded"
    return outcome.split(":", 1)[0] if ":" in outcome else "device"


@dataclasses.dataclass
class ShellResult:
    """A command result that cannot silently look like success.

    stderr and the return code are kept because discarding them is exactly how
    an ADB transport loss was laundered into a UI failure.
    """
    stdout: str = ""
    stderr: str = ""
    code: int | None = None
    timed_out: bool = False

    @property
    def ok(self) -> bool:
        return self.code == 0 and not self.timed_out


def adb_run(*args: str, timeout: int = 90) -> ShellResult:
    try:
        done = subprocess.run(
            ("adb",) + args, capture_output=True, text=True, timeout=timeout
        )
        return ShellResult(done.stdout, done.stderr, done.returncode, False)
    except subprocess.TimeoutExpired:
        return ShellResult(timed_out=True)
    except OSError as error:
        return ShellResult(stderr=str(error), code=-1)


def adb(*args: str, timeout: int = 90) -> str:
    """Convenience wrapper for fire-and-forget commands (taps, keyevents).

    Anything whose FAILURE must be distinguishable uses adb_run() directly.
    """
    return adb_run(*args, timeout=timeout).stdout


def check_adb_health() -> str | None:
    """Is the Android side reachable at all? Returns None when healthy."""
    result = adb_run("get-state", timeout=20)
    if result.timed_out:
        return HARNESS_ADB_COMMAND_FAILED
    combined = (result.stdout + result.stderr).lower()
    if "no devices" in combined or "device not found" in combined             or "offline" in combined:
        return HARNESS_ADB_DEVICE_LOST
    if not result.ok:
        return HARNESS_ADB_COMMAND_FAILED
    if "device" not in result.stdout:
        return HARNESS_ADB_DEVICE_LOST
    return None


def ui_dump_checked() -> tuple[str, str | None]:
    """UI hierarchy plus an explicit failure reason.

    A dump with no <node> in it is a FAILED dump, not an empty screen. That
    distinction is the whole fix for the cycle-491 misclassification.
    """
    result = adb_run("exec-out", "uiautomator", "dump", "/dev/tty")
    if not result.ok:
        return "", HARNESS_UI_DUMP_FAILED
    if "<node" not in result.stdout:
        return result.stdout, HARNESS_UI_DUMP_FAILED
    return result.stdout, None


def ui_dump() -> str:
    return ui_dump_checked()[0]


def _node_centre(node: str):
    bounds = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', node)
    if not bounds:
        return None
    x1, y1, x2, y2 = (int(g) for g in bounds.groups())
    return (x1 + x2) // 2, (y1 + y2) // 2


def find_text(xml: str, label: str):
    """Centre point of the node whose text is exactly `label`, or None."""
    for match in re.finditer(r"<node [^>]*>", xml):
        node = match.group(0)
        text = (re.search(r'text="([^"]*)"', node) or [None, ""])[1]
        if text == label:
            return _node_centre(node)
    return None


def find_desc(xml: str, label: str):
    """Centre point of the node whose content-description is exactly `label`.

    The management Disconnect control is an icon button with no text, so the UI
    tree has to be searched by description to reach it.
    """
    for match in re.finditer(r"<node [^>]*>", xml):
        node = match.group(0)
        desc = (re.search(r'content-desc="([^"]*)"', node) or [None, ""])[1]
        if desc == label:
            return _node_centre(node)
    return None


def tap_point(point) -> bool:
    if point is None:
        return False
    adb("shell", "input", "tap", str(point[0]), str(point[1]))
    return True


def tap_text(xml: str, label: str) -> bool:
    return tap_point(find_text(xml, label))


def tap_desc(xml: str, label: str) -> bool:
    return tap_point(find_desc(xml, label))


def uart(port: str, command: str) -> dict:
    if not port:
        return {}
    try:
        done = subprocess.run(
            [
                "pwsh", "-NoProfile", "-File", "tools/uart_query.ps1",
                "-Port", port, "-Command", command, "-TimeoutMs", "6000",
            ],
            capture_output=True, text=True, timeout=90,
        )
    except (subprocess.TimeoutExpired, OSError):
        return {}
    match = re.search(r"(\{.*\})", done.stdout)
    if not match:
        return {}
    try:
        return json.loads(match.group(1))
    except json.JSONDecodeError:
        return {}


# ---------------------------------------------------------------------------
# TIMELINE
#
# Markers are ordered by where they occur in a Controller Link establishment, so
# a timeline reads top to bottom and the DIVERGENCE POINT between a fast, slow
# and failed attempt is the first row that differs.
#
# Every string here was observed in the 2026-08-23 captures; none is speculative.
# ---------------------------------------------------------------------------
TIMELINE_MARKERS: tuple[tuple[str, str], ...] = (
    # LE management, before Classic starts
    ("le.last_txn",          "management/result:"),
    # App / HID registration
    ("app.touch_opened",     "controller/touch gamepad: opened"),
    ("app.hid_acquiring",    "transport/HID profile: acquiring"),
    ("app.hid_registered",   "transport/HID registration: registered"),
    ("app.connect_requested","transport/HID connection: requested"),
    ("app.state_connecting", "transport/HID connection state: connecting"),
    # Classic establishment, from the platform stack
    ("classic.acl_up",       "BTA_DM_LINK_UP_EVT"),
    ("classic.sec_required", "Outgoing authentication/encryption Required"),
    ("classic.enc_start",    "Security Manager: Start encryption"),
    ("classic.enc_change",   "btm_sec_encrypt_change: Security Manager encryption change request"),
    ("classic.enc_callback", "encryptionChangeCallback"),
    # HID up
    ("app.state_connected",  "transport/HID connection state: connected"),
    ("app.link_up",          "bridge/link up"),
    # Terminal events
    ("fail.connect_fail",    "OnConnectFail"),
    ("fail.page_timeout",    "PAGE_TIMEOUT"),
    ("app.bridge_failed",    "controller/bridge.phase: Failed"),
    ("app.callback_timeout", "HID connection: callback timeout"),
    ("fail.hid_rejected",    "transport/HID connection rejected"),
    ("fail.acl_down",        "ACL link down"),
    ("fail.gatt_error",      "management/gatt.error"),
)

TIMESTAMP = re.compile(r"^(\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})")


def _stamp_seconds(line: str) -> float | None:
    match = TIMESTAMP.match(line)
    if not match:
        return None
    clock = match.group(1).split(" ")[1]
    hours, minutes, rest = clock.split(":")
    return int(hours) * 3600 + int(minutes) * 60 + float(rest)


def build_timeline(log: str) -> list[dict]:
    """First occurrence of each marker, with an offset from the touch open.

    Returned in observed order so a failed attempt can be diffed against a
    successful one row by row.
    """
    seen: dict[str, tuple[str, float | None, str]] = {}
    for line in log.splitlines():
        for name, needle in TIMELINE_MARKERS:
            if needle in line and name not in seen:
                seen[name] = (line[:23], _stamp_seconds(line), line.strip()[:200])
    origin = seen.get("app.touch_opened", (None, None, ""))[1]
    rows = []
    for name, _ in TIMELINE_MARKERS:
        if name not in seen:
            continue
        stamp, seconds, text = seen[name]
        rows.append({
            "event": name,
            "at": stamp,
            "t_rel": (round(seconds - origin, 3)
                      if seconds is not None and origin is not None else None),
            "line": text,
        })
    rows.sort(key=lambda r: (r["t_rel"] is None, r["t_rel"]))
    return rows


def acl_down_reason(log: str) -> str | None:
    """The controller's own reason for the last ACL drop, if one is present."""
    reason = None
    for line in log.splitlines():
        if "btif_dm_acl_evt" in line and "link down" in line and "reason:" in line:
            reason = line.rsplit("reason:", 1)[1].strip()
    return reason


def connect_fail_reason(log: str) -> str | None:
    """The phone stack's own reason for abandoning a Classic connect.

    `OnConnectFail ... reason:X` is the discriminator that separates a real
    over-the-air paging failure from the phone declining locally. PAGE_TIMEOUT
    means the controller paged and heard nothing; anything else means it did not
    page at all, and the adapter cannot be implicated.
    """
    match = None
    for line in log.splitlines():
        if "OnConnectFail" in line:
            found = re.search(r"reason:(\S+)", line)
            if found:
                match = found.group(1)
    return match


def classify(log: str, timeline: list[dict] | None = None) -> tuple[str, dict]:
    """Name the device failure and say what the evidence was.

    Ordered most specific first. Every branch reports the facts that justified
    it, so a report never has to say only "failed".
    """
    rows = timeline if timeline is not None else build_timeline(log)
    reached = {row["event"] for row in rows}
    detail: dict = {"acl_down_reason": acl_down_reason(log)}

    # Defensive: this is only ever called on a failed attempt, but a classifier
    # that can invent a device failure out of a healthy window is worse than no
    # classifier at all.
    if "app.link_up" in reached and "fail.acl_down" not in reached:
        return OK, detail

    if "Process com.android.bluetooth" in log or "DeadObjectException" in log:
        return APP_BT_PROCESS_DIED, detail
    if "hostOk=false" in log:
        return APP_COD_REJECTED, detail

    # LE management died. Distinguished from every Classic failure by the reason
    # code on the ACL that dropped, not by guesswork.
    if detail["acl_down_reason"] == "HCI_ERR_CONNECTION_TOUT" or             "status=8 state=0" in log:
        last_le = next((r for r in rows if r["event"] == "le.last_txn"), None)
        detail["last_le_txn_at"] = last_le["at"] if last_le else None
        detail["classic_stage_reached"] = sorted(
            e for e in reached if e.startswith("classic.") or e.startswith("app.state")
        )
        return DEV_MGMT_LE_TIMEOUT, detail

    if "Encryption failure 35" in log or "LMP_ERR_TRANS_COLLISION" in log:
        return DEV_ENCRYPTION_FAILURE, detail
    if "encrypt failure" in log:
        return DEV_ENCRYPTION_FAILURE, detail
    if "btm_sec_auth_complete" in log and "status: 0" not in log:
        return DEV_AUTH_FAILURE, detail

    # The phone's own reason comes first, because it decides whether the radio
    # was ever asked to do anything. Never attribute a failure to the adapter
    # without evidence that a page was actually transmitted.
    reason = connect_fail_reason(log)
    detail["connect_fail_reason"] = reason
    if reason is not None and "PAGE_TIMEOUT" not in reason:
        detail["hid_registered"] = "app.hid_registered" in reached
        detail["connect_requested"] = "app.connect_requested" in reached
        return APP_CONNECTION_STATE_FAILURE, detail

    # The cycle demonstrably started but never asked for a connection: an
    # app-state fault, not a link fault. Requires app.touch_opened, because a
    # window with nothing in it is missing evidence rather than showing a
    # refusal, and those must not collapse into the same answer.
    if "app.touch_opened" in reached and "app.connect_requested" not in reached:
        detail["reached"] = sorted(reached)
        return APP_NO_CONNECT_ATTEMPT, detail

    if "fail.page_timeout" in reached or (reason and "PAGE_TIMEOUT" in reason):
        requested = next((r for r in rows if r["event"] == "app.connect_requested"), None)
        rejected = next((r for r in rows if r["event"] == "fail.hid_rejected"), None)
        detail["paging_seconds"] = (
            round(rejected["t_rel"] - requested["t_rel"], 3)
            if requested and rejected and requested["t_rel"] is not None
            and rejected["t_rel"] is not None else None)
        detail["acl_up_observed"] = "classic.acl_up" in reached
        return DEV_CLASSIC_PAGE_TIMEOUT, detail

    # Classic came up but HID never became usable.
    if "classic.acl_up" in reached and "app.link_up" not in reached:
        detail["last_stage"] = rows[-1]["event"] if rows else None
        return DEV_HID_TIMEOUT, detail

    # The app's watchdog fired with no stack outcome either way. That is the
    # phone giving up, not the link failing.
    if "app.callback_timeout" in reached:
        detail["last_stage"] = rows[-1]["event"] if rows else None
        return APP_HID_CALLBACK_TIMEOUT, detail

    detail["last_stage"] = rows[-1]["event"] if rows else None
    detail["markers_seen"] = sorted(reached)
    return DEV_UNKNOWN_TIMEOUT, detail


# ---------------------------------------------------------------------------
# Adapter-side truth
#
# The app's UI reports what the phone believes. These read what the adapter
# actually has, which is what the invariants are stated in terms of.
# ---------------------------------------------------------------------------

class Adapter:
    def __init__(self, port: str):
        self.port = port

    def state(self) -> dict:
        return uart(self.port, "btstate")

    def health(self) -> dict:
        return uart(self.port, "bthealth")

    def mgmt_connected(self, state: dict | None = None) -> bool:
        state = self.state() if state is None else state
        return bool(state.get("cble", {}).get("client"))

    def link_up(self, state: dict | None = None) -> bool:
        state = self.state() if state is None else state
        return state.get("connections", {}).get("classic_ready", 0) >= 1

    def reports_binding(self, state: dict | None = None) -> bool:
        """Does this firmware publish the Controller Link binding at all?

        Builds before the binding existed have no `clink` block. Such a run can
        still exercise the workloads, but it cannot observe the invariant, so it
        must never be reported as an acceptance -- see verdict().
        """
        state = self.state() if state is None else state
        return "clink" in state

    def link_bound(self, state: dict | None = None) -> bool:
        """Is a Controller Link bound to a management session right now?

        Vacuously true on firmware that does not publish the binding, so an old
        build produces "not observable" rather than a run of false failures.
        """
        state = self.state() if state is None else state
        if "clink" not in state:
            return True
        return state["clink"].get("handle", HANDLE_NONE) != HANDLE_NONE

    def wait(self, predicate, timeout: float, poll: float = 2.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(self.state()):
                return True
            time.sleep(poll)
        return False

    def set_mgmt(self, enabled: bool) -> None:
        uart(self.port, "mgmt on" if enabled else "mgmt off")


# ---------------------------------------------------------------------------
# App-side actions
# ---------------------------------------------------------------------------

# FLAG_ACTIVITY_SINGLE_TOP (0x20000000) | FLAG_ACTIVITY_CLEAR_TOP (0x04000000).
#
# A plain `am start -n` does NOT match the task's existing root intent, so under
# MainActivity's launch mode every harness cycle pushed ANOTHER Activity
# instance. Confirmed 2026-08-23: `dumpsys activity activities` showed five live
# MainActivity records in one task, each owning its own management transport,
# GATT connection and 5-second poller. Two of them were polling the adapter
# concurrently.
#
# The app now also declares launchMode="singleTask", so this is belt and braces
# on purpose -- the harness must not be the only thing preventing the condition,
# and it must not hide it on a build that regresses.
LAUNCH_FLAGS = ("-f", "0x20000000", "-f", "0x04000000")


def app_start(settle: float) -> None:
    adb("shell", "am", "start", "-n", ACTIVITY, *LAUNCH_FLAGS)
    time.sleep(settle + 3.0)


def live_activity_instances() -> int:
    """How many MainActivity records the platform currently holds."""
    dump = adb("shell", "dumpsys", "activity", "activities")
    return len(re.findall(r"Hist\s+#\d+: ActivityRecord\{[^}]*" +
                          re.escape("MainActivity"), dump))


def app_restart(settle: float) -> None:
    adb("shell", "am", "force-stop", PKG)
    time.sleep(settle)
    app_start(settle)


def app_reconnect_management(settle: float) -> bool:
    """Tap whichever connect affordance the main screen is offering."""
    xml = ui_dump()
    for label in ("Reconnect", "Pair Adapter"):
        if tap_text(xml, label):
            time.sleep(settle + 2.0)
            return True
    return False


def app_disconnect_management(settle: float) -> bool:
    if not tap_desc(ui_dump(), "Disconnect"):
        return False
    time.sleep(settle)
    return True


def capture_ui_evidence(tag: str) -> None:
    """Screenshot + hierarchy + visible text + focused activity.

    "The button was not found" is a claim about automation, not about the app.
    On 2026-08-23 that claim was made seven times and was wrong every time -- the
    device was not even attached. When it IS an app problem, this is what
    distinguishes "the control is absent" from "the control is present and the
    automation missed it", so the evidence is captured rather than assumed.
    """
    if EVIDENCE_DIR is None:
        return
    stem = EVIDENCE_DIR / tag
    xml, _ = ui_dump_checked()
    (stem.with_suffix(".hierarchy.xml")).write_text(xml, encoding="utf-8")
    texts = re.findall(r'text="([^"]+)"', xml)
    descs = re.findall(r'content-desc="([^"]+)"', xml)
    focus = adb_run("shell", "dumpsys", "window").stdout
    focus_lines = [l.strip() for l in focus.splitlines()
                   if "mCurrentFocus" in l or "mFocusedApp" in l]
    report = ["focus:"]
    report += [f"  {line}" for line in focus_lines[:4]]
    report += ["", "visible text nodes:"]
    report += [f"  {t}" for t in dict.fromkeys(texts)]
    report += ["", "content-desc nodes:"]
    report += [f"  {d}" for d in dict.fromkeys(descs)]
    (stem.with_suffix(".ui.txt")).write_text("\n".join(report), encoding="utf-8")
    # screencap is binary, so it bypasses adb_run()'s text decoding.
    try:
        raw = subprocess.run(("adb", "exec-out", "screencap", "-p"),
                             capture_output=True, timeout=60)
        if raw.returncode == 0 and raw.stdout[:4] == PNG_MAGIC:
            (stem.with_suffix(".png")).write_bytes(raw.stdout)
    except (subprocess.TimeoutExpired, OSError):
        pass


def app_enter_touch(settle: float) -> str | None:
    """Open Touch Gamepad. Returns None on success, else a classified reason.

    ADB health is proven by the caller before this runs, so a failure here is an
    app/UI fact rather than an infrastructure one -- except for a failed dump,
    which is reported as such rather than as a missing button.
    """
    xml, dump_failed = ui_dump_checked()
    if dump_failed:
        return dump_failed
    if find_text(xml, "Touch Gamepad") is None and tap_text(xml, "Gamepad"):
        time.sleep(settle - 1.0)
        xml, dump_failed = ui_dump_checked()
        if dump_failed:
            return dump_failed
    if tap_text(xml, "Touch Gamepad"):
        return None
    return APP_TOUCH_GAMEPAD_NOT_FOUND


def app_exit_touch(settle: float) -> None:
    adb("shell", "input", "keyevent", "KEYCODE_BACK")
    time.sleep(settle - 1.0)
    tap_text(ui_dump(), "Stop playing")
    time.sleep(settle)


def await_link(dev: "Adapter | None" = None, timeout: float = 26.0) -> tuple[bool, float]:
    """Wait for the Controller Link to be usable.

    Three signals, in decreasing order of authority:

      * the adapter reports a ready Classic connection. This is the invariant's
        own terms and it is unambiguous -- verified 2026-08-22, `classic_ready`
        is 1 while Touch Gamepad plays and 0 within seconds of Stop playing.
      * the app's bridge phase reads `Playing` in its management poll lines.
      * `bridge/link up`.

    The last one alone is NOT sufficient and used to be the only check: it is a
    TRANSITION event, so a session that starts on an already-established HID
    link never emits it. Measured against the installed build on 2026-08-22 that
    silently classified two healthy cycles as failures while the adapter's own
    counters showed both links coming up cleanly.

    THE RETURNED TIME IS DETECTION LATENCY, NOT CONNECT LATENCY. Each poll costs
    a 2 s sleep plus roughly 1.1 s to spawn pwsh for a UART read, so the value is
    quantised to a ~3.1 s grid. That grid, not the device, produced the "three
    timing groups" of 3.1 / 6.3 / 9.5 s in the 2026-08-23 soak: the same three
    cycles measured from the log timeline were 1.10 / 4.34 / 0.91 s. Callers
    must prefer the timeline's app.link_up offset; this value is only a fallback
    for when no timeline could be built.
    """
    started = time.time()
    while time.time() - started < timeout:
        time.sleep(2.0)
        if dev is not None and dev.port and dev.link_up():
            return True, round(time.time() - started, 1)
        log = adb("logcat", "-d", "-v", "time", "-t", "400")
        if "bridge/link up" in log or "bridge=Playing" in log:
            return True, round(time.time() - started, 1)
    return False, round(time.time() - started, 1)


def app_believes_connected() -> bool | None:
    """What the app's own UI says about management, or None if unreadable.

    Read from the Adapter screen's affordances rather than from a log line: a
    "Reconnect"/"Pair Adapter" control means the app considers itself
    disconnected, and the Disconnect control means it considers itself
    connected.
    """
    xml = ui_dump()
    if find_desc(xml, "Disconnect") is not None:
        return True
    if find_text(xml, "Reconnect") is not None or find_text(xml, "Pair Adapter") is not None:
        return False
    return None


def check_state_agreement(dev: Adapter) -> str | None:
    """Fail loudly when the app and the adapter disagree about management.

    This is the condition that silently invalidated a whole manual campaign on
    2026-08-23: the app displayed "Not connected" with a Reconnect button while
    the adapter reported `cble.client: true` and kept answering commands for five
    hours, because a different Activity instance owned the real session. Every
    "Controller Link without management" result from that campaign measured
    nothing. A soak must never run under that contradiction.
    """
    adapter_view = dev.mgmt_connected()
    app_view = app_believes_connected()
    if app_view is None:
        return None  # not on a screen that states it; not a disagreement
    if app_view != adapter_view:
        return (f"management state disagreement: app={app_view} "
                f"adapter.cble.client={adapter_view}")
    return None


def check_single_owner() -> str | None:
    """Exactly one Activity instance, therefore one management transport.

    The app makes this structural (launchMode=singleTask plus an application-
    scoped ManagementOwner), and the harness launches with SINGLE_TOP|CLEAR_TOP.
    Asserting it anyway is deliberate: if either protection regresses, a soak
    would otherwise keep running while two transports drove the adapter.

    FAILS OPEN NO LONGER. The previous version counted regex matches in whatever
    string came back, so a failed query returned 0, and `0 > 1` passed. During
    the 2026-08-23 ADB outage this check "succeeded" on every one of the seven
    dead cycles. If the harness cannot read the state, that is STATE_UNKNOWN.
    """
    result = adb_run("shell", "dumpsys", "activity", "activities")
    if not result.ok or "ActivityRecord" not in result.stdout:
        return HARNESS_STATE_UNKNOWN
    instances = len(re.findall(
        r"Hist\s+#\d+: ActivityRecord\{[^}]*" + re.escape("MainActivity"),
        result.stdout))
    if instances == 0:
        # The dump was readable but our own Activity is absent: an app state
        # fact, not an infrastructure one.
        return APP_UI_STATE_UNEXPECTED
    if instances > 1:
        return APP_DUPLICATE_OWNER
    return None


def recover_dependencies(port: str, attempts: int = 4) -> bool:
    """Pause and try to restore the control planes. True once observable again.

    Counting cycles while unable to observe the system produces numbers that
    look like data and are not. On 2026-08-23 the run continued for seven cycles
    after ADB vanished, and every one was recorded as a device-side "skipped".
    """
    for attempt in range(1, attempts + 1):
        delay = (1, 2, 5, 15)[min(attempt, 4) - 1]
        time.sleep(delay)
        adb_run("reconnect", timeout=30)
        adb_run("wait-for-device", timeout=delay + 20)
        adb_ok = check_adb_health() is None
        uart_ok = bool(uart(port, "btstate")) if port else True
        print(f"      | recovery attempt {attempt}: adb={'ok' if adb_ok else 'down'} "
              f"uart={'ok' if uart_ok else 'down'}", flush=True)
        if adb_ok and uart_ok:
            return True
    return False


# ---------------------------------------------------------------------------
# Workloads
# ---------------------------------------------------------------------------

def run_cycle(port: str, settle: float) -> tuple[str, float]:
    """One full lifecycle, 2026-08-22 baseline form. Unchanged on purpose."""
    adb("shell", "am", "force-stop", PKG)
    time.sleep(settle)
    adb("shell", "am", "start", "-n", ACTIVITY, *LAUNCH_FLAGS)
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

    linked, elapsed = await_link(Adapter(port) if port else None)
    result = "ok"
    if not linked:
        result = classify(adb("logcat", "-d", "-v", "time", "-t", "4000"))

    time.sleep(settle)
    app_exit_touch(settle)
    return result, elapsed


def cycle_touch_only(dev: Adapter, settle: float) -> tuple[str, float, dict]:
    """Workload A: enter and leave Touch Gamepad; management must never drop.

    Ordered so that a dependency can never be blamed on the device. Each control
    plane is proven healthy before any outcome that depends on it can be
    reported, and every attempt produces a timeline whether it passed or failed.
    """
    info: dict = {}

    # 1. ADB, before anything reads a UI or taps a control.
    adb_problem = check_adb_health()
    if adb_problem:
        return adb_problem, -1.0, info

    # 2. UART, distinguished from "management is down". A silent adapter is a
    #    harness failure; a responsive adapter reporting cble.client false is a
    #    device/lifecycle event. Conflating them produced 297 bogus "mgmt_lost"
    #    entries on 2026-08-23.
    state = dev.state()
    if not state:
        return HARNESS_UART_FAILED, -1.0, info
    if not dev.mgmt_connected(state):
        info["mgmt_lost"] = True
        info["adapter_state"] = {k: state.get(k) for k in ("cble", "clink", "connections")}
        return DEV_MGMT_LE_TIMEOUT, -1.0, info

    # 3. Ownership. "Cannot determine" is its own answer, never a pass.
    owner_problem = check_single_owner()
    if owner_problem:
        return owner_problem, -1.0, info

    adb("logcat", "-c")
    enter_problem = app_enter_touch(settle)
    if enter_problem:
        if domain(enter_problem) == "app":
            capture_ui_evidence(f"ui-{time.strftime('%H%M%S')}")
        return enter_problem, -1.0, info

    linked, detected_at = await_link(dev)
    window = adb("logcat", "-d", "-v", "time", "-t", "4000")
    timeline = build_timeline(window)
    info["timeline"] = timeline
    info["acl_down_reason"] = acl_down_reason(window)
    info["detected_at"] = detected_at

    # Report the connect time the DEVICE achieved, not the time this harness
    # took to notice. See await_link(): its polling grid invented the "three
    # timing groups" that a whole analysis pass was nearly spent explaining.
    link_up = next((r["t_rel"] for r in timeline
                    if r["event"] == "app.link_up" and r["t_rel"] is not None),
                   None)
    acl_up = next((r["t_rel"] for r in timeline
                   if r["event"] == "classic.acl_up" and r["t_rel"] is not None),
                  None)
    info["t_link_up"] = link_up
    info["t_acl_up"] = acl_up
    # Paging dominates the variance; keeping it as its own number makes that
    # checkable every cycle rather than by hand.
    connecting = next((r["t_rel"] for r in timeline
                       if r["event"] == "app.state_connecting"
                       and r["t_rel"] is not None), None)
    info["t_paging"] = (round(acl_up - connecting, 3)
                        if acl_up is not None and connecting is not None else None)
    elapsed = link_up if link_up is not None else detected_at

    if not linked:
        result, detail = classify(window, timeline)
        info["detail"] = detail
    elif not dev.wait(dev.link_bound, 8.0):
        # The app says the link is up but the adapter never bound it to the
        # management session. That is the invariant failing silently, which is
        # exactly the state this harness exists to make impossible to miss.
        result = DEV_UNBOUND_LINK
    else:
        result = OK

    app_exit_touch(settle)
    if not dev.wait(lambda s: not dev.link_up(s), 12.0):
        result = DEV_STALE_LINK if result == OK else result
    after = dev.state()
    if not after:
        result = HARNESS_UART_FAILED if result == OK else result
    elif not dev.mgmt_connected(after):
        # Management dropped DURING the cycle. The cycle itself carries no
        # verdict; main() records the event and decides whether to recover.
        info["mgmt_lost"] = True
        result = DEV_MGMT_LE_TIMEOUT if result == OK else result
    return result, elapsed, info


def cycle_full_lifecycle(dev: Adapter, settle: float) -> tuple[str, float]:
    """Workload B: one complete management generation around a Controller Link."""
    if not dev.mgmt_connected():
        app_reconnect_management(settle)
        if not dev.wait(dev.mgmt_connected, 25.0):
            return "mgmt_connect_failed", -1.0

    result, elapsed = cycle_touch_only(dev, settle)
    if result != "ok":
        return result, elapsed

    if not app_disconnect_management(settle):
        return "no_disconnect_control", elapsed
    if not dev.wait(lambda s: not dev.mgmt_connected(s), 20.0):
        return "mgmt_disconnect_failed", elapsed
    # The whole point of the generation: nothing of the Controller Link may
    # survive the session that authorised it.
    if not dev.wait(lambda s: not dev.link_up(s) and not dev.link_bound(s), 15.0):
        return "stale_link_after_mgmt_loss", elapsed

    app_reconnect_management(settle)
    if not dev.wait(dev.mgmt_connected, 30.0):
        return "mgmt_reconnect_failed", elapsed

    result, elapsed = cycle_touch_only(dev, settle)
    return ("ok" if result == "ok" else "post_reconnect_" + result), elapsed


def cycle_management_only(dev: Adapter, settle: float) -> tuple[str, float]:
    """Workload C: management alone must stay up with no Controller Link."""
    state = dev.state()
    if not state:
        return "uart_silent", -1.0
    if not dev.mgmt_connected(state):
        app_reconnect_management(settle)
        if not dev.wait(dev.mgmt_connected, 25.0):
            return "mgmt_connect_failed", -1.0
        return "recovered", -1.0
    if dev.link_up(state):
        return "unexpected_link", -1.0
    time.sleep(settle * 2.0)
    return ("ok" if dev.mgmt_connected() else "mgmt_lost"), -1.0


def cycle_forced_mgmt_loss(dev: Adapter, settle: float) -> tuple[str, float]:
    """Workload D: real management loss underneath a LIVE Controller Link.

    Driven from the adapter rather than the phone. `mgmt off` disconnects the LE
    management ACL and leaves the phone's Classic HID link alone, which is the
    only way to separate "management was lost" from "the app went away" -- the
    app owns both ends, so killing it proves nothing about the invariant.
    """
    if not dev.mgmt_connected():
        app_reconnect_management(settle)
        if not dev.wait(dev.mgmt_connected, 25.0):
            return "mgmt_connect_failed", -1.0

    adb("logcat", "-c")
    if not app_enter_touch(settle):
        return "skipped", -1.0
    linked, elapsed = await_link(dev)
    if not linked:
        app_exit_touch(settle)
        return classify(adb("logcat", "-d", "-v", "time", "-t", "4000")), elapsed
    if not dev.wait(dev.link_bound, 8.0):
        app_exit_touch(settle)
        return "unbound", elapsed

    before = dev.state().get("clink", {}).get("mgmt_teardowns", 0)
    dev.set_mgmt(False)
    try:
        if not dev.wait(lambda s: not dev.mgmt_connected(s), 20.0):
            return "mgmt_disconnect_failed", elapsed
        if not dev.wait(lambda s: not dev.link_up(s) and not dev.link_bound(s), 15.0):
            return "stale_link_after_mgmt_loss", elapsed
        after = dev.state().get("clink", {}).get("mgmt_teardowns", 0)
        if after <= before:
            # The link went away but not because we released it -- attributing
            # a coincidental drop to the fix would be the whole point missed.
            return "teardown_unattributed", elapsed
    finally:
        dev.set_mgmt(True)

    app_exit_touch(settle)
    app_reconnect_management(settle)
    if not dev.wait(dev.mgmt_connected, 40.0):
        return "mgmt_recovery_failed", elapsed

    result, elapsed = cycle_touch_only(dev, settle)
    return ("ok" if result == "ok" else "post_recovery_" + result), elapsed


def cycle_reconnect_abuse(dev: Adapter, settle: float) -> tuple[str, float]:
    """Workload E: rapid management drop/restore pressure, with a recovery clock.

    Driven from the adapter (`mgmt off`/`mgmt on`) because that is the only
    primitive that produces a real management loss -- the app's Disconnect
    control could not, which is what made the manual observation ambiguous.

    The manual report was: spamming reconnect/disconnect could leave management
    unavailable for ~10-15 s, with the app reporting that the saved adapter did
    not advertise management services, recovering on its own without a power
    cycle. The standing hypothesis is that `config_ble_service_task` suppresses
    the advertiser restart while `hid_state == BLE_STATE_CONNECTING`, and
    BLE_CONNECT_TIMEOUT_MS is 10000 ms.

    That is a HYPOTHESIS. This workload does not assume it: it records the radio
    state at the moment advertising is absent and measures how long recovery
    takes, so the correlation can be observed rather than asserted. A run that
    self-recovers is classified separately from a hard wedge.
    """
    dev.set_mgmt(False)
    time.sleep(1.0)
    dev.set_mgmt(True)

    started = time.time()
    samples = []
    advertising_at = None
    while time.time() - started < 40.0:
        state = dev.state()
        if not state:
            return "uart_silent", -1.0
        samples.append({
            "t": round(time.time() - started, 1),
            "hid_state": state.get("hid_state"),
            "advertising": state.get("cble", {}).get("advertising"),
            "client": state.get("cble", {}).get("client"),
            "scan": state.get("scan_active"),
            "inquiry": state.get("inquiry_active"),
            "adv": state.get("adv"),
        })
        if state.get("cble", {}).get("advertising") or state.get("cble", {}).get("client"):
            advertising_at = round(time.time() - started, 1)
            break
        time.sleep(1.0)

    if advertising_at is None:
        # Never came back inside the window: that is a wedge, not a stall, and
        # it is the only outcome here that should stop a campaign.
        print("    radio never resumed advertising:", json.dumps(samples[-6:]))
        return "advertiser_wedged", -1.0
    if advertising_at > 5.0:
        print(f"    advertiser resumed after {advertising_at}s;",
              json.dumps(samples[:4]))
        return "slow_recovery", advertising_at
    return "ok", advertising_at


WORKLOADS = {
    "legacy": ("force-stop each cycle (2026-08-22 baseline)", None),
    "A": ("Controller Link cycling, management stays connected", cycle_touch_only),
    "B": ("full management generation around a Controller Link", cycle_full_lifecycle),
    "C": ("management-only stability, no Controller Link", cycle_management_only),
    "D": ("forced management loss under a live Controller Link", cycle_forced_mgmt_loss),
    "E": ("rapid management drop/restore pressure", cycle_reconnect_abuse),
}

# Workloads whose own design takes management down for part of every cycle, so a
# refusal or a disconnect there is the architecture working rather than a fault.
MANAGEMENT_LOSS_WORKLOADS = {"D", "E"}


# ---------------------------------------------------------------------------
# Acceptance
# ---------------------------------------------------------------------------

def verdict(before: dict, after: dict, before_health: dict, after_health: dict,
            tally: dict, last_reject: dict, companion_addr: str,
            expect_refusals: bool, notes: list[str],
            last_auth: dict | None = None) -> list[str]:
    """The stated success criteria, evaluated against adapter counters.

    Every one of these is a delta over the run rather than an absolute, so a
    soak can be started on an adapter that already has history without the
    history being read as this run's failures.
    """
    def delta(group: str, key: str) -> int:
        return (after.get(group, {}).get(key, 0)
                - before.get(group, {}).get(key, 0))

    def health_delta(*path: str) -> int:
        def walk(source):
            node = source
            for step in path:
                node = node.get(step, {}) if isinstance(node, dict) else 0
            return node if isinstance(node, int) else 0
        return walk(after_health) - walk(before_health)

    failures = []
    if "clink" not in after:
        failures.append(
            "firmware predates the Controller Link binding (no `clink` in "
            "btstate): the invariant was NOT observable in this run")
    if "auth" not in after:
        failures.append(
            "firmware predates the authentication collision counter (no `auth` "
            "in btstate): authentication-side 0x23 was NOT observable")
    if last_auth and last_auth.get("btauth") == "last" and             "security_ok" not in last_auth:
        failures.append(
            "firmware predates the peer-led security verdict (no `security_ok` "
            "in btauth): the security invariant was NOT observable")
    if delta("auth", "collisions"):
        failures.append(f"authentication 0x23 collisions: {delta('auth', 'collisions')}")
    if delta("enc", "collisions"):
        failures.append(f"encryption 0x23 collisions: {delta('enc', 'collisions')}")
    if delta("enc", "unencrypted_active"):
        failures.append(
            f"unencrypted-active events: {delta('enc', 'unencrypted_active')}")
    # `reject_window` is anonymous -- it counts every unbonded Classic peer
    # refused outside a pairing window, and on 2026-08-22 the peer behind a
    # moving counter was an unrelated device (FE:20:11:18:93:4B), not the
    # companion. Attribute it before calling it a companion failure.
    rejects = delta("admission", "reject_window")
    if rejects:
        addr = (last_reject or {}).get("addr", "unknown")
        if companion_addr and addr.upper() == companion_addr.upper():
            failures.append(f"companion admission rejects: {rejects} (last {addr})")
        else:
            notes.append(
                f"{rejects} Classic admission reject(s) from {addr} -- "
                "not attributed to the companion")
    if delta("admission", "reject_lockout"):
        failures.append(
            f"lockout rejects: {delta('admission', 'reject_lockout')}")
    # This one IS companion-specific by construction: only a cross-transport
    # identity with no live management session can reach it.
    # Security is judged from the firmware's own verdict on the last link, which
    # is stated in terms of what exists on the PEER-LED path. Deliberately NOT
    # `enc.deferrals > 0`: BTstack only schedules its automatic encryption
    # request from the local Authentication Complete handler, and the
    # authentication stand-down means that event never arrives, so enc.deferrals
    # legitimately stays 0 on every intended reconnect. Requiring it would report
    # "MECHANISM FALSIFIED" on a perfectly healthy build -- observed 2026-08-23,
    # 50 clean links with enc.deferrals == 0 throughout.
    auth = last_auth or {}
    if auth.get("btauth") == "last":
        if auth.get("auth_outcome") == "observed_failed":
            failures.append("authentication failed on the last link")
        if not auth.get("security_ok", False):
            failures.append(
                "last link did not satisfy the companion security invariant: "
                f"auth_outcome={auth.get('auth_outcome')} "
                f"encrypted_ok={auth.get('encrypted_ok')} "
                f"key_size={auth.get('key_size')} "
                f"key_size_valid={auth.get('key_size_valid')} "
                f"hid_ready={auth.get('hid_ready')}")
    if delta("clink", "refused_no_mgmt") and not expect_refusals:
        failures.append(
            "Controller Link refused for lack of management: "
            f"{delta('clink', 'refused_no_mgmt')}")
    if health_delta("hci", "recovery", "attempts"):
        failures.append(
            f"HCI recovery attempts: {health_delta('hci', 'recovery', 'attempts')}")
    if health_delta("reboot", "requests"):
        failures.append(f"reboot requests: {health_delta('reboot', 'requests')}")
    if after.get("clink", {}).get("handle", HANDLE_NONE) != HANDLE_NONE \
            and not after.get("cble", {}).get("client"):
        failures.append("stale Controller Link bound with management disconnected")
    for name, count in tally.items():
        if name not in ("ok", "recovered"):
            failures.append(f"{name}: {count} cycle(s)")
    if tally.get("CoD"):
        failures.append("CoD-policy rejects observed")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--workload", default="legacy", choices=sorted(WORKLOADS))
    parser.add_argument("--port", default="", help="UART port, e.g. COM11")
    parser.add_argument("--settle", type=float, default=4.0,
                        help="seconds between UI steps; approximates human pacing")
    parser.add_argument("--log", default="", help="append per-cycle results here")
    parser.add_argument("--companion-addr", default="",
                        help="companion BD_ADDR (AA:BB:...); lets an admission "
                             "rejection be attributed instead of assumed")
    parser.add_argument("--evidence-dir", default="",
                        help="write per-attempt timelines, recorded events and "
                             "UI evidence here; without it a failure leaves only "
                             "a label")
    args = parser.parse_args()

    global EVIDENCE_DIR
    if args.evidence_dir:
        EVIDENCE_DIR = pathlib.Path(args.evidence_dir)
        EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)

    adb_problem = check_adb_health()
    if adb_problem:
        print(f"{adb_problem}: no usable adb device", file=sys.stderr)
        return 2
    if args.workload != "legacy" and not args.port:
        print("workloads A-D require --port: the acceptance invariants are only "
              "observable in the adapter's own counters", file=sys.stderr)
        return 2

    dev = Adapter(args.port)
    before = dev.state()
    before_health = dev.health()
    if before:
        print("baseline auth:", before.get("auth"), "enc:", before.get("enc"),
              "clink:", before.get("clink"), "admission:", before.get("admission"))

    if args.workload != "legacy":
        app_start(args.settle)
        if not dev.mgmt_connected():
            app_reconnect_management(args.settle)
            dev.wait(dev.mgmt_connected, 30.0)

    runner = WORKLOADS[args.workload][1]
    tally: dict[str, int] = {}
    events: list[dict] = []
    sink = open(args.log, "a", encoding="utf-8") if args.log else None
    timelines = (open(EVIDENCE_DIR / "timelines.jsonl", "a", encoding="utf-8")
                 if EVIDENCE_DIR else None)
    stop_reason = None
    print(f"workload {args.workload}: {WORKLOADS[args.workload][0]}")
    print("cycle | result                          | link_s | paging_s")
    try:
        for index in range(1, args.cycles + 1):
            if runner is None:
                result, elapsed = run_cycle(args.port, args.settle)
                info: dict = {}
            else:
                outcome = runner(dev, args.settle)
                result, elapsed, info = (outcome if len(outcome) == 3
                                         else (outcome[0], outcome[1], {}))

            # Every attempt gets a timeline, pass or fail. Comparing a 3 s
            # success against a 9 s success is impossible without the ones that
            # passed.
            if timelines is not None:
                timelines.write(json.dumps({
                    "cycle": index, "result": result,
                    "t_link_up": info.get("t_link_up"),
                    "t_acl_up": info.get("t_acl_up"),
                    "t_paging": info.get("t_paging"),
                    "detected_at": info.get("detected_at"),
                    "timeline": info.get("timeline", []),
                    "detail": info.get("detail", {}),
                }) + "\n")
                timelines.flush()

            recorded = result

            # --- management loss: record, exclude, recover exactly once ------
            if info.get("mgmt_lost") and args.workload not in MANAGEMENT_LOSS_WORKLOADS:
                event = {
                    "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
                    "cycle": index,
                    "hci_reason": info.get("acl_down_reason"),
                    "adapter_state": info.get("adapter_state"),
                }
                print(f"      | management loss: {event['hci_reason']}; "
                      "one controlled reconnect", flush=True)
                app_reconnect_management(args.settle)
                event["recovered"] = dev.wait(dev.mgmt_connected, 40.0)
                events.append(event)
                # The interrupted cycle proves nothing either way.
                recorded = EXCLUDED
                if not event["recovered"]:
                    stop_reason = ("management did not recover after one "
                                   "controlled reconnect")

            # --- infrastructure loss: pause, recover, never keep counting ----
            elif domain(result) == "harness":
                print(f"      | {result}: pausing for dependency recovery",
                      flush=True)
                restored = recover_dependencies(args.port)
                events.append({
                    "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
                    "cycle": index, "infrastructure": result,
                    "recovered": restored,
                })
                recorded = EXCLUDED if restored else result
                if not restored:
                    stop_reason = f"{result} could not be recovered"

            tally[recorded] = tally.get(recorded, 0) + 1
            paging = info.get("t_paging")
            shown = elapsed if elapsed is not None else -1.0
            line = (f"{index:5} | {recorded:<32} | {shown:>6} | "
                    f"{paging if paging is not None else chr(45):>8}")
            print(line, flush=True)
            if sink:
                sink.write(line + "\n")
                sink.flush()
            if stop_reason:
                print(f"\nSTOPPING: {stop_reason}", flush=True)
                break
    except KeyboardInterrupt:
        print("\ninterrupted; reporting what was measured")
    finally:
        if sink:
            sink.close()
        if timelines:
            timelines.close()
        if EVIDENCE_DIR and events:
            (EVIDENCE_DIR / "events.json").write_text(
                json.dumps(events, indent=2), encoding="utf-8")

    after = dev.state()
    after_health = dev.health()
    # Three domains, reported separately and never summed. A dependency failure
    # must not be readable as a Bluetooth failure, and that starts with not
    # printing them in the same column.
    print("\n=== summary ===")
    buckets: dict[str, list[tuple[str, int]]] = {
        "ok": [], "device": [], "app": [], "harness": [], "excluded": []}
    for name in sorted(tally):
        buckets[domain(name)].append((name, tally[name]))
    for label, title in (("ok", "SUCCESS"), ("device", "DEVICE failures"),
                         ("app", "APPLICATION failures"),
                         ("harness", "HARNESS failures (not device results)"),
                         ("excluded", "EXCLUDED (interrupted, no verdict)")):
        rows = buckets[label]
        if not rows:
            continue
        print(f"  {title}:")
        for name, count in rows:
            print(f"    {name:<34} {count}")
    attempted = sum(c for _, c in buckets["ok"] + buckets["device"] + buckets["app"])
    passed = sum(c for _, c in buckets["ok"])
    if attempted:
        print(f"  first-attempt success: {passed}/{attempted} "
              f"({100.0 * passed / attempted:.1f}%) over judgeable cycles")
    for event in events:
        print(f"  event: cycle {event.get('cycle')} "
              f"{event.get('hci_reason') or event.get('infrastructure')} "
              f"recovered={event.get('recovered')}")
    if after:
        print("  auth:", after.get("auth"))
        print("  enc:", after.get("enc"))
        print("  clink:", after.get("clink"))
        print("  admission:", after.get("admission"))
        notes: list[str] = []
        # Workload D deliberately runs with management down for part of
        # every cycle, so a refusal there is the fix working, not a fault.
        failures = verdict(before, after, before_health, after_health, tally,
                           uart(args.port, "btreject"), args.companion_addr,
                           args.workload in MANAGEMENT_LOSS_WORKLOADS, notes,
                           uart(args.port, "btauth"))
        for note in notes:
            print("  note:", note)
        if failures:
            print("  verdict: FAIL")
            for item in failures:
                print("    -", item)
            return 1
        print(f"  verdict: ACCEPT ({args.cycles} cycles, workload {args.workload})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
