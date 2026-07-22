#ifndef SWITCH2_PRO2_AUDIO_TRANSPORT_H
#define SWITCH2_PRO2_AUDIO_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "controller_headset.h"

#define SW2_PRO2_AUDIO_INPUT_REPORT_LEN 0x70u
#define SW2_PRO2_AUDIO_DATA_LEN         0x32u
#define SW2_PRO2_AUDIO_COMPACT_LEN      63u
#define SW2_PRO2_AUDIO_FALLBACK_US       50000u

// Convert the Pro Controller 2 firmware-2.x headset report (GATT value 0x002E)
// into the ordinary 0x000E layout already consumed by the native input/motion
// path. The audio block at 0x0F..0x40 (length 0 when inactive or 0x32 while
// streaming) is omitted; motion length/data move from 0x41/0x42 back to
// 0x0E/0x0F. Buttons, sticks and headset state stay byte exact.
bool switch2_pro2_audio_compact_input(
    const uint8_t *src, uint16_t len,
    uint8_t dst[SW2_PRO2_AUDIO_COMPACT_LEN]);

// Decode the alternating physical-jack state carried at report offset 0x0D.
// 0x05/0x0D = headphones, 0x07/0x0F = headset+microphone, 0 = unplugged.
uint8_t switch2_pro2_audio_headset_state(uint8_t raw_state);

// 0x002E may coexist with the ordinary 0x000E input/motion notification. Feed
// compacted 0x002E input only after the ordinary source has actually gone
// quiet, avoiding duplicate controls and motion samples. Unsigned subtraction
// deliberately preserves correct behavior across time_us_32() wraparound.
bool switch2_pro2_audio_needs_input_fallback(uint32_t now_us,
                                             uint32_t ordinary_last_us);

#endif
