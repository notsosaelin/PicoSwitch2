#include "controller_battery.h"

#include <stddef.h>

static uint8_t clamp_percent(uint16_t level)
{
    return level > 100 ? 100 : (uint8_t)level;
}

static bool store(controller_battery_t *out, uint16_t level, bool charging)
{
    if (!out) {
        return false;
    }
    out->level = clamp_percent(level);
    out->charging = charging;
    return true;
}

bool controller_battery_decode_ds3(uint8_t raw, controller_battery_t *out)
{
    static const uint8_t discharge[] = {0, 1, 25, 50, 75, 100};

    if (raw <= 5) {
        return store(out, discharge[raw], false);
    }
    if (raw == 0xEE) {
        return store(out, 100, true);
    }
    if (raw == 0xEF) {
        return store(out, 100, false);
    }
    return false;
}

bool controller_battery_decode_ds4(uint8_t raw, controller_battery_t *out)
{
    uint8_t level = raw & 0x0F;
    bool cable_connected = (raw & 0x10) != 0;

    if (!cable_connected) {
        return store(out, level < 10 ? level * 10 + 5 : 100, false);
    }
    if (level < 10) {
        return store(out, level * 10 + 5, true);
    }
    if (level == 10) {
        return store(out, 100, true);
    }
    if (level == 11) {
        return store(out, 100, false);
    }
    return false;
}

bool controller_battery_decode_ds5(uint8_t raw, controller_battery_t *out)
{
    uint8_t level = raw & 0x0F;
    uint8_t status = raw >> 4;

    switch (status) {
        case 0x0:
            return store(out, level > 10 ? 100 : level * 10 + 5, false);
        case 0x1:
            return store(out, level > 10 ? 100 : level * 10 + 5, true);
        case 0x2:
            return store(out, 100, false);
        default:
            return false;
    }
}

bool controller_battery_decode_switch_pro(uint8_t battery_conn,
                                          controller_battery_t *out)
{
    uint8_t raw = battery_conn >> 4;
    uint16_t level = raw > 8 ? 100 : raw * 12 + 5;
    return store(out, level, (battery_conn & 0x08) != 0);
}

bool controller_battery_decode_wii_u_pro(uint8_t status,
                                         controller_battery_t *out)
{
    uint8_t raw = (status >> 4) & 0x07;
    uint16_t level = raw >= 4 ? 100 : raw * 25;
    // The Wii U Pro charging flag is active-low.
    return store(out, level, (status & 0x04) == 0);
}

bool controller_battery_decode_wiimote(uint8_t raw,
                                       controller_battery_t *out)
{
    // Linux's upstream hid-wiimote driver exposes the complete status byte as
    // capacity = raw * 100 / 255.
    uint16_t level = ((uint16_t)raw * 100u) / 255u;
    return store(out, level, false);
}

uint8_t controller_battery_switch1_connection_info(bool valid,
                                                    uint8_t level)
{
    if (!valid) {
        return 0x81;  // Previous default: full battery + wired USB.
    }

    uint8_t clamped = clamp_percent(level);
    uint8_t quarter_steps = (uint8_t)(((uint16_t)clamped * 4u + 50u) / 100u);
    uint8_t switch_level = (uint8_t)(quarter_steps * 2u);  // 0,2,4,6,8
    return (uint8_t)((switch_level << 4) | 0x01u);
}

uint8_t controller_battery_switch2_power_info(bool valid, uint8_t level,
                                              bool charging)
{
    if (!valid) {
        return 0x25;  // Previous default: external power, not charging, 9/9.
    }

    uint8_t clamped = clamp_percent(level);
    uint8_t switch_level =
        (uint8_t)(((uint16_t)clamped * 9u + 50u) / 100u);
    return (uint8_t)(0x01u | (charging ? 0x02u : 0u) |
                     (switch_level << 2));
}
