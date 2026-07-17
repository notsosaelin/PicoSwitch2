#include "xbox_bt_report.h"

#include <string.h>

#include "core/buttons.h"

// Classic XInput uses consecutive button usages. This is deliberately different
// from Xbox BLE's sparse usage pattern (buttons 1,2,4,5,7,8,11..15).
#define XBOX_CLASSIC_A               0x0001
#define XBOX_CLASSIC_B               0x0002
#define XBOX_CLASSIC_X               0x0004
#define XBOX_CLASSIC_Y               0x0008
#define XBOX_CLASSIC_LEFT_SHOULDER   0x0010
#define XBOX_CLASSIC_RIGHT_SHOULDER  0x0020
#define XBOX_CLASSIC_BACK            0x0040
#define XBOX_CLASSIC_START           0x0080
#define XBOX_CLASSIC_LEFT_THUMB      0x0100
#define XBOX_CLASSIC_RIGHT_THUMB     0x0200
#define XBOX_CLASSIC_GUIDE           0x0400

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t scale_trigger(uint16_t value)
{
    return value >= 1023 ? 255 : (uint8_t)(value >> 2);
}

bool xbox_bt_decode_standard_report(const uint8_t *data, uint16_t len,
                                    xbox_bt_decoded_report_t *out)
{
    if (!data || !out || len < 16 || data[0] != 0x01) return false;

    memset(out, 0, sizeof(*out));

    // The axes are unsigned 0..65535, just like Xbox BLE: 0x8000 is center,
    // not INT16_MIN. The old Classic parser treated these as signed and turned
    // the BattlerGC Pro's neutral sticks into full deflection.
    out->lx = (uint8_t)(read_le16(&data[1]) >> 8);
    out->ly = (uint8_t)(read_le16(&data[3]) >> 8);
    out->rx = (uint8_t)(read_le16(&data[5]) >> 8);
    out->ry = (uint8_t)(read_le16(&data[7]) >> 8);
    out->lt = scale_trigger(read_le16(&data[9]));
    out->rt = scale_trigger(read_le16(&data[11]));

    uint8_t hat = data[13];
    if (hat == 1 || hat == 2 || hat == 8) out->buttons |= JP_BUTTON_DU;
    if (hat >= 2 && hat <= 4)             out->buttons |= JP_BUTTON_DR;
    if (hat >= 4 && hat <= 6)             out->buttons |= JP_BUTTON_DD;
    if (hat >= 6 && hat <= 8)             out->buttons |= JP_BUTTON_DL;

    uint16_t btn = read_le16(&data[14]);

    if (btn & XBOX_CLASSIC_A) out->buttons |= JP_BUTTON_B1;
    if (btn & XBOX_CLASSIC_B) out->buttons |= JP_BUTTON_B2;
    if (btn & XBOX_CLASSIC_X) out->buttons |= JP_BUTTON_B3;
    if (btn & XBOX_CLASSIC_Y) out->buttons |= JP_BUTTON_B4;

    if (btn & XBOX_CLASSIC_LEFT_SHOULDER)  out->buttons |= JP_BUTTON_L1;
    if (btn & XBOX_CLASSIC_RIGHT_SHOULDER) out->buttons |= JP_BUTTON_R1;
    if (out->lt > 10)                      out->buttons |= JP_BUTTON_L2;
    if (out->rt > 10)                      out->buttons |= JP_BUTTON_R2;
    if (btn & XBOX_CLASSIC_BACK)           out->buttons |= JP_BUTTON_S1;
    if (btn & XBOX_CLASSIC_START)          out->buttons |= JP_BUTTON_S2;
    if (btn & XBOX_CLASSIC_LEFT_THUMB)     out->buttons |= JP_BUTTON_L3;
    if (btn & XBOX_CLASSIC_RIGHT_THUMB)    out->buttons |= JP_BUTTON_R3;
    if (btn & XBOX_CLASSIC_GUIDE)          out->buttons |= JP_BUTTON_A1;

    return true;
}
