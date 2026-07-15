#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "xbox_rumble.h"

int main(void) {
    uint8_t p[XBOX_RUMBLE_DATA_LEN];

    xbox_rumble_build_payload(0, 0, p);
    // Xbox byte 0 is a write mask. An all-zero packet means "update nothing"
    // and was the exact regression that left full-strength rumble latched.
    assert(p[0] == 0x03);
    for (unsigned i = 1; i < sizeof(p); ++i) assert(p[i] == 0);

    xbox_rumble_build_payload(255, 128, p);
    assert(p[0] == 0x03);
    assert(p[1] == 0 && p[2] == 0);
    assert(p[3] == 100);
    assert(p[4] == 50);
    assert(p[5] == 0x05);
    assert(p[6] == 0x00);
    assert(p[7] == 0xEB);

    xbox_rumble_build_payload(1, 0, p);
    assert(p[0] == 0x03);
    assert(p[3] == 0 && p[4] == 0);  // scaling may round down; actuator remains enabled
    assert(p[5] == 0x05 && p[7] == 0xEB);

    puts("xbox_rumble: all tests passed");
    return 0;
}
