#ifndef _NS2_REMAP_H_
#define _NS2_REMAP_H_

#include <stdint.h>

// Per-device button remapping. Covers the Switch 2 extended outputs (GL/GR/C) and the
// extended sources (DualSense Edge Fn buttons, Xbox Elite lower paddles), so any physical
// button on a supported pad can be reassigned per controller family.
//
// A map is uint8_t[NS2_SRC_COUNT] of NS2_DST_* values, stored per family. The source
// index order is defined by SRC_TO_JP[] in ns2_seam.c and mirrored in the web UI;
// do not renumber without updating both.

#define NS2_FAM_COUNT 4     // 0 = Sony, 1 = Xbox, 2 = Nintendo, 3 = Generic
#define NS2_SRC_COUNT 25    // remappable source buttons (see SRC_TO_JP[])

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

#endif  // _NS2_REMAP_H_
