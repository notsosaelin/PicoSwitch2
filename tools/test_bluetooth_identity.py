"""Structural regression checks for the PicoSwitch2 Bluetooth product name.

This test intentionally checks source-owned identity surfaces rather than a
generated binary. A board build still remains the evidence that BTstack links
the checked-in GAP/ATT/EIR paths.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
BTSTACK = ROOT / "src" / "bt_hid" / "bt" / "btstack" / "btstack_host.c"
RUNTIME_SOURCES = tuple(
    path
    for path in (ROOT / "src").rglob("*")
    if path.is_file() and path.suffix.lower() in {".c", ".h", ".cc", ".cpp"}
)

CURRENT_NAME = "PicoSwitch2"
LEGACY_NAME = "Joypad Adapter"


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    source = BTSTACK.read_text(encoding="utf-8")
    _check(
        f'#define PICO_SWITCH2_BLUETOOTH_NAME "{CURRENT_NAME}"' in source,
        "firmware must define the canonical Bluetooth name",
    )
    _check(
        "gap_set_local_name(PICO_SWITCH2_BLUETOOTH_NAME);" in source,
        "Classic GAP local name must use the canonical Bluetooth name",
    )
    _check(
        f"static uint8_t host_att_device_name[] = PICO_SWITCH2_BLUETOOTH_NAME;" in source,
        "ATT Device Name must use the canonical Bluetooth name",
    )
    _check(
        LEGACY_NAME not in source,
        "legacy product name must not remain in firmware runtime source",
    )

    response = re.search(
        r"static uint8_t config_ble_scan_response\[\] = \{(.*?)\};",
        source,
        re.DOTALL,
    )
    _check(response is not None, "Config BLE scan response initializer is missing")
    body = response.group(1)
    chars = re.findall(r"'([^']+)'", body)
    _check(chars == list(CURRENT_NAME), "scan response must contain PicoSwitch2 bytes")
    length = re.search(
        r"\(uint8_t\)\(PICO_SWITCH2_BLUETOOTH_NAME_LEN \+ 1u\)", body,
    )
    _check(length is not None, "scan response AD length must derive from the name length")
    _check(
        len(CURRENT_NAME.encode("ascii")) == 11,
        "PicoSwitch2 byte length changed unexpectedly",
    )
    _check(len(chars) + 1 == 12, "complete-local-name AD length must be 0x0C")
    _check(len(chars) + 2 == 13, "complete-local-name AD must occupy 13 bytes")

    for path in RUNTIME_SOURCES:
        _check(
            LEGACY_NAME not in path.read_text(encoding="utf-8"),
            f"legacy product name leaked into firmware source: {path}",
        )

    print("Bluetooth identity checks passed: PicoSwitch2 (11 ASCII bytes), legacy runtime-free")


if __name__ == "__main__":
    main()
