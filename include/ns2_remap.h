#ifndef _NS2_REMAP_H_
#define _NS2_REMAP_H_

#include <stdbool.h>
#include <stdint.h>

// Locked physical-controller -> Switch 2 semantic mapping.
//
// Console-side remapping belongs to the emulated Nintendo controller identity, so
// it persists when the user changes which physical controller is paired to the
// dongle. PicoSwitch2 therefore exposes one stable base map rather than storing
// per-controller-family overrides.

#define NS2_SRC_COUNT 25    // source buttons (see SRC_TO_JP[] in ns2_seam.c)

// Remap destinations = Switch 2 Pro Controller outputs. 0 = unmapped (button does nothing).
enum {
    NS2_DST_NONE = 0,
    NS2_DST_B, NS2_DST_A, NS2_DST_Y, NS2_DST_X,
    NS2_DST_L, NS2_DST_R, NS2_DST_ZL, NS2_DST_ZR,
    NS2_DST_L3, NS2_DST_R3,
    NS2_DST_MINUS, NS2_DST_PLUS, NS2_DST_HOME, NS2_DST_CAPTURE,
    NS2_DST_DUP, NS2_DST_DDOWN, NS2_DST_DLEFT, NS2_DST_DRIGHT,
    NS2_DST_GL, NS2_DST_GR, NS2_DST_C,
    // Digital analog-stick directions. A physical controller never needs these
    // (its sticks are already analog), so NS2_BASE_BUTTON_MAP never uses them
    // and ns2_seam.c's ns2_apply_dst() deliberately ignores them. They exist so
    // a digital source -- currently the Bluetooth keyboard profiles in
    // ns2_kbm.c -- can express stick deflection in the same normalized
    // destination vocabulary as every other control, instead of inventing a
    // second one. Opposing pairs are resolved by the consumer; see
    // docs/bluetooth/keyboard-mouse-input.md.
    NS2_DST_LSTICK_UP, NS2_DST_LSTICK_DOWN, NS2_DST_LSTICK_LEFT, NS2_DST_LSTICK_RIGHT,
    NS2_DST_RSTICK_UP, NS2_DST_RSTICK_DOWN, NS2_DST_RSTICK_LEFT, NS2_DST_RSTICK_RIGHT,
    NS2_DST_COUNT
};

extern const uint8_t NS2_BASE_BUTTON_MAP[NS2_SRC_COUNT];

// Resolve one unified JP_BUTTON_* source slot to its canonical destination.
//
// Direct controllers keep the locked physical-controller map above. The Android
// bridge is different by declared contract: its first four HID usages have
// already been normalized to logical A/B/X/Y by the app. Applying the physical
// B/A/Y/X position map to those four would swap them a second time. All other
// bridge usages retain the ordinary base map.
uint8_t ns2_resolve_button_destination(uint8_t source_index,
                                       bool from_android_bridge);

#endif  // _NS2_REMAP_H_
