#include "ns2_nfc_mirror.h"

#include <string.h>

bool ns2_nfc_mirror_prepare_ble_command(
    uint8_t *destination, size_t destination_capacity,
    const uint8_t *source, size_t source_length) {
    if (!destination || !source || source_length < 8u ||
        source_length > NS2_NFC_MIRROR_COMMAND_MAX ||
        destination_capacity < source_length ||
        source[0] != 0x01u) {
        return false;
    }

    memcpy(destination, source, source_length);
    destination[2] = 0x01u;
    return true;
}

size_t ns2_nfc_mirror_prepare_extended_ble_command(
    uint8_t *destination, size_t destination_capacity,
    const uint8_t *source, size_t source_length) {
    const size_t framed_length =
        NS2_NFC_MIRROR_EXTENDED_PREFIX + source_length;
    if (!destination || destination_capacity < framed_length) return 0;

    memset(destination, 0, NS2_NFC_MIRROR_EXTENDED_PREFIX);
    if (!ns2_nfc_mirror_prepare_ble_command(
            &destination[NS2_NFC_MIRROR_EXTENDED_PREFIX],
            destination_capacity - NS2_NFC_MIRROR_EXTENDED_PREFIX,
            source, source_length)) {
        return 0;
    }
    return framed_length;
}

size_t ns2_nfc_mirror_translate_ble_response(
    uint8_t *destination, size_t destination_capacity,
    const uint8_t *source, size_t source_length) {
    if (!destination || !source || source_length < 8u ||
        source_length > NS2_NFC_MIRROR_RESPONSE_MAX ||
        destination_capacity < source_length ||
        source[0] != 0x01u) {
        return 0;
    }

    memcpy(destination, source, source_length);
    destination[1] = source_length == 8u ? 0x04u : 0x01u;
    destination[2] = 0x00u;
    destination[4] = 0x00u;
    destination[5] = 0xF8u;
    return source_length;
}
