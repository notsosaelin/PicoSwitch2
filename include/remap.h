#ifndef _REMAP_H_
#define _REMAP_H_

// Button remapping model. Each physical input (SOURCE, as normalized by
// bluepad32's virtual gamepad) is mapped to a Switch Pro Controller output
// (DESTINATION). Note: bluepad32 only exposes this standard set — controller-
// specific extras (Elite paddles, DualSense Edge back buttons, Steam grips) are
// not surfaced, so they can't be remapped here.

// Physical inputs (remap sources). Order is the UI order; do not renumber
// without updating the web page and stored configs.
enum {
    SRC_SOUTH, SRC_EAST, SRC_WEST, SRC_NORTH,  // face buttons (physical positions)
    SRC_L, SRC_R, SRC_ZL, SRC_ZR,              // shoulders / triggers
    SRC_L3, SRC_R3,                            // stick clicks
    SRC_MINUS, SRC_PLUS, SRC_HOME, SRC_CAPTURE,  // misc
    SRC_DPAD_UP, SRC_DPAD_DOWN, SRC_DPAD_LEFT, SRC_DPAD_RIGHT,
    SRC_COUNT
};

// Controller platform families. Each has its own remap profile and its own
// physical-button labels in the config UI. Detected from the Bluetooth vendor id.
enum {
    FAMILY_GENERIC = 0,
    FAMILY_NINTENDO,
    FAMILY_PLAYSTATION,
    FAMILY_XBOX,
    FAMILY_COUNT
};

// Switch Pro Controller outputs (remap destinations). DST_NONE = unmapped.
enum {
    DST_NONE = 0,
    DST_A, DST_B, DST_X, DST_Y,
    DST_L, DST_R, DST_ZL, DST_ZR,
    DST_L3, DST_R3,
    DST_MINUS, DST_PLUS, DST_HOME, DST_CAPTURE,
    DST_DPAD_UP, DST_DPAD_DOWN, DST_DPAD_LEFT, DST_DPAD_RIGHT,
    DST_COUNT
};

#endif  // _REMAP_H_
