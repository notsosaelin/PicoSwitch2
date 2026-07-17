/*
 * Host-compilable golden tests for the pure NSO GameCube report-0x0A encoder
 * (src/switch_gc/switch_gc_encode.c). No pico-sdk/TinyUSB dependency -- build
 * and run directly with a host C compiler:
 *
 *   gcc -I include -o test_switch_gc_report \
 *       tools/test_switch_gc_report.c src/switch_gc/switch_gc_encode.c
 *   ./test_switch_gc_report
 *
 * Exit code 0 = all assertions passed.
 *
 * Covers the 10-point list from PROMPT.md's Phase 3: neutral report, each
 * button group, native Z, ZL-vs-Z non-aliasing, analog L/R at min/mid/max,
 * digital detents independent of analog value, analog full-scale without
 * synthesizing a detent, Z+trigger simultaneity, L3/R3 unsynthesizable, and
 * counter behavior.
 */
#include <stdio.h>
#include <string.h>

#include "switch_gc_encode.h"

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
    // Neutral stick position matches report.c's own report_init() neutral (centered).
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in.right_stick);
    return in;
}

int main(void) {
    uint8_t out[63];

    // 1. Neutral report: all buttons/extras zero, sticks centered, everything else at its
    // documented safe-neutral default.
    {
        switch_pro_input_t in = neutral_input();
        switch_gc_encode_report(&in, 0x00, out);
        CHECK(out[0x0] == 0x00, "neutral: counter byte matches supplied value");
        CHECK(out[0x1] == 0x25, "neutral: power-info default (external power, battery 9/9)");
        CHECK(out[0x2] == 0 && out[0x3] == 0 && out[0x4] == 0, "neutral: all button bytes zero");
        uint8_t expect_stick[3];
        switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, expect_stick);
        CHECK(memcmp(&out[0x5], expect_stick, 3) == 0, "neutral: left stick centered");
        CHECK(memcmp(&out[0x8], expect_stick, 3) == 0, "neutral: right stick centered");
        CHECK(out[0xB] == 0x30, "neutral: feature byte default (bit 5 not set)");
        CHECK(out[0xC] == 0 && out[0xD] == 0, "neutral: analog triggers at 0");
        CHECK(out[0xE] == 0, "neutral: motion data length 0");
        bool reserved_zero = true;
        for (int i = 0x37; i <= 0x3E; i++) if (out[i] != 0) reserved_zero = false;
        CHECK(reserved_zero, "neutral: reserved tail (0x37-0x3E) all zero");
    }

    // 2. Each existing button group, one at a time.
    {
        switch_pro_input_t in;

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_A;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x2] == 0x02, "A -> byte0 0x02");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_B;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x2] == 0x01, "B -> byte0 0x01");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_X;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x2] == 0x08, "X -> byte0 0x08");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_Y;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x2] == 0x04, "Y -> byte0 0x04");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_PLUS;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x2] == 0x40, "Plus -> byte0 0x40");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_MINUS;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x3] == 0x40, "Minus -> byte1 0x40");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_ZL;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x3] == 0x20, "ZL -> byte1 0x20");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_UP;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x3] == 0x08, "D-pad Up -> byte1 0x08");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_LEFT;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x3] == 0x04, "D-pad Left -> byte1 0x04");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_RIGHT;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x3] == 0x02, "D-pad Right -> byte1 0x02");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_DOWN;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x3] == 0x01, "D-pad Down -> byte1 0x01");

        in = neutral_input(); in.extra = SWITCH_EXTRA_C;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x4] == 0x10, "C -> byte2 0x10");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_CAPTURE;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x4] == 0x02, "Capture -> byte2 0x02");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_HOME;
        switch_gc_encode_report(&in, 0, out); CHECK(out[0x4] == 0x01, "Home -> byte2 0x01");
    }

    // 3. Native Z only.
    {
        switch_pro_input_t in = neutral_input();
        in.gc_extra = GC_MASK_Z;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x2] == 0x20, "native Z alone -> byte0 0x20, nothing else");
        CHECK(out[0x3] == 0 && out[0x4] == 0, "native Z alone -> bytes 1,2 untouched");
    }

    // 4. ZL only, proving it does not alias Z.
    {
        switch_pro_input_t in = neutral_input();
        in.buttons[2] = SWITCH_MASK_ZL;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x3] == 0x20, "ZL alone -> byte1 0x20");
        CHECK((out[0x2] & 0x20) == 0, "ZL alone does NOT set byte0 0x20 (native Z bit)");

        in = neutral_input();
        in.gc_extra = GC_MASK_ZL;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x3] == 0x20, "native-layout ZL alone -> byte1 0x20");
        CHECK((out[0x2] & 0x20) == 0, "native-layout ZL does NOT alias native Z");
    }

    // 5. Analog L/R at 0x00, an intermediate value, and 0xFF.
    {
        switch_pro_input_t in = neutral_input();
        in.left_trigger = 0x00; in.right_trigger = 0x00;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0xC] == 0x00 && out[0xD] == 0x00, "analog L/R at 0x00 passthrough exact");

        in.left_trigger = 0x7F; in.right_trigger = 0x40;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0xC] == 0x7F && out[0xD] == 0x40, "analog L/R at intermediate values passthrough exact");

        in.left_trigger = 0xFF; in.right_trigger = 0xFF;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0xC] == 0xFF && out[0xD] == 0xFF, "analog L/R at 0xFF passthrough exact");
    }

    // 6. Digital L/R detents with analog values below full scale, proving independence.
    {
        switch_pro_input_t in = neutral_input();
        in.left_trigger = 0x10;   // well below full scale
        in.gc_extra = GC_MASK_L_DETENT;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x3] == 0x10, "L detent set independently of a low analog value");
        CHECK(out[0xC] == 0x10, "analog L value untouched by the detent bit");

        in = neutral_input();
        in.right_trigger = 0x05;
        in.gc_extra = GC_MASK_R_DETENT;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x2] == 0x10, "R detent set independently of a low analog value");
        CHECK(out[0xD] == 0x05, "analog R value untouched by the detent bit");
    }

    // 7. Analog full scale without detent, proving no synthesis.
    {
        switch_pro_input_t in = neutral_input();
        in.left_trigger = 0xFF;
        in.right_trigger = 0xFF;
        // gc_extra left at 0 -- no detent bits.
        switch_gc_encode_report(&in, 0, out);
        CHECK((out[0x3] & 0x10) == 0, "full-scale analog L does NOT synthesize L detent");
        CHECK((out[0x2] & 0x10) == 0, "full-scale analog R does NOT synthesize R detent");
        CHECK(out[0xC] == 0xFF && out[0xD] == 0xFF, "analog values still pass through exactly");
    }

    // 8. Modkit Z plus one trigger simultaneously, proving no collision.
    {
        switch_pro_input_t in = neutral_input();
        in.gc_extra = GC_MASK_Z | GC_MASK_L_DETENT;
        in.left_trigger = 0xFF;
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x2] == 0x20, "Z + L-detent + full L trigger: byte0 shows only Z (0x20)");
        CHECK(out[0x3] == 0x10, "Z + L-detent + full L trigger: byte1 shows only L detent (0x10)");
        CHECK(out[0xC] == 0xFF, "Z + L-detent + full L trigger: analog L still 0xFF");
    }

    // 9. L3/R3 source inputs cannot set native output stick-click bits.
    {
        switch_pro_input_t in = neutral_input();
        in.buttons[1] = SWITCH_MASK_L3 | SWITCH_MASK_R3;
        switch_gc_encode_report(&in, 0, out);
        CHECK((out[0x2] & 0x80) == 0, "SWITCH_MASK_R3 source never sets byte0 0x80 (Right Stick click)");
        CHECK((out[0x3] & 0x80) == 0, "SWITCH_MASK_L3 source never sets byte1 0x80 (Left Stick click)");
    }

    // 10. Counter progression and reset behavior (pure-encoder level: the caller supplies the
    // counter explicitly, so "progression" here just proves the byte is written verbatim and
    // wraps at 8 bits like a real uint8_t counter would).
    {
        switch_pro_input_t in = neutral_input();
        switch_gc_encode_report(&in, 0, out);
        CHECK(out[0x0] == 0, "counter=0 written verbatim");
        switch_gc_encode_report(&in, 1, out);
        CHECK(out[0x0] == 1, "counter=1 written verbatim");
        switch_gc_encode_report(&in, 255, out);
        CHECK(out[0x0] == 255, "counter=255 written verbatim (wrap is the caller's job, uint8_t here)");
    }

    // --- Report 0x05 (the format PC/Steam hosts actually select, Confirmed 2026-07-13) ---

    // 11. Neutral report 0x05: 32-bit counter, buttons zero, sticks centered, GC-specific
    // analog-trigger tail at 0x3C/0x3D zero, reserved byte 0x3E zero.
    {
        switch_pro_input_t in = neutral_input();
        switch_gc_encode_report05(&in, 0x00000000u, out);
        CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0,
              "report05 neutral: 32-bit counter bytes all zero");
        CHECK(out[0x4] == 0 && out[0x5] == 0 && out[0x6] == 0,
              "report05 neutral: all button bytes zero");
        uint8_t expect_stick[3];
        switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, expect_stick);
        CHECK(memcmp(&out[0x0A], expect_stick, 3) == 0, "report05 neutral: left stick centered");
        CHECK(memcmp(&out[0x0D], expect_stick, 3) == 0, "report05 neutral: right stick centered");
        CHECK(out[0x3C] == 0 && out[0x3D] == 0, "report05 neutral: analog L/R tail zero");
        CHECK(out[0x3E] == 0, "report05 neutral: reserved byte zero");
    }

    // 12. Counter is 32-bit little-endian, unlike report 0x0A's 8-bit counter.
    {
        switch_pro_input_t in = neutral_input();
        switch_gc_encode_report05(&in, 0x01020304u, out);
        CHECK(out[0] == 0x04 && out[1] == 0x03 && out[2] == 0x02 && out[3] == 0x01,
              "report05: 32-bit counter written little-endian");
    }

    // 13. Buttons and sticks in report 0x05 -- native controls this format supports.
    {
        switch_pro_input_t in;

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_A;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x4] == 0x08, "report05: A -> byte0x4 0x08");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_B;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x4] == 0x04, "report05: B -> byte0x4 0x04");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_X;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x4] == 0x02, "report05: X -> byte0x4 0x02");

        in = neutral_input(); in.buttons[0] = SWITCH_MASK_Y;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x4] == 0x01, "report05: Y -> byte0x4 0x01");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_PLUS;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x5] == 0x02, "report05: Plus -> byte0x5 0x02");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_MINUS;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x5] == 0x01, "report05: Minus -> byte0x5 0x01");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_HOME;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x5] == 0x10, "report05: Home -> byte0x5 0x10");

        in = neutral_input(); in.buttons[1] = SWITCH_MASK_CAPTURE;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x5] == 0x20, "report05: Capture -> byte0x5 0x20");

        in = neutral_input(); in.extra = SWITCH_EXTRA_C;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x5] == 0x40, "report05: C -> byte0x5 0x40");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_ZL;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x6] == 0x80, "report05: ZL -> byte0x6 0x80");

        in = neutral_input(); in.gc_extra = GC_MASK_ZL;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x6] == 0x80, "report05: native ZL -> byte0x6 0x80");

        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_UP;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x6] == 0x02, "report05: D-pad Up -> byte0x6 0x02");
        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_DOWN;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x6] == 0x01, "report05: D-pad Down -> byte0x6 0x01");
        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_LEFT;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x6] == 0x08, "report05: D-pad Left -> byte0x6 0x08");
        in = neutral_input(); in.buttons[2] = SWITCH_MASK_DPAD_RIGHT;
        switch_gc_encode_report05(&in, 0, out); CHECK(out[0x6] == 0x04, "report05: D-pad Right -> byte0x6 0x04");

        in = neutral_input();
        in.left_stick[0] = 0xAB; in.left_stick[1] = 0xCD; in.left_stick[2] = 0xEF;
        switch_gc_encode_report05(&in, 0, out);
        CHECK(out[0x0A] == 0xAB && out[0x0B] == 0xCD && out[0x0C] == 0xEF,
              "report05: left stick passthrough exact at 0x0A-0x0C");
        in = neutral_input();
        in.right_stick[0] = 0x12; in.right_stick[1] = 0x34; in.right_stick[2] = 0x56;
        switch_gc_encode_report05(&in, 0, out);
        CHECK(out[0x0D] == 0x12 && out[0x0E] == 0x34 && out[0x0F] == 0x56,
              "report05: right stick passthrough exact at 0x0D-0x0F");
    }

    // 14. Analog L/R at offsets 0x3C/0x3D -- the GC-specific tail, continuous passthrough.
    {
        switch_pro_input_t in = neutral_input();
        in.left_trigger = 0x00; in.right_trigger = 0x00;
        switch_gc_encode_report05(&in, 0, out);
        CHECK(out[0x3C] == 0x00 && out[0x3D] == 0x00, "report05: analog L/R at 0x00 exact");

        in.left_trigger = 0x7F; in.right_trigger = 0x40;
        switch_gc_encode_report05(&in, 0, out);
        CHECK(out[0x3C] == 0x7F && out[0x3D] == 0x40, "report05: analog L/R at intermediate values exact");

        in.left_trigger = 0xFF; in.right_trigger = 0xFF;
        switch_gc_encode_report05(&in, 0, out);
        CHECK(out[0x3C] == 0xFF && out[0x3D] == 0xFF, "report05: analog L/R at 0xFF exact");
    }

    // 15. No accidental L3/R3 (or GL/GR, or ZR/plain-shoulder -- bits with no GC hardware
    // equivalent in this shared format) output, even when the source sets them.
    {
        switch_pro_input_t in = neutral_input();
        in.buttons[1] = SWITCH_MASK_L3 | SWITCH_MASK_R3;
        in.buttons[0] = SWITCH_MASK_ZR | SWITCH_MASK_R;
        in.buttons[2] = SWITCH_MASK_L;
        in.extra = SWITCH_EXTRA_GL | SWITCH_EXTRA_GR;
        switch_gc_encode_report05(&in, 0, out);
        CHECK((out[0x5] & 0x08) == 0, "report05: SWITCH_MASK_L3 never sets byte0x5 0x08");
        CHECK((out[0x5] & 0x04) == 0, "report05: SWITCH_MASK_R3 never sets byte0x5 0x04");
        CHECK((out[0x4] & 0x80) == 0, "report05: SWITCH_MASK_ZR never sets byte0x4 0x80 (no GC equivalent bit)");
        CHECK((out[0x4] & 0x40) == 0, "report05: SWITCH_MASK_R never sets byte0x4 0x40 (no GC equivalent bit)");
        CHECK((out[0x6] & 0x40) == 0, "report05: SWITCH_MASK_L never sets byte0x6 0x40 (no GC equivalent bit)");
        CHECK(out[0x7] == 0, "report05: GL/GR never touch byte0x7 -- GC has no grip hardware");
    }

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
