#include "xbox_rumble.h"

#include <stdbool.h>

#define XBOX_RUMBLE_MAIN_MOTORS 0x03
#define XBOX_RUMBLE_PULSE_10MS  0x05
#define XBOX_RUMBLE_LOOP_COUNT  0xEB

void xbox_rumble_build_payload(uint8_t left, uint8_t right,
                               uint8_t out[XBOX_RUMBLE_DATA_LEN]) {
    const bool stopping = left == 0 && right == 0;

    // Byte 0 is a write/update mask, not an actuator master-enable. A stop
    // therefore MUST select the main motors while writing zero magnitudes.
    // Sending an all-zero report means "update nothing" and leaves the prior
    // motor state latched -- the regression caught by test_xbox_rumble.c.
    out[0] = XBOX_RUMBLE_MAIN_MOTORS;
    out[1] = 0x00;  // left trigger motor disabled
    out[2] = 0x00;  // right trigger motor disabled
    out[3] = (uint8_t)(((uint16_t)left * 100u) / 255u);
    out[4] = (uint8_t)(((uint16_t)right * 100u) / 255u);
    out[5] = stopping ? 0x00 : XBOX_RUMBLE_PULSE_10MS;
    out[6] = 0x00;
    out[7] = stopping ? 0x00 : XBOX_RUMBLE_LOOP_COUNT;
}
