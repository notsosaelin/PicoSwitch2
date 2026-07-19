#ifndef CONTROLLER_HEADSET_H
#define CONTROLLER_HEADSET_H

#include <stdint.h>

// Normalized physical jack state. Keep the values byte-sized in the shared
// input structures; they cross from Bluetooth/core1 to USB/core0.
enum {
    CONTROLLER_HEADSET_NONE = 0,
    CONTROLLER_HEADSET_HEADPHONES = 1,
    CONTROLLER_HEADSET_HEADSET = 2,
};

// Switch 2 Pro Controller report 0x09 uses a nonzero alternating state while
// the jack is occupied. Headphones (no microphone) use 0x05/0x0D; a headset
// with microphone uses 0x07/0x0F.
static inline uint8_t
controller_headset_switch2_state(uint8_t headset_state,
                                 uint8_t report_counter) {
    uint8_t base;
    switch (headset_state) {
        case CONTROLLER_HEADSET_HEADPHONES:
            base = 0x05;
            break;
        case CONTROLLER_HEADSET_HEADSET:
            base = 0x07;
            break;
        case CONTROLLER_HEADSET_NONE:
        default:
            return 0;
    }
    return (uint8_t)(base | ((report_counter & 1u) ? 0x08u : 0u));
}

#endif
