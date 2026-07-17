#include <string.h>

#include "switch_gc_encode.h"

// Right Stick/Left Stick click bits (byte 0x2/0x3, bit 0x80) are hardcoded to 0 always and
// must never read SWITCH_MASK_L3/R3 -- per NSO-GC.md's explicit physical correction, the
// genuine NSO GameCube Controller has no L3/R3 hardware; docs/switch2-gc/mapping.md: "the
// output personality must never expose L3/R3 destinations."
void switch_gc_encode_report(const switch_pro_input_t *in, uint8_t counter, uint8_t out[63]) {
    memset(out, 0, 63);
    out[0x0] = counter;
    out[0x1] = 0x25;  // power: external power + not charging + battery level 9/9 (safe default;
                      // no real battery-telemetry source for a USB-tethered Pico)

    uint8_t s0 = in->buttons[0], s1 = in->buttons[1], s2 = in->buttons[2];
    uint8_t b0 = 0, b1 = 0, b2 = 0;
    // Z/L-detent/R-detent/ZL bit positions: Corrected 2026-07-13 by direct hardware evidence on
    // a real Switch 2 console's Test Input screen -- this superseded ndeadly's documented table,
    // which this originally followed byte-for-byte. With the original (ndeadly-table) encoding, a
    // live console displayed: Z (was byte0 0x10) as "L", R-detent (was byte0 0x20) as "ZR",
    // L-detent (was byte1 0x20) as "ZL" -- a clean 4-way rotation across all four slots (the
    // untested ZL bit, by elimination, must have displayed as "R"). A first fix applied the
    // inverse rotation; a second real-console re-test confirmed Z and ZL now show correctly but
    // found L/R detent still swapped with each other -- fixed here by swapping just that pair
    // (Z and ZL slots are unchanged, confirmed correct). Treat ndeadly's table as wrong for this
    // nibble until independently re-verified.
    //
    // Byte 0: 0x80 Right Stick (N/A, never set -- no L3/R3 on genuine hardware) | 0x40 Plus |
    //         0x20 Z | 0x10 R detent | 0x08 X | 0x04 Y | 0x02 A | 0x01 B
    if (s0 & SWITCH_MASK_A) b0 |= 0x02;
    if (s0 & SWITCH_MASK_B) b0 |= 0x01;
    if (s0 & SWITCH_MASK_X) b0 |= 0x08;
    if (s0 & SWITCH_MASK_Y) b0 |= 0x04;
    if (s1 & SWITCH_MASK_PLUS) b0 |= 0x40;
    if (in->gc_extra & GC_MASK_R_DETENT) b0 |= 0x10;
    if (in->gc_extra & GC_MASK_Z) b0 |= 0x20;
    // Byte 1: 0x80 Left Stick (N/A, never set) | 0x40 Minus | 0x20 ZL |
    //         0x10 L detent | 0x08 Up | 0x04 Left | 0x02 Right | 0x01 Down
    if (s1 & SWITCH_MASK_MINUS) b1 |= 0x40;
    if (in->gc_extra & GC_MASK_L_DETENT) b1 |= 0x10;
    if ((s2 & SWITCH_MASK_ZL) || (in->gc_extra & GC_MASK_ZL)) b1 |= 0x20;
    if (s2 & SWITCH_MASK_DPAD_UP) b1 |= 0x08;
    if (s2 & SWITCH_MASK_DPAD_LEFT) b1 |= 0x04;
    if (s2 & SWITCH_MASK_DPAD_RIGHT) b1 |= 0x02;
    if (s2 & SWITCH_MASK_DPAD_DOWN) b1 |= 0x01;
    // Byte 2: 0x10 C | 0x02 Capture | 0x01 Home
    if (in->extra & SWITCH_EXTRA_C) b2 |= 0x10;
    if (s1 & SWITCH_MASK_CAPTURE) b2 |= 0x02;
    if (s1 & SWITCH_MASK_HOME) b2 |= 0x01;
    out[0x2] = b0;
    out[0x3] = b1;
    out[0x4] = b2;

    // Sticks arrive packed as two 12-bit values in 3 bytes, the shared Switch 2 report convention.
    memcpy(&out[0x5], in->left_stick, 3);
    memcpy(&out[0x8], in->right_stick, 3);

    out[0xB] = 0x30;  // feature bit 5 not set (Stage D has not implemented feature negotiation)
    out[0xC] = in->left_trigger;   // continuous, never thresholded/collapsed -- raw passthrough
    out[0xD] = in->right_trigger;
    out[0xE] = 0;     // Motion Data Length -- 0 (no motion source for GC yet; matches one of
                      // ndeadly's documented observed values {0, 30, 40})
    // 0xF..0x36 motion data, 0x37..0x3E reserved: left zeroed by the memset above.
}

// Report 0x05 -- Confirmed 2026-07-13 by direct hardware evidence to be REQUIRED, not optional:
// a live USBPcap capture of Steam initializing this project's own Pico (GameCube personality)
// found Steam selects report ID 0x05 exclusively, never 0x0A -- see
// docs/switch2-gc/protocol.md "Input Report 0x05" and the capture evidence cited in
// docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md. Report 0x0A remains implemented for
// the real Switch 2 console path; 0x05 is what PC/Steam hosts use for every Switch-family
// controller, GC included -- exactly like
// switch_pro2.c's own ns2_build_report_05() already does for Pro Controller 2.
//
// Deliberately a SEPARATE, independent encoder rather than calling into switch_pro2.c's static
// ns2_build_report_05() -- that function is private to the Pro2 module and carries Pro2-only
// assumptions (its own counter, its own motion block) that would couple this personality to
// Pro2's internal state. The bit LAYOUT is genuinely common (per protocol.md, "common framing
// across controller types") and is intentionally duplicated here, not shared code -- see that
// function for the byte-for-byte-identical button bit assignments this mirrors.
//
// Bits with no real GC hardware are hardcoded to 0, never read from the shared struct:
//   - ZR (byte0 0x80) and plain L/R shoulder (byte0 0x40, byte2 0x40): Pro2-only concepts with no
//     GC equivalent bit position in this shared format at all (GC's own Z/L-detent/R-detent have
//     no representable bit here -- report 0x05 simply cannot carry them; only report 0x0A can).
//   - L3/R3 (byte1 0x08/0x04) and GL/GR (byte3 0x02/0x01): genuine NSO GameCube Controller has
//     none of these -- same rule as switch_gc_encode_report()'s stick-click bits.
void switch_gc_encode_report05(const switch_pro_input_t *in, uint32_t counter, uint8_t out[63]) {
    memset(out, 0, 63);
    out[0] = (uint8_t)counter;
    out[1] = (uint8_t)(counter >> 8);
    out[2] = (uint8_t)(counter >> 16);
    out[3] = (uint8_t)(counter >> 24);

    uint8_t s0 = in->buttons[0], s1 = in->buttons[1], s2 = in->buttons[2];
    uint8_t b0 = 0, b1 = 0, b2 = 0;
    if (s0 & SWITCH_MASK_A) b0 |= 0x08;
    if (s0 & SWITCH_MASK_B) b0 |= 0x04;
    if (s0 & SWITCH_MASK_X) b0 |= 0x02;
    if (s0 & SWITCH_MASK_Y) b0 |= 0x01;
    if (s1 & SWITCH_MASK_CAPTURE) b1 |= 0x20;
    if (s1 & SWITCH_MASK_HOME)    b1 |= 0x10;
    if (s1 & SWITCH_MASK_PLUS)    b1 |= 0x02;
    if (s1 & SWITCH_MASK_MINUS)   b1 |= 0x01;
    if (in->extra & SWITCH_EXTRA_C) b1 |= 0x40;
    if ((s2 & SWITCH_MASK_ZL) || (in->gc_extra & GC_MASK_ZL)) b2 |= 0x80;
    if (s2 & SWITCH_MASK_DPAD_LEFT)  b2 |= 0x08;
    if (s2 & SWITCH_MASK_DPAD_RIGHT) b2 |= 0x04;
    if (s2 & SWITCH_MASK_DPAD_UP)    b2 |= 0x02;
    if (s2 & SWITCH_MASK_DPAD_DOWN)  b2 |= 0x01;
    out[0x4] = b0;
    out[0x5] = b1;
    out[0x6] = b2;
    // byte 0x7 (GL/GR in Pro2's layout): left 0 -- no GC hardware equivalent.

    memcpy(&out[0x0A], in->left_stick, 3);
    memcpy(&out[0x0D], in->right_stick, 3);

    out[0x1F] = 0xA0;  // battery voltage ~4000 mV (0x0FA0 LE) -- same safe default as Pro2's
    out[0x20] = 0x0F;
    out[0x21] = 0x20;  // charge state
    out[0x29] = 0x01;  // always 0x01, per documented layout

    // GC-specific tail (Confirmed, protocol.md "Input Report 0x05"): continuous analog L/R
    // trigger, raw passthrough, never thresholded -- same source fields as report 0x0A's tail.
    out[0x3C] = in->left_trigger;
    out[0x3D] = in->right_trigger;
    out[0x3E] = 0;  // reserved
    // 0x2A..0x3B (motion block): left zeroed -- GC has no motion source in this project yet.
}
