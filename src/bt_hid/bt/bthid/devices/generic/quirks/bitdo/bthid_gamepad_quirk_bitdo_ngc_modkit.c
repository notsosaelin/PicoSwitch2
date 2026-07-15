// 8BitDo NGC Modkit, VID:PID 2DC8:286A. PID-specific because its 16-button report is not the
// paddle-controller layout. Evidence: docs/bluetooth/8bitdo-ngc-diy-profile.md.

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"

// Direct face labels; analog triggers are handled by the seam; Z stays distinct; stick clicks
// are intentionally repurposed as Capture/Home. Unwired and suppressed usages remain explicit in
// the table. The Android/D-Input mode still needs a separate capture; see the profile document.
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
