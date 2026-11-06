// bthid_gamepad_quirk_bitdo_paddle.c - the generic 8BitDo (VID 0x2DC8) paddle-equipped-model
// quirk. Named for the MECHANISM (back paddles reachable at fixed HID usage numbers 3 and 6),
// not an exact model, because the original comment this table was built from ("Ultimate, Pro 2,
// etc.") never claimed an exhaustive or confirmed exact PID list -- unlike bthid_gamepad_quirk_
// bitdo_ultimate_mg.c, which IS PID-confirmed (0x200B) and uses a completely different
// mechanism (a fixed raw-report byte, not these usage numbers).

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"

extern const uint32_t SEQ_BUTTON_MAP[16];  // defined in bthid_gamepad_quirks.c -- the universal
                                            // non-paddle fallback table

// 8BitDo controllers with back paddles (Ultimate, Pro 2, etc.)
// VID 0x2DC8. Descriptor order inserts R4 at 3 and L4 at 6:
//   A B R4 X Y L4 LB RB LT RT Select Start L3 R3 Home Capture
static const uint32_t BITDO_BUTTON_MAP[17] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: A
    JP_BUTTON_B2,       // usage 2: B
    JP_BUTTON_R4,       // usage 3: R4 (back paddle)
    JP_BUTTON_B3,       // usage 4: X
    JP_BUTTON_B4,       // usage 5: Y
    JP_BUTTON_L4,       // usage 6: L4 (back paddle)
    JP_BUTTON_L1,       // usage 7: LB
    JP_BUTTON_R1,       // usage 8: RB
    JP_BUTTON_L2,       // usage 9: LT (digital)
    JP_BUTTON_R2,       // usage 10: RT (digital)
    JP_BUTTON_S1,       // usage 11: Select
    JP_BUTTON_S2,       // usage 12: Start
    JP_BUTTON_A1,       // usage 13: Home
    JP_BUTTON_L3,       // usage 14: L3
    JP_BUTTON_R3,       // usage 15: R3
    JP_BUTTON_A2,       // usage 16: Capture
};

// Shared by QUIRK_BITDO_PADDLE and QUIRK_BITDO_ULTIMATE_MG (non-static so that file can reuse
// it) -- both are "generic 8BitDo, VID 0x2DC8, no more specific PID match" as far as
// button-table selection goes; Ultimate MG's own extra 2 back paddles are read from a fixed
// byte offset separately (see that quirk's file), not via this table's own usage 3/6 slots.
// Models without paddles (SN30 Pro, M30) have <=14 buttons and use the plain sequential map.
void bitdo_select_button_map(uint8_t button_count, bool has_sim_triggers,
                              const uint32_t **out_map, uint8_t *out_size)
{
    (void)has_sim_triggers;
    if (button_count > 14) {
        *out_map = BITDO_BUTTON_MAP;
        *out_size = sizeof(BITDO_BUTTON_MAP) / sizeof(BITDO_BUTTON_MAP[0]);
    } else {
        *out_map = SEQ_BUTTON_MAP;
        *out_size = 16;
    }
}

const gamepad_quirk_t QUIRK_BITDO_PADDLE = {
    .name = "bitdo_paddle",
    .select_button_map = bitdo_select_button_map,
};
