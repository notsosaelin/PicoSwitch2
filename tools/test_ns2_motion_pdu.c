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

    {
        // Genuine normal/status-0x0D Pro Controller 2 PDU captured over the
        // UART bridge on 2026-07-24. Python's independent magprobe decoder
        // yields [45724, -180222, 272234].
        uint8_t pdu40[NS2_MOTION_PDU40_LENGTH] = {
            0x40, 0x7C, 0x00, 0x0D, 0x63, 0xDA, 0x2E, 0x27,
            0xD5, 0x18, 0x93, 0xB4, 0x6E, 0xF6, 0xB3, 0x00,
            0x83, 0x41, 0x3D, 0x00, 0x28, 0x14, 0xEA, 0xFF,
            0x9B, 0xF6, 0xFF, 0x3F, 0x01, 0xC0, 0x29, 0x0B,
            0x20, 0x00, 0xD4, 0xAB, 0x76, 0x42, 0xC0, 0x01,
        };
        const uint8_t preserved29 = pdu40[29] & 0x0Fu;
        const uint8_t preserved32 = pdu40[32] & 0x0Cu;
        const uint8_t preserved35 = pdu40[35] & 0x0Cu;
        const uint8_t preserved38 = pdu40[38];
        const uint8_t preserved39 = pdu40[39];
        int32_t reference[3] = {0};
        check(ns2_motion_pdu40_get_reference(pdu40, reference),
              "decode genuine 0x28 reference succeeds");
        check(reference[0] == 45724 && reference[1] == -180222 &&
                  reference[2] == 272234,
              "decode genuine 0x28 reference matches independent decoder");

        const int32_t reference_replacement[3] = {
            NS2_MOTION_REFERENCE_G6_MIN,
            NS2_MOTION_REFERENCE_G7_MAX,
            NS2_MOTION_REFERENCE_G8_MIN,
        };
        check(ns2_motion_pdu40_set_reference(pdu40, reference_replacement),
              "encode signed reference limits succeeds");
        memset(reference, 0, sizeof(reference));
        check(ns2_motion_pdu40_get_reference(pdu40, reference),
              "decode replaced 0x28 reference succeeds");
        check(memcmp(reference, reference_replacement, sizeof(reference)) == 0,
              "0x28 reference round-trip is exact");
        check((pdu40[29] & 0x0Fu) == preserved29 &&
                  (pdu40[32] & 0x0Cu) == preserved32 &&
                  (pdu40[35] & 0x0Cu) == preserved35 &&
                  pdu40[38] == preserved38 && pdu40[39] == preserved39,
              "0x28 replacement preserves every unassigned bit");

        const int32_t too_large[3] = {
            NS2_MOTION_REFERENCE_G6_MAX + 1, 0, 0,
        };
        check(!ns2_motion_pdu40_set_reference(pdu40, too_large),
              "0x28 encoder rejects out-of-range values");

        const int32_t edge22[] = {
            NS2_MOTION_REFERENCE_G6_MIN, -1, 0, 1,
            NS2_MOTION_REFERENCE_G6_MAX,
        };
        const int32_t edge20[] = {
            NS2_MOTION_REFERENCE_G8_MIN, -1, 0, 1,
            NS2_MOTION_REFERENCE_G8_MAX,
        };
        for (unsigned a = 0; a < sizeof(edge22) / sizeof(edge22[0]); ++a) {
            for (unsigned b = 0; b < sizeof(edge22) / sizeof(edge22[0]); ++b) {
                for (unsigned c = 0;
                     c < sizeof(edge20) / sizeof(edge20[0]); ++c) {
                    uint8_t patterned[NS2_MOTION_PDU40_LENGTH];
                    for (unsigned i = 0; i < sizeof(patterned); ++i)
                        patterned[i] = (uint8_t)(i * 37u + a * 11u +
                                                b * 7u + c * 3u);
                    uint8_t untouched[NS2_MOTION_PDU40_LENGTH];
                    memcpy(untouched, patterned, sizeof(untouched));
                    const int32_t edges[3] = {
                        edge22[a], edge22[b], edge20[c],
                    };
                    int32_t decoded[3] = {0};
                    check(ns2_motion_pdu40_set_reference(patterned, edges),
                          "0x28 edge combination encodes");
                    check(ns2_motion_pdu40_get_reference(patterned, decoded),
                          "0x28 edge combination decodes");
                    check(memcmp(edges, decoded, sizeof(edges)) == 0,
                          "0x28 edge combination round-trips");
                    for (unsigned i = 0; i < sizeof(patterned); ++i) {
                        if (i < 29u || i > 37u)
                            check(patterned[i] == untouched[i],
                                  "0x28 codec touches no outside byte");
                    }
                    check((patterned[29] & 0x0Fu) ==
                              (untouched[29] & 0x0Fu) &&
                              (patterned[32] & 0x0Cu) ==
                              (untouched[32] & 0x0Cu) &&
                              (patterned[35] & 0x0Cu) ==
                              (untouched[35] & 0x0Cu),
                          "0x28 codec preserves shared packing gaps");
                }
            }
        }
    }

    if (failures) return 1;
    puts("ns2_motion_pdu: all tests passed");
    return 0;
}
