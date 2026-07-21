#include "ns2_joycon2_identity.h"

#include <string.h>

static const uint8_t IDENTITY_LEFT[NS2_JOYCON2_IDENTITY_LEN] = {
    0x01, 0x00,
    'H', 'B',
    'W', '9', '9', '9', '9', '9', '9', '9', '9', '9', '9', '9',
    0x00, 0x00,
    0x7E, 0x05,
    0x67, 0x20,
    0x01, 0x08,
    0x02, 0x32, 0x32, 0x32, 0xAA, 0xAA, 0xAA, 0x9B, 0xE1, 0xE6, 0x32, 0x32, 0x32,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

_Static_assert(sizeof(IDENTITY_LEFT) == NS2_JOYCON2_IDENTITY_LEN,
               "Joy-Con 2 identity must be 64 bytes");

void ns2_joycon2_build_identity(bool right, const uint8_t accent[3],
                                uint8_t out[NS2_JOYCON2_IDENTITY_LEN]) {
    memcpy(out, IDENTITY_LEFT, NS2_JOYCON2_IDENTITY_LEN);
    if (right) {
        out[2] = 'H';
        out[3] = 'C';
        out[20] = 0x66;
        out[21] = 0x20;
    }
    memcpy(&out[0x1F], accent, 3);
}

void ns2_joycon2_build_ep0_info(const uint8_t unit_id[6],
                                uint8_t out[NS2_JOYCON2_EP0_INFO_LEN]) {
    memset(out, 0, NS2_JOYCON2_EP0_INFO_LEN);
    out[0] = 0x02;
    out[1] = 0x01;
    out[2] = 0x04;  // current genuine Joy-Con 2: controller firmware 2.1.4
    out[6] = 0x0C;  // Bluetooth 12.x
    memcpy(&out[10], unit_id, 6);
}

void ns2_joycon2_build_command_info(bool right,
                                    uint8_t out[NS2_JOYCON2_COMMAND_INFO_LEN]) {
    memset(out, 0, NS2_JOYCON2_COMMAND_INFO_LEN);
    out[0] = 0x02;
    out[1] = 0x01;
    out[2] = 0x04;
    out[3] = right ? 0x01 : 0x00;
    out[4] = 0x0C;
    // out[8..10] = 00 00 00: genuine current Joy-Con 2 has no DSP firmware.
}
