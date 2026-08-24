#include "ns2_remap.h"

// Source order:
// B1 B2 B3 B4 L1 R1 L2 R2 S1 S2 L3 R3 DU DD DL DR
// A1 A2 A3 A4 L4 R4 A5 L5 R5.
//
// Driver-specific parsing still exposes hardware extras (Edge Fn/paddles,
// Elite paddles, C/GL/GR). This table only locks their canonical Nintendo
// semantic destination so console-side remapping remains authoritative.
const uint8_t NS2_BASE_BUTTON_MAP[NS2_SRC_COUNT] = {
    NS2_DST_B, NS2_DST_A, NS2_DST_Y, NS2_DST_X,
    NS2_DST_L, NS2_DST_R, NS2_DST_ZL, NS2_DST_ZR,
    NS2_DST_MINUS, NS2_DST_PLUS, NS2_DST_L3, NS2_DST_R3,
    NS2_DST_DUP, NS2_DST_DDOWN, NS2_DST_DLEFT, NS2_DST_DRIGHT,
    NS2_DST_HOME, NS2_DST_CAPTURE, NS2_DST_C, NS2_DST_GL,
    NS2_DST_GL, NS2_DST_GR, NS2_DST_GR, NS2_DST_GL, NS2_DST_GR,
};

uint8_t ns2_resolve_button_destination(uint8_t source_index,
                                       bool from_android_bridge) {
    if (source_index >= NS2_SRC_COUNT)
        return NS2_DST_NONE;

    // Android's companion descriptor names usages 1..4 as logical A/B/X/Y.
    // The generic HID parser necessarily exposes those usages through the same
    // JP_BUTTON_B1..B4 source slots used by positional physical controllers, but
    // their meaning is already normalized before it reaches the wire. Correct
    // only the canonical destination. The raw JP bitmap remains unchanged for
    // personality encoders (notably sideways Joy-Con 2) that consume it.
    if (from_android_bridge) {
        static const uint8_t ANDROID_BRIDGE_FACE_MAP[4] = {
            NS2_DST_A, NS2_DST_B, NS2_DST_X, NS2_DST_Y,
        };
        if (source_index < 4u)
            return ANDROID_BRIDGE_FACE_MAP[source_index];
    }

    return NS2_BASE_BUTTON_MAP[source_index];
}
