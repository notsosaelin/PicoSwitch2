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
// Against a ~763 Hz DualSense this is still a decimation, so slot placement
// decides which samples survive; see SAMPLES SPAN THE EMIT WINDOW below.
//
// EMISSION MODE
// -------------
// 0x28-only. Genuine controllers run in two modes and the 12-bit elapsed count
// means different things in each; in 0x28-only mode it is simply the tick delta
// since the previous 0x28, which holds in 1,196 of 1,196 genuine packets across
// 14 captures. Interleaving 0x1E would put us in the other mode, whose elapsed
// relation is NOT resolved. Callers must therefore not mix the two.
//
// SAMPLES SPAN THE EMIT WINDOW
// ----------------------------
// The slots are not consecutive sensor samples clustered at one end of the
// window: slot 0 is the OLDEST sample in the window and the last slot is the
// NEWEST. Measured on 973 genuine catch-up packets, the mean-square difference
// between slots orders strictly by slot index, and every gap sits below the
// full-window value that the accelerometer structure function saturates at:
//
//     seam a2->a0[N+1]   0.572   <-- smallest gap in the stream
//     a0->a1             0.607
//     a1->a2             0.680
//     a0->a2             0.866
//     one whole window   1.000   <-- saturated asymptote
//
// The decisive fact is the seam. If a packet held three consecutive samples
// taken at the start of its window, the step from its last slot to the next
// packet's first slot would be the LARGEST gap in the stream, not the
// smallest. A paired sign test over 894 tick-contiguous packet pairs puts the
// seam below the within-packet a0->a2 gap in 67.1% of pairs (z = +10.2).
//
// What is NOT resolved is the exact fractional position of each slot. The
// structure function saturates before one window elapses, so the map from
// mean-square difference back to elapsed time is compressive, and the corpus
// is stationary (per-axis noise ~2.0 counts, no coherent motion at any lag
// from 20 to 150 ms). The gaps can be ordered but not measured. This module
// therefore anchors the ends -- oldest sample first, newest sample last -- and
// spaces the interior evenly, which is the choice that both matches the
// confirmed ordering and minimises latency.
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

// The ring must outlast one emit window, or it evicts the oldest samples in
// that window -- the ones anchoring slot 0 -- and the packet silently stops
// spanning the window.
//
// MEASURED, not assumed: a DualSense supplying a valid sensor timestamp takes
// the ungated path in ns2_ds5_motion_update() (the 3800 us period gate only
// applies to the host-time fallback), so samples arrive at the controller's
// own IMU rate. On hardware, 2026-07-31: 9,721 samples in 12.7 s = ~763 Hz,
// with sensor_dt_us reading 1247-1870. That is ~15.3 samples per 20 ms
// window, not the ~5 a 250 Hz source would give.
//
// 64 entries is ~84 ms at that rate, or four windows of headroom, for 1 KB of
// state. Size this against the SOURCE rate, never against the slot count.
#define NS2_DS5_MOTION40_RING 64u

typedef struct {
    int16_t accel[3];  // raw DualSense counts, 8192/g
    int16_t gyro[3];   // de-biased DualSense counts, ~16.4/dps
    uint32_t us;
} ns2_ds5_motion40_entry_t;

typedef struct {
    // Timestamped source samples. Slot selection happens at build time, when
    // the window length is finally known; sampling time cannot know which
    // position in the window a sample will turn out to occupy.
    ns2_ds5_motion40_entry_t ring[NS2_DS5_MOTION40_RING];
    uint8_t head;    // next write index
    uint8_t filled;  // entries written, saturating at NS2_DS5_MOTION40_RING

    // Wire values chosen for the most recent emission. Retained so a host test
    // and the UART diagnostic can see what actually went out.
    int32_t accel[NS2_DS5_MOTION40_ACCEL_SLOTS][3];
    int32_t gyro[NS2_DS5_MOTION40_GYRO_SLOTS][3];

    // Two distinct clocks, and conflating them re-sends samples or drifts.
    // `last_emit_us` is the console-visible timeline: it advances by exactly
    // the elapsed count reported, so truncation remainders carry forward
    // instead of accumulating as drift. `last_sample_us` is the timestamp of
    // the newest sample already sent, which is what bounds the next selection
    // window -- a sample must never appear in two packets, and the tick-
    // aligned origin can fall either side of it.
    uint32_t last_emit_us;
    uint32_t last_sample_us;
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

// Record one physical IMU sample and the time it was taken, in raw DualSense
// counts. Call this once per source sample, not once per USB poll: the
// timestamps are what let the builder place samples across the window, so
// repeating a stale sample at the poll rate would misreport the timeline.
void ns2_ds5_motion40_sample(ns2_ds5_motion40_t *state, const int16_t accel[3],
                             const int16_t gyro[3], uint32_t now_us);

// Build a PDU if at least NS2_DS5_MOTION40_MIN_TICKS have passed and the
// window holds enough distinct samples to fill every slot without repeating
// one. `carrier_raw` is the current length-0x1E orientation carrier (three
// unsigned lanes of 26/25/24 bits); its modular slice becomes the packet's
// prefix. Returns false when not yet due, which is the normal case.
bool ns2_ds5_motion40_build(ns2_ds5_motion40_t *state,
                            const uint32_t carrier_raw[3], uint32_t now_us,
                            uint8_t out[NS2_MOTION_PDU40_LENGTH]);

// Slice a 0x1E carrier into the catch-up prefix's modular windows. Exposed for
// testing against the reference implementation.
void ns2_ds5_motion40_prefix(const uint32_t carrier_raw[3], int32_t out[3]);

#endif  // NS2_DS5_MOTION40_H
