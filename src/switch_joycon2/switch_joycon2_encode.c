/*
 * Pure Joy-Con 2 report encoders. See include/switch_joycon2_encode.h.
 */
#include <string.h>

#include "controller_battery.h"
#include "switch_joycon2_encode.h"
#include "core/buttons.h"  // normalized physical JP_BUTTON_* sources

static uint16_t joycon2_axis_from_delta(int32_t delta) {
    if (delta < -(int32_t)SWITCH_STICK_MID) delta = -(int32_t)SWITCH_STICK_MID;
    if (delta > (int32_t)(SWITCH_STICK_MAX - SWITCH_STICK_MID))
        delta = (int32_t)(SWITCH_STICK_MAX - SWITCH_STICK_MID);
    return (uint16_t)((int32_t)SWITCH_STICK_MID + delta);
}

static int32_t joycon2_negate_axis_delta(int32_t delta) {
    // The 12-bit range has one more value below center than above it. Preserve
    // center and both exact endpoints when reflecting an axis.
    if (delta == (int32_t)(SWITCH_STICK_MAX - SWITCH_STICK_MID))
        return -(int32_t)SWITCH_STICK_MID;
    if (delta == -(int32_t)SWITCH_STICK_MID)
        return (int32_t)(SWITCH_STICK_MAX - SWITCH_STICK_MID);
    return -delta;
}

static void joycon2_pack_sideways_stick(const switch_pro_input_t *in, uint32_t source_buttons,
                                         joycon2_side_t side,
                                         uint8_t out[3]) {
    uint16_t x = (uint16_t)(in->left_stick[0] |
                            ((uint16_t)(in->left_stick[1] & 0x0F) << 8));
    uint16_t y = (uint16_t)((in->left_stick[1] >> 4) |
                            ((uint16_t)in->left_stick[2] << 4));

    // The paired controller's D-pad is a second, digital source for the lone
    // Joy-Con stick. A direction overrides only its own analog axis, so a
    // digital horizontal direction can still combine with analog vertical input.
    // Opposing directions cancel and leave that analog axis unchanged.
    const bool left = (source_buttons & JP_BUTTON_DL) != 0;
    const bool right = (source_buttons & JP_BUTTON_DR) != 0;
    const bool up = (source_buttons & JP_BUTTON_DU) != 0;
    const bool down = (source_buttons & JP_BUTTON_DD) != 0;
    if (left != right) x = left ? SWITCH_STICK_MIN : SWITCH_STICK_MAX;
    if (up != down) y = up ? SWITCH_STICK_MAX : SWITCH_STICK_MIN;

    // The Switch sees axes in the Joy-Con shell's local coordinates. Convert the
    // normally-held paired controller into those coordinates: L is held 90 degrees
    // counter-clockwise (so apply the inverse/clockwise rotation); R is held clockwise.
    const int32_t dx = (int32_t)x - SWITCH_STICK_MID;
    const int32_t dy = (int32_t)y - SWITCH_STICK_MID;
    const int32_t rotated_x = side == JOYCON2_SIDE_LEFT ? dy : joycon2_negate_axis_delta(dy);
    const int32_t rotated_y = side == JOYCON2_SIDE_LEFT ? joycon2_negate_axis_delta(dx) : dx;
    switch_pro_pack_stick(joycon2_axis_from_delta(rotated_x),
                          joycon2_axis_from_delta(rotated_y), out);
}

// Report 0x07 (Left) / 0x08 (Right) -- Confirmed byte layout and button bitmaps, see
// docs/switch2-joycon2/protocol.md "Wire input/output report contents". The source-side
// translation implements the project's deliberate single-Joy-Con sideways profile; the
// SWITCH_MASK_* names still describe the paired controller's Pro2 remap destinations.
void switch_joycon2_encode_report(const switch_pro_input_t *in, uint32_t source_buttons,
                                   joycon2_side_t side, uint8_t counter, uint8_t out[63]) {
    memset(out, 0, 63);
    out[0x0] = counter;
    out[0x1] = controller_battery_switch2_power_info(
        in->battery_valid != 0, in->battery_level,
        in->battery_charging != 0);

    const uint8_t s1 = in->buttons[1];
    uint8_t b0 = 0, b1 = 0;

    if (side == JOYCON2_SIDE_LEFT) {
        // Paired controller -> sideways Joy-Con 2 (L):
        // L3->stick, Select->Minus, R2->ZL, L2->L,
        // Square/X->Up, Cross/A->Left, Triangle/Y->Right, Circle/B->Down.
        if (source_buttons & JP_BUTTON_L3) b0 |= 0x80;
        if (source_buttons & JP_BUTTON_S1) b0 |= 0x40;
        if (source_buttons & JP_BUTTON_R2) b0 |= 0x20;
        if (source_buttons & JP_BUTTON_L2) b0 |= 0x10;
        if (source_buttons & JP_BUTTON_B3) b0 |= 0x08;
        if (source_buttons & JP_BUTTON_B1) b0 |= 0x04;
        if (source_buttons & JP_BUTTON_B4) b0 |= 0x02;
        if (source_buttons & JP_BUTTON_B2) b0 |= 0x01;
        // L1/R1 become the rail buttons; Capture keeps the Pro2 family mapping.
        if (source_buttons & JP_BUTTON_L1) b1 |= 0x80;
        if (source_buttons & JP_BUTTON_R1) b1 |= 0x40;
        if (s1 & SWITCH_MASK_CAPTURE) b1 |= 0x01;
    } else {  // JOYCON2_SIDE_RIGHT
        // Paired controller -> sideways Joy-Con 2 (R):
        // L3->stick, Start->Plus, R2->ZR, L2->R,
        // Square/X->B, Triangle/Y->Y, Cross/A->A, Circle/B->X.
        if (source_buttons & JP_BUTTON_L3) b0 |= 0x80;
        if (source_buttons & JP_BUTTON_S2) b0 |= 0x40;
        if (source_buttons & JP_BUTTON_R2) b0 |= 0x20;
        if (source_buttons & JP_BUTTON_L2) b0 |= 0x10;
        if (source_buttons & JP_BUTTON_B2) b0 |= 0x08;
        if (source_buttons & JP_BUTTON_B4) b0 |= 0x04;
        if (source_buttons & JP_BUTTON_B1) b0 |= 0x02;
        if (source_buttons & JP_BUTTON_B3) b0 |= 0x01;
        // L1/R1 become the rail buttons; C/Home keep the Pro2 family mapping.
        if (source_buttons & JP_BUTTON_L1) b1 |= 0x80;
        if (source_buttons & JP_BUTTON_R1) b1 |= 0x40;
        if (in->extra & SWITCH_EXTRA_C) b1 |= 0x10;
        if (source_buttons & JP_BUTTON_A1) b1 |= 0x01;
    }
    out[0x2] = b0;
    out[0x3] = b1;
    out[0x4] = 0x07;  // observed constant on both sides

    joycon2_pack_sideways_stick(in, source_buttons, side, &out[0x5]);

    // 0x8 unknown; 0x9..0xD mouse; 0xE NFC state; 0xF motion length; remaining motion and
    // reserved fields stay zero. This project currently has no source for them.
}

// Report 0x05 -- shared Switch-family format. Apply the exact same sideways translation as
// report 0x07/0x08, then place the translated controls in this format's physical-side fields.
void switch_joycon2_encode_report05(const switch_pro_input_t *in, uint32_t source_buttons,
                                     joycon2_side_t side, uint32_t counter, uint8_t out[63]) {
    memset(out, 0, 63);
    out[0] = (uint8_t)counter;
    out[1] = (uint8_t)(counter >> 8);
    out[2] = (uint8_t)(counter >> 16);
    out[3] = (uint8_t)(counter >> 24);

    const uint8_t s1 = in->buttons[1];
    uint8_t b0 = 0, b1 = 0, b2 = 0;

    if (side == JOYCON2_SIDE_LEFT) {
        if (s1 & SWITCH_MASK_CAPTURE) b1 |= 0x20;
        if (source_buttons & JP_BUTTON_S1) b1 |= 0x01;
        if (source_buttons & JP_BUTTON_L3) b1 |= 0x08;

        if (source_buttons & JP_BUTTON_R2) b2 |= 0x80;
        if (source_buttons & JP_BUTTON_L2) b2 |= 0x40;
        if (source_buttons & JP_BUTTON_L1) b2 |= 0x20;
        if (source_buttons & JP_BUTTON_R1) b2 |= 0x10;
        if (source_buttons & JP_BUTTON_B1) b2 |= 0x08;
        if (source_buttons & JP_BUTTON_B4) b2 |= 0x04;
        if (source_buttons & JP_BUTTON_B3) b2 |= 0x02;
        if (source_buttons & JP_BUTTON_B2) b2 |= 0x01;
    } else {
        if (source_buttons & JP_BUTTON_R2) b0 |= 0x80;
        if (source_buttons & JP_BUTTON_L2) b0 |= 0x40;
        if (source_buttons & JP_BUTTON_L1) b0 |= 0x20;
        if (source_buttons & JP_BUTTON_R1) b0 |= 0x10;
        if (source_buttons & JP_BUTTON_B1) b0 |= 0x08;
        if (source_buttons & JP_BUTTON_B3) b0 |= 0x04;
        if (source_buttons & JP_BUTTON_B2) b0 |= 0x02;
        if (source_buttons & JP_BUTTON_B4) b0 |= 0x01;

        if (in->extra & SWITCH_EXTRA_C) b1 |= 0x40;
        if (source_buttons & JP_BUTTON_A1) b1 |= 0x10;
        if (source_buttons & JP_BUTTON_L3) b1 |= 0x04;
        if (source_buttons & JP_BUTTON_S2) b1 |= 0x02;
    }

    out[0x4] = b0;
    out[0x5] = b1;
    out[0x6] = b2;

    joycon2_pack_sideways_stick(in, source_buttons, side, &out[0xA]);

    out[0x1F] = 0xA0;  // battery voltage ~4000 mV (0x0FA0 LE)
    out[0x20] = 0x0F;
    out[0x21] = 0x20;  // charge state
    out[0x29] = 0x01;  // documented constant
}
