#ifndef BOOTSEL_ACTION_H
#define BOOTSEL_ACTION_H

#include <stdbool.h>

#include "bootsel_gesture.h"

// Pure policy layer kept separate from Pico sampling, Bluetooth, and TinyUSB
// ownership so the paired/unpaired/config gesture matrix is host-testable.
typedef enum {
    BOOTSEL_ACTION_NONE = 0,
    BOOTSEL_ACTION_CYCLE_CONTROLLER,
    BOOTSEL_ACTION_OPEN_PAIRING,
    BOOTSEL_ACTION_WIPE_DEVICES,
    BOOTSEL_ACTION_TOGGLE_CONFIG,
} bootsel_action_t;

bootsel_action_t bootsel_action_resolve(bootsel_gesture_t gesture,
                                        bool in_config,
                                        bool controller_connected);

#endif
