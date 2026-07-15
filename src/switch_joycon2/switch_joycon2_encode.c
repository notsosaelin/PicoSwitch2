/*
 * Pure Joy-Con 2 report encoders. See include/switch_joycon2_encode.h.
 */
#include <string.h>

#include "switch_joycon2_encode.h"

// Report 0x07 (Left) / 0x08 (Right) -- Confirmed byte layout and button bitmaps, see
// docs/switch2-joycon2/protocol.md "Wire input/output report contents".
void switch_joycon2_encode_report(const switch_pro_input_t *in, joycon2_side_t side,
                                   uint8_t counter, uint8_t out[63]) {
    memset(out, 0, 63);
    out[0x0] = counter;
    out[0x1] = 0x25;  // power: external power + not charging + battery level 9/9 -- same safe
                       // default as switch_gc_encode_report()'s identical field, no real
                       // battery-telemetry source for a USB-tethered Pico.

    uint8_t s0 = in->buttons[0], s1 = in->buttons[1], s2 = in->buttons[2];
    uint8_t b0 = 0, b1 = 0;

    if (side == JOYCON2_SIDE_LEFT) {
        // Byte 0: 0x80 Stick(click) | 0x40 Minus | 0x20 ZL | 0x10 L | 0x08 Up | 0x04 Left |
        //         0x02 Right | 0x01 Down
        if (s1 & SWITCH_MASK_L3) b0 |= 0x80;  // Left stick click -- L3 is this project's shared
                                               // name for "left stick pressed", the correct source
                                               // regardless of which physical side is emulated.
        if (s1 & SWITCH_MASK_MINUS) b0 |= 0x40;
        if (s2 & SWITCH_MASK_ZL) b0 |= 0x20;
        if (s2 & SWITCH_MASK_L) b0 |= 0x10;
        if (s2 & SWITCH_MASK_DPAD_UP) b0 |= 0x08;
        if (s2 & SWITCH_MASK_DPAD_LEFT) b0 |= 0x04;
        if (s2 & SWITCH_MASK_DPAD_RIGHT) b0 |= 0x02;
        if (s2 & SWITCH_MASK_DPAD_DOWN) b0 |= 0x01;
        // Byte 1: 0x80 SL | 0x40 SR | 0x01 Capture.
        if (in->extra & SWITCH_EXTRA_SL) b1 |= 0x80;
        if (in->extra & SWITCH_EXTRA_SR) b1 |= 0x40;
        if (s1 & SWITCH_MASK_CAPTURE) b1 |= 0x01;
    } else {  // JOYCON2_SIDE_RIGHT
        // Byte 0: 0x80 Stick(click) | 0x40 Plus | 0x20 ZR | 0x10 R | 0x08 X | 0x04 Y | 0x02 A |
        //         0x01 B
        if (s1 & SWITCH_MASK_R3) b0 |= 0x80;
        if (s1 & SWITCH_MASK_PLUS) b0 |= 0x40;
        if (s0 & SWITCH_MASK_ZR) b0 |= 0x20;
        if (s0 & SWITCH_MASK_R) b0 |= 0x10;
        if (s0 & SWITCH_MASK_X) b0 |= 0x08;
        if (s0 & SWITCH_MASK_Y) b0 |= 0x04;
        if (s0 & SWITCH_MASK_A) b0 |= 0x02;
        if (s0 & SWITCH_MASK_B) b0 |= 0x01;
        // Byte 1: 0x80 SL | 0x40 SR | 0x10 C | 0x01 Home.
        if (in->extra & SWITCH_EXTRA_SL) b1 |= 0x80;
        if (in->extra & SWITCH_EXTRA_SR) b1 |= 0x40;
        if (in->extra & SWITCH_EXTRA_C) b1 |= 0x10;
        if (s1 & SWITCH_MASK_HOME) b1 |= 0x01;
    }
    out[0x2] = b0;
    out[0x3] = b1;

    out[0x4] = 0x07;  // Unknown, observed always 0x07 on both sides (hid_reports.md) -- not
                       // understood, replayed as a safe constant.

    // Analog stick: a lone Joy-Con 2 has exactly one physical stick (Confirmed,
    // docs/experiments/joycon2-spi-dump-analysis-2026-07-14.md §3.8 -- only one of the two
    // factory calibration slots is ever populated). Deliberate design choice, not evidence-based:
    // always source from `in->left_stick` regardless of emulated side, since this project's
    // shared input model conventionally carries a source device's primary/movement stick there.
    // Revisit if a real source device's mapping convention turns out to disagree.
    memcpy(&out[0x5], in->left_stick, 3);

    out[0x8] = 0;      // Unknown
    // 0x9..0xD mouse data: left zero, no source. 0xE NFC state: 0 (idle/none) on both sides --
    // correct as-is for Left (which genuinely has no NFC hardware); a safe "not present" default
    // for Right (which does have real NFC hardware but this project emulates none).
    // 0xF motion data length: 0, no motion source. 0x10..0x37 motion data, 0x38..0x3E reserved:
    // left zeroed by the memset above.
}

// Report 0x05 -- Confirmed shared format (ndeadly's hid_reports.md "Input Report 0x05"), same
// table switch_gc_encode_report05() already implements a subset of. Adds the side-specific
// L/R/ZL/ZR bits a lone Joy-Con genuinely has (unlike GameCube, which has no equivalent and
// leaves these at zero) via the ordinary shared SWITCH_MASK_L/ZL/R/ZR fields.
void switch_joycon2_encode_report05(const switch_pro_input_t *in, joycon2_side_t side,
                                     uint32_t counter, uint8_t out[63]) {
    memset(out, 0, 63);
    out[0] = (uint8_t)counter;
    out[1] = (uint8_t)(counter >> 8);
    out[2] = (uint8_t)(counter >> 16);
    out[3] = (uint8_t)(counter >> 24);

    uint8_t s0 = in->buttons[0], s1 = in->buttons[1], s2 = in->buttons[2];
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    // Byte 0: 0x80 ZR | 0x40 R | 0x20 SL(right) | 0x10 SR(right) | 0x08 A | 0x04 B | 0x02 X | 0x01 Y
    if (s0 & SWITCH_MASK_A) b0 |= 0x08;
    if (s0 & SWITCH_MASK_B) b0 |= 0x04;
    if (s0 & SWITCH_MASK_X) b0 |= 0x02;
    if (s0 & SWITCH_MASK_Y) b0 |= 0x01;
    if (side == JOYCON2_SIDE_RIGHT) {
        if (s0 & SWITCH_MASK_ZR) b0 |= 0x80;
        if (s0 & SWITCH_MASK_R) b0 |= 0x40;
        if (in->extra & SWITCH_EXTRA_SL) b0 |= 0x20;
        if (in->extra & SWITCH_EXTRA_SR) b0 |= 0x10;
    }
    // Byte 1: 0x40 C | 0x20 Capture | 0x10 Home | 0x08 Left stick | 0x04 Right stick | 0x02 Plus
    //         | 0x01 Minus
    if (s1 & SWITCH_MASK_CAPTURE) b1 |= 0x20;
    if (s1 & SWITCH_MASK_HOME)    b1 |= 0x10;
    if (s1 & SWITCH_MASK_PLUS)    b1 |= 0x02;
    if (s1 & SWITCH_MASK_MINUS)   b1 |= 0x01;
    if (in->extra & SWITCH_EXTRA_C) b1 |= 0x40;
    if (s1 & SWITCH_MASK_L3) b1 |= 0x08;
    if (s1 & SWITCH_MASK_R3) b1 |= 0x04;
    // Byte 2: 0x80 ZL | 0x40 L | 0x20 SL(left) | 0x10 SR(left) | 0x08 Left | 0x04 Right |
    //         0x02 Up | 0x01 Down
    if (s2 & SWITCH_MASK_DPAD_LEFT)  b2 |= 0x08;
    if (s2 & SWITCH_MASK_DPAD_RIGHT) b2 |= 0x04;
    if (s2 & SWITCH_MASK_DPAD_UP)    b2 |= 0x02;
    if (s2 & SWITCH_MASK_DPAD_DOWN)  b2 |= 0x01;
    if (side == JOYCON2_SIDE_LEFT) {
        if (s2 & SWITCH_MASK_ZL) b2 |= 0x80;
        if (s2 & SWITCH_MASK_L) b2 |= 0x40;
        if (in->extra & SWITCH_EXTRA_SL) b2 |= 0x20;
        if (in->extra & SWITCH_EXTRA_SR) b2 |= 0x10;
    }
    // Byte 3: 0x02 GL | 0x01 GR (0x10 Headset not modeled -- no source)
    if (in->extra & SWITCH_EXTRA_GL) b3 |= 0x02;
    if (in->extra & SWITCH_EXTRA_GR) b3 |= 0x01;
    out[0x4] = b0;
    out[0x5] = b1;
    out[0x6] = b2;
    out[0x7] = b3;

    // Left/right analog stick fields both exist in this shared format regardless of controller
    // type (Confirmed, ndeadly's table). Same design choice as switch_joycon2_encode_report()
    // above: always source the single physical stick from in->left_stick, leave right_stick at
    // its neutral (zeroed = 0,0 raw, not centered -- matches switch_gc_encode_report05()'s
    // identical treatment of its own unused stick fields) rather than inventing a centered value.
    memcpy(&out[0xA], in->left_stick, 3);

    out[0x1F] = 0xA0;  // battery voltage ~4000 mV (0x0FA0 LE) -- same safe default as
    out[0x20] = 0x0F;  // switch_gc_encode_report05()'s identical field.
    out[0x21] = 0x20;  // charge state
    out[0x29] = 0x01;  // always 0x01, per documented layout
    // 0x10..0x18 mouse data, 0x19..0x1E magnetometer, 0x22..0x28 unknown/battery-current,
    // 0x2A..0x3B motion, 0x3C..0x3E (GameCube-only trigger tail, not applicable here): left
    // zeroed by the memset above -- no source for any of them yet.
}
