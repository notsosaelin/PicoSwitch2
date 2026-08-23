"""Source-level guards for Bluetooth closeout lifecycle wiring."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "src/bt_hid/bt/btstack/btstack_host.c"
AUDIO_BRIDGE = ROOT / "src/ds5_audio_bridge.c"
DS5_BT = ROOT / "src/bt_hid/bt/bthid/devices/vendors/sony/ds5_bt.c"
BT_TRANSPORT = ROOT / "src/bt_hid/ns2_bt_host.c"


def function_body(source: str, start: str, end: str) -> str:
    match = re.search(start + r"(.*?)" + end, source, flags=re.DOTALL)
    assert match, f"could not locate source region beginning {start!r}"
    return match.group(1)


def main() -> None:
    source = HOST.read_text(encoding="utf-8")

    check_le_appearance_is_a_host(source)

    start_wake = function_body(
        source,
        r"bool btstack_host_start_wake_advertisement\(",
        r"\n\}\s*\n\s*bool btstack_host_wake_advertisement_active",
    )
    clear_transient = function_body(
        source,
        r"static void btstack_host_clear_transient_radio_state\(void\)\s*\{",
        r"\n\}\s*\n\s*// =+\s*\n// HCI EVENT HANDLER",
    )
    for name, body in (
        ("wake start", start_wake),
        ("HCI transient reset", clear_transient),
    ):
        remove = body.find("btstack_run_loop_remove_timer(&wake_adv.timer);")
        clear = body.find("memset(&wake_adv, 0, sizeof(wake_adv));")
        assert 0 <= remove < clear, (
            f"{name} must detach the registered wake timer before clearing it"
        )

    # BTstack's process-global auto-accept cannot express per-attempt authority.
    # It stays disabled; the application responds from the admitted attempt latch.
    auto_accepts = re.findall(r"gap_ssp_set_auto_accept\((.*?)\);", source,
                              flags=re.DOTALL)
    assert [entry.strip() for entry in auto_accepts] == ["0"]
    assert "case HCI_EVENT_USER_CONFIRMATION_REQUEST:" in source
    assert "case HCI_EVENT_USER_PASSKEY_REQUEST:" in source
    assert source.count("ns2_bt_classic_ssp_response_admitted(") == 2
    assert "gap_ssp_confirmation_response(ssp_addr);" in source
    assert "gap_ssp_confirmation_negative(ssp_addr);" in source
    assert "gap_ssp_passkey_response(ssp_addr, 0u);" in source
    assert "gap_ssp_passkey_negative(ssp_addr);" in source

    working = function_body(
        source,
        r"case BTSTACK_EVENT_STATE:\s*\{",
        r"\n\s*case GAP_EVENT_ADVERTISING_REPORT:",
    )
    assert "ns2_bt_install_reset_bootstrap_take(" in working
    assert "&install_reset_bootstrap_consumed" in working

    check_controller_link_is_bound_to_management(source)
    check_management_bond_admission_is_latched(source)
    check_audio_sink_is_independent_of_input_ownership()
    check_controller_discovery_never_touches_management(source)

    print("Bluetooth closeout wiring tests passed")


def check_controller_discovery_never_touches_management(source: str) -> None:
    """Opening or closing controller discovery is not collateral damage for the
    management session.

    Field case 2026-08-21: a pairing window was opened while an Android management
    client was connected and a controller was already trying to pair. Management
    dropped, the controller handshake collapsed, and the radio wedged badly enough
    that the watchdog had to rescue the device.

    The chosen product behaviour is COEXISTENCE: controller discovery is a
    central-role LE scan plus Classic inquiry, while management owns the
    peripheral-role advertiser and its own ACL. They are different radio
    functions and the code already treats them as independent. If that ever has
    to become deliberate serialisation instead, it must be an explicit,
    reason-carrying retirement with recorded reconnect ownership -- never a side
    effect of a discovery routine. These guards make the silent version
    impossible to reintroduce.
    """
    regions = {
        "start_scan": function_body(
            source,
            r"void btstack_host_start_scan\(void\)\s*\{",
            r"\n\}\s*\n\s*void btstack_host_stop_scan",
        ),
        "stop_scan": function_body(
            source,
            r"void btstack_host_stop_scan\(void\)\s*\{",
            r"\n\}",
        ),
        "pairing_window": function_body(
            source,
            r"void btstack_host_set_pairing_window_open\(bool open\)\s*\{",
            r"\n\}",
        ),
        "disconnect_all": function_body(
            source,
            r"void btstack_host_disconnect_all_devices\(void\)\s*\{",
            r"\n\}",
        ),
    }
    for name, body in regions.items():
        assert "config_ble" not in body, (
            f"{name}() must not touch management session state; opening or "
            f"closing controller discovery cannot retire a management client"
        )

    # The BOOTSEL pairing gesture itself must not reach for management either.
    transport = BT_TRANSPORT.read_text(encoding="utf-8")
    open_window = function_body(
        transport,
        r"static void open_pairing_window\(uint32_t now_ms\) \{",
        r"\n\}",
    )
    for forbidden in ("config_ble", "mgmt_", "g_mgmt_enabled"):
        assert forbidden not in open_window, (
            f"open_pairing_window() must not reach into management state "
            f"({forbidden})"
        )

    # Liveness escalation must consult whether a security procedure is running,
    # so an ordinary pairing is never mistaken for a wedged radio.
    service = function_body(
        source,
        r"static void bt_health_service\(void\)\s*\{",
        r"\n\}",
    )
    assert ".security_in_flight = bt_health_security_in_flight()" in service, (
        "liveness escalation must know when an admitted security procedure owns "
        "the radio"
    )
    # And the escalation context must be captured before the reboot erases it.
    assert service.count("bt_health_note_escalation();") == 2, (
        "both the power-cycle and the reboot escalation must record a snapshot "
        "that survives the reboot"
    )


def check_controller_link_is_bound_to_management(source: str) -> None:
    """Controller Link lives and dies with the management session.

    PRODUCT INVARIANT. BLE management may run by itself; the Android companion's
    Controller Link may not. It may be established only while that same peer
    holds a connected, bonded, encrypted management session, and it must be torn
    down when that session is genuinely lost.

    Observed 2026-08-22: a degraded session left the Controller Link active with
    management genuinely disconnected. Admission reached the peer through its
    stored Classic link key alone, where it is no longer recognisable AS the
    companion -- companion trust is live state -- so it also fell through into
    the ordinary physical-controller security path and this host raced Android
    to start LMP authentication (the 0x23 collision).

    Three things are pinned here: the refusal happens at BOTH admission points,
    the teardown is wired into management disconnect, and the binding is cleared
    per-handle so a reused handle cannot inherit it.
    """
    # Refused at the HCI filter (before an ACL exists) and again at the
    # forwarded connection request.
    assert source.count("btstack_host_classic_admission_allowed(addr)") == 2, (
        "both Classic admission points must apply the management-required rule"
    )
    assert "ns2_bt_companion_classic_admission_allowed(" in source

    disconnect = function_body(
        source,
        r"static bool config_ble_handle_disconnect\(\s*\n?\s*hci_con_handle_t handle, uint8_t reason\)\s*\{",
        r"\n\}\s*\n\s*static void config_ble_service_task",
    )
    assert "classic_companion_release_on_mgmt_loss();" in disconnect, (
        "management disconnect must tear the Controller Link down; leaving it "
        "alive is the invariant violation captured on 2026-08-22"
    )

    # Teardown goes through the existing Classic close path rather than a second
    # mechanism that could disagree with it about released state.
    release = function_body(
        source,
        r"static void classic_companion_release_on_mgmt_loss\(void\)\s*\{",
        r"\n\}",
    )
    assert "gap_disconnect(link);" in release
    assert release.index("classic_companion_acl_handle = HCI_CON_HANDLE_INVALID;") < \
        release.index("gap_disconnect(link);"), (
        "clear the binding before disconnecting so the resulting Disconnection "
        "Complete cannot re-enter the teardown"
    )

    # The binding is per-handle state: cleared when that ACL drops, and on the
    # transient-radio reset that follows HCI loss.
    assert source.count("classic_companion_acl_handle = HCI_CON_HANDLE_INVALID;") == 4, (
        "the Controller Link binding must be initialised and cleared on its own "
        "disconnect, on HCI loss, and on teardown -- handles are reused"
    )

    # btauth attribution is per-ACL: every late field may only be recorded while
    # the record still owns the live link. Checked by call site rather than by a
    # magic total, so adding a legitimate new observation does not fail the guard
    # while dropping a guard silently would.
    assert "static bool auth_decision_owns(" in source
    for guarded in (
        "if (!auth_decision_owns(handle)) {",          # Authentication Complete
        "if (auth_decision_owns(handle)) {",           # Encryption Change
        "auth_decision_owns(acl->con_handle)",         # HID ready
        "status != ERROR_CODE_SUCCESS && auth_decision_owns(handle)",  # auth failed
    ):
        assert guarded in source, (
            f"per-ACL attribution missing its ownership check: {guarded!r}"
        )
    assert "last_auth_decision.link_closed = true;" in source, (
        "the record must be retired on disconnect so a reused handle cannot "
        "inherit it"
    )

    # The key size must be sampled where it is valid. For Classic, BTstack sends
    # HCI_Read_Encryption_Key_Size AFTER the Encryption Change event, so reading
    # it in that handler records 0 on every peer-led link and reads as a security
    # failure. It is sampled at the HID-ready acceptance gate instead, from the
    # exact value that gate judged.
    assignments = re.findall(r"last_auth_decision\.encryption_key_size\s*=\s*([^;]+);",
                             source)
    assert assignments == ["key_size"], (
        "the Classic key size must be recorded exactly once, from the value the "
        f"HID-ready acceptance gate judged; found {assignments!r}. Sampling "
        "gap_encryption_key_size() in the Encryption Change handler records 0, "
        "because BTstack only issues HCI_Read_Encryption_Key_Size after that event"
    )
    assert "last_auth_decision.key_size_valid = true;" in source

    # Authentication observation is tri-state: "not observed" is the EXPECTED
    # value on the peer-led path and must never be representable as failure.
    assert "bool auth_completed_ok" not in source, (
        "a bool cannot distinguish 'authentication failed' from 'this host "
        "deliberately never asked'; see ns2_bt_auth_observation_t"
    )
    for outcome in ("NS2_BT_AUTH_OBSERVED_OK", "NS2_BT_AUTH_OBSERVED_FAILED"):
        assert outcome in source


def check_management_bond_admission_is_latched(source: str) -> None:
    """Management fresh-bond admission is per-attempt, like controller candidates.

    Confirmed on hardware 2026-08-21: the management peripheral used to re-read the
    live 30 s pairing window at SM confirmation time, which sits AFTER Android's own
    pairing dialog. Authorization the user had already given could therefore expire
    mid-procedure. The window must be sampled when the connection is ACCEPTED and
    latched for that connection only.
    """
    accept = function_body(
        source,
        r"static bool config_ble_accept_connection\(",
        r"\n\}\s*\n\s*static bool config_ble_handle_disconnect",
    )
    assert "config_ble.fresh_bond_admitted = config_ble_bond_admission_open();" in accept, (
        "the fresh-bond decision must be latched at connection admission"
    )
    assert accept.index("config_ble.handle = handle;") < accept.index(
        "config_ble.fresh_bond_admitted ="
    ), "the admission test reads config_ble.handle, so the latch must follow the assignment"

    decide = function_body(
        source,
        r"static bool config_ble_accept_new_bond\(void\)\s*\{",
        r"\n\}",
    )
    # SM confirmation consults the latch and the runtime feature switch, never the
    # live window: `pairing_window_open` must not reappear on this path.
    assert "mgmt_accept_latched_bonding(" in decide
    assert "config_ble.fresh_bond_admitted" in decide
    assert "hid_pairing_window_open" not in decide, (
        "SM confirmation must not re-read the live pairing window"
    )
    assert "mgmt_accept_bonding(" not in decide

    # `mgmt off` must still revoke a latched attempt.
    assert "g_mgmt_enabled" in decide

    # The latch is bounded by its own connection: cleared on disconnect and on the
    # transient-radio reset that follows HCI loss.
    assert source.count("config_ble.fresh_bond_admitted = false;") == 2


def check_audio_sink_is_independent_of_input_ownership() -> None:
    """Audio sink ownership follows the physical audio-capable link, not the arbiter.

    Confirmed on hardware 2026-08-21: with Controller Link selected as the active
    CONSOLE INPUT source, audio kept flowing to the physically connected DualSense.
    That is the intended product behaviour -- the Android bridge cannot transport
    controller audio, so tying the audio sink to input ownership would silence a
    working headset for no reason.

    These are two deliberately independent ownership domains:
      * console input owner  -> ns2_input_arbiter / ns2_active_input
      * audio sink owner     -> ds5_audio_bridge, bound at DS5 HID connect
    Neither reads the other, and nothing here may start doing so.
    """
    bridge = AUDIO_BRIDGE.read_text(encoding="utf-8")
    for forbidden in ("ns2_active_input", "ns2_input_arbiter", "active_id"):
        assert forbidden not in bridge, (
            f"ds5_audio_bridge.c must not consult console input ownership ({forbidden})"
        )

    # The sink is claimed and released purely by the audio-capable link's own
    # HID lifecycle.
    assert "void ds5_audio_bridge_connect(uint8_t conn_index)" in bridge
    assert "void ds5_audio_bridge_disconnect(uint8_t conn_index)" in bridge
    owns = re.findall(
        r"bool ds5_audio_bridge_owns_connection\(uint8_t conn_index\) \{\s*"
        r"return bridge_connected && bridge_conn_index == conn_index;",
        bridge,
    )
    assert len(owns) == 2, (
        "both audio builds must gate on connection identity alone, not input ownership"
    )

    ds5 = DS5_BT.read_text(encoding="utf-8")
    audio_task = function_body(
        ds5,
        r"static void ds5_audio_task\(bthid_device_t \*device, ds5_bt_data_t \*ds5,\s*\n\s*bool run_codec\) \{",
        r"\n\}\s*\n",
    )
    assert "if (!ds5_audio_bridge_owns_connection(device->conn_index)) return;" in audio_task
    for forbidden in ("ns2_active_input", "ns2_input_arbiter"):
        assert forbidden not in audio_task, (
            "the DS5 audio task must not gate on console input ownership"
        )
    # The claim/release sites are the DS5 HID lifecycle hooks and nothing else.
    assert ds5.count("ds5_audio_bridge_connect(device->conn_index);") == 1
    assert ds5.count("ds5_audio_bridge_disconnect(device->conn_index);") == 1


def check_le_appearance_is_a_host(source: str) -> None:
    """The LE management Appearance must never describe a HID peripheral.

    PicoSwitch2 is the HID *host* for Controller Link. It previously advertised
    GAP Appearance 0x03C0 (Generic HID), and Android turns that into a stored
    Class of Device with major class 5 (Peripheral) whenever it pairs with no
    Classic class on record. Its own HID Device profile then refuses the
    adapter -- `btif_hd` checks `check_cod_hid()`, `(cod & 0x1F00) == 0x0500`,
    at `BTA_HD_OPEN_EVT` -- after the ACL, authentication, encryption and both
    HID channels have already succeeded. Controller Link fails deterministically
    and nothing about the failure points at the Appearance.

    The firmware carries a `_Static_assert` for this too. This is the second
    guard because the constant is easy to "correct" back to a HID value by
    someone reasoning that the adapter carries HID traffic.
    """
    match = re.search(r"#define HOST_ATT_APPEARANCE\s+0x([0-9A-Fa-f]{4})u", source)
    assert match, "HOST_ATT_APPEARANCE is no longer a single named constant"
    appearance = int(match.group(1), 16)
    assert appearance & 0xFFC0 != 0x03C0, (
        f"LE Appearance 0x{appearance:04X} is in the HID range (0x03C0-0x03FF); "
        "Android will store a HID-peripheral Class of Device for the adapter and "
        "refuse the Controller Link"
    )
    # And it must stay coherent with the Classic identity we actually advertise.
    assert "gap_set_class_of_device(0x000104)" in source, (
        "Classic Class of Device changed; re-check that it still agrees with the "
        "LE Appearance (both should describe a computer/host, not a peripheral)"
    )


if __name__ == "__main__":
    main()
