// 8BitDo Ultimate Bluetooth compatibility for the Switch Pro driver.
//
// The first-generation Ultimate Bluetooth controller impersonates a Nintendo
// Pro Controller over Bluetooth (057E:2009, product name "Pro Controller").
// Stock firmware omits its independent P1/P2 input bits from Switch reports.
// The PicoSwitch2 controller-firmware patch exposes them through the two
// reserved system-button bits. A tested stock-firmware profile fallback also
// maps P1/P2 to two held chords; both paths are restricted to the controller's
// IEEE-assigned 8BitDo OUI.

#ifndef SWITCH_PRO_8BITDO_H
#define SWITCH_PRO_8BITDO_H

#include <stdbool.h>
#include <stdint.h>

bool switch_pro_8bitdo_ultimate_match(const uint8_t bd_addr[6],
                                      const char *name,
                                      uint16_t vendor_id,
                                      uint16_t product_id);

uint8_t switch_pro_extract_reserved_paddles(
    const uint8_t *report, uint16_t report_len);

uint32_t switch_pro_translate_reserved_paddles(
    uint32_t buttons, uint8_t firmware_paddle_bits);

uint32_t switch_pro_8bitdo_ultimate_translate_paddles(
    uint32_t buttons, uint8_t firmware_paddle_bits);

#endif // SWITCH_PRO_8BITDO_H
