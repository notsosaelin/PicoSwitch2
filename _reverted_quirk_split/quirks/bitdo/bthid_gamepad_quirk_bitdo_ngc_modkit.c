// bthid_gamepad_quirk_bitdo_ngc_modkit.c - 8BitDo NGC Modkit (VID 0x2DC8, PID 0x286A
// specifically -- NOT the same physical layout as the paddle-equipped 8BitDo Ultimate/Pro 2
// that bthid_gamepad_quirk_bitdo_paddle.c's BITDO_BUTTON_MAP is for). A GameCube-shell modkit;
// button_cnt also happens to be 16 like the paddle controllers, which is exactly why this needs
// its own PID-specific quirk rather than falling into the generic 8BitDo+buttonCnt>14 rule --
// confirmed 2026-07-12 via live hardware capture (see docs/bluetooth/8bitdo-ngc-diy-profile.md
// and docs/experiments/gate2-identity-log-hardware-captures-2026-07-12.md for the full raw
// report evidence, including two design iterations this table went through before the owner
// confirmed the final mapping on real hardware -- see that doc for what was tried and rejected,
// and why).

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"

// Face buttons are DIRECT, not rotated: this is a GameCube-shaped controller, so physical
// A/B/X/Y should land on Switch A/B/X/Y directly -- unlike Xbox/PlayStation pads, which use the
// standard rotated convention (physical "A" position -> Switch B slot, etc.) baked into
// JP_BUTTON_B1..B4's own default remap. Getting the *labels* right here means picking the
// JP_BUTTON_* source whose DEFAULT destination is the matching letter (see NS2_DEFAULT_MAP in
// config.c): JP_BUTTON_B2->NS2 A, JP_BUTTON_B1->NS2 B, JP_BUTTON_B4->NS2 X, JP_BUTTON_B3->NS2 Y.
//
// Usages 3, 6, 13, 16 are not wired to any physical control on this unit (interactively
// confirmed -- pressed every remaining button on the controller, nothing else fires).
//
// Usages 7/8 (partial-travel echo bits, fire well before the real click -- see the R2/L2
// investigation in the capture doc) and usages 9/10 (the TRUE mechanical click) are ALL
// suppressed here (mapped to 0). This was NOT the original design -- an earlier iteration
// mapped 9/10 to JP_BUTTON_L1/R1 for discrete "L/R pressed" semantics, but the owner wants the
// simpler, hardware-owner-confirmed behavior instead: the trigger's continuous ANALOG value
// (bytes 6/7, already fed to ANALOG_L2/ANALOG_R2 correctly) is what should drive ZL/ZR, via
// this project's existing generic seam-level fold in ns2_seam.c's router_submit_input()
// (`if (analog[ANALOG_L2] > 64) ... JP_BUTTON_L2`, which NS2_DEFAULT_MAP already routes to
// NS2_DST_ZL/ZR) -- "any real press" registers as ZL/ZR, not just the discrete click. No digital
// bit needs to come from the button-usage table at all for this to work correctly.
//
// Usage 11 (Z) maps to JP_BUTTON_R1 (NOT R2/L2 -- deliberately a different bit than whatever the
// analog fold touches, so Z can never conflict with a simultaneous real trigger press), which
// NS2_DEFAULT_MAP routes to plain NS2_DST_R. Nothing here ever emits JP_BUTTON_L1, so NS2 "L"
// correctly never fires (there's no left-side equivalent of Z on a real GameCube controller).
// NS2 "C" (Gamechat) also never fires -- nothing maps to JP_BUTTON_A3/A5, its only sources.
//
// Usages 14/15 (physical L3/R3 stick clicks) are deliberately repurposed, not passed through
// as L3/R3: a genuine GameCube controller has no clickable sticks at all, so this modkit's
// extra hardware becomes CAPTURE/HOME instead (JP_BUTTON_A2/A1, routed by NS2_DEFAULT_MAP to
// NS2_DST_CAPTURE/NS2_DST_HOME) -- explicit product decision, not a claim about printed labels.
//
// This is Pro-Controller-2-mode-specific: it approximates "trigger touched" as a digital ZL/ZR
// press because that's all today's output supports. The eventual NSO GameCube USB personality
// (Gate 3, not started) will differ: real continuous L/R analog output instead of the ZL/ZR
// approximation, and Z will very likely target ZR (or whatever NSO's own GC-adapter convention
// turns out to be) rather than plain R -- do not assume this table's choices carry over
// unchanged when that work starts; re-derive against NSO's actual behavior.
//
// This table covers the pairing mode captured 2026-07-12 (Classic BT, resolved via SDP as
// 0x2DC8:0x286A). The controller is reported to also support a second "Android/D-Input" BLE
// pairing mode -- unconfirmed whether it uses the same PID or the same report shape; needs its
// own capture before this table can be assumed to cover it. See the profile doc's "Status"
// section.
static const uint32_t NGC_MODKIT_BUTTON_MAP[17] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B2,       // usage 1: A -> NS2 A (direct, not rotated)
    JP_BUTTON_B1,       // usage 2: B -> NS2 B
    0,                  // usage 3: not wired on this unit
    JP_BUTTON_B4,       // usage 4: X -> NS2 X
    JP_BUTTON_B3,       // usage 5: Y -> NS2 Y
    0,                  // usage 6: not wired on this unit
    0,                  // usage 7: L-trigger partial-travel echo -- suppressed
    0,                  // usage 8: R-trigger partial-travel echo -- suppressed
    0,                  // usage 9: L trigger true click -- suppressed; analog fold drives ZL instead
    0,                  // usage 10: R trigger true click -- suppressed; analog fold drives ZR instead
    JP_BUTTON_R1,       // usage 11: Z -> NS2 "R" (not ZR -- must not collide with the analog fold)
    JP_BUTTON_S2,       // usage 12: Start
    0,                  // usage 13: not wired on this unit (no Home/Menu button)
    JP_BUTTON_A2,       // usage 14: physical L3 repurposed -> CAPTURE
    JP_BUTTON_A1,       // usage 15: physical R3 repurposed -> HOME
    0,                  // usage 16: not wired on this unit
};

static bool usage_pressed(const ble_report_map_t *map, const uint8_t *data, uint16_t len,
                          uint8_t usage_number)
{
    const ble_usage_loc_t *loc = &map->buttonLoc[usage_number - 1];
    return loc->bitMask && loc->byteIndex < len && (data[loc->byteIndex] & loc->bitMask);
}

// NSO GameCube-native semantic bits (2026-07-13). Only the 8BitDo NGC Modkit has confirmed
// evidence for these -- see docs/bluetooth/8bitdo-ngc-diy-profile.md "Raw hardware
// observations": usage 9 (byte9 0x01) and usage 10 (byte9 0x02) are the TRUE mechanical
// trigger clicks (confirmed to fire only at full press, composing cleanly with no bit
// aliasing); usage 11 (byte9 0x04) is Z. Usages 7/8 are deliberately NOT used here -- they
// are a partial-travel echo that fires well before the true click and stays asserted
// through it (confirmed: "L full/click" shows byte8=0x40 AND byte9=0x01 simultaneously),
// so using them would double-fire/false-trigger the detent early. These are independent of
// (and never OR'd into) the standard button-usage table above -- they reach the router via
// their own gc_native_z/gc_l_detent/gc_r_detent fields, not the JP_BUTTON_*/NS2_DST_* remap
// table, since they are fixed evidence-backed physical mappings for this exact PID, not a
// user-remappable destination.
static void extract_extra(const ble_report_map_t *map, const uint8_t *data, uint16_t len,
                          input_event_t *event)
{
    event->gc_has_native_layout = true;
    event->gc_l_detent = usage_pressed(map, data, len, 9);
    event->gc_r_detent = usage_pressed(map, data, len, 10);
    event->gc_native_z = usage_pressed(map, data, len, 11);
    // NGC Modkit does NOT need suppress_l2r2_analog_fold: an earlier iteration mapped Z to
    // JP_BUTTON_R2 (colliding with the seam's analog-fold, which also drives JP_BUTTON_R2 from
    // ANALOG_R2) and needed this flag to avoid the conflict. The current design maps Z to
    // JP_BUTTON_R1 instead (see NGC_MODKIT_BUTTON_MAP above) and deliberately WANTS the analog
    // fold to keep driving ZL/ZR from the real trigger values -- so nothing to suppress here
    // anymore. suppress_l2r2_analog_fold itself (input_event.h) is left in place as available
    // infrastructure for a future device that has the same kind of collision this one no longer
    // does.
}

const gamepad_quirk_t QUIRK_BITDO_NGC_MODKIT = {
    .name = "bitdo_ngc_modkit",
    .button_map = NGC_MODKIT_BUTTON_MAP,
    .button_map_size = sizeof(NGC_MODKIT_BUTTON_MAP) / sizeof(NGC_MODKIT_BUTTON_MAP[0]),
    .extract_extra = extract_extra,
};
