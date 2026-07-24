#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_motion_pdu.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void)
{
    // Genuine 0x1E PDU from the 2026-07-22 stationary UART capture.
    uint8_t pdu[30] = {
        0x3C,0x44,0x00,0x0C,0x00,0x06,0xE2,0xED,0x01,0x43,
        0x6C,0xE7,0x00,0x44,0xBF,0x86,0x3B,0xBC,0x1E,0x00,
        0x70,0x7F,0x24,0xFD,0xE0,0x7F,0x98,0x10,0x00,0x02
    };
    const uint8_t original[30] = {
        0x3C,0x44,0x00,0x0C,0x00,0x06,0xE2,0xED,0x01,0x43,
        0x6C,0xE7,0x00,0x44,0xBF,0x86,0x3B,0xBC,0x1E,0x00,
        0x70,0x7F,0x24,0xFD,0xE0,0x7F,0x98,0x10,0x00,0x02
    };
    uint32_t before[3];
    uint32_t after[3];
    check(ns2_motion_pdu30_get_orientation(pdu, before), "decode succeeds");
    check(before[0] == 0x01EDE206u, "G0 genuine decode");
    check(before[1] == 0x00E76C43u, "G1 genuine decode");
    check(before[2] == 0x0086BF44u, "G2 genuine decode");

    const uint32_t replacement[3] = {0x03ABCDEFu, 0x02123456u, 0x0155AA33u};
    check(ns2_motion_pdu30_set_orientation(pdu, replacement), "encode succeeds");
    check(ns2_motion_pdu30_get_orientation(pdu, after), "round-trip decode succeeds");
    check(memcmp(after, replacement, sizeof(after)) == 0, "all carriers round-trip");

    for (int i = 0; i < 30; ++i) {
        const int orientation_byte =
            (i >= 5 && i <= 15) || i == 4;
        if (!orientation_byte)
            check(pdu[i] == original[i], "non-orientation byte preserved");
    }
    check((pdu[4] & 0xFCu) == (original[4] & 0xFCu), "G2 shared-byte flags preserved");
    check((pdu[8] & 0xFCu) == (original[8] & 0xFCu), "G0 shared-byte flags preserved");
    check((pdu[12] & 0xFCu) == (original[12] & 0xFCu), "G1 shared-byte flags preserved");

    if (failures) return 1;
    puts("ns2_motion_pdu: all tests passed");
    return 0;
}
