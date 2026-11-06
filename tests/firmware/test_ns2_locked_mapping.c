#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ns2_remap.h"

int main(void)
{
    static const uint8_t expected[NS2_SRC_COUNT] = {
        NS2_DST_B, NS2_DST_A, NS2_DST_Y, NS2_DST_X,
        NS2_DST_L, NS2_DST_R, NS2_DST_ZL, NS2_DST_ZR,
        NS2_DST_MINUS, NS2_DST_PLUS, NS2_DST_L3, NS2_DST_R3,
        NS2_DST_DUP, NS2_DST_DDOWN, NS2_DST_DLEFT, NS2_DST_DRIGHT,
        NS2_DST_HOME, NS2_DST_CAPTURE, NS2_DST_C, NS2_DST_GL,
        NS2_DST_GL, NS2_DST_GR, NS2_DST_GR, NS2_DST_GL, NS2_DST_GR,
    };

    for (size_t i = 0; i < NS2_SRC_COUNT; ++i) {
        assert(NS2_BASE_BUTTON_MAP[i] == expected[i]);
        assert(NS2_BASE_BUTTON_MAP[i] < NS2_DST_COUNT);
        assert(ns2_resolve_button_destination((uint8_t)i, false) == expected[i]);
    }

    // The companion has already normalized its four face usages to logical
    // A/B/X/Y. They must not pass through the positional physical-controller
    // B/A/Y/X base map a second time.
    static const uint8_t expected_bridge_faces[4] = {
        NS2_DST_A, NS2_DST_B, NS2_DST_X, NS2_DST_Y,
    };
    for (size_t i = 0; i < 4; ++i)
        assert(ns2_resolve_button_destination((uint8_t)i, true) ==
               expected_bridge_faces[i]);

    // Only those four inputs differ. Shoulders, system buttons, C/GameChat and
    // every future non-face source continue to use the locked map.
    for (size_t i = 4; i < NS2_SRC_COUNT; ++i)
        assert(ns2_resolve_button_destination((uint8_t)i, true) == expected[i]);

    assert(ns2_resolve_button_destination(NS2_SRC_COUNT, false) == NS2_DST_NONE);
    assert(ns2_resolve_button_destination(NS2_SRC_COUNT, true) == NS2_DST_NONE);

    puts("NS2 locked and Android bridge mapping tests passed");
    return 0;
}
