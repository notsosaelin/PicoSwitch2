#ifndef NS2_MOTION_PDU_H
#define NS2_MOTION_PDU_H

#include <stdbool.h>
#include <stdint.h>

#define NS2_MOTION_PDU30_LENGTH 30u
#define NS2_MOTION_PDU40_LENGTH 40u
#define NS2_MOTION_ORIENTATION_MASK 0x03FFFFFFu

#define NS2_MOTION_REFERENCE_G6_MIN (-2097152)
#define NS2_MOTION_REFERENCE_G6_MAX 2097151
#define NS2_MOTION_REFERENCE_G7_MIN (-2097152)
#define NS2_MOTION_REFERENCE_G7_MAX 2097151
#define NS2_MOTION_REFERENCE_G8_MIN (-524288)
#define NS2_MOTION_REFERENCE_G8_MAX 524287

// Extract/replace the three 26-bit live-orientation carriers in a genuine
// 0x1E Switch 2 motion PDU. The fields cross byte boundaries and their high
// bits live in the following slot (G0/G1) or the swap-state byte (G2).
// Replacing them preserves every timing, status, acceleration, and reserved
// bit outside those fields.
bool ns2_motion_pdu30_get_orientation(const uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                                      uint32_t out[3]);
bool ns2_motion_pdu30_set_orientation(uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                                      const uint32_t values[3]);

// Extract/replace the signed G6/G7/G8 lanes in a normal length-0x28 PDU.
// The three values are 22/22/20-bit signed integers packed through shared
// nibbles. Replacement deliberately preserves every unassigned bit:
// p29[3:0], p32[3:2], p35[3:2], the escalation byte, and the final byte.
// This codec does not claim semantics for the rest of the 0x28 PDU.
bool ns2_motion_pdu40_get_reference(
    const uint8_t pdu[NS2_MOTION_PDU40_LENGTH], int32_t out[3]);
bool ns2_motion_pdu40_set_reference(
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH], const int32_t values[3]);

#endif  // NS2_MOTION_PDU_H
