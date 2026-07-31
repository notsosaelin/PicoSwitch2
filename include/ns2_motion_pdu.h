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

// DEPRECATED -- DIAGNOSTIC READ ONLY. DO NOT GENERATE THROUGH THIS.
//
// These lanes were named "G6/G7/G8 reference vector" while the length-0x28 PDU
// was believed to carry magnetometer data. That interpretation is REFUTED. The
// 0x28 payload is a packed multi-sample IMU record whose layout is selected by
// the packet's own elapsed count, and it is now fully decoded and byte-exact
// (docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md).
//
// The 22/22/20-bit window below is a real bit range, but it is an ALIAS that
// crosses genuine packed acceleration and gyro fields. Writing through it
// corrupts IMU samples while leaving a well-formed packet -- precisely the
// static-template failure AGENTS.md records as causing random motion on
// hardware.
//
// No production code calls these; only tools/test_ns2_motion_pdu.c, which
// pins the historical codec for traceability. A generator must use the full
// layout-aware packer, not this window.
bool ns2_motion_pdu40_get_reference(
    const uint8_t pdu[NS2_MOTION_PDU40_LENGTH], int32_t out[3]);
bool ns2_motion_pdu40_set_reference(
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH], const int32_t values[3]);

#endif  // NS2_MOTION_PDU_H
