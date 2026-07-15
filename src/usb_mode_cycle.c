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
        case USB_PERSONALITY_JOYCON2_L:    return true;  // experimental/test -- see
        case USB_PERSONALITY_JOYCON2_R:    return true;  // docs/switch2-joycon2/protocol.md
        case USB_PERSONALITY_CDC_CONFIG:   return true;
        default:                           return false;
    }
}

usb_personality_t usb_next_personality(usb_personality_t current) {
    // Walk forward from current+1 to the end of the enum first...
    for (int next = (int)current + 1; next <= (int)USB_PERSONALITY_CDC_CONFIG; next++) {
        if (usb_personality_available((usb_personality_t)next))
            return (usb_personality_t)next;
    }
    // ...then wrap back to the start (Pro2). Covers Config's own live exit back to Pro2
    // (2026-07-14) the same way it covers every other step -- no special-casing CDC_CONFIG.
    for (int next = 0; next <= (int)current; next++) {
        if (usb_personality_available((usb_personality_t)next))
            return (usb_personality_t)next;
    }
    return current;  // degenerate: no personality available at all (never happens in practice)
}

#endif  // NS2_PRO
