/*
 * Host-compilable golden tests for the pure Joy-Con 2 report encoders
 * (src/switch_joycon2/switch_joycon2_encode.c). No pico-sdk/TinyUSB dependency:
 *
 *   gcc -I include -o test_switch_joycon2_report \
 *       tools/test_switch_joycon2_report.c src/switch_joycon2/switch_joycon2_encode.c
 *   ./test_switch_joycon2_report
 *
 * Exit code 0 = all assertions passed.
 */
#include <stdio.h>
#include <string.h>

#include "switch_joycon2_encode.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                      \
            printf("FAIL: %s\n", msg);                                      \
            failures++;                                                    \
        } else {                                                            \
            printf("OK:   %s\n", msg);                                     \
        }                                                                   \
    } while (0)

static switch_pro_input_t neutral_input(void) {
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.right_stick);
    return in;
}

int main(void) {
    uint8_t out[63];

    // --- Report 7 (Left) ---
    {
        switch_pro_input_t in = neutral_input();
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0x00, out);
        CHECK(out[0x0] == 0, "L neutral: counter written verbatim");
        CHECK(out[0x1] == 0x25, "L neutral: power-info default");
        CHECK(out[0x2] == 0 && out[0x3] == 0, "L neutral: button bytes zero");
        CHECK(out[0x4] == 0x07, "L neutral: unknown byte 0x4 == 0x07");
        uint8_t expect_stick[3];
        switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, expect_stick);
        CHECK(memcmp(&out[0x5], expect_stick, 3) == 0, "L neutral: stick centered at 0x5-0x7");
        CHECK(out[0xE] == 0, "L neutral: NFC state 0 (Left has no NFC)");
        CHECK(out[0xF] == 0, "L neutral: motion data length 0");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_MINUS;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x2] == 0x40, "L: Minus -> byte0 0x40");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_ZL;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x2] == 0x20, "L: ZL -> byte0 0x20");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_L;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x2] == 0x10, "L: L -> byte0 0x10");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_UP;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x2] == 0x08, "L: D-pad Up -> byte0 0x08");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_L3;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x2] == 0x80, "L: stick click (L3) -> byte0 0x80");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_CAPTURE;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x3] == 0x01, "L: Capture -> byte1 0x01");

        in = neutral_input(); in.extra = SWITCH_EXTRA_SL;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x3] == 0x80, "L: SL -> byte1 0x80");

        in = neutral_input(); in.extra = SWITCH_EXTRA_SR;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x3] == 0x40, "L: SR -> byte1 0x40");

        // Right-side-only controls must never leak into the Left report.
        in = neutral_input();
        in.buttons[0] = SWITCH_MASK_A | SWITCH_MASK_ZR | SWITCH_MASK_R;
        in.buttons[1] = SWITCH_MASK_PLUS | SWITCH_MASK_R3 | SWITCH_MASK_HOME;
        in.extra = SWITCH_EXTRA_C;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x2] == 0 && out[0x3] == 0, "L: Right-side-only inputs produce no output bits");
    }

    // --- Report 8 (Right) ---
    {
        switch_pro_input_t in = neutral_input();
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0 && out[0x3] == 0, "R neutral: button bytes zero");
        CHECK(out[0xE] == 0, "R neutral: NFC state 0 (no NFC source emulated)");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_PLUS;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x40, "R: Plus -> byte0 0x40");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_ZR;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x20, "R: ZR -> byte0 0x20");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_R;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x10, "R: R -> byte0 0x10");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_X;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x08, "R: X -> byte0 0x08");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_Y;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x04, "R: Y -> byte0 0x04");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_A;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x02, "R: A -> byte0 0x02");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_B;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x01, "R: B -> byte0 0x01");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_R3;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0x80, "R: stick click (R3) -> byte0 0x80");

        in = neutral_input(); in.extra = SWITCH_EXTRA_C;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x3] == 0x10, "R: C -> byte1 0x10");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_HOME;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x3] == 0x01, "R: Home -> byte1 0x01");

        in = neutral_input(); in.extra = SWITCH_EXTRA_SL;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x3] == 0x80, "R: SL -> byte1 0x80");

        in = neutral_input(); in.extra = SWITCH_EXTRA_SR;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x3] == 0x40, "R: SR -> byte1 0x40");

        // Left-side-only controls must never leak into the Right report.
        in = neutral_input();
        in.buttons[2] = SWITCH_MASK_ZL | SWITCH_MASK_L | SWITCH_MASK_DPAD_UP;
        in.buttons[1] = SWITCH_MASK_MINUS | SWITCH_MASK_L3 | SWITCH_MASK_CAPTURE;
        switch_joycon2_encode_report(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x2] == 0 && out[0x3] == 0, "R: Left-side-only inputs produce no output bits");
    }

    // --- Report 0x05 (shared format) ---
    {
        switch_pro_input_t in = neutral_input();
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0x01020304u, out);
        CHECK(out[0] == 0x04 && out[1] == 0x03 && out[2] == 0x02 && out[3] == 0x01,
              "report05: 32-bit counter written little-endian");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_ZL;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x6] == 0x80, "report05 (Left side): ZL -> byte0x6 0x80");
        in = neutral_input(); in.buttons[2] = SWITCH_MASK_ZL;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x6] == 0, "report05 (Right side): ZL from source does not surface (side-gated)");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_ZR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x4] == 0x80, "report05 (Right side): ZR -> byte0x4 0x80");
        in = neutral_input(); in.buttons[0] = SWITCH_MASK_ZR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x4] == 0, "report05 (Left side): ZR from source does not surface (side-gated)");

        in = neutral_input(); in.extra = SWITCH_EXTRA_GL | SWITCH_EXTRA_GR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x7] == 0x03, "report05: GL/GR -> byte0x7 0x02|0x01");

        in = neutral_input(); in.extra = SWITCH_EXTRA_SL;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x4] == 0x20, "report05 (Right side): SL -> byte0x4 0x20");
        in = neutral_input(); in.extra = SWITCH_EXTRA_SR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x4] == 0x10, "report05 (Right side): SR -> byte0x4 0x10");
        in = neutral_input(); in.extra = SWITCH_EXTRA_SL | SWITCH_EXTRA_SR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x4] == 0, "report05 (Left side): SL/SR from source does not surface at byte0x4 (side-gated)");

        in = neutral_input(); in.extra = SWITCH_EXTRA_SL;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x6] == 0x20, "report05 (Left side): SL -> byte0x6 0x20");
        in = neutral_input(); in.extra = SWITCH_EXTRA_SR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x6] == 0x10, "report05 (Left side): SR -> byte0x6 0x10");
        in = neutral_input(); in.extra = SWITCH_EXTRA_SL | SWITCH_EXTRA_SR;
        switch_joycon2_encode_report05(&in, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x6] == 0, "report05 (Right side): SL/SR from source does not surface at byte0x6 (side-gated)");
    }

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
