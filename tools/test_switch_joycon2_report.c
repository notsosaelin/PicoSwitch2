/*
 * Host-compilable golden tests for the pure Joy-Con 2 sideways report encoders.
 *
 *   gcc -I include -I src/bt_hid -o test_switch_joycon2_report \
 *       tools/test_switch_joycon2_report.c src/switch_joycon2/switch_joycon2_encode.c \
 *       src/controller_battery.c
 */
#include <stdio.h>
#include <string.h>

#include "switch_joycon2_encode.h"
#include "core/buttons.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s\n", msg);                                      \
            failures++;                                                     \
        } else {                                                             \
            printf("OK:   %s\n", msg);                                      \
        }                                                                    \
    } while (0)

enum source_field { SRC_S0, SRC_S1, SRC_S2, SRC_EXTRA, SRC_RAW };

typedef struct {
    enum source_field field;
    uint32_t source;
    uint8_t output_offset;
    uint8_t expected;
    const char *name;
} button_case_t;

static switch_pro_input_t neutral_input(void) {
    switch_pro_input_t in;
    memset(&in, 0, sizeof(in));
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.right_stick);
    return in;
}

static void set_source(switch_pro_input_t *in, uint32_t *raw,
                       enum source_field field, uint32_t source) {
    if (field == SRC_RAW) *raw = source;
    else if (field == SRC_EXTRA) in->extra = (uint8_t)source;
    else in->buttons[field] = (uint8_t)source;
}

static uint16_t stick_x(const uint8_t p[3]) {
    return (uint16_t)(p[0] | ((uint16_t)(p[1] & 0x0F) << 8));
}

static uint16_t stick_y(const uint8_t p[3]) {
    return (uint16_t)((p[1] >> 4) | ((uint16_t)p[2] << 4));
}

static void check_button_cases(const button_case_t *cases, size_t count,
                               joycon2_side_t side, uint8_t source_joycon_side,
                               bool report05) {
    uint8_t out[63];
    for (size_t i = 0; i < count; ++i) {
        switch_pro_input_t in = neutral_input();
        in.source_joycon_side = source_joycon_side;
        uint32_t raw = 0;
        set_source(&in, &raw, cases[i].field, cases[i].source);
        if (report05)
            switch_joycon2_encode_report05(&in, raw, side, 0, out);
        else
            switch_joycon2_encode_report(&in, raw, side, 0, out);
        CHECK(out[cases[i].output_offset] == cases[i].expected, cases[i].name);
    }
}

int main(void) {
    uint8_t out[63];

    const button_case_t left_ext[] = {
        {SRC_RAW, JP_BUTTON_L3,        0x2, 0x80, "L ext: L3 -> stick click"},
        {SRC_RAW, JP_BUTTON_S1,        0x2, 0x40, "L ext: Select -> Minus"},
        {SRC_RAW, JP_BUTTON_R2,        0x2, 0x20, "L ext: R2/RT -> ZL"},
        {SRC_RAW, JP_BUTTON_L2,        0x2, 0x10, "L ext: L2/LT -> L"},
        {SRC_RAW, JP_BUTTON_B3,        0x2, 0x08, "L ext: Square/X -> D-pad Up"},
        {SRC_RAW, JP_BUTTON_B1,        0x2, 0x04, "L ext: Cross/A -> D-pad Left"},
        {SRC_RAW, JP_BUTTON_B4,        0x2, 0x02, "L ext: Triangle/Y -> D-pad Right"},
        {SRC_RAW, JP_BUTTON_B2,        0x2, 0x01, "L ext: Circle/B -> D-pad Down"},
        {SRC_RAW, JP_BUTTON_L1,        0x3, 0x80, "L ext: L1/LB -> SL"},
        {SRC_RAW, JP_BUTTON_R1,        0x3, 0x40, "L ext: R1/RB -> SR"},
        {SRC_S1, SWITCH_MASK_CAPTURE, 0x3, 0x01, "L ext: Pro2 Capture source -> Capture"},
    };
    const button_case_t right_ext[] = {
        {SRC_RAW, JP_BUTTON_L3,      0x2, 0x80, "R ext: L3 -> stick click"},
        {SRC_RAW, JP_BUTTON_S2,      0x2, 0x40, "R ext: Start -> Plus"},
        {SRC_RAW, JP_BUTTON_R2,      0x2, 0x20, "R ext: R2/RT -> ZR"},
        {SRC_RAW, JP_BUTTON_L2,      0x2, 0x10, "R ext: L2/LT -> R"},
        {SRC_RAW, JP_BUTTON_B2,      0x2, 0x08, "R ext: Circle/B -> X"},
        {SRC_RAW, JP_BUTTON_B4,      0x2, 0x04, "R ext: Triangle/Y -> Y"},
        {SRC_RAW, JP_BUTTON_B1,      0x2, 0x02, "R ext: Cross/A -> A"},
        {SRC_RAW, JP_BUTTON_B3,      0x2, 0x01, "R ext: Square/X -> B"},
        {SRC_RAW, JP_BUTTON_L1,      0x3, 0x80, "R ext: L1/LB -> SL"},
        {SRC_RAW, JP_BUTTON_R1,      0x3, 0x40, "R ext: R1/RB -> SR"},
        {SRC_EXTRA, SWITCH_EXTRA_C, 0x3, 0x10, "R ext: Pro2 C/mute source -> Chat"},
        {SRC_RAW, JP_BUTTON_A1,      0x3, 0x01, "R ext: Home/Guide -> Home"},
    };

    check_button_cases(left_ext, sizeof(left_ext) / sizeof(left_ext[0]),
                       JOYCON2_SIDE_LEFT, SWITCH_SOURCE_JOYCON_NONE, false);
    check_button_cases(right_ext, sizeof(right_ext) / sizeof(right_ext[0]),
                       JOYCON2_SIDE_RIGHT, SWITCH_SOURCE_JOYCON_NONE, false);

    const button_case_t native_left_ext[] = {
        {SRC_RAW, JP_BUTTON_L3, 0x2, 0x80, "native L ext: stick click"},
        {SRC_RAW, JP_BUTTON_S1, 0x2, 0x40, "native L ext: Minus"},
        {SRC_RAW, JP_BUTTON_L2, 0x2, 0x20, "native L ext: ZL"},
        {SRC_RAW, JP_BUTTON_L1, 0x2, 0x10, "native L ext: L"},
        {SRC_RAW, JP_BUTTON_DU, 0x2, 0x08, "native L ext: D-pad Up"},
        {SRC_RAW, JP_BUTTON_DL, 0x2, 0x04, "native L ext: D-pad Left"},
        {SRC_RAW, JP_BUTTON_DR, 0x2, 0x02, "native L ext: D-pad Right"},
        {SRC_RAW, JP_BUTTON_DD, 0x2, 0x01, "native L ext: D-pad Down"},
        {SRC_RAW, JP_BUTTON_SL, 0x3, 0x80, "native L ext: SL"},
        {SRC_RAW, JP_BUTTON_SR, 0x3, 0x40, "native L ext: SR"},
        {SRC_RAW, JP_BUTTON_A2, 0x3, 0x01, "native L ext: Capture"},
    };
    const button_case_t native_right_ext[] = {
        {SRC_RAW, JP_BUTTON_R3, 0x2, 0x80, "native R ext: stick click"},
        {SRC_RAW, JP_BUTTON_S2, 0x2, 0x40, "native R ext: Plus"},
        {SRC_RAW, JP_BUTTON_R2, 0x2, 0x20, "native R ext: ZR"},
        {SRC_RAW, JP_BUTTON_R1, 0x2, 0x10, "native R ext: R"},
        {SRC_RAW, JP_BUTTON_B4, 0x2, 0x08, "native R ext: X"},
        {SRC_RAW, JP_BUTTON_B3, 0x2, 0x04, "native R ext: Y"},
        {SRC_RAW, JP_BUTTON_B2, 0x2, 0x02, "native R ext: A"},
        {SRC_RAW, JP_BUTTON_B1, 0x2, 0x01, "native R ext: B"},
        {SRC_RAW, JP_BUTTON_SL, 0x3, 0x80, "native R ext: SL"},
        {SRC_RAW, JP_BUTTON_SR, 0x3, 0x40, "native R ext: SR"},
        {SRC_RAW, JP_BUTTON_A3, 0x3, 0x10, "native R ext: C / GameChat"},
        {SRC_RAW, JP_BUTTON_A1, 0x3, 0x01, "native R ext: Home"},
    };
    check_button_cases(native_left_ext,
                       sizeof(native_left_ext) / sizeof(native_left_ext[0]),
                       JOYCON2_SIDE_LEFT, SWITCH_SOURCE_JOYCON_LEFT, false);
    check_button_cases(native_right_ext,
                       sizeof(native_right_ext) / sizeof(native_right_ext[0]),
                       JOYCON2_SIDE_RIGHT, SWITCH_SOURCE_JOYCON_RIGHT, false);

    {
        switch_pro_input_t in = neutral_input();
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_LEFT, 0x5A, out);
        CHECK(out[0] == 0x5A && out[1] == 0x25 && out[4] == 0x07,
              "ext: counter, power, and constant fields preserved");
        CHECK(out[2] == 0 && out[3] == 0, "ext: neutral buttons stay neutral");
    }

    {
        switch_pro_input_t in = neutral_input();
        in.battery_valid = 1;
        in.battery_level = 50;
        in.battery_charging = 1;
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x1] == 0x17,
              "L battery: half-full charging source passes through");
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x1] == 0x17,
              "R battery: half-full charging source passes through");
        in.battery_level = 0;
        in.battery_charging = 0;
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(out[0x1] == 0x01,
              "Joy-Con battery: valid empty is distinct from unknown");
    }

    // The paired D-pad drives the single stick, never the Joy-Con's physical button cluster.
    {
        switch_pro_input_t in = neutral_input();
        switch_pro_pack_stick(0x345, 0xABC, in.left_stick);
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(stick_x(&out[5]) == 0xABC && stick_y(&out[5]) == 0xCBB,
              "stick L: source left analog rotates clockwise into local coordinates");

        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(stick_x(&out[5]) == 0x544 && stick_y(&out[5]) == 0x345,
              "stick R: source left analog rotates counter-clockwise into local coordinates");

        uint32_t raw = JP_BUTTON_DL;
        switch_joycon2_encode_report(&in, raw, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(stick_x(&out[5]) == 0xABC && stick_y(&out[5]) == SWITCH_STICK_MAX,
              "stick L: source D-pad Left becomes local Up after rotation");
        CHECK(out[2] == 0 && out[3] == 0,
              "stick: source D-pad does not leak into Left physical D-pad buttons");

        raw = JP_BUTTON_DR | JP_BUTTON_DU;
        switch_joycon2_encode_report(&in, raw, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(stick_x(&out[5]) == SWITCH_STICK_MIN && stick_y(&out[5]) == SWITCH_STICK_MAX,
              "stick R: source D-pad Up+Right rotates to local Up+Left");
        CHECK(out[2] == 0 && out[3] == 0,
              "stick: source D-pad does not leak into Right face buttons");

        raw = JP_BUTTON_DL | JP_BUTTON_DR | JP_BUTTON_DU | JP_BUTTON_DD;
        switch_joycon2_encode_report(&in, raw, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(stick_x(&out[5]) == 0xABC && stick_y(&out[5]) == 0xCBB,
              "stick: opposing D-pad directions cancel before axis rotation");
    }

    const button_case_t left_05[] = {
        {SRC_S1, SWITCH_MASK_CAPTURE, 0x5, 0x20, "L 05: Capture"},
        {SRC_RAW, JP_BUTTON_S1,        0x5, 0x01, "L 05: Minus"},
        {SRC_RAW, JP_BUTTON_L3,        0x5, 0x08, "L 05: L3 -> physical left stick click"},
        {SRC_RAW, JP_BUTTON_R2,        0x6, 0x80, "L 05: R2/RT -> ZL"},
        {SRC_RAW, JP_BUTTON_L2,        0x6, 0x40, "L 05: L2/LT -> L"},
        {SRC_RAW, JP_BUTTON_L1,        0x6, 0x20, "L 05: L1/LB -> SL"},
        {SRC_RAW, JP_BUTTON_R1,        0x6, 0x10, "L 05: R1/RB -> SR"},
        {SRC_RAW, JP_BUTTON_B1,        0x6, 0x08, "L 05: Cross/A -> D-pad Left"},
        {SRC_RAW, JP_BUTTON_B4,        0x6, 0x04, "L 05: Triangle/Y -> D-pad Right"},
        {SRC_RAW, JP_BUTTON_B3,        0x6, 0x02, "L 05: Square/X -> D-pad Up"},
        {SRC_RAW, JP_BUTTON_B2,        0x6, 0x01, "L 05: Circle/B -> D-pad Down"},
    };
    const button_case_t right_05[] = {
        {SRC_RAW, JP_BUTTON_R2,      0x4, 0x80, "R 05: R2/RT -> ZR"},
        {SRC_RAW, JP_BUTTON_L2,      0x4, 0x40, "R 05: L2/LT -> R"},
        {SRC_RAW, JP_BUTTON_L1,      0x4, 0x20, "R 05: L1/LB -> SL"},
        {SRC_RAW, JP_BUTTON_R1,      0x4, 0x10, "R 05: R1/RB -> SR"},
        {SRC_RAW, JP_BUTTON_B1,      0x4, 0x08, "R 05: Cross/A -> A"},
        {SRC_RAW, JP_BUTTON_B3,      0x4, 0x04, "R 05: Square/X -> B"},
        {SRC_RAW, JP_BUTTON_B2,      0x4, 0x02, "R 05: Circle/B -> X"},
        {SRC_RAW, JP_BUTTON_B4,      0x4, 0x01, "R 05: Triangle/Y -> Y"},
        {SRC_EXTRA, SWITCH_EXTRA_C, 0x5, 0x40, "R 05: Chat"},
        {SRC_RAW, JP_BUTTON_A1,      0x5, 0x10, "R 05: Home"},
        {SRC_RAW, JP_BUTTON_L3,      0x5, 0x04, "R 05: L3 -> physical right stick click"},
        {SRC_RAW, JP_BUTTON_S2,      0x5, 0x02, "R 05: Plus"},
    };

    check_button_cases(left_05, sizeof(left_05) / sizeof(left_05[0]),
                       JOYCON2_SIDE_LEFT, SWITCH_SOURCE_JOYCON_NONE, true);
    check_button_cases(right_05, sizeof(right_05) / sizeof(right_05[0]),
                       JOYCON2_SIDE_RIGHT, SWITCH_SOURCE_JOYCON_NONE, true);

    const button_case_t native_left_05[] = {
        {SRC_RAW, JP_BUTTON_A2, 0x5, 0x20, "native L 05: Capture"},
        {SRC_RAW, JP_BUTTON_S1, 0x5, 0x01, "native L 05: Minus"},
        {SRC_RAW, JP_BUTTON_L3, 0x5, 0x08, "native L 05: stick click"},
        {SRC_RAW, JP_BUTTON_L2, 0x6, 0x80, "native L 05: ZL"},
        {SRC_RAW, JP_BUTTON_L1, 0x6, 0x40, "native L 05: L"},
        {SRC_RAW, JP_BUTTON_SL, 0x6, 0x20, "native L 05: SL"},
        {SRC_RAW, JP_BUTTON_SR, 0x6, 0x10, "native L 05: SR"},
        {SRC_RAW, JP_BUTTON_DL, 0x6, 0x08, "native L 05: D-pad Left"},
        {SRC_RAW, JP_BUTTON_DR, 0x6, 0x04, "native L 05: D-pad Right"},
        {SRC_RAW, JP_BUTTON_DU, 0x6, 0x02, "native L 05: D-pad Up"},
        {SRC_RAW, JP_BUTTON_DD, 0x6, 0x01, "native L 05: D-pad Down"},
    };
    const button_case_t native_right_05[] = {
        {SRC_RAW, JP_BUTTON_R2, 0x4, 0x80, "native R 05: ZR"},
        {SRC_RAW, JP_BUTTON_R1, 0x4, 0x40, "native R 05: R"},
        {SRC_RAW, JP_BUTTON_SL, 0x4, 0x20, "native R 05: SL"},
        {SRC_RAW, JP_BUTTON_SR, 0x4, 0x10, "native R 05: SR"},
        {SRC_RAW, JP_BUTTON_B2, 0x4, 0x08, "native R 05: A"},
        {SRC_RAW, JP_BUTTON_B1, 0x4, 0x04, "native R 05: B"},
        {SRC_RAW, JP_BUTTON_B4, 0x4, 0x02, "native R 05: X"},
        {SRC_RAW, JP_BUTTON_B3, 0x4, 0x01, "native R 05: Y"},
        {SRC_RAW, JP_BUTTON_A3, 0x5, 0x40, "native R 05: C / GameChat"},
        {SRC_RAW, JP_BUTTON_A1, 0x5, 0x10, "native R 05: Home"},
        {SRC_RAW, JP_BUTTON_R3, 0x5, 0x04, "native R 05: stick click"},
        {SRC_RAW, JP_BUTTON_S2, 0x5, 0x02, "native R 05: Plus"},
    };
    check_button_cases(native_left_05,
                       sizeof(native_left_05) / sizeof(native_left_05[0]),
                       JOYCON2_SIDE_LEFT, SWITCH_SOURCE_JOYCON_LEFT, true);
    check_button_cases(native_right_05,
                       sizeof(native_right_05) / sizeof(native_right_05[0]),
                       JOYCON2_SIDE_RIGHT, SWITCH_SOURCE_JOYCON_RIGHT, true);

    {
        switch_pro_input_t in = neutral_input();
        switch_pro_pack_stick(0x234, 0x567, in.left_stick);
        switch_pro_pack_stick(0x89A, 0xBCD, in.right_stick);

        in.source_joycon_side = SWITCH_SOURCE_JOYCON_LEFT;
        switch_joycon2_encode_report(&in, JP_BUTTON_DU, JOYCON2_SIDE_LEFT, 0, out);
        CHECK(stick_x(&out[5]) == 0x234 && stick_y(&out[5]) == 0x567,
              "native L stick is emitted unrotated and D-pad stays a button");
        CHECK(out[2] == 0x08, "native L D-pad remains in its canonical button field");

        in.source_joycon_side = SWITCH_SOURCE_JOYCON_RIGHT;
        switch_joycon2_encode_report(&in, JP_BUTTON_B1, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(stick_x(&out[5]) == 0x89A && stick_y(&out[5]) == 0xBCD,
              "native R stick uses the physical right slot without rotation");
        CHECK(out[2] == 0x01, "native R physical B keeps the canonical B bit");

        // A physical half only gets the native path when it matches the USB
        // personality. This protects the already-enumerated side identity.
        in.source_joycon_side = SWITCH_SOURCE_JOYCON_LEFT;
        switch_joycon2_encode_report(&in, JP_BUTTON_B1, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[2] == 0x02,
              "mismatched physical half retains the generic mapping safely");
    }

    {
        switch_pro_input_t in = neutral_input();
        in.has_mouse = 1;
        in.mouse_enabled = 1;
        in.mouse_delta_x = -321;
        in.mouse_delta_y = 1234;
        in.mouse_motion_timing = 0x2345;
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x9] == 0xBF && out[0xA] == 0xFE,
              "mouse: signed delta X is emitted little-endian");
        CHECK(out[0xB] == 0xD2 && out[0xC] == 0x04,
              "mouse: signed delta Y is emitted little-endian");
        CHECK(out[0xD] == 0x17, "mouse: synthesized lift-off matches the working reference");
        CHECK(out[0x8] == 0x38, "mouse: steady-state Joy-Con report status is present");
        CHECK(out[0xF] == 30 && out[0x10] == 0x45 && out[0x11] == 0x23,
              "mouse: stationary posture block has length and advancing timing");
        CHECK(out[0x12] == 0x00 && out[0x13] == 0x0C && out[0x23] == 0x10,
              "mouse: stationary posture carries temperature and +X gravity");

        uint8_t expected_stick[3];
        in.mouse_scroll = 1;
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_RIGHT, 0, out);
        switch_pro_pack_stick(SWITCH_STICK_MID, 0, expected_stick);
        CHECK(memcmp(&out[0x5], expected_stick, sizeof(expected_stick)) == 0,
              "mouse: wheel up holds the local Joy-Con stick Up");

        in.mouse_scroll = -1;
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_LEFT, 0, out);
        switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MAX, expected_stick);
        CHECK(memcmp(&out[0x5], expected_stick, sizeof(expected_stick)) == 0,
              "mouse: wheel down holds the local Joy-Con stick Down");

        in.mouse_enabled = 0;
        switch_joycon2_encode_report(&in, 0, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(out[0x9] == 0 && out[0xA] == 0 && out[0xB] == 0 &&
              out[0xC] == 0 && out[0xD] == 0 && out[0xF] == 0,
              "mouse: fields stay zero until the console enables feature bit 0x10");
    }

    {
        switch_pro_input_t in = neutral_input();
        in.extra = SWITCH_EXTRA_GL | SWITCH_EXTRA_GR;
        switch_joycon2_encode_report05(&in, 0, JOYCON2_SIDE_LEFT, 0x01020304u, out);
        CHECK(out[0] == 0x04 && out[1] == 0x03 && out[2] == 0x02 && out[3] == 0x01,
              "report05: 32-bit counter written little-endian");
        CHECK(out[7] == 0, "report05: Pro2 GL/GR paddles do not masquerade as Joy-Con controls");

        switch_pro_pack_stick(0x456, 0x789, in.left_stick);
        switch_joycon2_encode_report05(&in, JP_BUTTON_DD, JOYCON2_SIDE_RIGHT, 0, out);
        CHECK(stick_x(&out[0xA]) == SWITCH_STICK_MAX && stick_y(&out[0xA]) == 0x456,
              "report05: same D-pad synthesis and Right-side rotation are used");
    }

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
