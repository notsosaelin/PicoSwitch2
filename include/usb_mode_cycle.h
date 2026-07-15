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

// Is this personality actually implemented/enumerable right now? All four
// (Pro2, GameCube, Joy-Con2 Left, Joy-Con2 Right) plus CDC config are
// available -- Joy-Con2 L/R are experimental/test personalities, not the
// recommended full-controller mode (that's Pro Controller 2), but they are
// real, individually-selectable, always-available cycle stops, not reserved
// placeholders.
bool usb_personality_available(usb_personality_t p);

// Walk forward from `current`, skipping anything not yet available, and return the next real
// personality to switch to. Wraps from the last available personality (CDC_CONFIG) back to the
// first (SWITCH2_PRO2) rather than staying put -- a live BOOTSEL-hold exit from Config back to
// Pro2 (2026-07-14 addition; Config was originally terminal-for-the-session by deliberate initial
// scope limit, not a technical constraint -- see docs/switch2-gc/usb-personality.md "Runtime mode
// cycle"). Only returns `current` unchanged in the truly-degenerate case where NO personality is
// available (impossible in practice, since Pro2 is always available).
usb_personality_t usb_next_personality(usb_personality_t current);

#endif  // NS2_PRO

#endif  // USB_MODE_CYCLE_H
