/*
 * Pure USB personality mode-cycle logic. See include/usb_mode_cycle.h.
 * Moved out of usb.c (2026-07-13) specifically so it has zero pico-sdk/TinyUSB
 * dependency and can be compiled + tested on the host -- see
 * tools/test_usb_mode_cycle.c.
 */
#include "usb_mode_cycle.h"

#ifdef NS2_PRO

bool usb_personality_available(usb_personality_t p) {
    switch (p) {
        case USB_PERSONALITY_SWITCH2_PRO2: return true;
        case USB_PERSONALITY_NSO_GAMECUBE: return true;
        case USB_PERSONALITY_JOYCON2:      return false;  // reserved, not implemented
        case USB_PERSONALITY_CDC_CONFIG:   return true;
        default:                           return false;
    }
}

usb_personality_t usb_next_personality(usb_personality_t current) {
    for (int next = (int)current + 1; next <= (int)USB_PERSONALITY_CDC_CONFIG; next++) {
        if (usb_personality_available((usb_personality_t)next))
            return (usb_personality_t)next;
    }
    return current;  // already terminal (Config) or nothing further available
}

#endif  // NS2_PRO
