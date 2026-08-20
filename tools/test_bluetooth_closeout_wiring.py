"""Source-level guards for Bluetooth closeout lifecycle wiring."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "src/bt_hid/bt/btstack/btstack_host.c"


def function_body(source: str, start: str, end: str) -> str:
    match = re.search(start + r"(.*?)" + end, source, flags=re.DOTALL)
    assert match, f"could not locate source region beginning {start!r}"
    return match.group(1)


def main() -> None:
    source = HOST.read_text(encoding="utf-8")

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

    print("Bluetooth closeout wiring tests passed")


if __name__ == "__main__":
    main()
