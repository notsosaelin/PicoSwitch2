#include "ds5_output.h"

#include <stddef.h>
#include <string.h>

#define DS5_REPORT_ID 0x31
#define DS5_BT_TAG    0x10

#define DS5_FLAG0_COMPATIBLE_VIBRATION 0x01
#define DS5_FLAG0_HAPTICS_SELECT       0x02
#define DS5_FLAG1_LIGHTBAR_CONTROL     0x04
#define DS5_FLAG1_PLAYER_INDICATOR     0x10
#define DS5_FLAG2_LIGHTBAR_SETUP       0x02
#define DS5_LIGHTBAR_SETUP_LIGHT_OUT   0x02
#define DS5_DAIDR_VALID_BASELINE       0xF7

// Report offsets from Linux's packed dualsense_output_report_bt/common structs.
#define DS5_OFS_VALID_FLAG0    3
#define DS5_OFS_VALID_FLAG1    4
#define DS5_OFS_MOTOR_RIGHT    5
#define DS5_OFS_MOTOR_LEFT     6
#define DS5_OFS_VALID_FLAG2   41
#define DS5_OFS_LIGHTBAR_SETUP 44
#define DS5_OFS_LED_BRIGHTNESS 45
#define DS5_OFS_PLAYER_LEDS    46
#define DS5_OFS_LIGHTBAR_R     47
#define DS5_OFS_LIGHTBAR_G     48
#define DS5_OFS_LIGHTBAR_B     49
#define DS5_OFS_CRC            74

static uint32_t ds5_crc32_raw(uint32_t seed, const uint8_t *data, size_t len) {
    uint32_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc;
}

static uint32_t ds5_bt_output_crc(const uint8_t *report, size_t len) {
    const uint8_t seed = 0xA2;
    uint32_t crc = ds5_crc32_raw(0xFFFFFFFFu, &seed, 1);
    return ~ds5_crc32_raw(crc, report, len);
}

void ds5_build_bt_output_report(uint8_t sequence,
                                const ds5_output_state_t *state,
                                uint8_t out[DS5_BT_OUTPUT_REPORT_LEN]) {
    memset(out, 0, DS5_BT_OUTPUT_REPORT_LEN);

    out[0] = DS5_REPORT_ID;
    out[1] = (uint8_t)((sequence & 0x0Fu) << 4);
    out[2] = DS5_BT_TAG;

    // daidr/dualsense-tester's hardware-working output struct initializes
    // validFlag1 to 0xF7 and preserves it on subsequent rumble reports. This
    // baseline is part of the observed working controller configuration, not
    // merely an optimization around individual LED updates.
    out[DS5_OFS_VALID_FLAG1] = DS5_DAIDR_VALID_BASELINE;

    if (state->initialize_compat) {
        // The tester's first output write sets bits 0,1,2,4,5,6,7 in flag0,
        // producing 0xF7 alongside the persistent flag1=0xF7 baseline. Our
        // previous LIGHT_OUT-only setup left tested controllers dark and with
        // non-functional compatibility rumble.
        out[DS5_OFS_VALID_FLAG0] = DS5_DAIDR_VALID_BASELINE;
    }

    if (state->setup_lightbar) {
        // DualSense ignores later RGB programming until the host explicitly
        // releases/configures its lightbar. Linux hid-playstation performs this
        // one-time LIGHT_OUT transaction before ordinary color updates.
        out[DS5_OFS_VALID_FLAG2] |= DS5_FLAG2_LIGHTBAR_SETUP;
        out[DS5_OFS_LIGHTBAR_SETUP] = DS5_LIGHTBAR_SETUP_LIGHT_OUT;
    }

    if (state->update_rumble) {
        // Exact flag pair used by daidr's working standard/Edge rumble sliders.
        out[DS5_OFS_VALID_FLAG0] |=
            DS5_FLAG0_COMPATIBLE_VIBRATION | DS5_FLAG0_HAPTICS_SELECT;
        // These flags remain set for STOP: selecting both motors and writing
        // zero is what actually turns a previous effect off.
        out[DS5_OFS_MOTOR_RIGHT] = state->rumble_right;
        out[DS5_OFS_MOTOR_LEFT] = state->rumble_left;
    }

    // The working tester retains one output struct between writes. Because the
    // 0xF7 validity baseline says these fields are valid on every report, copy
    // the cached values even for a rumble-only update instead of accidentally
    // applying zeroed LEDs/config alongside the motor command.
    out[DS5_OFS_LED_BRIGHTNESS] = 0x01;
    out[DS5_OFS_PLAYER_LEDS] = state->player_leds;
    out[DS5_OFS_LIGHTBAR_R] = state->led_r;
    out[DS5_OFS_LIGHTBAR_G] = state->led_g;
    out[DS5_OFS_LIGHTBAR_B] = state->led_b;

    uint32_t crc = ds5_bt_output_crc(out, DS5_OFS_CRC);
    out[DS5_OFS_CRC + 0] = (uint8_t)(crc >> 0);
    out[DS5_OFS_CRC + 1] = (uint8_t)(crc >> 8);
    out[DS5_OFS_CRC + 2] = (uint8_t)(crc >> 16);
    out[DS5_OFS_CRC + 3] = (uint8_t)(crc >> 24);
}
