"""Every failure must have a classification, and it must name the right domain.

Fixtures are real lines from the 2026-08-23 soak captures, trimmed. The point of
this file is the property the soak lacked: a dependency failure can never be
reported as a Bluetooth failure, and no failure can come back as a bare label.

    python tools/test_controller_link_classification.py
"""

import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location(
    "clc", ROOT / "tools" / "controller_link_cycle.py")
clc = importlib.util.module_from_spec(_spec)
sys.modules["clc"] = clc
_spec.loader.exec_module(clc)


# --- fixtures, all copied from real captures -------------------------------

SUCCESS = """\
08-23 06:56:15.174 D/PicoSwitch( 3953): management/result: input: complete (99 bytes) seq=897 gatt=4 elapsedMs=101
08-23 06:56:17.196 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 06:56:17.236 D/PicoSwitch( 3953): transport/HID profile: acquiring
08-23 06:56:17.269 D/PicoSwitch( 3953): transport/HID registration: registered
08-23 06:56:17.273 D/PicoSwitch( 3953): transport/HID connection: requested accepted=true hostOk=true
08-23 06:56:17.276 D/PicoSwitch( 3953): transport/HID connection state: connecting
08-23 06:56:19.413 D/bt_btm_sec(10280): btm_sec_execute_procedure: Outgoing authentication/encryption Required
08-23 06:56:19.414 V/bt_btif_dm(10280): btif_dm_acl_evt: BTA_DM_LINK_UP_EVT. Sending BT_ACL_STATE_CONNECTED
08-23 06:56:19.509 V/bt_btm_sec(10280): btm_sec_execute_procedure: Security Manager: Start encryption
08-23 06:56:19.602 D/bt_btm_sec(10280): btm_sec_encrypt_change: Security Manager encryption change request hci_status:HCI_SUCCESS
08-23 06:56:19.602 D/BluetoothRemoteDevices(10280): encryptionChangeCallback device: XX, status: 0, enabled: true
08-23 06:56:19.645 D/PicoSwitch( 3953): transport/HID connection state: connected
08-23 06:56:19.651 D/PicoSwitch( 3953): bridge/link up: interrupt link ready
"""

LE_TIMEOUT = """\
08-23 06:56:36.558 D/PicoSwitch( 3953): management/result: input: complete (99 bytes) seq=901 gatt=4 elapsedMs=128
08-23 06:56:37.090 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 06:56:37.124 D/PicoSwitch( 3953): transport/HID profile: acquiring
08-23 06:56:37.161 D/PicoSwitch( 3953): transport/HID registration: registered
08-23 06:56:37.165 D/PicoSwitch( 3953): transport/HID connection: requested accepted=true hostOk=true
08-23 06:56:37.182 D/PicoSwitch( 3953): transport/HID connection state: connecting
08-23 06:56:40.439 D/bt_btm_sec(10280): btm_sec_execute_procedure: Outgoing authentication/encryption Required
08-23 06:56:40.440 V/bt_btif_dm(10280): btif_dm_acl_evt: BTA_DM_LINK_UP_EVT. Sending BT_ACL_STATE_CONNECTED
08-23 06:56:40.685 V/bt_btm_sec(10280): btm_sec_execute_procedure: Security Manager: Start encryption
08-23 06:56:41.230 D/bt_btm_sec(10280): btm_sec_encrypt_change: Security Manager encryption change request hci_status:HCI_SUCCESS
08-23 06:56:41.231 D/BluetoothRemoteDevices(10280): encryptionChangeCallback device: XX, status: 0, enabled: true
08-23 06:56:42.122 D/bt_btif_dm(10280): btif_dm_acl_evt: Sent BT_ACL_STATE_DISCONNECTED upward as ACL link down event device:xx reason:HCI_ERR_PEER_USER
08-23 06:56:43.660 D/PicoSwitch( 3953): transport/HID connection rejected: elapsedMs=6491
08-23 06:56:46.955 D/bt_btif_dm(10280): btif_dm_acl_evt: Sent BT_ACL_STATE_DISCONNECTED upward as ACL link down event device:xx reason:HCI_ERR_CONNECTION_TOUT
08-23 06:56:46.996 D/PicoSwitch( 3953): management/gatt.error: attempt=7 gatt=4 stage=connect status=8
"""

PAGE_TIMEOUT = """\
08-23 06:51:50.798 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 06:51:50.875 D/PicoSwitch( 3953): transport/HID connection: requested accepted=true hostOk=true
08-23 06:51:50.891 D/PicoSwitch( 3953): transport/HID connection state: connecting
08-23 06:51:56.070 D/bt_btm_sec(10280): btm_sec_disconnected: device:xx reason:PAGE_TIMEOUT
08-23 06:51:56.101 D/PicoSwitch( 3953): transport/HID connection rejected: elapsedMs=5215
"""

ANDROID_BT_DIED = """\
08-23 06:00:00.000 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 06:00:01.000 E/AndroidRuntime(  999): Process com.android.bluetooth has died
"""


def test_success_window_is_never_a_device_failure():
    # classify() is only invoked on failures, but a classifier that can invent
    # one from a healthy window is worse than none at all.
    outcome, _ = clc.classify(SUCCESS)
    assert outcome == clc.OK, outcome


def test_le_supervision_timeout_is_named_and_evidenced():
    timeline = clc.build_timeline(LE_TIMEOUT)
    outcome, detail = clc.classify(LE_TIMEOUT, timeline)
    assert outcome == clc.DEV_MGMT_LE_TIMEOUT, outcome
    # The HCI reason is reported, not inferred.
    assert detail["acl_down_reason"] == "HCI_ERR_CONNECTION_TOUT"
    # And it says how far Classic had progressed, which is the divergence point.
    assert "classic.enc_change" in detail["classic_stage_reached"]
    assert detail["last_le_txn_at"].startswith("08-23 06:56:36")


def test_divergence_point_is_visible_between_success_and_failure():
    good = {row["event"]: row["t_rel"] for row in clc.build_timeline(SUCCESS)}
    bad = {row["event"]: row["t_rel"] for row in clc.build_timeline(LE_TIMEOUT)}
    # Both reach encryption...
    for shared in ("app.touch_opened", "classic.acl_up", "classic.enc_change"):
        assert shared in good and shared in bad, shared
    # ...and exactly here they part company.
    assert "app.state_connected" in good and "app.link_up" in good
    assert "app.state_connected" not in bad and "app.link_up" not in bad
    assert "fail.acl_down" in bad and "fail.acl_down" not in good
    # Offsets are relative to the touch open so two attempts can be compared.
    assert good["app.touch_opened"] == 0.0
    assert good["app.link_up"] > good["classic.enc_change"]


def test_page_timeout_reports_paging_duration():
    outcome, detail = clc.classify(PAGE_TIMEOUT)
    assert outcome == clc.DEV_CLASSIC_PAGE_TIMEOUT, outcome
    assert detail["paging_seconds"] is not None
    assert 5.0 < detail["paging_seconds"] < 5.5, detail["paging_seconds"]
    # Whether the ACL ever came up separates "never answered the page" from
    # "answered and then failed later".
    assert detail["acl_up_observed"] is False


def test_android_bluetooth_death_is_an_app_failure_not_a_device_one():
    outcome, _ = clc.classify(ANDROID_BT_DIED)
    assert outcome == clc.APP_BT_PROCESS_DIED
    assert clc.domain(outcome) == "app"


def test_every_outcome_has_a_domain_and_only_device_ones_are_device():
    for name in dir(clc):
        if name.startswith(("DEV_", "APP_", "HARNESS_")):
            value = getattr(clc, name)
            expected = {"DEV": "device", "APP": "app", "HARNESS": "harness"}[
                name.split("_", 1)[0]]
            assert clc.domain(value) == expected, (name, value)
    assert clc.domain(clc.OK) == "ok"
    assert clc.domain(clc.EXCLUDED) == "excluded"


def test_unclassifiable_window_is_explicitly_unknown_with_evidence():
    outcome, detail = clc.classify("08-23 06:00:00.000 D/nothing: quiet\n")
    assert outcome == clc.DEV_UNKNOWN_TIMEOUT
    # UNKNOWN must still carry what was seen, or it is just "failed" again.
    assert "markers_seen" in detail


def test_shell_result_cannot_look_like_success_when_it_failed():
    # The precise shape of the cycle-491 laundering: empty stdout, error on
    # stderr, non-zero code. `.ok` must be False.
    bad = clc.ShellResult(stdout="", stderr="error: no devices/emulators found",
                          code=1)
    assert not bad.ok
    assert clc.ShellResult(stdout="device\n", code=0).ok
    assert not clc.ShellResult(timed_out=True).ok


CONNECTION_ALREADY_EXISTS = """08-23 08:42:31.257 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 08:42:31.285 D/PicoSwitch( 3953): transport/HID profile: acquiring
08-23 08:42:31.312 D/PicoSwitch( 3953): transport/HID registration: registered
08-23 08:42:31.316 D/PicoSwitch( 3953): transport/HID connection: requested accepted=true hostOk=true
08-23 08:42:31.324 D/PicoSwitch( 3953): transport/HID connection state: connecting
08-23 08:42:31.359 D/PicoSwitch( 3953): management/result: input: complete (98 bytes) elapsedMs=135
08-23 08:42:35.926 W/bluetooth(10280): OnConnectFail: Connection failed classic remote:xx reason:CONNECTION_ALREADY_EXISTS(0x0b)
08-23 08:42:39.316 D/PicoSwitch( 3953): controller/bridge.phase: Failed
08-23 08:42:39.318 D/PicoSwitch( 3953): transport/HID connection: callback timeout after 8001ms bond=12 type=3
"""

NEVER_REQUESTED = """08-23 08:42:31.257 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 08:42:31.285 D/PicoSwitch( 3953): transport/HID profile: acquiring
08-23 08:42:39.316 D/PicoSwitch( 3953): controller/bridge.phase: Failed
"""


def test_phone_side_connect_refusal_is_not_a_paging_failure():
    """The 2026-08-23 cycle-3 case, and the whole point of this split.

    CONNECTION_ALREADY_EXISTS means the phone's own stack abandoned the connect;
    no page is transmitted, so the adapter is uninvolved. Reporting it as
    CLASSIC_PAGE_TIMEOUT would have inflated the paging failure count by 11% and
    sent the investigation at the wrong radio.
    """
    outcome, detail = clc.classify(CONNECTION_ALREADY_EXISTS)
    assert outcome == clc.APP_CONNECTION_STATE_FAILURE, outcome
    assert clc.domain(outcome) == "app"
    assert detail["connect_fail_reason"] == "CONNECTION_ALREADY_EXISTS(0x0b)"
    # It must record that the app DID try -- that is what separates this from
    # "the app refused to start".
    assert detail["hid_registered"] is True
    assert detail["connect_requested"] is True


def test_page_timeout_survives_the_new_ordering():
    # The reason check runs first, so PAGE_TIMEOUT must still reach the device
    # verdict rather than being swallowed as a phone-side refusal.
    outcome, detail = clc.classify(PAGE_TIMEOUT)
    assert outcome == clc.DEV_CLASSIC_PAGE_TIMEOUT, outcome
    assert clc.domain(outcome) == "device"


def test_no_connect_attempt_is_named_separately():
    outcome, _ = clc.classify(NEVER_REQUESTED)
    assert outcome == clc.APP_NO_CONNECT_ATTEMPT, outcome
    assert clc.domain(outcome) == "app"


def test_connect_fail_reason_takes_the_last_occurrence():
    doubled = (CONNECTION_ALREADY_EXISTS +
               "08-23 08:42:44.000 W/bluetooth(1): OnConnectFail: "
               "reason:PAGE_TIMEOUT(0x04)\n")
    assert "PAGE_TIMEOUT" in clc.connect_fail_reason(doubled)
    assert clc.connect_fail_reason("nothing here") is None


ACL_TIMEOUT_AFTER_PAGE = """08-23 10:02:04.690 D/PicoSwitch( 3953): controller/touch gamepad: opened
08-23 10:02:04.756 D/PicoSwitch( 3953): transport/HID registration: registered
08-23 10:02:04.760 D/PicoSwitch( 3953): transport/HID connection: requested accepted=true hostOk=true
08-23 10:02:04.763 D/PicoSwitch( 3953): transport/HID connection state: connecting
08-23 10:02:12.767 D/PicoSwitch( 3953): transport/HID connection: callback timeout after 8002ms
08-23 10:02:29.117 W/bluetooth(10280): OnConnectFail: Connection failed classic remote:xx reason:CONNECTION_TIMEOUT(0x08)
"""


def test_acl_timeout_after_a_received_page_is_a_device_failure():
    """0x08 is NOT a phone-side refusal, and the earlier rule got this wrong.

    40-cycle run, cycle 3: the adapter recorded page_rx AND page_accept at
    10:02:08.846, then both sides reported HCI 0x08 CONNECTION_TIMEOUT 24.4 s
    after the request. The page was received and answered; establishment failed
    afterwards. Excluding only PAGE_TIMEOUT would have filed this under
    app:CONNECTION_STATE_FAILURE and hidden a real link failure.
    """
    outcome, detail = clc.classify(ACL_TIMEOUT_AFTER_PAGE)
    assert outcome == clc.DEV_CLASSIC_ACL_TIMEOUT, outcome
    assert clc.domain(outcome) == "device"
    assert detail["connect_fail_reason"] == "CONNECTION_TIMEOUT(0x08)"


def test_phone_state_reasons_are_whitelisted_not_inferred():
    # Only these mean "the phone declined locally". Anything else must not be
    # laundered into the app domain just because it is not PAGE_TIMEOUT.
    for reason in clc.PHONE_STATE_REASONS:
        log = (CONNECTION_ALREADY_EXISTS.replace(
            "CONNECTION_ALREADY_EXISTS(0x0b)", f"{reason}(0xff)"))
        outcome, _ = clc.classify(log)
        assert outcome == clc.APP_CONNECTION_STATE_FAILURE, (reason, outcome)
    assert "CONNECTION_TIMEOUT" not in clc.PHONE_STATE_REASONS


def test_missing_or_partial_logcat_never_raises():
    """The cycle-82 crash shape: None reaching build_timeline().

    A decode failure inside subprocess's reader thread left stdout as None, and
    the visible error was AttributeError three frames from the cause. Nothing in
    the parsing path may raise on absent input, and none of it may invent a
    device verdict from it.
    """
    for empty in (None, "", "   ", "garbage with no timestamps"):
        assert clc.build_timeline(empty) == [] or isinstance(
            clc.build_timeline(empty), list)
        assert clc.acl_down_reason(empty) is None
        assert clc.connect_fail_reason(empty) is None
        outcome, detail = clc.classify(empty)
        # Unknown, with evidence -- never a specific device failure invented
        # from nothing.
        assert outcome == clc.DEV_UNKNOWN_TIMEOUT, (empty, outcome)
        assert "markers_seen" in detail


def test_adb_output_is_always_a_string():
    # ShellResult must never carry None, whatever subprocess hands back.
    assert clc.ShellResult().stdout == ""
    assert clc.ShellResult().stderr == ""
    original = _with_adb(lambda *a, **k: clc.ShellResult(stdout="x", code=0))
    try:
        assert isinstance(clc.adb("shell", "true"), str)
    finally:
        clc.adb_run = original


def test_adb_run_survives_undecodable_bytes():
    """The real wrapper, against the exact byte that killed cycle 82.

    0x9d is undefined in cp1252. With text=True the decode raised inside
    subprocess's reader THREAD, so subprocess.run returned with stdout as None
    instead of propagating, and the visible failure surfaced three frames later
    as AttributeError on None.splitlines(). Decoding must be explicit UTF-8 with
    errors="replace" so the byte becomes a replacement character and the run
    continues.
    """
    import subprocess as sp
    bad = bytes((0x41, 0x9D, 0x42))          # "A", undecodable in cp1252, "B"
    real = clc.subprocess.run

    def fake_run(args, **kwargs):
        assert kwargs.get("encoding") == "utf-8", (
            "adb output must not be decoded with the locale codec")
        assert kwargs.get("errors") == "replace"
        return sp.CompletedProcess(
            args, 0, bad.decode(kwargs["encoding"], errors=kwargs["errors"]), "")

    clc.subprocess.run = fake_run
    try:
        result = clc.adb_run("logcat", "-d")
        assert result.ok
        assert result.stdout.startswith("A") and result.stdout.endswith("B")
    finally:
        clc.subprocess.run = real


def test_adb_serial_is_applied_to_every_wrapped_command():
    import subprocess as sp
    real_run = clc.subprocess.run
    real_serial = clc.ADB_SERIAL
    seen = []

    def fake_run(args, **kwargs):
        seen.append(args)
        return sp.CompletedProcess(args, 0, "device\n", "")

    clc.subprocess.run = fake_run
    clc.ADB_SERIAL = "transport-serial"
    try:
        assert clc.adb_run("get-state").ok
        assert seen == [("adb", "-s", "transport-serial", "get-state")]
    finally:
        clc.ADB_SERIAL = real_serial
        clc.subprocess.run = real_run


def _with_adb(stub):
    """Swap adb_run for the duration of a test."""
    original = clc.adb_run
    clc.adb_run = stub
    return original


def test_ui_dump_without_nodes_is_a_failed_dump_not_a_missing_button():
    original = _with_adb(lambda *a, **k: clc.ShellResult(stdout="", code=0))
    try:
        xml, problem = clc.ui_dump_checked()
        assert problem == clc.HARNESS_UI_DUMP_FAILED, problem
        assert clc.domain(problem) == "harness"
    finally:
        clc.adb_run = original


def test_adb_transport_loss_is_reported_as_adb_loss():
    # The exact cycle-491 shape: the error is on stderr, stdout is empty.
    original = _with_adb(lambda *a, **k: clc.ShellResult(
        stdout="", stderr="error: no devices/emulators found", code=1))
    try:
        assert clc.check_adb_health() == clc.HARNESS_ADB_DEVICE_LOST
    finally:
        clc.adb_run = original


def test_healthy_adb_passes():
    original = _with_adb(lambda *a, **k: clc.ShellResult(stdout="device\n", code=0))
    try:
        assert clc.check_adb_health() is None
    finally:
        clc.adb_run = original


def test_owner_check_fails_closed_when_state_cannot_be_read():
    # Previously an unreadable dump yielded 0 matches and `0 > 1` passed, so the
    # check "succeeded" on every dead cycle of the 2026-08-23 ADB outage.
    original = _with_adb(lambda *a, **k: clc.ShellResult(stdout="", code=1))
    try:
        assert clc.check_single_owner() == clc.HARNESS_STATE_UNKNOWN
    finally:
        clc.adb_run = original


def test_owner_check_names_duplicate_ownership_when_readable():
    record = ("      * Hist  #{n}: ActivityRecord{{abc u0 "
              "dev.picoswitch.companion.debug/"
              "dev.picoswitch.companion.MainActivity t515}}")
    two = "\n".join(record.format(n=i) for i in range(2))
    original = _with_adb(lambda *a, **k: clc.ShellResult(stdout=two, code=0))
    try:
        assert clc.check_single_owner() == clc.APP_DUPLICATE_OWNER
    finally:
        clc.adb_run = original
    one = record.format(n=0)
    original = _with_adb(lambda *a, **k: clc.ShellResult(stdout=one, code=0))
    try:
        assert clc.check_single_owner() is None
    finally:
        clc.adb_run = original


def test_owner_check_ignores_other_apps_named_main_activity():
    companion = ("      * Hist  #0: ActivityRecord{abc u0 "
                 "dev.picoswitch.companion.debug/"
                 "dev.picoswitch.companion.MainActivity t515}")
    launcher = ("      * Hist  #0: ActivityRecord{def u0 "
                "rip.moth.cocoonshell/.MainActivity t54}")
    original = _with_adb(lambda *a, **k: clc.ShellResult(
        stdout=companion + "\n" + launcher, code=0))
    try:
        assert clc.check_single_owner() is None
    finally:
        clc.adb_run = original


def main() -> int:
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
    print(f"controller link classification tests passed ({len(tests)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
