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
// WHY HIGH-RATE (this replaced catch-up on 2026-07-31)
// ----------------------------------------------------
// Ground truth. Of the 773 genuine 0x28 packets that have a length-0x1E
// alongside them to validate against, 768 are high-rate and 2 are catch-up --
// catch-up appears almost only in 0x28-ONLY captures, which carry no carrier
// by definition. Catch-up was targeted first because its tail is one
// always-zero bit while this layout carries a 16-bit Q3 temperature pair a
// DualSense cannot measure. That optimised for ease of filling over strength
// of evidence, and the hardware A/B failed.
//
// Retargeting also settles three questions structurally rather than by
// experiment. Genuine high-rate runs INTERLEAVED, so a 0x1E rides alongside
// and supplies the chart state the modular prefix must be unwrapped against;
// each 0x28 reaches the console once between carriers instead of ~20 times at
// a 1 kHz poll; and at elapsed 7..8 the two candidate prefix-epoch models
// agree to within one tick instead of the 9 they differ by at a 16-tick
// cadence.
//
// Two acceleration slots and one gyro slot, all 22-bit with EIGHT FRACTIONAL
// BITS: wire = ordinary counts * 256. Same +/-2 g and +/-499.5 dps range as
// every other layout, at finer resolution.
//
// EMISSION MODE
// -------------
// INTERLEAVED, which is what genuine hardware does at this cadence: the 0x1E
// carrier fills the polls between 0x28 packets (NS2_PDU40_FILL_CARRIER).
//
// UNRESOLVED: the elapsed relation in interleaved mode. In 0x28-only mode
// elapsed is the tick delta since the previous 0x28 (1196/1196 packets), and
// interleaved traffic does not follow that. This module emits the span its own
// samples cover, the only self-consistent choice available, and the ambiguity
// is why the readiness gate still reports UNKNOWN.
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
#define NS2_DS5_MOTION40_MIN_TICKS 7u
#define NS2_DS5_MOTION40_MAX_TICKS NS2_MOTION40_HIGH_RATE_MAX_ELAPSED
#define NS2_DS5_MOTION40_ACCEL_SLOTS 2u
#define NS2_DS5_MOTION40_GYRO_SLOTS 1u

// The modal genuine tail, 155 of 771 high-rate packets. A DualSense exposes no
// IMU die temperature, so this replays the most common REAL value rather than
// inventing a plausible-looking one. Not a measurement, and labelled as such.
#define NS2_DS5_MOTION40_TEMPERATURE_TAIL 0x01C0u

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

// How far after the window START the orientation prefix must be sampled.
//
// MEASURED across 24 interleaved captures and 773 paired packets
// (tools/ns2_motion40_prefix_epoch.py): a genuine prefix describes
// `tick - elapsed + 4` ticks, NOT the packet's own tick. Passing the current
// carrier put the orientation ~15 ms late at a 16-tick cadence and scored 13x
// the achievable error floor on the worst moving capture.
//
// 🔵 UNRESOLVED: whether the lag is window-relative (this) or a fixed ~3 ticks
// behind the tick. The corpus cannot separate them -- elapsed is 7 in almost
// every paired packet, where the two agree to within one tick. They diverge by
// 9 ticks at a 16-tick cadence, which is one more reason to emit near elapsed
// 7-8 where the question does not arise.
#define NS2_DS5_MOTION40_PREFIX_LAG_TICKS 4u

typedef struct {
    int16_t accel[3];  // raw DualSense counts, 8192/g
    int16_t gyro[3];   // de-biased DualSense counts, ~16.4/dps
    // The 0x1E orientation carrier as it stood at this instant. Buffered per
    // sample because the prefix describes a PAST moment: by the time a packet
    // is built, the orientation it must carry is already history.
    uint32_t carrier[3];
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
    uint32_t skipped_overlong;  // window outran the high-rate elapsed band
    uint32_t saturated_accel;
    uint32_t saturated_gyro;
} ns2_ds5_motion40_t;

void ns2_ds5_motion40_reset(ns2_ds5_motion40_t *state);

// Record one physical IMU sample, the orientation carrier as it stood at that
// instant, and the time. Call this once per source sample, not once per USB
// poll: the timestamps are what let the builder place samples across the
// window and pick the orientation from the right past moment, so repeating a
// stale sample at the poll rate would misreport the timeline.
//
// `carrier_raw` is the length-0x1E carrier (three unsigned lanes of 26/25/24
// bits) built from this same sample.
void ns2_ds5_motion40_sample(ns2_ds5_motion40_t *state, const int16_t accel[3],
                             const int16_t gyro[3],
                             const uint32_t carrier_raw[3], uint32_t now_us);

// Build a PDU if at least NS2_DS5_MOTION40_MIN_TICKS have passed and the
// window holds enough distinct samples to fill every slot without repeating
// one. The prefix comes from the buffered carrier nearest
// NS2_DS5_MOTION40_PREFIX_LAG_TICKS after the window start -- NOT from the
// current orientation. Returns false when not yet due, the normal case.
bool ns2_ds5_motion40_build(ns2_ds5_motion40_t *state, uint32_t now_us,
                            uint8_t out[NS2_MOTION_PDU40_LENGTH]);

// Slice a 0x1E carrier into the catch-up prefix's modular windows. Exposed for
// testing against the reference implementation.
void ns2_ds5_motion40_prefix(const uint32_t carrier_raw[3], int32_t out[3],
                             bool high_rate);

#endif  // NS2_DS5_MOTION40_H
