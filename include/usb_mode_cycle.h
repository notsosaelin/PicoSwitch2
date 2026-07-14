/*
 * Pure USB personality mode-cycle logic (which personality is "next", which
 * are actually implemented). Deliberately has NO pico-sdk/TinyUSB dependency
 * -- only usb.h's usb_personality_t enum (itself dependency-free) -- so this
 * one piece of the mode-cycle feature can be compiled and tested on the host,
 * per NSO-GC.md's explicit validation requirement for "mode-cycle
 * next-available logic" / "Joy-Con 2 placeholder is skipped while
 * unavailable". See tools/test_usb_mode_cycle.c for the host-side test.
 */
#ifndef USB_MODE_CYCLE_H
#define USB_MODE_CYCLE_H

#include "usb.h"  // usb_personality_t

#ifdef NS2_PRO

// Is this personality actually implemented/enumerable right now? Only
// USB_PERSONALITY_JOYCON2 is reserved-but-unavailable today.
bool usb_personality_available(usb_personality_t p);

// Walk forward from `current`, skipping anything not yet available, and
// return the next real personality to switch to. Returns `current` unchanged
// if already at the last available personality (e.g. CDC_CONFIG, which is
// terminal for this pass).
usb_personality_t usb_next_personality(usb_personality_t current);

#endif  // NS2_PRO

#endif  // USB_MODE_CYCLE_H
