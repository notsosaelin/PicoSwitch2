#include "bootsel_action.h"

bootsel_action_t bootsel_action_resolve(bootsel_gesture_t gesture,
                                        bool in_config,
                                        bool controller_connected)
{
    switch (gesture) {
        case BOOTSEL_SINGLE_TAP:
            return !in_config && controller_connected
                ? BOOTSEL_ACTION_CYCLE_CONTROLLER
                : BOOTSEL_ACTION_NONE;
        case BOOTSEL_DOUBLE_TAP:
            // Pairing requires the normal Bluetooth/controller runtime.
            return in_config
                ? BOOTSEL_ACTION_NONE
                : BOOTSEL_ACTION_OPEN_PAIRING;
        case BOOTSEL_TRIPLE_TAP:
            // Bond clearing is useful even if no controller is currently
            // connected and remains an emergency escape while in Config.
            return BOOTSEL_ACTION_WIPE_DEVICES;
        case BOOTSEL_HOLD:
            // From a controller personality this enters Config directly; from
            // Config the USB owner returns directly to Pro2.
            return BOOTSEL_ACTION_TOGGLE_CONFIG;
        case BOOTSEL_NONE:
        default:
            return BOOTSEL_ACTION_NONE;
    }
}
