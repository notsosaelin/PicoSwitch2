"""Regression guard for production Bluetooth diagnostic secret boundaries."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/bt_hid/bt/btstack/btstack_host.h"
UART = ROOT / "src/ns2_uart_diag.c"


def require_region(text: str, pattern: str, description: str) -> str:
    match = re.search(pattern, text, flags=re.DOTALL)
    if not match:
        raise AssertionError(f"could not locate {description}")
    return match.group(0)


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    uart = UART.read_text(encoding="utf-8")

    reconnect_struct = require_region(
        header,
        r"typedef struct \{.*?\} btstack_host_reconnect_diag_t;",
        "reconnect diagnostic structure",
    )
    reconnect_command = require_region(
        uart,
        r'else if \(strcmp\(rx_line, "btreconnect"\) == 0\).*?'
        r'else if \(strcmp\(rx_line, "btfresh"\) == 0\)',
        "btreconnect UART command",
    )
    reconnect_output = reconnect_command.replace(r'\"', '"')

    forbidden_members = (
        "pairing_ltk_raw[",
        "pairing_ltk_normalized[",
        "link_key[",
        "irk[",
        "pin[",
    )
    forbidden_output_fields = (
        '"spi_ltk_raw"',
        '"spi_ltk_normalized"',
        '"link_key"',
        '"irk"',
        '"pin"',
    )

    for token in forbidden_members:
        assert token not in reconnect_struct, (
            f"production reconnect diagnostics expose secret member {token!r}"
        )
        assert token not in reconnect_command, (
            f"btreconnect serializes secret storage {token!r}"
        )
    for field in forbidden_output_fields:
        assert field not in reconnect_output, (
            f"btreconnect emits forbidden secret field {field!r}"
        )

    # Safe metadata remains observable: removal of key bytes must not erase the
    # evidence needed to distinguish an unread key, byte-order disagreement,
    # or a failed re-encryption attempt.
    for safe_field in (
        '"spi_ltk_valid"',
        '"spi_norm_matches_derived"',
        '"spi_raw_matches_derived"',
        '"reencrypt_status"',
        '"link_key_size"',
    ):
        assert safe_field in reconnect_output, (
            f"btreconnect lost safe diagnostic metadata {safe_field!r}"
        )

    print("Bluetooth diagnostic secret-boundary tests passed")


if __name__ == "__main__":
    main()
