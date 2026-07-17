#ifndef BATTLERGC_PRO_REPORT_H
#define BATTLERGC_PRO_REPORT_H

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
    bool gc_native_zl;
    bool gc_native_z;
    bool gc_l_detent;
    bool gc_r_detent;
} battlergc_pro_decoded_report_t;

// Decode Bluetooth XInput report ID 0x01. The tested controller's HOME+B mode
// exposes its physical L/R trigger clicks through the sequential L3/R3 bits.
bool battlergc_pro_decode_report(const uint8_t *data, uint16_t len,
                                battlergc_pro_decoded_report_t *out);

// Home is not part of report 0x01 on the tested controller. It arrives as a
// separate two-byte boolean event: 02 01 while pressed, 02 00 when released.
bool battlergc_pro_decode_home_report(const uint8_t *data, uint16_t len,
                                     bool *pressed);

#endif
