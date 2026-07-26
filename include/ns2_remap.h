#ifndef _NS2_REMAP_H_
#define _NS2_REMAP_H_

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
    NS2_DST_COUNT
};

extern const uint8_t NS2_BASE_BUTTON_MAP[NS2_SRC_COUNT];

#endif  // _NS2_REMAP_H_
