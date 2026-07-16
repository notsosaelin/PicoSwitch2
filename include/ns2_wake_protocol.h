#ifndef NS2_WAKE_PROTOCOL_H
#define NS2_WAKE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define NS2_WAKE_ADV_LEN 31

// Decode the data portion of Nintendo command 0x15/01. `data[0]` is the
// currently-unknown leading byte, `data[1]` is the host-address count, and the
// following addresses are already in the wire order used by wake packets.
bool ns2_wake_parse_pairing_data(const uint8_t *data, size_t len,
                                 uint16_t product_id,
                                 const uint8_t controller_addr_wire[6],
                                 config_wake_identity_t *out);

// Build the complete legacy 31-byte BLE advertising payload for one host.
void ns2_wake_build_advertisement(uint16_t product_id,
                                  const uint8_t host_addr_wire[6],
                                  uint8_t out[NS2_WAKE_ADV_LEN]);

#endif
