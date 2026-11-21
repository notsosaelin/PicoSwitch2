"""The diagnostic buffer profile keeps lab history available without paying for
it on every Pico W.

Three RAM rings (Bluetooth lifecycle, protocol trace, Switch 2 BLE capture)
total about 104 KB at the sizes a soak or a full capture needs. On Pico W that
was 40 % of RAM. The profile shrinks them for ordinary Pico W builds and leaves
the full sizes one build flag away; nothing is deleted.

These are source-level guards, like the other wiring tests: they hold the
relationships that a later edit could quietly break.

    python tests/tools/test_diag_profile.py
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def source_default(text, name):
    """The #ifndef-guarded default in the source: the LAB size."""
    match = re.search(
        rf"#ifndef {name}\s*\n#define {name}\s+(\d+)u?\s*\n#endif", text)
    assert match, f"{name} must keep an #ifndef-guarded default so a build can override it"
    return int(match.group(1))


def main():
    cmake = read("firmware/CMakeLists.txt")
    lab = {
        "BTLIFE_RING_SIZE": source_default(
            read("firmware/src/hid_host/bt/btstack/btstack_host.c"), "BTLIFE_RING_SIZE"),
        "NS2_PROTOCOL_TRACE_CAPACITY": source_default(
            read("firmware/src/diagnostics/ns2_protocol_trace.h"),
            "NS2_PROTOCOL_TRACE_CAPACITY"),
        "SW2_CAP_RING": source_default(
            read("firmware/src/diagnostics/sw2_capture.c"), "SW2_CAP_RING"),
    }

    # The lab sizes are the ones the earlier investigations were sized for. They
    # are the defaults, so a host test or an unconfigured build still gets them.
    assert lab["BTLIFE_RING_SIZE"] >= 4096, "a 200-cycle soak needs >= 4096 lifecycle entries"
    assert lab["NS2_PROTOCOL_TRACE_CAPACITY"] >= 256, "a full amiibo read trace needs 256 records"
    assert lab["SW2_CAP_RING"] >= 128

    # The standard profile overrides all three, and each override is smaller
    # than the lab size but still a usable history.
    block = re.search(
        r'if\(_ns2_diag_profile STREQUAL "standard"\)(.*?)elseif', cmake, re.S)
    assert block, "the standard profile block is missing from firmware/CMakeLists.txt"
    floors = {"BTLIFE_RING_SIZE": 128, "NS2_PROTOCOL_TRACE_CAPACITY": 32, "SW2_CAP_RING": 16}
    for name, lab_size in lab.items():
        match = re.search(rf"\b{name}=(\d+)u?\b", block.group(1))
        assert match, f"the standard profile must set {name}"
        value = int(match.group(1))
        assert floors[name] <= value < lab_size, (
            f"standard {name}={value} must be below the lab size {lab_size} "
            f"and at least {floors[name]}")

    # The lab profile must not define sizes: it IS the source defaults, and a
    # second copy of those numbers would drift.
    lab_block = re.search(
        r'elseif\(_ns2_diag_profile STREQUAL "lab"\)(.*?)else\(\)', cmake, re.S)
    assert lab_block
    for name in lab:
        assert name not in lab_block.group(1), f"the lab profile must not restate {name}"

    # auto is standard on Pico W only. Pico 2 W has the RAM and keeps the history.
    auto = re.search(r'if\(_ns2_diag_profile STREQUAL "auto"\)(.*?)endif\(\)\s*\nendif\(\)',
                     cmake, re.S)
    assert auto and 'PICO_BOARD STREQUAL "pico_w"' in auto.group(1)
    assert auto.group(1).index('"standard"') < auto.group(1).index('"lab"')

    # The build script always states the profile, so a cached lab setting cannot
    # leak into an ordinary build.
    build = read("firmware/build.ps1")
    assert '"-DNS2_DIAG_PROFILE=$DiagProfile"' in build
    assert "[ValidateSet('auto', 'standard', 'lab')]" in build

    print("diag_profile: all checks passed")


if __name__ == "__main__":
    main()
