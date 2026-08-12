#ifndef NS2_MOTION_PDU_H
#define NS2_MOTION_PDU_H

#include <stdbool.h>
#include <stdint.h>

#define NS2_MOTION_PDU30_LENGTH 30u
#define NS2_MOTION_PDU40_LENGTH 40u
// The hardware-validated translated 0x1E carrier applies this established
// output calibration (`source counts * 68963`) rather than bare Q16.16
// (`source counts * 65536`). A synthesized 0x28 sharing that carrier must
// publish the same calibrated acceleration vector at representation changes.
#define NS2_MOTION30_ACCEL_Q16_PER_COUNT 68963
#define NS2_MOTION30_ACCEL_Q16_ONE       65536
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

// Replace only the mode-3 length-0x28 orientation prefix. The packet's own
// elapsed field selects high-rate (s24/s23/s25) or normal/catch-up
// (s22/s21/s23) widths. Every timing, status, packing, IMU and tail bit is
// preserved. Values must already be in the selected layout's signed wire form.
bool ns2_motion_pdu40_set_carrier(
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH], const int32_t values[3]);

// Decode the legacy strict-unit length-0x1E approximation into canonical
// [x,y,z,w]. This is used only to seed a diagnostic donor at a stable genuine
// boundary. Genuine transition captures refute this as the controller's exact
// private chart model, and some packets have retained energy >1; those are
// rejected rather than clamped or normalized. Never use this helper to claim
// exact genuine orientation or chart-transition semantics.
bool ns2_motion_pdu30_get_quaternion(
    const uint8_t pdu[NS2_MOTION_PDU30_LENGTH], float out_xyzw[4],
    uint8_t *omitted_state);

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

// ---------------------------------------------------------------------------
// Length-0x28 catch-up packer
// ---------------------------------------------------------------------------
//
// Builds a complete genuine-shaped multi-sample IMU PDU. This is the real
// layout-aware encoder; it supersedes the aliased reference window above and
// is the only supported way to generate a 0x28.
//
// Catch-up is the layout the translation path targets. Its tail is a single
// bit, zero in all 981 corpus packets, whereas the normal and high-rate
// layouts carry a 16-bit tail holding two Q3 die-temperature samples that a
// translated source cannot produce without fabricating a physical quantity.
// Catch-up also carries 3 acceleration and 2 gyro samples per packet, so a
// 20 ms cadence delivers ~250 IMU samples/s against the 133 Hz single-sample
// 0x1E path.
//
// The layout is selected by the packet's own elapsed count, so elapsed_ticks
// must be >= 15; the builder fails closed rather than silently emitting a
// different layout. In the 0x28-only emission mode this path uses, elapsed is
// simply the tick delta since the previous 0x28 -- exact in 1,196 of 1,196
// genuine packets across 14 captures.
//
// Sample values are in ordinary ICM counts (4096/g, 16.4/dps) EXCEPT that
// accel[1] is the half-resolution middle slot and both gyros sit at four times
// the ordinary scale; callers must pre-scale, matching
// ns2_motion_reference.WIRE_TO_COUNTS. Widths are 14/13/14 for acceleration,
// 16/16 for gyro, and 22/21/23 for the carrier prefix.
//
// See docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md.
#define NS2_MOTION40_CATCHUP_MIN_ELAPSED 15u
#define NS2_MOTION40_STATUS_CATCHUP 0x0Fu

typedef struct {
    uint16_t tick;           // 12-bit internal 800 Hz tick
    uint16_t elapsed_ticks;  // 12-bit, must be >= 15 to select catch-up
    int32_t carrier[3];      // orientation prefix, signed 22/21/23
    int32_t accel[3][3];     // signed 14/13/14, wire scale
    int32_t gyro[2][3];      // signed 16/16, wire scale
    uint8_t tail_bit;        // payload bit 287; zero in every genuine packet
    uint8_t packing_mode;    // 3 in every genuine packet
    uint8_t status;          // 0 selects NS2_MOTION40_STATUS_CATCHUP
} ns2_motion40_catchup_t;

// Returns false and leaves `pdu` untouched if any field exceeds its slot or
// the elapsed count would not select the catch-up layout.
bool ns2_motion_pdu40_build_catchup(uint8_t pdu[NS2_MOTION_PDU40_LENGTH],
                                    const ns2_motion40_catchup_t *fields);

// High-rate layout: elapsed 0..10 ticks, status 0x0D.
//
// WHY THIS LAYOUT AND NOT CATCH-UP
// --------------------------------
// Ground truth. Of the 773 genuine 0x28 packets that have a length-0x1E
// alongside them to validate against, 768 are high-rate and 2 are catch-up --
// because catch-up appears almost exclusively in 0x28-ONLY captures, which
// carry no 0x1E by definition. Catch-up was targeted first for the wrong
// reason: its tail is a single always-zero bit while this layout carries a
// 16-bit Q3 temperature pair. That optimised for ease of filling over strength
// of evidence, and the hardware A/B failed.
//
// Emitting here at elapsed 7..8 also lands where the two candidate prefix-epoch
// models agree to within one tick, instead of the 9 ticks they differ by at a
// catch-up cadence.
//
// Two acceleration slots and one gyro slot are all 22-bit, but the binary
// point is field-specific: acceleration uses eight fractional bits
// (wire = counts * 256), gyro uses seven (wire = counts * 128). Carrier lanes
// are two bits wider than their catch-up counterparts.
#define NS2_MOTION40_HIGH_RATE_MAX_ELAPSED 10u
#define NS2_MOTION40_STATUS_HIGH_RATE 0x0Du
#define NS2_MOTION40_HIGH_RATE_ACCEL_FRACTIONAL_BITS 8u
#define NS2_MOTION40_HIGH_RATE_GYRO_FRACTIONAL_BITS 7u

typedef struct {
    uint16_t tick;           // 12-bit internal 800 Hz tick
    uint16_t elapsed_ticks;  // 12-bit, must be <= 10 to select high-rate
    int32_t carrier[3];      // orientation prefix, signed 24/23/25
    int32_t accel[2][3];     // signed 22, wire = counts * 256
    int32_t gyro[1][3];      // signed 22, wire = counts * 128
    uint16_t tail_value;     // 16-bit, two Q3 die-temperature samples
    uint8_t packing_mode;
    uint8_t status;
} ns2_motion40_high_rate_t;

bool ns2_motion_pdu40_build_high_rate(uint8_t pdu[NS2_MOTION_PDU40_LENGTH],
                                      const ns2_motion40_high_rate_t *fields);

#endif  // NS2_MOTION_PDU_H
