#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "switch_gc_rumble_decode.h"

int main(void) {
    static const uint8_t captured[8][4] = {
        {0x50, 0x02, 0x00, 0x00},
        {0x61, 0x00, 0x01, 0x00},
        {0x63, 0x00, 0x01, 0x00},
        {0x55, 0x02, 0x00, 0x00},
        {0x66, 0x00, 0x01, 0x00},
        {0x68, 0x00, 0x01, 0x00},
        {0x5A, 0x02, 0x00, 0x00},
        {0x5B, 0x00, 0x00, 0x00},
    };
    static const switch_gc_rumble_command_t expected[8] = {
        SWITCH_GC_RUMBLE_STOP,
        SWITCH_GC_RUMBLE_ON,
        SWITCH_GC_RUMBLE_ON,
        SWITCH_GC_RUMBLE_STOP,
        SWITCH_GC_RUMBLE_ON,
        SWITCH_GC_RUMBLE_ON,
        SWITCH_GC_RUMBLE_STOP,
        SWITCH_GC_RUMBLE_OFF,
    };

    for (unsigned i = 0; i < 8; ++i) {
        assert(switch_gc_decode_rumble(0x03, captured[i], 4) == expected[i]);
    }

    const uint8_t documented_on[4] = {0x51, 0x01, 0x00, 0x00};
    assert(switch_gc_decode_rumble(0x03, documented_on, 4) == SWITCH_GC_RUMBLE_ON);
    assert(switch_gc_decode_rumble(0, NULL, 0) == SWITCH_GC_RUMBLE_NO_CHANGE);
    assert(switch_gc_decode_rumble(0x02, documented_on, 4) == SWITCH_GC_RUMBLE_NO_CHANGE);
    assert(switch_gc_decode_rumble(0x03, documented_on, 2) == SWITCH_GC_RUMBLE_STOP);

    puts("switch_gc_rumble_decode: all genuine-capture tests passed");
    return 0;
}
