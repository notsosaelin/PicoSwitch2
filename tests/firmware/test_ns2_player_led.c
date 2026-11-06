#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ns2_player_led.h"

static void expect(uint8_t wire, uint8_t expected) {
    uint8_t player = 0;
    assert(ns2_player_led_decode(wire, &player));
    assert(player == expected);
}

int main(void) {
    expect(0x01, 1);
    expect(0x03, 2);
    expect(0x07, 3);
    expect(0x0F, 4);
    expect(0x10, 1);
    expect(0x30, 2);
    expect(0x70, 3);
    expect(0xF0, 4);

    uint8_t unchanged = 0xA5;
    assert(!ns2_player_led_decode(0x00, &unchanged));
    assert(unchanged == 0xA5);
    assert(!ns2_player_led_decode(0x02, NULL));
    assert(!ns2_player_led_decode(0xFF, NULL));

    puts("ns2_player_led: all tests passed");
    return 0;
}
