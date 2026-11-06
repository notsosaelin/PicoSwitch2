#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/vendors/retrofighters/battlergc_pro_report.h"
#include "core/buttons.h"

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void set_buttons(uint8_t report[16], uint16_t buttons)
{
    put_le16(&report[14], buttons);
}

int main(void)
{
    const uint8_t neutral[16] = {
        0x01, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF,
        0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t report[16];
    battlergc_pro_decoded_report_t out;

    memcpy(report, neutral, sizeof(report));
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert(out.lx == 128 && out.ly == 127);
    assert(out.rx == 128 && out.ry == 127);
    assert(out.buttons == 0 && out.lt == 0 && out.rt == 0);

    // Physical GameCube labels remain direct.
    set_buttons(report, 0x0001 | 0x0002 | 0x0004 | 0x0008);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert((out.buttons & (JP_BUTTON_B1 | JP_BUTTON_B2 |
                           JP_BUTTON_B3 | JP_BUTTON_B4)) ==
           (JP_BUTTON_B1 | JP_BUTTON_B2 | JP_BUTTON_B3 | JP_BUTTON_B4));

    // Left and right shoulders keep distinct native ZL and Z meanings.
    set_buttons(report, 0x0010);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert(out.buttons & JP_BUTTON_L1);
    assert(out.gc_native_zl && !out.gc_native_z);
    set_buttons(report, 0x0020);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert(out.buttons & JP_BUTTON_R1);
    assert(!out.gc_native_zl && out.gc_native_z);

    // The captured raw range is compressed below the established 224
    // near-full boundary until the real click.
    set_buttons(report, 0);
    put_le16(&report[9], 1020);
    put_le16(&report[11], 1023);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert(out.lt == 223 && out.rt == 223);
    assert(!out.gc_l_detent && !out.gc_r_detent);
    assert((out.buttons & (JP_BUTTON_L2 | JP_BUTTON_R2)) == 0);

    set_buttons(report, 0x0100);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert(out.lt == 255 && out.rt == 223);
    assert(out.gc_l_detent && !out.gc_r_detent);
    assert((out.buttons & JP_BUTTON_L3) && !(out.buttons & JP_BUTTON_R3));

    set_buttons(report, 0x0200);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert(out.lt == 223 && out.rt == 255);
    assert(!out.gc_l_detent && out.gc_r_detent);
    assert(!(out.buttons & JP_BUTTON_L3) && (out.buttons & JP_BUTTON_R3));

    // The report-0x01 center cluster contains Select and Start. Home is a
    // separate report-ID-0x02 boolean event on the tested controller.
    set_buttons(report, 0x0040 | 0x0080);
    assert(battlergc_pro_decode_report(report, sizeof(report), &out));
    assert((out.buttons & (JP_BUTTON_S1 | JP_BUTTON_S2)) ==
           (JP_BUTTON_S1 | JP_BUTTON_S2));
    assert(!(out.buttons & JP_BUTTON_A1));

    bool home_pressed = false;
    const uint8_t home_down[2] = {0x02, 0x01};
    const uint8_t home_up[2] = {0x02, 0x00};
    assert(battlergc_pro_decode_home_report(home_down, sizeof(home_down),
                                           &home_pressed));
    assert(home_pressed);
    assert(battlergc_pro_decode_home_report(home_up, sizeof(home_up),
                                           &home_pressed));
    assert(!home_pressed);
    assert(!battlergc_pro_decode_home_report(report, sizeof(report),
                                            &home_pressed));

    puts("battlergc_pro_report: all tests passed");
    return 0;
}
