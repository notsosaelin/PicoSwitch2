"""Source-level guard: the console has exactly one output slot.

This project is a single-controller milestone. Every console-facing reader is a
hardcoded `get_global_gamepad_input(0, ...)`, and the input arbiter guarantees
that at most one source is ever accepted for publication. A publisher that
indexes the shared slot arrays by a *BTstack connection index* therefore writes
state nothing will ever read, silently and only for whichever peer happened to
be allocated a non-zero connection index.

That bug has now happened twice:

  * 2026-07-12, feedback direction -- `find_player_index()` returned the caller's
    connection index whenever it was < NS2_SLOTS, so a Classic controller on
    connection 1..3 read a rumble slot that never received anything.
  * 2026-08-21, input direction -- `router_submit_input()` published through
    `ns2_slot(e->dev_addr)`. Hardware-confirmed: a DualSense Edge on connection 0
    drove the console normally while the Android Controller Link on connection 1
    was accepted by the arbiter yet never reached slot 0. `pipe.inputAgeMs` grew
    monotonically and `input status` reported vid/pid 0x0000, which also made the
    companion's Adapter page correctly say "None paired".

Both directions are now constants. This guard exists so the third occurrence is a
failing test instead of another hardware session.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SEAM = ROOT / "src/bt_hid/ns2_seam.c"
KBM = ROOT / "src/bt_hid/ns2_kbm_runtime.c"

# Writers into the shared per-slot report arrays. Their first argument is the slot.
SLOT_WRITERS = (
    "set_global_gamepad_input",
    "set_global_device",
    "set_global_raw_buttons",
    "set_global_raw_report",
    "accumulate_global_mouse_input",
)

ALLOWED_SLOT_ARGS = {"0", "0u", "slot", "NS2_CONSOLE_SLOT"}


def code_only(source: str) -> str:
    """Drop comments so the guard reads implementation, not the prose about it."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", source)


def main() -> None:
    seam_text = SEAM.read_text(encoding="utf-8")
    seam = code_only(seam_text)

    # The console slot is a constant, and the connection-index helper is gone.
    assert "#define NS2_CONSOLE_SLOT 0u" in seam, (
        "the console output slot must be a named constant"
    )
    assert "ns2_slot(" not in seam, (
        "ns2_slot() mapped a BTstack connection index to an output slot; "
        "it must not come back"
    )
    assert re.search(r"const uint8_t slot = NS2_CONSOLE_SLOT;", seam), (
        "router_submit_input must publish through the console-slot constant"
    )

    # No publisher may index a slot by anything else.
    for source in (seam, code_only(KBM.read_text(encoding="utf-8"))):
        for writer in SLOT_WRITERS:
            for match in re.finditer(re.escape(writer) + r"\(\s*([^,()]+)\s*,", source):
                arg = match.group(1).strip()
                assert arg in ALLOWED_SLOT_ARGS, (
                    f"{writer}() must publish to the single console slot, got {arg!r}"
                )

    # The feedback direction stays collapsed to the same one identity.
    player_index = re.search(
        r"int find_player_index\(int dev_addr, int instance\) \{(.*?)\n\}",
        seam,
        flags=re.DOTALL,
    )
    assert player_index, "find_player_index() not found"
    body = player_index.group(1)
    assert "return 0;" in body
    assert not re.search(r"return\s+dev_addr", body), (
        "find_player_index() must not return a connection index again"
    )

    print("console slot wiring tests passed")


if __name__ == "__main__":
    main()
