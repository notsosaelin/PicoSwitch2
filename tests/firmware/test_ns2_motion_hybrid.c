#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_motion_hybrid.h"
#include "ns2_motion_pdu.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void build_high_rate(uint8_t out[40], uint16_t tick, int32_t seed)
{
    ns2_motion40_high_rate_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.tick = tick;
    fields.elapsed_ticks = 8u;
    fields.carrier[0] = seed;
    fields.carrier[1] = -seed;
    fields.carrier[2] = seed + 1;
    for (unsigned slot = 0; slot < 2u; ++slot)
        for (unsigned axis = 0; axis < 3u; ++axis)
            fields.accel[slot][axis] = seed + (int32_t)(slot * 3u + axis);
    for (unsigned axis = 0; axis < 3u; ++axis)
        fields.gyro[0][axis] = seed * 2 + (int32_t)axis;
    fields.tail_value = 0x01C0u;
    fields.packing_mode = 3u;
    fields.status = NS2_MOTION40_STATUS_HIGH_RATE;
    check(ns2_motion_pdu40_build_high_rate(out, &fields),
          "high-rate fixture builds");
}

static void test_high_rate_group_splice(void)
{
    uint8_t base[40];
    uint8_t donor[40];
    uint8_t out[40];
    build_high_rate(base, 100u, 10);
    build_high_rate(donor, 108u, 30);

    check(ns2_motion_hybrid_splice(base, donor, 40u,
              NS2_MOTION_HYBRID_GYRO, out) == NS2_MOTION_HYBRID_OK,
          "gyro-only splice succeeds");
    // High-rate gyro occupies bits 172..237, or bytes 21.5 through 29.75.
    for (unsigned bit = 0; bit < 320u; ++bit) {
        const uint8_t mask = (uint8_t)(1u << (bit & 7u));
        const unsigned byte = bit >> 3;
        const int in_gyro = bit >= 172u && bit < 238u;
        const int expected = (in_gyro ? donor[byte] : base[byte]) & mask;
        check((out[byte] & mask) == expected,
              "gyro splice preserves every unselected bit");
    }

    const uint32_t all = ns2_motion_hybrid_available_groups(base, 40u);
    check(ns2_motion_hybrid_splice(base, donor, 40u, all, out) ==
              NS2_MOTION_HYBRID_OK,
          "all-group splice succeeds");
    check(memcmp(out, donor, sizeof(out)) == 0,
          "all semantic groups reproduce donor byte-exactly");
}

static void test_fail_closed(void)
{
    uint8_t base[40];
    uint8_t donor[40];
    uint8_t out[40];
    build_high_rate(base, 100u, 10);
    build_high_rate(donor, 108u, 30);
    memset(out, 0xA5, sizeof(out));

    donor[1] = (uint8_t)((donor[1] & 0x0Fu) | 0xC0u);  // elapsed 12, normal
    check(ns2_motion_hybrid_splice(base, donor, 40u,
              NS2_MOTION_HYBRID_ACCEL, out) ==
              NS2_MOTION_HYBRID_LAYOUT_MISMATCH,
          "layout mismatch is rejected");
    for (unsigned i = 0; i < sizeof(out); ++i)
        check(out[i] == 0xA5u, "failure leaves output untouched");

    build_high_rate(donor, 108u, 30);
    donor[4] &= 0xFCu;
    check(ns2_motion_hybrid_splice(base, donor, 40u,
              NS2_MOTION_HYBRID_ACCEL, out) == NS2_MOTION_HYBRID_BAD_MODE,
          "non-mode-3 donor is rejected");

    build_high_rate(base, 100u, 10);
    build_high_rate(donor, 108u, 30);
    base[3] = 0x00u;
    donor[3] = 0x00u;
    check(ns2_motion_hybrid_splice(base, donor, 40u,
              NS2_MOTION_HYBRID_GYRO, out) == NS2_MOTION_HYBRID_OK,
          "matching genuine non-modal status is spliceable");
    check(out[3] == 0x00u,
          "genuine non-modal status remains byte-exact");
}

static void test_motion30_prefix_preserves_flag(void)
{
    uint8_t base[30] = {
        0x3C,0x44,0x00,0x0C,0x00,0x06,0xE2,0xED,0x01,0x43,
        0x6C,0xE7,0x80,0x44,0xBF,0x86,0x3B,0xBC,0x1E,0x00,
        0x70,0x7F,0x24,0xFD,0xE0,0x7F,0x98,0x10,0x00,0x02,
    };
    uint8_t donor[30];
    uint8_t out[30];
    memcpy(donor, base, sizeof(donor));
    donor[5] ^= 0x55u;
    donor[12] &= 0x7Fu;
    check(ns2_motion_hybrid_splice(base, donor, 30u,
              NS2_MOTION_HYBRID_PREFIX, out) == NS2_MOTION_HYBRID_OK,
          "0x1E prefix splice succeeds");
    check((out[12] & 0x80u) != 0u,
          "0x1E unexplained flag remains genuine-base authored");
    check(out[5] == donor[5], "0x1E selected carrier lane comes from donor");
}

int main(void)
{
    test_high_rate_group_splice();
    test_fail_closed();
    test_motion30_prefix_preserves_flag();
    if (failures) {
        fprintf(stderr, "ns2_motion_hybrid: %d failure(s)\n", failures);
        return 1;
    }
    puts("ns2_motion_hybrid: all tests passed");
    return 0;
}
