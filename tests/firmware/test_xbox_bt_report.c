#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "bt/bthid/devices/vendors/microsoft/xbox_bt_report.h"
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
    uint8_t report[16] = {
        0x01, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x80, 0xFF,
        0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    xbox_bt_decoded_report_t out;

    assert(xbox_bt_decode_standard_report(report, sizeof(report), &out));
    assert(out.lx == 128 && out.ly == 127);
    assert(out.rx == 128 && out.ry == 127);
    assert(out.lt == 0 && out.rt == 0 && out.buttons == 0);

    set_buttons(report, 0x0001 | 0x0004);
    assert(xbox_bt_decode_standard_report(report, sizeof(report), &out));
    assert((out.buttons & (JP_BUTTON_B1 | JP_BUTTON_B3)) ==
           (JP_BUTTON_B1 | JP_BUTTON_B3));

    set_buttons(report, 0x0010 | 0x0020 | 0x0040 | 0x0080 |
                        0x0100 | 0x0200 | 0x0400);
    assert(xbox_bt_decode_standard_report(report, sizeof(report), &out));
    assert((out.buttons & (JP_BUTTON_L1 | JP_BUTTON_R1 |
                           JP_BUTTON_S1 | JP_BUTTON_S2 |
                           JP_BUTTON_L3 | JP_BUTTON_R3 | JP_BUTTON_A1)) ==
           (JP_BUTTON_L1 | JP_BUTTON_R1 |
            JP_BUTTON_S1 | JP_BUTTON_S2 |
            JP_BUTTON_L3 | JP_BUTTON_R3 | JP_BUTTON_A1));

    set_buttons(report, 0);
    put_le16(&report[9], 512);
    put_le16(&report[11], 1023);
    assert(xbox_bt_decode_standard_report(report, sizeof(report), &out));
    assert(out.lt == 128 && out.rt == 255);
    assert((out.buttons & (JP_BUTTON_L2 | JP_BUTTON_R2)) ==
           (JP_BUTTON_L2 | JP_BUTTON_R2));

    puts("xbox_bt_report: all tests passed");
    return 0;
}
