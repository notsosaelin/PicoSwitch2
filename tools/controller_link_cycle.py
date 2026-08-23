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
import json
import re
import subprocess
import sys
import time

PKG = "dev.picoswitch.companion.debug"
ACTIVITY = PKG + "/dev.picoswitch.companion.MainActivity"

HANDLE_NONE = "0xFFFF"

# Signatures that classify a failed cycle. Order matters: Type C is checked
# first because an Android abort can follow it in the same log window.
SIGNATURES = (
    ("TypeC", ("Encryption failure 35", "LMP_ERR_TRANS_COLLISION")),
    ("CoD", ("hostOk=false",)),
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


def classify(log: str) -> str:
    for name, needles in SIGNATURES:
        if any(needle in log for needle in needles):
            return name
    return "unknown"


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


def app_enter_touch(settle: float) -> bool:
    xml = ui_dump()
    if find_text(xml, "Touch Gamepad") is None and tap_text(xml, "Gamepad"):
        time.sleep(settle - 1.0)
        xml = ui_dump()
    return tap_text(xml, "Touch Gamepad")


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
    """
    instances = live_activity_instances()
    if instances > 1:
        return (f"duplicate ownership: {instances} live MainActivity instances "
                "(each owns its own management transport and poller)")
    return None


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


def cycle_touch_only(dev: Adapter, settle: float) -> tuple[str, float]:
    """Workload A: enter and leave Touch Gamepad; management must never drop."""
    if not dev.mgmt_connected():
        return "mgmt_lost", -1.0
    owner_problem = check_single_owner()
    if owner_problem:
        return "duplicate_owner", -1.0

    adb("logcat", "-c")
    if not app_enter_touch(settle):
        return "skipped", -1.0

    linked, elapsed = await_link(dev)
    if not linked:
        result = classify(adb("logcat", "-d", "-v", "time", "-t", "4000"))
    elif not dev.wait(dev.link_bound, 8.0):
        # The app says the link is up but the adapter never bound it to the
        # management session. That is the invariant failing silently, which is
        # exactly the state this harness exists to make impossible to miss.
        result = "unbound"
    else:
        result = "ok"

    app_exit_touch(settle)
    if not dev.wait(lambda s: not dev.link_up(s), 12.0):
        result = "stale_link" if result == "ok" else result
    if not dev.mgmt_connected():
        result = "mgmt_lost" if result == "ok" else result
    return result, elapsed


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
    args = parser.parse_args()

    if not adb("devices").strip():
        print("no adb device", file=sys.stderr)
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
    sink = open(args.log, "a", encoding="utf-8") if args.log else None
    print(f"workload {args.workload}: {WORKLOADS[args.workload][0]}")
    print("cycle | result                     | link_s")
    try:
        for index in range(1, args.cycles + 1):
            if runner is None:
                result, elapsed = run_cycle(args.port, args.settle)
            else:
                result, elapsed = runner(dev, args.settle)
            tally[result] = tally.get(result, 0) + 1
            line = f"{index:5} | {result:<26} | {elapsed}"
            print(line, flush=True)
            if sink:
                sink.write(line + "\n")
                sink.flush()
    except KeyboardInterrupt:
        print("\ninterrupted; reporting what was measured")
    finally:
        if sink:
            sink.close()

    after = dev.state()
    after_health = dev.health()
    print("\n=== summary ===")
    for name in sorted(tally):
        print(f"  {name:<28} {tally[name]}")
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
