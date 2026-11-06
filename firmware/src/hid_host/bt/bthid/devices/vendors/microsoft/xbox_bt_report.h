// Pure report decoder shared by the Classic Xbox driver and host regression tests.

#ifndef XBOX_BT_REPORT_H
#define XBOX_BT_REPORT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t buttons;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t lt;
    uint8_t rt;
} xbox_bt_decoded_report_t;

// Decode the 16-byte Classic XInput report (including report ID 0x01).
bool xbox_bt_decode_standard_report(const uint8_t *data, uint16_t len,
                                    xbox_bt_decoded_report_t *out);

#endif
