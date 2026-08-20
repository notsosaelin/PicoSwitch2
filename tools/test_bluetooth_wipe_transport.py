"""Regression guard for wipe-all transport teardown wiring."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "src/bt_hid/bt/btstack/btstack_host.c"


def main() -> None:
    source = HOST.read_text(encoding="utf-8")
    match = re.search(
        r"void btstack_host_delete_all_bonds\(void\)\s*\{(.*?)\n\}"
        r"\s*\n\s*bool btstack_host_get_last_connected",
        source,
        flags=re.DOTALL,
    )
    assert match, "could not locate btstack_host_delete_all_bonds"
    wipe = match.group(1)

    ordered_boundaries = (
        "pairing_lockout = true;",
        "gap_connect_cancel();",
        "btstack_host_stop_scan();",
        "gap_connectable_control(0);",
        "hci_disconnect_all();",
        "gap_delete_all_link_keys();",
        "btstack_host_store_pairing_lockout(true);",
    )
    offsets = []
    for boundary in ordered_boundaries:
        offset = wipe.find(boundary)
        assert offset >= 0, f"wipe lost required transport boundary: {boundary}"
        offsets.append(offset)
    assert offsets == sorted(offsets), (
        "wipe must lock admission, stop radio admission, terminate every HCI "
        "link, erase trust, then persist the lock"
    )

    # A tracked-slot loop is still useful for profile cleanup, but it cannot be
    # the sole transport teardown because raw HCI links may not own such a slot.
    disconnect_all = re.search(
        r"void btstack_host_disconnect_all_devices\(void\)\s*\{(.*?)\n\}",
        source,
        flags=re.DOTALL,
    )
    assert disconnect_all and "MAX_BLE_CONNECTIONS" in disconnect_all.group(1)
    assert "hci_disconnect_all();" in wipe

    print("Bluetooth wipe transport-boundary tests passed")


if __name__ == "__main__":
    main()
