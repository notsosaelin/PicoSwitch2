#ifndef NS2_PLAYER_LED_H
#define NS2_PLAYER_LED_H

#include <stdbool.h>
#include <stdint.h>

// Decode the Switch player-LED wire bitfield. Low nibble means steady LEDs;
// high nibble means flashing LEDs during registration. Both use cumulative
// patterns: 1/3/7/F = players 1/2/3/4.
bool ns2_player_led_decode(uint8_t wire_mask, uint8_t *player_number);

#endif  // NS2_PLAYER_LED_H
