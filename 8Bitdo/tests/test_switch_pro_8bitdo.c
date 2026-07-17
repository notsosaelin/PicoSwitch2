// Host-side tests for the first-generation 8BitDo Ultimate Bluetooth paddle
// translation used by the Nintendo Switch Pro Bluetooth driver.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../integration/switch_pro_8bitdo.h"
#include "core/buttons.h"

static int failures;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            printf("FAIL: %s\n", message);                                     \
            failures++;                                                       \
        }                                                                      \
    } while (0)

static void test_identity(void)
{
    // Synthetic suffix plus the tested E4:17:D8 OUI, in BTstack byte order.
    const uint8_t ultimate_addr[6] = {0x03, 0x02, 0x01, 0xD8, 0x17, 0xE4};
    const uint8_t genuine_addr[6] = {0x01, 0x02, 0x03, 0xCC, 0x9E, 0x98};

    CHECK(switch_pro_8bitdo_ultimate_match(
              ultimate_addr, "Pro Controller", 0x057E, 0x2009),
          "captured 8BitDo spoof identity matches");
    CHECK(switch_pro_8bitdo_ultimate_match(
              ultimate_addr, "Pro Controller", 0, 0),
          "provisional identity matches before late SDP VID/PID");
    CHECK(switch_pro_8bitdo_ultimate_match(
              ultimate_addr, "Pro Controller", 0x057E, 0),
          "partially resolved identity matches");
    CHECK(!switch_pro_8bitdo_ultimate_match(
              genuine_addr, "Pro Controller", 0x057E, 0x2009),
          "genuine Nintendo OUI does not match");
    CHECK(!switch_pro_8bitdo_ultimate_match(
              ultimate_addr, "Joy-Con (L)", 0x057E, 0x2006),
          "8BitDo OUI alone does not claim another Switch device");
    CHECK(!switch_pro_8bitdo_ultimate_match(
              ultimate_addr, "Pro Controller", 0x057E, 0x2069),
          "contradictory resolved PID does not match");
}

static void test_translation(void)
{
    const uint32_t p1 =
        JP_BUTTON_R1 | JP_BUTTON_A1 | JP_BUTTON_L1 | JP_BUTTON_L2;
    const uint32_t p2 =
        JP_BUTTON_B1 | JP_BUTTON_B2 | JP_BUTTON_B3 | JP_BUTTON_B4;

    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(p1, 0) == JP_BUTTON_L4,
          "P1 signature becomes left grip source");
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(p2, 0) == JP_BUTTON_R4,
          "P2 signature becomes right grip source");
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(p1 | p2, 0) ==
              (JP_BUTTON_L4 | JP_BUTTON_R4),
          "both paddle signatures combine cleanly");

    uint32_t with_extra = p1 | JP_BUTTON_B1 | JP_BUTTON_R2;
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(with_extra, 0) ==
              (JP_BUTTON_L4 | JP_BUTTON_B1 | JP_BUTTON_R2),
          "ordinary buttons outside a signature are preserved");

    uint32_t partial = p1 & ~JP_BUTTON_A1;
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(partial, 0) == partial,
          "partial P1 chord is not consumed");
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(JP_BUTTON_B1, 0) ==
              JP_BUTTON_B1,
          "ordinary single button is unchanged");
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(0, 0) == 0,
          "neutral report remains neutral");

    CHECK(switch_pro_translate_reserved_paddles(0, 0x02) ==
              JP_BUTTON_L4,
          "wire bit 7 / physical P1 becomes left grip source");
    CHECK(switch_pro_translate_reserved_paddles(0, 0x01) ==
              JP_BUTTON_R4,
          "wire bit 6 / physical P2 becomes right grip source");
    CHECK(switch_pro_translate_reserved_paddles(0, 0x03) ==
              (JP_BUTTON_L4 | JP_BUTTON_R4),
          "custom firmware paddle bits combine cleanly");
    CHECK(switch_pro_translate_reserved_paddles(
              JP_BUTTON_B1 | JP_BUTTON_R2, 0x02) ==
              (JP_BUTTON_B1 | JP_BUTTON_R2 | JP_BUTTON_L4),
          "custom firmware paddle preserves ordinary held buttons");
    CHECK(switch_pro_8bitdo_ultimate_translate_paddles(p2, 0x02) ==
              (JP_BUTTON_L4 | JP_BUTTON_R4),
          "custom firmware and stock-profile fallback can coexist");
}

static void test_hardware_report_capture(void)
{
    const uint8_t p1_report[] = {
        0x30, 0xF0, 0x60, 0x00, 0x80, 0x00,
    };
    const uint8_t p2_report[] = {
        0x30, 0xF0, 0x60, 0x00, 0x40, 0x00,
    };
    const uint8_t both_report[] = {
        0x30, 0xF0, 0x60, 0x00, 0xC0, 0x00,
    };

    CHECK(switch_pro_translate_reserved_paddles(
              0, switch_pro_extract_reserved_paddles(
                     p1_report, sizeof(p1_report))) == JP_BUTTON_L4,
          "captured P1 report becomes GL");
    CHECK(switch_pro_translate_reserved_paddles(
              0, switch_pro_extract_reserved_paddles(
                     p2_report, sizeof(p2_report))) == JP_BUTTON_R4,
          "captured P2 report becomes GR");
    CHECK(switch_pro_translate_reserved_paddles(
              0, switch_pro_extract_reserved_paddles(
                     both_report, sizeof(both_report))) ==
              (JP_BUTTON_L4 | JP_BUTTON_R4),
          "captured simultaneous report becomes GL+GR");
    CHECK(switch_pro_extract_reserved_paddles(p1_report, 4) == 0,
          "short report cannot expose reserved paddle bits");
}

int main(void)
{
    test_identity();
    test_translation();
    test_hardware_report_capture();

    if (failures) {
        printf("switch_pro_8bitdo: %d failure(s)\n", failures);
        return 1;
    }
    puts("switch_pro_8bitdo: all tests passed");
    return 0;
}
