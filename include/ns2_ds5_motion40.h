#ifndef NS2_DS5_MOTION40_H
#define NS2_DS5_MOTION40_H

#include <stdbool.h>
#include <stdint.h>

#include "ns2_motion_pdu.h"

// Translate a DualSense IMU stream into genuine-shaped length-0x28 catch-up
// motion PDUs.
//
// WHY A SEPARATE MODULE
// ---------------------
// ns2_ds5_motion.c owns the hardware-validated length-0x1E path and is not
// touched by this. The two share nothing but their input samples, so enabling
// or disabling 0x28 cannot regress orientation carrier behaviour.
//
// WHY CATCH-UP
// ------------
// Its tail is a single always-zero bit; the normal and high-rate layouts carry
// a 16-bit tail holding two Q3 die-temperature samples, which a DualSense
// cannot supply without fabricating a physical quantity. Catch-up also carries
// five IMU samples per packet, so a 20 ms cadence delivers ~250 samples/s
// against the 133 Hz single-sample 0x1E path -- a fidelity gain, not a trade.
//
// EMISSION MODE
// -------------
// 0x28-only. Genuine controllers run in two modes and the 12-bit elapsed count
// means different things in each; in 0x28-only mode it is simply the tick delta
// since the previous 0x28, which holds in 1,196 of 1,196 genuine packets across
// 14 captures. Interleaving 0x1E would put us in the other mode, whose elapsed
// relation is NOT resolved. Callers must therefore not mix the two.
//
// See docs/experiments/pro2-carrier-unknown-fields-2026-07-31.md.

// 800 Hz internal tick.
#define NS2_DS5_MOTION40_TICK_US 1250u

// Catch-up needs elapsed >= 15 ticks (18.75 ms). 16 ticks is 20 ms, which
// leaves a ~250 Hz source about five samples to fill three acceleration and
// two gyro slots.
#define NS2_DS5_MOTION40_MIN_TICKS NS2_MOTION40_CATCHUP_MIN_ELAPSED
#define NS2_DS5_MOTION40_ACCEL_SLOTS 3u
#define NS2_DS5_MOTION40_GYRO_SLOTS 2u

typedef struct {
    // Samples in ordinary ICM counts (4096/g, 16.4/dps), oldest first.
    int32_t accel[NS2_DS5_MOTION40_ACCEL_SLOTS][3];
    int32_t gyro[NS2_DS5_MOTION40_GYRO_SLOTS][3];
    uint8_t accel_count;
    uint8_t gyro_count;

    uint32_t last_emit_us;
    uint16_t tick;
    bool primed;

    // Diagnostics. Saturation is expected occasionally -- the wire fields cap
    // near +/-2 g and +/-499 dps -- but a high rate means the scaling is wrong.
    uint32_t emitted;
    uint32_t skipped_no_samples;
    uint32_t saturated_accel;
    uint32_t saturated_gyro;
} ns2_ds5_motion40_t;

void ns2_ds5_motion40_reset(ns2_ds5_motion40_t *state);

// Push one physical IMU sample, in raw DualSense counts. Acceleration is
// halved (DualSense 8192/g into the Pro 2's 4096/g); gyro passes through, the
// two sensors agreeing on ~16.4 counts/dps. Samples beyond the layout's slots
// are dropped rather than overwriting: a packet must carry consecutive
// samples, and this module never invents or reorders data.
void ns2_ds5_motion40_sample(ns2_ds5_motion40_t *state,
                             const int16_t accel[3], const int16_t gyro[3]);

// Build a PDU if at least NS2_DS5_MOTION40_MIN_TICKS have passed and every
// slot is filled. `carrier_raw` is the current length-0x1E orientation carrier
// (three unsigned lanes of 26/25/24 bits); its modular slice becomes the
// packet's prefix. Returns false when not yet due, which is the normal case.
bool ns2_ds5_motion40_build(ns2_ds5_motion40_t *state,
                            const uint32_t carrier_raw[3], uint32_t now_us,
                            uint8_t out[NS2_MOTION_PDU40_LENGTH]);

// Slice a 0x1E carrier into the catch-up prefix's modular windows. Exposed for
// testing against the reference implementation.
void ns2_ds5_motion40_prefix(const uint32_t carrier_raw[3], int32_t out[3]);

#endif  // NS2_DS5_MOTION40_H
