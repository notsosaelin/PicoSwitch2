/*
 * Host-compilable tests for Switch 2 wake identity parsing and advertisement
 * construction. No Pico SDK, TinyUSB, or BTstack dependency:
 *
 *   gcc -I include -o test_ns2_wake_protocol \
 *       tools/test_ns2_wake_protocol.c src/ns2_wake_protocol.c
 */
#include <stdio.h>
#include <string.h>

#include "ns2_wake_protocol.h"

static int failures;

#define CHECK(cond, msg) do {                 \
    if (!(cond)) {                            \
        printf("FAIL: %s\n", msg);           \
        failures++;                           \
    } else {                                  \
        printf("OK:   %s\n", msg);           \
    }                                         \
} while (0)

int main(void) {
    const uint8_t controller_wire[6] = {0x9E, 0x2B, 0xAB, 0xAB, 0xA9, 0x3C};
    const uint8_t host_wire[6] = {0xAB, 0x66, 0x9B, 0x55, 0xE2, 0x98};
    const uint8_t pairing[] = {
        0x00, 0x01,
        0xAB, 0x66, 0x9B, 0x55, 0xE2, 0x98,
    };
    config_wake_identity_t identity;

    CHECK(ns2_wake_parse_pairing_data(pairing, sizeof(pairing), 0x2069,
                                      controller_wire, &identity),
          "parse one-host 0x15/01 pairing data");
    CHECK(identity.product_id == 0x2069 && identity.host_count == 1,
          "preserve product ID and host count");
    CHECK(memcmp(identity.controller_addr_wire, controller_wire, 6) == 0 &&
          memcmp(identity.host_addr_wire[0], host_wire, 6) == 0,
          "preserve Nintendo wire byte order");

    CHECK(!ns2_wake_parse_pairing_data(pairing, sizeof(pairing) - 1, 0x2069,
                                       controller_wire, &identity),
          "reject truncated host address");
    const uint8_t zero_count[] = {0x00, 0x00};
    CHECK(!ns2_wake_parse_pairing_data(zero_count, sizeof(zero_count), 0x2069,
                                       controller_wire, &identity),
          "reject zero-host pairing data");

    uint8_t adv[NS2_WAKE_ADV_LEN];
    ns2_wake_build_advertisement(0x2066, host_wire, adv);
    const uint8_t known_good[NS2_WAKE_ADV_LEN] = {
        0x02,0x01,0x06,0x1B,0xFF,0x53,0x05,0x01,0x00,0x03,
        0x7E,0x05,0x66,0x20,0x00,0x01,0x81,
        0xAB,0x66,0x9B,0x55,0xE2,0x98,
        0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    };
    CHECK(memcmp(adv, known_good, sizeof(adv)) == 0,
          "match independently hardware-tested Joy-Con 2 wake payload");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All NS2 wake protocol tests passed.\n");
    return 0;
}
