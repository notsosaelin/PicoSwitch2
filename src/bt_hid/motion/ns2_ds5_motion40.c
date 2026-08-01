#include "ns2_ds5_motion40.h"

#include <string.h>

// DualSense acceleration is 8192 counts/g against the Pro Controller 2's
// 4096 counts/g, so ordinary counts are the raw value halved (half_round
// below). Both devices report gyro near 16.4 counts/dps (the DualSense at
// 16.384), so gyro passes through unscaled.
//
// Catch-up wire scaling, from ns2_motion_reference.WIRE_TO_COUNTS. Slot width
// alone does not determine these: the middle acceleration slot is
// half-resolution and both gyros sit at four times the ordinary scale.
//   accel slot 0 (14-bit): wire = counts
//   accel slot 1 (13-bit): wire = counts / 2
//   accel slot 2 (14-bit): wire = counts
//   gyro  slots  (16-bit): wire = counts * 4
//
// Every layout and slot converges on the same physical range: 22 bits at
// 1/256, 14 bits at 1, 13 bits at 2 and 16 bits at 1/4 all reach +/-8192
// ordinary counts, which at 4096 counts/g and 16.4 counts/dps is +/-2 g and
// +/-499.5 dps -- stock ICM full-scale settings. The wire limit is therefore
// the sensor's own limit, so clamping to it is what genuine hardware does.
static const int32_t k_accel_limit[NS2_DS5_MOTION40_ACCEL_SLOTS] = {
    8191, 4095, 8191
};
#define GYRO_WIRE_LIMIT 32767

static int32_t clamp32(int32_t value, int32_t limit)
{
    if (value > limit) return limit;
    if (value < -limit - 1) return -limit - 1;
    return value;
}

// Arithmetic right shift of a negative value is implementation-defined before
// C++20 and merely "usual" in C. The prefix slice needs a floor, matching
// Python's >>, so do it explicitly rather than trusting the compiler.
static int32_t floor_shift(int32_t value, unsigned bits)
{
    const int32_t divisor = (int32_t)1 << bits;
    int32_t quotient = value / divisor;
    if (value % divisor != 0 && value < 0) quotient -= 1;
    return quotient;
}

// Halve, rounding to nearest rather than toward -inf. A plain floor would put
// a -0.5 count DC offset on every axis; on x and y, which rest near zero, that
// is a systematic bias rather than a rounding detail.
static int32_t half_round(int32_t value)
{
    return floor_shift(value + 1, 1);
}

static int32_t sign_extend(int32_t value, unsigned bits)
{
    const int32_t field = (int32_t)1 << bits;
    value &= field - 1;
    return (value >= (field >> 1)) ? value - field : value;
}

void ns2_ds5_motion40_prefix(const uint32_t carrier_raw[3], int32_t out[3])
{
    if (!carrier_raw || !out) return;
    // Lane 2 is centred half a window away from lanes 0 and 1 and carries one
    // extra bit of resolution. Both facts come from integer residuals measured
    // against genuine captures, not from the field widths.
    const int32_t centred[3] = {
        (int32_t)carrier_raw[0] - ((int32_t)1 << 25),
        (int32_t)carrier_raw[1] - ((int32_t)1 << 24),
        2 * ((int32_t)carrier_raw[2] - ((int32_t)1 << 23)) + ((int32_t)1 << 24),
    };
    static const unsigned widths[3] = {22u, 21u, 23u};
    for (unsigned lane = 0; lane < 3u; ++lane)
        out[lane] = sign_extend(floor_shift(centred[lane], 2u), widths[lane]);
}

void ns2_ds5_motion40_reset(ns2_ds5_motion40_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

void ns2_ds5_motion40_sample(ns2_ds5_motion40_t *state, const int16_t accel[3],
                             const int16_t gyro[3], uint32_t now_us)
{
    if (!state || !accel || !gyro) return;
    ns2_ds5_motion40_entry_t *entry = &state->ring[state->head];
    memcpy(entry->accel, accel, sizeof(entry->accel));
    memcpy(entry->gyro, gyro, sizeof(entry->gyro));
    entry->us = now_us;
    state->head = (uint8_t)((state->head + 1u) % NS2_DS5_MOTION40_RING);
    if (state->filled < NS2_DS5_MOTION40_RING) state->filled++;
}

// Collect the samples that fall inside (start, start + span], oldest first.
// Returns how many were found, writing at most NS2_DS5_MOTION40_RING indices.
static unsigned window_samples(const ns2_ds5_motion40_t *state, uint32_t start,
                               uint32_t span, uint8_t out[NS2_DS5_MOTION40_RING])
{
    unsigned count = 0;
    for (unsigned i = 0; i < state->filled; ++i) {
        // Unsigned subtraction wraps cleanly, so a sample older than the window
        // start becomes a huge offset and is rejected by the same comparison.
        const uint32_t offset = state->ring[i].us - start;
        // Half-open at the start: a sample sitting exactly on the boundary was
        // the previous packet's newest and has already been sent.
        if (offset == 0u || offset > span) continue;
        // Insertion sort by timestamp; the ring is at most 16 entries and is
        // only in chronological order until head wraps.
        unsigned at = count;
        while (at > 0 && (state->ring[out[at - 1]].us - start) > offset) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = (uint8_t)i;
        count++;
    }
    return count;
}

// Index within `sorted` whose timestamp is closest to `num/den` of the way
// through the window, searched only in [lo, hi] so callers can force a
// strictly increasing, non-repeating selection.
static unsigned nearest_at(const ns2_ds5_motion40_t *state,
                           const uint8_t *sorted, unsigned lo, unsigned hi,
                           uint32_t start, uint32_t span, unsigned num,
                           unsigned den)
{
    const uint32_t target = (uint32_t)(((uint64_t)span * num) / den);
    unsigned best = lo;
    uint32_t best_distance = 0xFFFFFFFFu;
    for (unsigned i = lo; i <= hi; ++i) {
        const uint32_t offset = state->ring[sorted[i]].us - start;
        const uint32_t distance =
            (offset > target) ? (offset - target) : (target - offset);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

bool ns2_ds5_motion40_build(ns2_ds5_motion40_t *state,
                            const uint32_t carrier_raw[3], uint32_t now_us,
                            uint8_t out[NS2_MOTION_PDU40_LENGTH])
{
    if (!state || !carrier_raw || !out) return false;

    if (!state->primed) {
        // First call establishes the epoch; nothing is due yet.
        state->primed = true;
        state->last_emit_us = now_us;
        state->last_sample_us = now_us;
        return false;
    }

    // Cheap gate first: the newest sample can never be later than now_us, so
    // if now_us has not reached the minimum interval, nothing else can have.
    if ((now_us - state->last_emit_us) / NS2_DS5_MOTION40_TICK_US <
        NS2_DS5_MOTION40_MIN_TICKS)
        return false;

    uint8_t sorted[NS2_DS5_MOTION40_RING];
    const unsigned available = window_samples(
        state, state->last_sample_us, now_us - state->last_sample_us, sorted);
    if (available < NS2_DS5_MOTION40_ACCEL_SLOTS) {
        // The interval elapsed but the source did not supply enough distinct
        // samples to fill the slots. Emitting anyway would mean repeating one
        // sample at two different points in the window, which misstates the
        // timeline; wait instead. A steady rise here means the emit interval
        // is too short for the source rate.
        state->skipped_no_samples++;
        return false;
    }

    // The window ends at the newest sample being sent, NOT at now_us. Taking
    // now_us would leave the gap between that sample and this poll unaccounted
    // for: the elapsed count would claim a span wider than the samples cover,
    // and the error would accumulate against the console's own clock. Ending
    // on the newest sample makes the reported elapsed exactly the time from
    // the previous packet's last sample to this one's, which is the relation
    // the genuine 0x28-only captures show.
    const uint32_t newest_us = state->ring[sorted[available - 1u]].us;
    const uint32_t elapsed_ticks =
        (newest_us - state->last_emit_us) / NS2_DS5_MOTION40_TICK_US;
    // Slot placement spans from the previous packet's newest sample to this
    // one's, which is the interval the packet actually represents.
    const uint32_t span_us = newest_us - state->last_sample_us;
    if (elapsed_ticks < NS2_DS5_MOTION40_MIN_TICKS) {
        // now_us cleared the minimum but the freshest sample has not. Ordinary
        // jitter, not starvation -- the next poll will have a newer sample.
        return false;
    }

    // Anchor the ends and space the interior evenly. Slot 0 takes the oldest
    // sample in the window and the last slot the newest, which is the ordering
    // the genuine corpus confirms; the interior slot takes whichever sample
    // sits nearest the midpoint, constrained to stay strictly between them.
    unsigned accel_pick[NS2_DS5_MOTION40_ACCEL_SLOTS];
    accel_pick[0] = 0;
    accel_pick[NS2_DS5_MOTION40_ACCEL_SLOTS - 1u] = available - 1u;
    for (unsigned slot = 1; slot + 1u < NS2_DS5_MOTION40_ACCEL_SLOTS; ++slot) {
        accel_pick[slot] =
            nearest_at(state, sorted, accel_pick[slot - 1u] + 1u,
                       available - (NS2_DS5_MOTION40_ACCEL_SLOTS - slot),
                       state->last_sample_us, span_us, slot,
                       NS2_DS5_MOTION40_ACCEL_SLOTS - 1u);
    }

    // The two gyro slots are placed at the quarter points instead. Their
    // ordering is NOT resolved by the corpus -- a stationary gyro is pure
    // noise, and the paired sign test came out weak and with the opposite sign
    // to acceleration (z = -4.0), which is what quarter-point spacing would
    // produce, since it makes the within-packet and seam gaps equal. Quarter
    // points also give the unbiased trapezoidal estimate of the window's
    // integral, and the console integrates gyro, so the mean matters more than
    // the freshness of either endpoint.
    unsigned gyro_pick[NS2_DS5_MOTION40_GYRO_SLOTS];
    unsigned lo = 0;
    for (unsigned slot = 0; slot < NS2_DS5_MOTION40_GYRO_SLOTS; ++slot) {
        gyro_pick[slot] = nearest_at(
            state, sorted, lo, available - (NS2_DS5_MOTION40_GYRO_SLOTS - slot),
            state->last_sample_us, span_us, 2u * slot + 1u,
            2u * NS2_DS5_MOTION40_GYRO_SLOTS);
        lo = gyro_pick[slot] + 1u;
    }

    for (unsigned slot = 0; slot < NS2_DS5_MOTION40_ACCEL_SLOTS; ++slot) {
        const int16_t *raw = state->ring[sorted[accel_pick[slot]]].accel;
        for (unsigned axis = 0; axis < 3u; ++axis) {
            const int32_t counts = half_round((int32_t)raw[axis]);
            // Slot 1 is half-resolution on the wire.
            const int32_t wire = (slot == 1u) ? half_round(counts) : counts;
            const int32_t limited = clamp32(wire, k_accel_limit[slot]);
            if (limited != wire) state->saturated_accel++;
            state->accel[slot][axis] = limited;
        }
    }
    for (unsigned slot = 0; slot < NS2_DS5_MOTION40_GYRO_SLOTS; ++slot) {
        const int16_t *raw = state->ring[sorted[gyro_pick[slot]]].gyro;
        for (unsigned axis = 0; axis < 3u; ++axis) {
            // Gyro sits at four times the ordinary scale on the wire. The
            // 16-bit slot therefore caps near +/-499 dps, well inside a
            // DualSense's range, so clamping is load-bearing rather than
            // defensive.
            const int32_t wire = (int32_t)raw[axis] * 4;
            const int32_t limited = clamp32(wire, GYRO_WIRE_LIMIT);
            if (limited != wire) state->saturated_gyro++;
            state->gyro[slot][axis] = limited;
        }
    }

    ns2_motion40_catchup_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.elapsed_ticks =
        (uint16_t)((elapsed_ticks > 0x0FFFu) ? 0x0FFFu : elapsed_ticks);
    state->tick = (uint16_t)((state->tick + fields.elapsed_ticks) & 0x0FFFu);
    fields.tick = state->tick;
    fields.packing_mode = 3u;
    fields.tail_bit = 0u;  // zero in all 981 genuine catch-up packets
    fields.status = NS2_MOTION40_STATUS_CATCHUP;
    ns2_ds5_motion40_prefix(carrier_raw, fields.carrier);
    memcpy(fields.accel, state->accel, sizeof(fields.accel));
    memcpy(fields.gyro, state->gyro, sizeof(fields.gyro));

    if (!ns2_motion_pdu40_build_catchup(out, &fields)) return false;

    // Advance by exactly the elapsed we reported, so the tick the console
    // reconstructs and our own window boundary can never drift apart. This is
    // the timestamp of the newest sample sent, truncated to a whole tick.
    state->last_emit_us += elapsed_ticks * NS2_DS5_MOTION40_TICK_US;
    state->last_sample_us = newest_us;
    state->emitted++;
    return true;
}
