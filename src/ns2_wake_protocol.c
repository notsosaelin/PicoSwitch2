#include "ns2_wake_protocol.h"

#include <string.h>

static bool address_is_nonzero(const uint8_t addr[6]) {
    uint8_t any = 0;
    for (int i = 0; i < 6; i++) any |= addr[i];
    return any != 0;
}

bool ns2_wake_parse_pairing_data(const uint8_t *data, size_t len,
                                 uint16_t product_id,
                                 const uint8_t controller_addr_wire[6],
                                 config_wake_identity_t *out) {
    if (!data || !controller_addr_wire || !out || len < 2 ||
        !address_is_nonzero(controller_addr_wire)) {
        return false;
    }

    uint8_t count = data[1];
    if (count == 0 || count > CONFIG_WAKE_MAX_HOSTS ||
        len < (size_t)(2 + count * 6)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->product_id = product_id;
    out->host_count = count;
    memcpy(out->controller_addr_wire, controller_addr_wire, 6);
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *host = &data[2 + i * 6];
        if (!address_is_nonzero(host)) {
            memset(out, 0, sizeof(*out));
            return false;
        }
        memcpy(out->host_addr_wire[i], host, 6);
    }
    return true;
}

void ns2_wake_build_advertisement(uint16_t product_id,
                                  const uint8_t host_addr_wire[6],
                                  uint8_t out[NS2_WAKE_ADV_LEN]) {
    static const uint8_t prefix[] = {
        0x02, 0x01, 0x06,       // LE General Discoverable + BR/EDR unsupported
        0x1B, 0xFF,             // 27-byte manufacturer-specific AD structure
        0x53, 0x05,             // Nintendo company ID 0x0553, little-endian
        0x01, 0x00, 0x03,
        0x7E, 0x05,             // Nintendo VID 0x057E, little-endian
    };

    memset(out, 0, NS2_WAKE_ADV_LEN);
    memcpy(out, prefix, sizeof(prefix));
    out[12] = (uint8_t)product_id;
    out[13] = (uint8_t)(product_id >> 8);
    out[14] = 0x00;
    out[15] = 0x01;
    out[16] = 0x81;
    memcpy(&out[17], host_addr_wire, 6);
    out[23] = 0x0F;
}
