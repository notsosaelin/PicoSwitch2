#!/usr/bin/env python3
"""Structural guards for the Pico SDK 2.3.0 / BTstack 1.8.2 dependency contract.

These pin decisions from the Stage C modernization
(docs/experiments/pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md) that a
compiler cannot pin, because in every case the wrong thing still compiles:

  * the HIDS Host pool macro was renamed, and BTstack silently builds a
    zero-entry pool if the old spelling comes back;
  * upstream's HID Host send guard accepts a state the DualSense audio path must
    not be accepted in, so "simplifying" the local shim away is a silent race;
  * BTstack 1.8.2 flipped LE Secure Connections Only ON by default, so deleting
    the explicit call restores the flipped default rather than the old one;
  * the SDK version is declared in two places that override each other in a
    non-obvious order.

Run: python tools/test_btstack_dependency_contract.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKELISTS = ROOT / "CMakeLists.txt"
BUILD_PS1 = ROOT / "build.ps1"
BTSTACK_CONFIG = ROOT / "src" / "btstack_config.h"
BTSTACK_HOST = ROOT / "src" / "bt_hid" / "bt" / "btstack" / "btstack_host.c"
HID_HOST_SHIM = ROOT / "cmake" / "btstack_hid_host_long_report.c"

# Directories whose contents are ours to keep migrated. Vendored trees and
# captured evidence are deliberately excluded.
FIRMWARE_SOURCE_DIRS = ("src", "include", "cmake", "8Bitdo")


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def code_lines(text):
    """Lines with `//` comments and comment-only lines removed.

    Retired names are supposed to survive in comments -- that is where the
    negative knowledge lives. Only their use as code is a regression.
    """
    out = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("*") or \
                stripped.startswith("/*"):
            continue
        out.append(line.split("//", 1)[0])
    return out


def firmware_sources():
    for directory in FIRMWARE_SOURCE_DIRS:
        base = ROOT / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix in (".c", ".h") and path.is_file():
                yield path


def check_hids_host_naming():
    """BTstack 1.8 renamed hids_client -> hids_host, pool macro included.

    The rename matters beyond tidiness: btstack_memory.c keys the connection
    pool off MAX_NR_HIDS_HOSTS, and #defining only the retired
    MAX_NR_HIDS_CLIENTS leaves MAX_NR_HIDS_HOSTS undefined -> a pool of zero.
    That builds and links; BLE HID connections just fail to allocate.
    """
    config = read(BTSTACK_CONFIG)
    assert re.search(r"^#define\s+MAX_NR_HIDS_HOSTS\s+[1-9]", config, re.M), (
        "btstack_config.h must define MAX_NR_HIDS_HOSTS with a non-zero pool"
    )
    assert not any("MAX_NR_HIDS_CLIENTS" in line for line in code_lines(config)), (
        "MAX_NR_HIDS_CLIENTS is ignored by BTstack 1.8.2, which then builds a "
        "zero-entry HIDS Host pool"
    )

    stale = [
        path.relative_to(ROOT).as_posix()
        for path in firmware_sources()
        if any("hids_client" in line for line in code_lines(read(path)))
    ]
    assert not stale, f"BTstack 1.8.2 has no hids_client API; still referenced in {stale}"


def check_removed_protocol_mode_is_not_reintroduced():
    """HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT no longer exists.

    bt_device_db.h records why nothing here wanted it. A comment may name it;
    code may not use it.
    """
    offenders = []
    for path in firmware_sources():
        for lineno, line in enumerate(code_lines(read(path)), 1):
            if "HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT" in line:
                offenders.append(f"{path.relative_to(ROOT).as_posix()}:~{lineno}")
    assert not offenders, (
        "HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT was removed in BTstack "
        f"1.8.2; still used at {offenders}"
    )


def check_hid_host_send_guard_survives():
    """The shim must refuse a send while one is pending, not widen to upstream's range.

    Upstream hid_host_send_report() accepts
    [HID_HOST_CONNECTION_ESTABLISHED, HID_HOST_W4_INTERRUPT_CONNECTION_DISCONNECTED),
    and HID_HOST_W2_SEND_REPORT is inside it -- so report B overwrites
    connection->report while report A is still queued for L2CAP. The DS5 audio
    path retries every 2 ms service pass and hits that window routinely.
    """
    shim = read(HID_HOST_SHIM)
    shim_code = "\n".join(code_lines(shim))

    assert "connection->state != HID_HOST_CONNECTION_ESTABLISHED" in shim_code, (
        "the shim must accept only the exact idle established state; anything "
        "broader lets a pending report be overwritten"
    )
    # Upstream's two-sided range check is precisely what must NOT be copied here.
    assert "connection->state < HID_HOST_CONNECTION_ESTABLISHED" not in shim_code
    assert "HID_HOST_W4_INTERRUPT_CONNECTION_DISCONNECTED" not in shim_code, (
        "reproducing upstream's range check would readmit the overwrite race"
    )

    # The diagnostic interposition is the shim's other reason to exist.
    assert "#define l2cap_send_prepared ns2_hid_host_l2cap_send_prepared" in shim
    assert "#include <classic/hid_host.c>" in shim, (
        "hid_host_get_connection_for_hid_cid() is static upstream; the textual "
        "include is what makes the guard implementable at all"
    )

    cmakelists = read(CMAKELISTS)
    assert "cmake/btstack_hid_host_long_report.c" in cmakelists
    assert "message(FATAL_ERROR" in cmakelists and \
        "_ns2_btstack_hid_host_index EQUAL -1" in cmakelists, (
        "failing to remove upstream hid_host.c must stay a configure error, not "
        "a duplicate-symbol link error or a silently unsubstituted build"
    )


def check_le_security_policy_is_explicit():
    """Every security default BTstack 1.8.2 changed must be set here on purpose.

    1.8.2's sm_init() sets sm_min_encryption_key_size = 16 and, under
    ENABLE_LE_SECURE_CONNECTIONS, sm_sc_only_mode = true; hci_init() sets
    ssp_auto_accept = 0. 1.6.2 defaulted to 7, false, and 1. Deleting any of
    these calls therefore does not restore the historical behaviour -- it adopts
    the new default silently.
    """
    host = read(BTSTACK_HOST)

    assert "sm_set_secure_connections_only_mode(false);" in host, (
        "LE Secure Connections Only defaults to ON in BTstack 1.8.2 and would "
        "reject legacy-pairing peers plus existing sub-16-byte-key bonds; "
        "raising it is a product decision, not an SDK default to inherit"
    )
    assert "sm_set_encryption_key_size_range(7, 16);" in host
    assert "gap_ssp_set_auto_accept(0);" in host
    assert ("sm_set_authentication_requirements(SM_AUTHREQ_BONDING | "
            "SM_AUTHREQ_SECURE_CONNECTION);") in host, (
        "requesting Secure Connections while still permitting legacy fallback "
        "is the documented policy; see setup_hid_handlers()"
    )


def check_disconnect_convergence_is_wired():
    """gap_disconnect() no longer synthesises a disconnection-complete event.

    Call sites that own a durable record must route through the helper so a
    handle the controller already released converges locally instead of leaving
    that record occupied for the rest of the boot.
    """
    host = read(BTSTACK_HOST)

    assert "static bool btstack_host_request_disconnect(" in host
    assert "ns2_bt_disconnect_outcome(status)" in host, (
        "the pending-vs-converge rule lives in ns2_bt_lifecycle so tests can "
        "pin it; the helper must consult it rather than re-deriving it"
    )

    for owner in (
        'btstack_host_request_disconnect(closing, "management session")',
        'btstack_host_request_disconnect(stuck_handle, "Switch 2 init recovery")',
        'btstack_host_request_disconnect(c->handle, "disconnect-all BLE")',
        'btstack_host_request_disconnect(c->handle, "forget BLE link")',
    ):
        assert owner in host, f"durable-record disconnect not converged: {owner}"

    assert "static void ble_connection_release_orphan(" in host


def check_single_sdk_version_declaration():
    """CMakeLists.txt and build.ps1 must not drift.

    pico-vscode.cmake sets PICO_SDK_PATH as a CMake variable from
    CMakeLists.txt's $sdkVersion before pico_sdk_import.cmake consults the
    environment, so build.ps1's $env:PICO_SDK_PATH loses silently whenever the
    two disagree. build.ps1 checks the resolved cache at configure time; this
    catches the mismatch before a build runs at all.
    """
    cmakelists = read(CMAKELISTS)
    build = read(BUILD_PS1)

    def cmake_var(name):
        match = re.search(r"^set\(%s\s+([^\)]+)\)" % name, cmakelists, re.M)
        assert match, f"CMakeLists.txt does not declare {name}"
        return match.group(1).strip()

    def ps_var(name):
        match = re.search(r"^\$%s\s*=\s*'([^']+)'" % name, build, re.M)
        assert match, f"build.ps1 does not declare ${name}"
        return match.group(1)

    for cmake_name, ps_name in (
        ("sdkVersion", "sdkVersion"),
        ("toolchainVersion", "toolchainVersion"),
        ("picotoolVersion", "picotoolVersion"),
    ):
        assert cmake_var(cmake_name) == ps_var(ps_name), (
            f"{cmake_name} disagrees between CMakeLists.txt "
            f"({cmake_var(cmake_name)}) and build.ps1 ({ps_var(ps_name)}); "
            "CMakeLists.txt wins silently"
        )

    assert "SDK selection skew" in build, (
        "build.ps1 must verify the SDK path CMake actually resolved"
    )


def check_canonical_btstack_root():
    """One BTstack tree for the SDK targets and for our direct includes.

    PicoSwitch includes BTstack headers directly and substitutes one of its
    translation units. If PICO_BTSTACK_PATH ever pointed elsewhere while
    BTSTACK_ROOT stayed hardcoded to the bundled copy, the two would compile
    different BTstack revisions against each other -- and it would link.
    """
    cmakelists = read(CMAKELISTS)

    assert not re.search(
        r"^\s*set\(BTSTACK_ROOT\s+\$\{PICO_SDK_PATH\}/lib/btstack\)",
        cmakelists,
        re.M,
    ), "BTSTACK_ROOT must honour PICO_BTSTACK_PATH, not hardcode the bundled tree"

    assert "if(NOT PICO_BTSTACK_PATH)" in cmakelists
    assert 'if(DEFINED ENV{PICO_BTSTACK_PATH})' in cmakelists, (
        "reproduce the SDK's own precedence: variable, then environment, then "
        "${PICO_SDK_PATH}/lib/btstack"
    )
    assert "BTstack root skew" in cmakelists, (
        "the resolved root must be checked against what pico_btstack_base "
        "actually compiles"
    )


def check_no_stale_toolchain_paths():
    """Editor/tool metadata must not point at the superseded SDK."""
    stale = []
    for path in sorted((ROOT / ".vscode").glob("*.json")):
        text = read(path)
        for needle in ("sdk/2.2.0", "toolchain/14_2_Rel1", "picotool/2.2.0-a4"):
            if needle in text:
                stale.append(f"{path.name}:{needle}")
    assert not stale, f"stale pre-Stage-C tool paths remain: {stale}"


def main():
    checks = [
        check_hids_host_naming,
        check_removed_protocol_mode_is_not_reintroduced,
        check_hid_host_send_guard_survives,
        check_le_security_policy_is_explicit,
        check_disconnect_convergence_is_wired,
        check_single_sdk_version_declaration,
        check_canonical_btstack_root,
        check_no_stale_toolchain_paths,
    ]
    for check in checks:
        check()
    print("BTstack dependency-contract tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
