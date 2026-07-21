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

static bool joycon2_has_matching_native_source(const switch_pro_input_t *in,
                                                joycon2_side_t side) {
    return (side == JOYCON2_SIDE_LEFT &&
            in->source_joycon_side == SWITCH_SOURCE_JOYCON_LEFT) ||
           (side == JOYCON2_SIDE_RIGHT &&
            in->source_joycon_side == SWITCH_SOURCE_JOYCON_RIGHT);
}

static void joycon2_pack_native_stick(const switch_pro_input_t *in,
                                      joycon2_side_t side, uint8_t out[3]) {
    // Nintendo's full reports retain the physical-side stick slot: Left uses
    // left_stick and Right uses right_stick. Emit it in the Joy-Con's own local
    // coordinates. The console applies its upright/sideways interpretation
    // after the user registers the half with SL+SR.
    memcpy(out, side == JOYCON2_SIDE_LEFT ? in->left_stick : in->right_stick, 3);
}

static void joycon2_write_mouse_posture(const switch_pro_input_t *in, uint8_t out[63]) {
    // The working NS-PC-Control implementation accompanies optical mouse data
    // with a stationary Pro-format 30-byte IMU block. Its +X acceleration is
    // the posture signal for a Joy-Con resting on its rail/sensor face. A
    // desktop mouse cannot supply an IMU, so synthesize that exact stationary
    // state only while the console-negotiated mouse feature is active.
    out[0xF] = 30;
    uint8_t *motion = &out[0x10];
    motion[0] = (uint8_t)in->mouse_motion_timing;
    motion[1] = (uint8_t)(in->mouse_motion_timing >> 8);
    motion[2] = 0x00;
    motion[3] = 0x0C;  // temperature 0x0C00 LE
    // phase X/Y/Z remain zero for a stationary source (motion[4..15]).
    // Acceleration X = 4096 raw counts in Q16; Y/Z and correction are zero.
    motion[19] = 0x10; // int32 LE 0x10000000 at motion[16..19]
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

    const bool native = joycon2_has_matching_native_source(in, side);
    if (native && side == JOYCON2_SIDE_LEFT) {
        // Physical Joy-Con (L), canonical local layout. Do not pre-rotate or
        // turn its directional cluster into another controller's face layout;
        // the console owns that upright/sideways transformation.
        if (source_buttons & JP_BUTTON_L3) b0 |= 0x80;
        if (source_buttons & JP_BUTTON_S1) b0 |= 0x40;
        if (source_buttons & JP_BUTTON_L2) b0 |= 0x20;  // ZL
        if (source_buttons & JP_BUTTON_L1) b0 |= 0x10;  // L
        if (source_buttons & JP_BUTTON_DU) b0 |= 0x08;
        if (source_buttons & JP_BUTTON_DL) b0 |= 0x04;
        if (source_buttons & JP_BUTTON_DR) b0 |= 0x02;
        if (source_buttons & JP_BUTTON_DD) b0 |= 0x01;
        if (source_buttons & JP_BUTTON_SL) b1 |= 0x80;
        if (source_buttons & JP_BUTTON_SR) b1 |= 0x40;
        if (source_buttons & JP_BUTTON_A2) b1 |= 0x01;  // Capture
    } else if (native) {
        // Physical Joy-Con (R), canonical local layout.
        if (source_buttons & JP_BUTTON_R3) b0 |= 0x80;
        if (source_buttons & JP_BUTTON_S2) b0 |= 0x40;
        if (source_buttons & JP_BUTTON_R2) b0 |= 0x20;  // ZR
        if (source_buttons & JP_BUTTON_R1) b0 |= 0x10;  // R
        if (source_buttons & JP_BUTTON_B4) b0 |= 0x08;  // X
        if (source_buttons & JP_BUTTON_B3) b0 |= 0x04;  // Y
        if (source_buttons & JP_BUTTON_B2) b0 |= 0x02;  // A
        if (source_buttons & JP_BUTTON_B1) b0 |= 0x01;  // B
        if (source_buttons & JP_BUTTON_SL) b1 |= 0x80;
        if (source_buttons & JP_BUTTON_SR) b1 |= 0x40;
        if (source_buttons & JP_BUTTON_A3) b1 |= 0x10;  // C / GameChat
        if (source_buttons & JP_BUTTON_A1) b1 |= 0x01;  // Home
    } else if (side == JOYCON2_SIDE_LEFT) {
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

    if (native)
        joycon2_pack_native_stick(in, side, &out[0x5]);
    else
        joycon2_pack_sideways_stick(in, source_buttons, side, &out[0x5]);

    if (in->mouse_enabled && in->has_mouse && in->mouse_scroll != 0) {
        // A real Joy-Con scrolls with its one local stick. Keep this in the
        // Joy-Con's local coordinates: the console turns Up/Down into
        // Left/Right automatically when the half is registered sideways.
        switch_pro_pack_stick(SWITCH_STICK_MID,
                              in->mouse_scroll > 0 ? 0 : SWITCH_STICK_MAX,
                              &out[0x5]);
    }

    // Steady-state Joy-Con 2 captures consistently carry 0x38 here (0x30 is
    // only seen during initialization). The console uses the surrounding
    // native report state while deciding whether optical mouse data is live.
    out[0x8] = 0x38;

    // Native relative mouse block. A desktop mouse has no optical lift-off
    // sensor, so use a constant in the genuine on-surface range (0x10..0x1e).
    if (in->mouse_enabled && in->has_mouse) {
        out[0x9] = (uint8_t)in->mouse_delta_x;
        out[0xA] = (uint8_t)((uint16_t)in->mouse_delta_x >> 8);
        out[0xB] = (uint8_t)in->mouse_delta_y;
        out[0xC] = (uint8_t)((uint16_t)in->mouse_delta_y >> 8);
        out[0xD] = 0x17;
        joycon2_write_mouse_posture(in, out);
    }

    // 0x8 unknown; 0xE NFC state; 0xF motion length; remaining motion and
    // reserved fields stay zero.
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

    const bool native = joycon2_has_matching_native_source(in, side);
    if (native && side == JOYCON2_SIDE_LEFT) {
        if (source_buttons & JP_BUTTON_A2) b1 |= 0x20;  // Capture
        if (source_buttons & JP_BUTTON_S1) b1 |= 0x01;  // Minus
        if (source_buttons & JP_BUTTON_L3) b1 |= 0x08;

        if (source_buttons & JP_BUTTON_L2) b2 |= 0x80;  // ZL
        if (source_buttons & JP_BUTTON_L1) b2 |= 0x40;  // L
        if (source_buttons & JP_BUTTON_SL) b2 |= 0x20;
        if (source_buttons & JP_BUTTON_SR) b2 |= 0x10;
        if (source_buttons & JP_BUTTON_DL) b2 |= 0x08;
        if (source_buttons & JP_BUTTON_DR) b2 |= 0x04;
        if (source_buttons & JP_BUTTON_DU) b2 |= 0x02;
        if (source_buttons & JP_BUTTON_DD) b2 |= 0x01;
    } else if (native) {
        if (source_buttons & JP_BUTTON_R2) b0 |= 0x80;  // ZR
        if (source_buttons & JP_BUTTON_R1) b0 |= 0x40;  // R
        if (source_buttons & JP_BUTTON_SL) b0 |= 0x20;
        if (source_buttons & JP_BUTTON_SR) b0 |= 0x10;
        if (source_buttons & JP_BUTTON_B2) b0 |= 0x08;  // A
        if (source_buttons & JP_BUTTON_B1) b0 |= 0x04;  // B
        if (source_buttons & JP_BUTTON_B4) b0 |= 0x02;  // X
        if (source_buttons & JP_BUTTON_B3) b0 |= 0x01;  // Y

        if (source_buttons & JP_BUTTON_A3) b1 |= 0x40;  // C / GameChat
        if (source_buttons & JP_BUTTON_A1) b1 |= 0x10;  // Home
        if (source_buttons & JP_BUTTON_R3) b1 |= 0x04;
        if (source_buttons & JP_BUTTON_S2) b1 |= 0x02;  // Plus
    } else if (side == JOYCON2_SIDE_LEFT) {
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

    if (native)
        joycon2_pack_native_stick(in, side, &out[0xA]);
    else
        joycon2_pack_sideways_stick(in, source_buttons, side, &out[0xA]);

    out[0x1F] = 0xA0;  // battery voltage ~4000 mV (0x0FA0 LE)
    out[0x20] = 0x0F;
    out[0x21] = 0x20;  // charge state
    out[0x29] = 0x01;  // documented constant
}
