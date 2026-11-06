#include "switch2_pro2_audio_transport.h"

#include <string.h>

bool switch2_pro2_audio_compact_input(
    const uint8_t *src, uint16_t len,
    uint8_t dst[SW2_PRO2_AUDIO_COMPACT_LEN])
{
    if (!src || !dst || len < SW2_PRO2_AUDIO_INPUT_REPORT_LEN) return false;
    // The 50-byte mic/audio field is present only while the controller is
    // actually streaming it. A live unplugged Pro2 reports zero here while
    // retaining the same fixed 112-byte envelope and motion offsets.
    if (src[0x0E] != 0 && src[0x0E] != SW2_PRO2_AUDIO_DATA_LEN) return false;

    const uint8_t motion_len = src[0x41];
    if (motion_len != 0 && motion_len != 0x1E && motion_len != 0x28) return false;
    if ((uint16_t)(0x42u + motion_len) > len) return false;

    memset(dst, 0, SW2_PRO2_AUDIO_COMPACT_LEN);
    memcpy(dst, src, 0x0Eu);             // counter through headset state
    dst[0x0E] = motion_len;
    if (motion_len) memcpy(&dst[0x0F], &src[0x42], motion_len);
    return true;
}

uint8_t switch2_pro2_audio_headset_state(uint8_t raw_state)
{
    switch (raw_state & 0x07u) {
        case 0x05:
            return CONTROLLER_HEADSET_HEADPHONES;
        case 0x07:
            return CONTROLLER_HEADSET_HEADSET;
        default:
            return CONTROLLER_HEADSET_NONE;
    }
}

bool switch2_pro2_audio_needs_input_fallback(uint32_t now_us,
                                             uint32_t ordinary_last_us)
{
    return ordinary_last_us == 0 ||
           (uint32_t)(now_us - ordinary_last_us) >= SW2_PRO2_AUDIO_FALLBACK_US;
}
