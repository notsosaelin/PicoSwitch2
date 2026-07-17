#include "battlergc_pro_report.h"

#include <string.h>

#include "core/buttons.h"

#define BATTLER_A               0x0001
#define BATTLER_B               0x0002
#define BATTLER_X               0x0004
#define BATTLER_Y               0x0008
#define BATTLER_ZL_SHOULDER     0x0010
#define BATTLER_Z_SHOULDER      0x0020
#define BATTLER_SELECT          0x0040
#define BATTLER_START           0x0080
#define BATTLER_L_CLICK         0x0100
#define BATTLER_R_CLICK         0x0200

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t scale_trigger(uint16_t raw, bool clicked)
{
    if (clicked) return 255;

    // Hardware capture (2026-07-17): this controller reaches raw 1020 around
    // half of its physical trigger travel and then reports no further analog
    // information until the mechanical click. We cannot reconstruct that
    // missing second half. Scale the reported continuous range into 0..223
    // (one below the project's established 224 "near-full/detent" boundary),
    // then reserve 255 exclusively for the real click above. This preserves
    // all reported analog resolution without showing a premature full press.
    if (raw > 1020) raw = 1020;
    return (uint8_t)(((uint32_t)raw * 223u) / 1020u);
}

bool battlergc_pro_decode_report(const uint8_t *data, uint16_t len,
                                battlergc_pro_decoded_report_t *out)
{
    if (!data || !out || len < 16 || data[0] != 0x01) return false;
    memset(out, 0, sizeof(*out));

    out->lx = (uint8_t)(read_le16(&data[1]) >> 8);
    out->ly = (uint8_t)(read_le16(&data[3]) >> 8);
    out->rx = (uint8_t)(read_le16(&data[5]) >> 8);
    out->ry = (uint8_t)(read_le16(&data[7]) >> 8);

    uint16_t btn = read_le16(&data[14]);
    out->gc_l_detent = (btn & BATTLER_L_CLICK) != 0;
    out->gc_r_detent = (btn & BATTLER_R_CLICK) != 0;
    out->lt = scale_trigger(read_le16(&data[9]), out->gc_l_detent);
    out->rt = scale_trigger(read_le16(&data[11]), out->gc_r_detent);

    uint8_t hat = data[13];
    if (hat == 1 || hat == 2 || hat == 8) out->buttons |= JP_BUTTON_DU;
    if (hat >= 2 && hat <= 4)             out->buttons |= JP_BUTTON_DR;
    if (hat >= 4 && hat <= 6)             out->buttons |= JP_BUTTON_DD;
    if (hat >= 6 && hat <= 8)             out->buttons |= JP_BUTTON_DL;

    // Preserve the printed GameCube labels, not Xbox cluster positions.
    if (btn & BATTLER_A) out->buttons |= JP_BUTTON_B2;
    if (btn & BATTLER_B) out->buttons |= JP_BUTTON_B1;
    if (btn & BATTLER_X) out->buttons |= JP_BUTTON_B4;
    if (btn & BATTLER_Y) out->buttons |= JP_BUTTON_B3;

    // Pro2 sees separate L/R shoulders. Their native GC meanings travel in
    // independent fields and are consumed only by the GC output personality.
    if (btn & BATTLER_ZL_SHOULDER) out->buttons |= JP_BUTTON_L1;
    if (btn & BATTLER_Z_SHOULDER)  out->buttons |= JP_BUTTON_R1;
    out->gc_native_zl = (btn & BATTLER_ZL_SHOULDER) != 0;
    out->gc_native_z = (btn & BATTLER_Z_SHOULDER) != 0;

    if (btn & BATTLER_SELECT)  out->buttons |= JP_BUTTON_S1;
    if (btn & BATTLER_START)   out->buttons |= JP_BUTTON_S2;
    if (btn & BATTLER_L_CLICK) out->buttons |= JP_BUTTON_L3;
    if (btn & BATTLER_R_CLICK) out->buttons |= JP_BUTTON_R3;

    // Do not synthesize L2/R2 from the analog values here. The shared seam
    // performs that fold in Pro2 mode and deliberately omits it in GC mode.
    // The controller transports its click switches in XInput L3/R3 slots:
    // Pro2 keeps those normal destinations, while the GameCube personality
    // discards them because their independent real trigger-detent meanings also
    // travel in gc_l/r_detent.
    return true;
}

bool battlergc_pro_decode_home_report(const uint8_t *data, uint16_t len,
                                     bool *pressed)
{
    if (!data || !pressed || len < 2 || data[0] != 0x02) return false;
    *pressed = (data[1] & 0x01u) != 0;
    return true;
}
