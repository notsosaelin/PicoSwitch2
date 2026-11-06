#ifndef DS5_OUTPUT_H
#define DS5_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#define DS5_BT_OUTPUT_REPORT_LEN 78

typedef struct {
    bool initialize_compat;
    bool setup_lightbar;
    bool update_rumble;
    bool update_leds;
    uint8_t rumble_left;
    uint8_t rumble_right;
    uint8_t led_r;
    uint8_t led_g;
    uint8_t led_b;
    uint8_t player_leds;
} ds5_output_state_t;

// Build the complete Bluetooth report, beginning with report ID 0x31 and
// ending with its little-endian CRC32. The HID transaction byte (0xA2) is
// deliberately not part of this buffer; BTstack adds it on the interrupt CID.
void ds5_build_bt_output_report(uint8_t sequence,
                                const ds5_output_state_t *state,
                                uint8_t out[DS5_BT_OUTPUT_REPORT_LEN]);

#endif  // DS5_OUTPUT_H
