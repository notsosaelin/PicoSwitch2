#include <stdio.h>
#include <string.h>

#include "bt/bthid/devices/generic/bthid_mouse_report.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) printf("OK:   %s\n", msg); \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    // Standard 3-button relative mouse with X/Y/wheel and no report ID.
    static const uint8_t mouse_desc[] = {
        0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
        0x95,0x03, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x05, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38,
        0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03, 0x81,0x06,
        0xC0, 0xC0
    };
    bthid_mouse_report_map_t map;
    CHECK(bthid_mouse_parse_descriptor(mouse_desc, sizeof(mouse_desc), &map),
          "relative X+Y descriptor is classified as a mouse");
    CHECK(map.button_count == 3 && map.wheel.present,
          "mouse buttons and wheel are discovered from the descriptor");

    static const uint8_t report[] = {0x05, 0xFE, 0x03, 0xFF};
    bthid_mouse_report_t decoded;
    CHECK(bthid_mouse_decode_report(&map, report, sizeof(report), &decoded),
          "standard mouse report decodes");
    CHECK(decoded.buttons == 0x05, "left and middle mouse buttons retain their usage indices");
    CHECK(decoded.delta_x == -2 && decoded.delta_y == 3,
          "relative axes are sign-extended");
    CHECK(decoded.wheel == -1, "relative wheel is sign-extended");

    // Absolute gamepad axes must not trigger mouse reclassification.
    static const uint8_t gamepad_desc[] = {
        0x05,0x01, 0x09,0x05, 0xA1,0x01,
        0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x00,
        0x75,0x08, 0x95,0x02, 0x81,0x02, 0xC0
    };
    CHECK(!bthid_mouse_parse_descriptor(gamepad_desc, sizeof(gamepad_desc), &map),
          "absolute gamepad X+Y is not misclassified as a mouse");

    printf("\n%s\n", failures ? "One or more checks FAILED." : "All checks passed.");
    return failures ? 1 : 0;
}
