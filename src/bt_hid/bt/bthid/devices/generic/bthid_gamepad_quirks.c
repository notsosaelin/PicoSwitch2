// bthid_gamepad_quirks.c - the ONE ordered, most-specific-first quirk match table.
//
// Every named gamepad_quirk_t is defined in its own file under quirks/ (one per exact model or
// mechanism -- see bthid_gamepad_quirks.h's naming rule). This file only holds the shared
// SEQ_BUTTON_MAP fallback table (genuinely cross-vendor, not any one quirk's own data) and the
// priority order that decides which quirk a given device resolves to.

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"
#include <string.h>

// Every named quirk profile, defined in its own quirks/ file. Declared extern here (not via one
// header per file) to avoid a header-per-quirk proliferation for a single exported const.
extern const gamepad_quirk_t QUIRK_BITDO_NGC_MODKIT;
extern const gamepad_quirk_t QUIRK_BITDO_ULTIMATE_MG;
extern const gamepad_quirk_t QUIRK_BITDO_M30;
extern const gamepad_quirk_t QUIRK_BITDO_PADDLE;
extern const gamepad_quirk_t QUIRK_XBOX_ELITE2;
extern const gamepad_quirk_t QUIRK_XBOX;

// Standard sequential HID gamepads: shoulders before triggers. The universal fallback table --
// shared by the generic quirk below, 8BitDo devices without paddles (SN30 Pro, M30), and as the
// non-paddle half of QUIRK_BITDO_PADDLE/QUIRK_BITDO_ULTIMATE_MG's own buttonCnt-conditional
// selection (see bthid_gamepad_quirk_bitdo_paddle.c). Kept here, not in any one vendor file,
// since it's genuinely cross-vendor, not any one controller's own data.
const uint32_t SEQ_BUTTON_MAP[16] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: face 1 (A/Cross)
    JP_BUTTON_B2,       // usage 2: face 2 (B/Circle)
    JP_BUTTON_B3,       // usage 3: face 3 (X/Square)
    JP_BUTTON_B4,       // usage 4: face 4 (Y/Triangle)
    JP_BUTTON_L1,       // usage 5: left shoulder
    JP_BUTTON_R1,       // usage 6: right shoulder
    JP_BUTTON_L2,       // usage 7: left trigger (digital)
    JP_BUTTON_R2,       // usage 8: right trigger (digital)
    JP_BUTTON_S1,       // usage 9: select/back
    JP_BUTTON_S2,       // usage 10: start/menu
    JP_BUTTON_L3,       // usage 11: left stick
    JP_BUTTON_R3,       // usage 12: right stick
    JP_BUTTON_A1,       // usage 13: guide/home
    JP_BUTTON_A2,       // usage 14: capture/share
    JP_BUTTON_A3,       // usage 15: assistant/mute
};

static const gamepad_quirk_t QUIRK_GENERIC = {
    .name = "generic",
    .button_map = SEQ_BUTTON_MAP,
    .button_map_size = sizeof(SEQ_BUTTON_MAP) / sizeof(SEQ_BUTTON_MAP[0]),
};

const gamepad_quirk_t *gamepad_quirks_generic(void)
{
    return &QUIRK_GENERIC;
}

// Ordered, most-specific-first -- exact-PID matches before VID-only/name-only fallbacks. This
// centralizes a priority order that used to be implicit in an if/else if chain in
// process_report_dynamic(): the NGC Modkit had to be checked before the generic 8BitDo
// VID+buttonCnt>14 rule since both match VID 0x2DC8 with 16 buttons declared (see
// bthid_gamepad_quirk_bitdo_ngc_modkit.c's own comment for the full evidence trail). Kept
// visible in one place instead of implicit in if/else-if ordering.
const gamepad_quirk_t *gamepad_quirks_identify(uint16_t vendor_id, uint16_t product_id,
                                                const char *name, uint8_t button_count)
{
    bool is_8bitdo_vid = (vendor_id == 0x2DC8);
    bool has_name = name && name[0];

    if (is_8bitdo_vid && product_id == 0x286A) return &QUIRK_BITDO_NGC_MODKIT;
    if (is_8bitdo_vid && product_id == 0x200B) return &QUIRK_BITDO_ULTIMATE_MG;
    // 8BitDo M30: identify primarily by device NAME (stable across M30 firmware
    // revisions/power-on modes, which report different BT PIDs; some units never resolve
    // VID/PID at all). VID/PID is a secondary match for when the SDP Device ID query succeeds.
    if ((has_name && strstr(name, "8BitDo") && strstr(name, "M30")) ||
        (is_8bitdo_vid && (product_id == 0x0651 ||   // M30 over Bluetooth (SDP Device ID)
                           product_id == 0x5006)))    // M30 USB PID (defensive)
        return &QUIRK_BITDO_M30;
    if (is_8bitdo_vid && button_count > 14) return &QUIRK_BITDO_PADDLE;

    if (vendor_id == 0x045E && (product_id == 0x0B05 || product_id == 0x0B22))
        return &QUIRK_XBOX_ELITE2;
    // BLE PnP VID/PID often doesn't resolve, so also match Xbox by name (the driver already
    // relies on this; button output is correct, which proves the name-match holds).
    if (vendor_id == 0x045E || (has_name && strstr(name, "Xbox")))
        return &QUIRK_XBOX;

    return &QUIRK_GENERIC;
}

bool gamepad_quirks_is_generic(const gamepad_quirk_t *quirk)
{
    // NULL means identification has not run yet, which is not the same as
    // "recognized". Treat it as unresolved.
    return quirk == NULL || quirk == &QUIRK_GENERIC;
}
