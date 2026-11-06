#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_nfc_mirror.h"

int main(void) {
    const uint8_t usb_command[] = {
        0x01, 0x91, 0x00, 0x15, 0x00, 0x04, 0x00, 0x00,
        0x34, 0x12, 0x20, 0x00,
    };
    uint8_t ble_command[sizeof(usb_command)];
    memset(ble_command, 0xCC, sizeof(ble_command));

    assert(ns2_nfc_mirror_prepare_ble_command(
        ble_command, sizeof(ble_command), usb_command, sizeof(usb_command)));
    assert(ble_command[2] == 0x01);
    assert(memcmp(ble_command, usb_command, 2) == 0);
    assert(memcmp(&ble_command[3], &usb_command[3],
                  sizeof(usb_command) - 3) == 0);

    assert(!ns2_nfc_mirror_prepare_ble_command(
        ble_command, sizeof(ble_command) - 1, usb_command,
        sizeof(usb_command)));
    assert(!ns2_nfc_mirror_prepare_ble_command(
        ble_command, sizeof(ble_command), usb_command, 7));

    uint8_t wrong_command[8] = {0x10, 0x91, 0x00, 0x01};
    assert(!ns2_nfc_mirror_prepare_ble_command(
        ble_command, sizeof(ble_command), wrong_command,
        sizeof(wrong_command)));

    uint8_t extended[NS2_NFC_MIRROR_FRAME_MAX];
    memset(extended, 0xCC, sizeof(extended));
    const size_t extended_length =
        ns2_nfc_mirror_prepare_extended_ble_command(
            extended, sizeof(extended), usb_command, sizeof(usb_command));
    assert(extended_length ==
           NS2_NFC_MIRROR_EXTENDED_PREFIX + sizeof(usb_command));
    for (size_t i = 0; i < NS2_NFC_MIRROR_EXTENDED_PREFIX; ++i)
        assert(extended[i] == 0);
    assert(extended[NS2_NFC_MIRROR_EXTENDED_PREFIX + 2] == 0x01);
    assert(memcmp(
        &extended[NS2_NFC_MIRROR_EXTENDED_PREFIX + 3],
        &usb_command[3], sizeof(usb_command) - 3) == 0);
    assert(ns2_nfc_mirror_prepare_extended_ble_command(
        extended, extended_length - 1, usb_command,
        sizeof(usb_command)) == 0);

    const uint8_t ble_ack[] =
        {0x01, 0x01, 0x01, 0x03, 0x10, 0x78, 0x00, 0x00};
    uint8_t usb_ack[sizeof(ble_ack)];
    assert(ns2_nfc_mirror_translate_ble_response(
        usb_ack, sizeof(usb_ack), ble_ack, sizeof(ble_ack)) ==
        sizeof(ble_ack));
    const uint8_t expected_usb_ack[] =
        {0x01, 0x04, 0x00, 0x03, 0x00, 0xF8, 0x00, 0x00};
    assert(memcmp(usb_ack, expected_usb_ack, sizeof(usb_ack)) == 0);

    uint8_t ble_status[69] = {
        0x01, 0x01, 0x01, 0x05, 0x10, 0x78, 0x00, 0x00,
    };
    for (size_t i = 8; i < sizeof(ble_status); ++i)
        ble_status[i] = (uint8_t)i;
    uint8_t usb_status[sizeof(ble_status)];
    assert(ns2_nfc_mirror_translate_ble_response(
        usb_status, sizeof(usb_status), ble_status,
        sizeof(ble_status)) == sizeof(ble_status));
    assert(memcmp(
        usb_status,
        (const uint8_t[]){0x01, 0x01, 0x00, 0x05,
                          0x00, 0xF8, 0x00, 0x00},
        8) == 0);
    assert(memcmp(&usb_status[8], &ble_status[8],
                  sizeof(ble_status) - 8) == 0);
    assert(ns2_nfc_mirror_translate_ble_response(
        usb_status, 7, ble_ack, sizeof(ble_ack)) == 0);

    puts("ns2_nfc_mirror: all tests passed");
    return 0;
}
