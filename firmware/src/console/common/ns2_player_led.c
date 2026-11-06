#include "ns2_player_led.h"

bool ns2_player_led_decode(uint8_t wire_mask, uint8_t *player_number) {
    uint8_t steady = wire_mask & 0x0Fu;
    uint8_t flashing = (wire_mask >> 4) & 0x0Fu;
    if (steady != 0 && flashing != 0) return false;
    uint8_t pattern = steady != 0 ? steady : flashing;

    uint8_t player;
    switch (pattern) {
        case 0x01: player = 1; break;
        case 0x03: player = 2; break;
        case 0x07: player = 3; break;
        case 0x0F: player = 4; break;
        default: return false;
    }

    if (player_number) *player_number = player;
    return true;
}
