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
