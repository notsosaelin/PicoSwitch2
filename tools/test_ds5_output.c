#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ds5_output.h"

static uint32_t report_crc(const uint8_t report[DS5_BT_OUTPUT_REPORT_LEN]) {
    return (uint32_t)report[74] |
           ((uint32_t)report[75] << 8) |
           ((uint32_t)report[76] << 16) |
           ((uint32_t)report[77] << 24);
}

int main(void) {
    uint8_t report[DS5_BT_OUTPUT_REPORT_LEN];
    ds5_output_state_t state;

    memset(&state, 0, sizeof(state));
    state.initialize_compat = true;
    state.setup_lightbar = true;
    state.update_leds = true;
    state.player_leds = 0x04;
    state.led_r = 1;
    state.led_g = 2;
    state.led_b = 3;
    ds5_build_bt_output_report(0, &state, report);
    assert(report[0] == 0x31 && report[1] == 0x00 && report[2] == 0x10);
    assert(report[3] == 0xF7 && report[4] == 0xF7);
    assert(report[41] == 0x02 && report[44] == 0x02);
    assert(report[45] == 1 && report[46] == 0x04);
    assert(report[47] == 1 && report[48] == 2 && report[49] == 3);
    // Independently generated with zlib.crc32(A2 || report[0:74]).
    assert(report_crc(report) == 0xD9076D7Cu);

    memset(&state, 0, sizeof(state));
    state.update_rumble = true;
    state.rumble_left = 0x34;
    state.rumble_right = 0x12;
    state.player_leds = 0x04;
    state.led_r = 1;
    state.led_g = 2;
    state.led_b = 3;
    ds5_build_bt_output_report(5, &state, report);
    assert(report[1] == 0x50);
    assert(report[3] == 0x03 && report[4] == 0xF7 && report[41] == 0x00);
    assert(report[5] == 0x12 && report[6] == 0x34);
    assert(report[45] == 1 && report[46] == 0x04);
    assert(report[47] == 1 && report[48] == 2 && report[49] == 3);
    assert(report_crc(report) == 0x76C2B8D0u);

    // STOP must still carry the vibration selectors; otherwise zero magnitudes
    // are ignored and the previous effect can remain active.
    state.rumble_left = 0;
    state.rumble_right = 0;
    ds5_build_bt_output_report(7, &state, report);
    assert(report[3] == 0x03 && report[4] == 0xF7 && report[41] == 0x00);
    assert(report[5] == 0 && report[6] == 0);
    assert(report_crc(report) == 0x79BE9156u);

    memset(&state, 0, sizeof(state));
    state.update_leds = true;
    state.player_leds = 0x04;
    state.led_r = 1;
    state.led_g = 2;
    state.led_b = 3;
    ds5_build_bt_output_report(15, &state, report);
    assert(report[4] == 0xF7);
    assert(report[41] == 0 && report[44] == 0);  // no repeated setup
    assert(report[45] == 1 && report[46] == 0x04);
    assert(report[47] == 1 && report[48] == 2 && report[49] == 3);

    puts("ds5_output: all tests passed");
    return 0;
}
