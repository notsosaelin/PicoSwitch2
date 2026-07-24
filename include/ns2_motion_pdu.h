#ifndef NS2_MOTION_PDU_H
#define NS2_MOTION_PDU_H

#include <stdbool.h>
#include <stdint.h>

#define NS2_MOTION_PDU30_LENGTH 30u
#define NS2_MOTION_ORIENTATION_MASK 0x03FFFFFFu

// Extract/replace the three 26-bit live-orientation carriers in a genuine
// 0x1E Switch 2 motion PDU. The fields cross byte boundaries and their high
// bits live in the following slot (G0/G1) or the swap-state byte (G2).
// Replacing them preserves every timing, status, acceleration, and reserved
// bit outside those fields.
bool ns2_motion_pdu30_get_orientation(const uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                                      uint32_t out[3]);
bool ns2_motion_pdu30_set_orientation(uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                                      const uint32_t values[3]);

#endif  // NS2_MOTION_PDU_H
