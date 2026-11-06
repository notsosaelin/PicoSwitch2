#include "switch_pro2_encode.h"

void switch_pro2_encode_buttons(const switch_pro_input_t *in, uint8_t out[3]) {
    const uint8_t s0 = in->buttons[0];
    const uint8_t s1 = in->buttons[1];
    const uint8_t s2 = in->buttons[2];
    uint8_t b0 = 0, b1 = 0, b2 = 0;

    if (s0 & SWITCH_MASK_B)  b0 |= 0x01;
    if (s0 & SWITCH_MASK_A)  b0 |= 0x02;
    if (s0 & SWITCH_MASK_Y)  b0 |= 0x04;
    if (s0 & SWITCH_MASK_X)  b0 |= 0x08;
    if (s0 & SWITCH_MASK_R)  b0 |= 0x10;
    if (s0 & SWITCH_MASK_ZR) b0 |= 0x20;
    if (s1 & SWITCH_MASK_PLUS) b0 |= 0x40;
    if (s1 & SWITCH_MASK_R3)   b0 |= 0x80;

    if (s2 & SWITCH_MASK_DPAD_DOWN)  b1 |= 0x01;
    if (s2 & SWITCH_MASK_DPAD_RIGHT) b1 |= 0x02;
    if (s2 & SWITCH_MASK_DPAD_LEFT)  b1 |= 0x04;
    if (s2 & SWITCH_MASK_DPAD_UP)    b1 |= 0x08;
    if (s2 & SWITCH_MASK_L)  b1 |= 0x10;
    if (s2 & SWITCH_MASK_ZL) b1 |= 0x20;
    if (s1 & SWITCH_MASK_MINUS) b1 |= 0x40;
    if (s1 & SWITCH_MASK_L3)    b1 |= 0x80;

    if (s1 & SWITCH_MASK_HOME)    b2 |= 0x01;
    if (s1 & SWITCH_MASK_CAPTURE) b2 |= 0x02;
    if (in->extra & SWITCH_EXTRA_GR) b2 |= 0x04;
    if (in->extra & SWITCH_EXTRA_GL) b2 |= 0x08;
    if (in->extra & SWITCH_EXTRA_C)  b2 |= 0x10;

    out[0] = b0;
    out[1] = b1;
    out[2] = b2;
}
