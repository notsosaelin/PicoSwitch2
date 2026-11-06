#include "ns2_ds5_motion40.h"

#include <string.h>

// The motion seam has ALREADY normalized every translated controller to the
// Pro Controller 2 frame and acceleration scale before this module sees it:
// 4096 counts/g. In particular, ns2_motion_seam_apply() halves the native
// DualSense 8192-count/g samples. Do not halve them again here. Both devices
// report gyro near 16.4 counts/dps (the DualSense at 16.384), so gyro passes
// through unscaled as well.
//
// Catch-up wire scaling, from ns2_motion_reference.WIRE_TO_COUNTS. Slot width
// alone does not determine these: the middle acceleration slot is
// half-resolution and both gyros sit at four times the ordinary scale.
//   accel slot 0 (14-bit): wire = counts
//   accel slot 1 (13-bit): wire = counts / 2
//   accel slot 2 (14-bit): wire = counts
//   gyro  slots  (16-bit): wire = counts * 4
//
// Normal and catch-up accel/gyro slots converge on +/-8192 ordinary counts,
// or +/-2 g and +/-499.5 dps. High-rate is field-specific instead:
// High-rate acceleration has eight fractional bits (wire = counts * 256).
// High-rate gyro has SEVEN (wire = counts * 128). Treating both alike was the
// factor-of-two defect exposed by integrating the existing 0x28 corpus against
// its own 0x1E carrier. The controller's sensor and normal-layout gyro remain
// the documented/measured 16.4 counts/dps; only this wire fixed-point scale is
// different. A signed22 gyro therefore reaches about +/-999 dps, while accel
// retains the +/-2 g range of the other layouts.
#define HIGH_RATE_ACCEL_FRACTIONAL_SCALE 256
#define HIGH_RATE_GYRO_FRACTIONAL_SCALE 128
#define VECTOR_WIRE_MAX 2097151

// Convert one post-seam source count into the same calibrated acceleration
// value the validated 0x1E path publishes. The 0x1E lane is Q16.16 at
// `raw * 68963`; high-rate 0x28 is Q8, so divide that exact value by 256.
// This is a cross-representation contract, not a new physical sensor scale.
static int32_t coherent_accel_wire(int16_t raw)
{
    const int64_t product =
        (int64_t)raw * NS2_MOTION30_ACCEL_Q16_PER_COUNT;
    const int64_t half = 1ll << 7;
    return (int32_t)((product >= 0 ? product + half : product - half) /
                     (1ll << 8));
}

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

static int32_t sign_extend(int32_t value, unsigned bits)
{
    const int32_t field = (int32_t)1 << bits;
    value &= field - 1;
    return (value >= (field >> 1)) ? value - field : value;
}

void ns2_ds5_motion40_prefix(const uint32_t carrier_raw[3], int32_t out[3],
                             bool high_rate)
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
    // High-rate lanes are two bits wider and take NO precision shift; the
    // narrower catch-up and normal lanes shift by two. Getting this wrong
    // produces a well-formed packet carrying a quartered orientation.
    static const unsigned narrow[3] = {22u, 21u, 23u};
    static const unsigned wide[3] = {24u, 23u, 25u};
    const unsigned *widths = high_rate ? wide : narrow;
    const unsigned shift = high_rate ? 0u : 2u;
    for (unsigned lane = 0; lane < 3u; ++lane)
        out[lane] = sign_extend(floor_shift(centred[lane], shift), widths[lane]);
}

void ns2_ds5_motion40_reset(ns2_ds5_motion40_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

void ns2_ds5_motion40_sample(ns2_ds5_motion40_t *state, const int16_t accel[3],
                             const int16_t gyro[3],
                             const uint32_t carrier_raw[3], uint16_t timing,
                             uint32_t now_us)
{
    if (!state || !accel || !gyro || !carrier_raw) return;
    ns2_ds5_motion40_entry_t *entry = &state->ring[state->head];
    memcpy(entry->accel, accel, sizeof(entry->accel));
    memcpy(entry->gyro, gyro, sizeof(entry->gyro));
    memcpy(entry->carrier, carrier_raw, sizeof(entry->carrier));
    entry->tick = timing & 0x0FFFu;
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

// The buffered entry whose timestamp is closest to `target_us`, searched over
// the WHOLE ring rather than the emit window: the orientation the prefix needs
// sits near the window start, which the previous packet's samples bracket.
static const ns2_ds5_motion40_entry_t *
entry_nearest(const ns2_ds5_motion40_t *state, uint32_t target_us)
{
    const ns2_ds5_motion40_entry_t *best = NULL;
    uint32_t best_distance = 0xFFFFFFFFu;
    for (unsigned i = 0; i < state->filled; ++i) {
        const uint32_t us = state->ring[i].us;
        const uint32_t distance =
            (us > target_us) ? (us - target_us) : (target_us - us);
        if (distance < best_distance) {
            best_distance = distance;
            best = &state->ring[i];
        }
    }
    return best;
}

static const ns2_ds5_motion40_entry_t *latest_entry(
    const ns2_ds5_motion40_t *state)
{
    if (!state || state->filled == 0u) return NULL;
    const uint8_t index =
        (uint8_t)((state->head + NS2_DS5_MOTION40_RING - 1u) %
                  NS2_DS5_MOTION40_RING);
    return &state->ring[index];
}

static void anchor_to_entry(ns2_ds5_motion40_t *state,
                            const ns2_ds5_motion40_entry_t *entry)
{
    state->last_sample_us = entry->us;
    state->last_pdu_tick = entry->tick;
    state->anchored = true;
}

bool ns2_ds5_motion40_build(ns2_ds5_motion40_t *state, uint32_t now_us,
                            uint8_t out[NS2_MOTION_PDU40_LENGTH])
{
    if (!state || !out) return false;

    const ns2_ds5_motion40_entry_t *latest = latest_entry(state);
    if (!latest) {
        // Host tests and the legacy 0x28-only diagnostic prime before the
        // first source sample. Production interleaving seeds from a real 0x1E
        // through select(), so this synthetic anchor never enters that path.
        if (!state->anchored) {
            state->last_sample_us = now_us;
            state->last_pdu_tick =
                (uint16_t)((now_us / NS2_DS5_MOTION40_TICK_US) & 0x0FFFu);
            state->anchored = true;
        } else if ((now_us - state->last_sample_us) /
                       NS2_DS5_MOTION40_TICK_US >=
                   NS2_DS5_MOTION40_MIN_TICKS) {
            state->skipped_no_samples++;
        }
        return false;
    }
    if (latest->us > now_us) return false;

    if (!state->anchored) {
        // A 0x28 cannot define an interleaved epoch on its own: elapsed is the
        // shared tick delta from the immediately preceding PDU of either
        // length. Seed from the latest carrier/sample and wait for a window.
        anchor_to_entry(state, latest);
        return false;
    }

    const uint16_t latest_elapsed =
        (uint16_t)((latest->tick - state->last_pdu_tick) & 0x0FFFu);
    if (latest_elapsed < NS2_DS5_MOTION40_MIN_TICKS) return false;

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
    const ns2_ds5_motion40_entry_t *newest =
        &state->ring[sorted[available - 1u]];
    const uint32_t elapsed_ticks =
        (newest->tick - state->last_pdu_tick) & 0x0FFFu;
    // Slot placement spans from the previous packet's newest sample to this
    // one's, which is the interval the packet actually represents.
    const uint32_t span_us = newest_us - state->last_sample_us;
    if (elapsed_ticks < NS2_DS5_MOTION40_MIN_TICKS) {
        // now_us cleared the minimum but the freshest sample has not. Ordinary
        // jitter, not starvation -- the next poll will have a newer sample.
        return false;
    }
    if (elapsed_ticks > NS2_DS5_MOTION40_MAX_TICKS) {
        // Elapsed selects the layout, so a window this long cannot be sent as
        // high-rate: the console would read the fields with the normal or
        // catch-up map. Clamping the count would lie about the span. Genuine
        // hardware switches layout here instead -- that is what the layouts
        // are for -- so re-anchor and drop this window rather than emit a
        // packet that decodes wrong. A rising count means the source stalled.
        state->skipped_overlong++;
        // The legacy 0x28-only diagnostic has no carrier scheduler to recover
        // it. Re-anchor there; coherent interleaving never calls build() with
        // an overlong delta and handles its own carrier fallback.
        anchor_to_entry(state, newest);
        return false;
    }

    // Anchor the ends: slot 0 takes the oldest sample in the window and the
    // last slot the newest. Confirmed on 973 catch-up packets, where the seam
    // between packets is the shortest gap in the stream; high-rate's own
    // ordering cannot be measured independently because the interleaved tick
    // relation is unresolved, so it is inherited rather than proven.
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

    // Input is the post-seam Pro 2 scale: 4096 counts/g. The high-rate wire
    // carries eight fractional bits, but it must also match the calibrated
    // acceleration carried by the validated 0x1E path. Publishing bare
    // `raw * 256` beside 0x1E's `raw * 68963` made the same sample jump by
    // 5.23% whenever the representation changed. HALF deliberately recreates
    // the older, larger 0.5 g double-normalization defect for UART A/B.
    for (unsigned slot = 0; slot < NS2_DS5_MOTION40_ACCEL_SLOTS; ++slot) {
        const int16_t *raw = state->ring[sorted[accel_pick[slot]]].accel;
        for (unsigned axis = 0; axis < 3u; ++axis) {
            int32_t wire = coherent_accel_wire(raw[axis]);
            if (state->accel_mode == NS2_DS5_MOTION40_ACCEL_HALF)
                wire = (int32_t)raw[axis] *
                       (HIGH_RATE_ACCEL_FRACTIONAL_SCALE / 2);
            else if (state->accel_mode == NS2_DS5_MOTION40_ACCEL_ZERO)
                wire = 0;
            const int32_t limited = clamp32(wire, VECTOR_WIRE_MAX);
            if (limited != wire) state->saturated_accel++;
            state->accel[slot][axis] = limited;
        }
    }
    // High-rate has one gyro vector for the whole elapsed window. Publishing
    // one midpoint sensor reading made it incoherent with the 0x1E carrier:
    // that carrier integrated EVERY source sample, while the 0x28 discarded
    // all but one. At rest, selecting one DualSense noise spike made the
    // console see motion that was absent from the packet's own orientation
    // trajectory; actual movement partly hid the mismatch.
    //
    // Reproduce the carrier's integration area instead. ns2_ds5_motion_update
    // applies each newly arrived gyro sample over the elapsed interval since
    // its predecessor. The shared 800 Hz ticks encode the same intervals, so
    // weight every sample in (previous PDU, current PDU] by its tick delta.
    // This also handles delayed reports: one sample spanning multiple ticks
    // contributes proportionally instead of being treated as one ordinary
    // sample. The weights sum to the packet's encoded elapsed count.
    int64_t gyro_area[3] = {0, 0, 0};
    uint16_t previous_tick = state->last_pdu_tick;
    uint32_t integrated_ticks = 0u;
    for (unsigned i = 0; i < available; ++i) {
        const ns2_ds5_motion40_entry_t *sample = &state->ring[sorted[i]];
        const uint16_t sample_ticks =
            (uint16_t)((sample->tick - previous_tick) & 0x0FFFu);
        if (sample_ticks == 0u) continue;
        for (unsigned axis = 0; axis < 3u; ++axis)
            gyro_area[axis] += (int64_t)sample->gyro[axis] * sample_ticks;
        integrated_ticks += sample_ticks;
        previous_tick = sample->tick;
    }
    if (integrated_ticks != elapsed_ticks || integrated_ticks == 0u) {
        // A missing/duplicate source interval would make the gyro claim a
        // different area from the timing prefix. Fail closed to a carrier;
        // select() records that fallback and re-anchors the next window.
        state->skipped_no_samples++;
        return false;
    }
    for (unsigned axis = 0; axis < 3u; ++axis) {
        // Round the signed mean symmetrically before applying the wire's seven
        // fractional bits. C integer division truncates toward zero.
        const int64_t half = (int64_t)integrated_ticks / 2;
        const int32_t mean = (int32_t)(
            (gyro_area[axis] >= 0 ? gyro_area[axis] + half
                                  : gyro_area[axis] - half) /
            (int64_t)integrated_ticks);
        const int32_t wire = mean * HIGH_RATE_GYRO_FRACTIONAL_SCALE;
        const int32_t limited = clamp32(wire, VECTOR_WIRE_MAX);
        if (limited != wire) state->saturated_gyro++;
        state->gyro[0][axis] = limited;
    }

    ns2_motion40_high_rate_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.elapsed_ticks = (uint16_t)elapsed_ticks;
    fields.tick = newest->tick;
    fields.packing_mode = 3u;
    fields.tail_value = NS2_DS5_MOTION40_TEMPERATURE_TAIL;
    fields.status = NS2_MOTION40_STATUS_HIGH_RATE;

    // The prefix describes a PAST instant -- a fixed lag after the window
    // START, not the packet's own tick. Using the current orientation while
    // also sending the window's IMU samples double-counts the window's
    // rotation: the console anchors on the prefix and integrates forward from
    // it. Measured at 13x the achievable error floor before this fix.
    const uint32_t prefix_us =
        state->last_sample_us +
        NS2_DS5_MOTION40_PREFIX_LAG_TICKS * NS2_DS5_MOTION40_TICK_US;
    const ns2_ds5_motion40_entry_t *anchor = entry_nearest(state, prefix_us);
    if (!anchor) return false;
    ns2_ds5_motion40_prefix(anchor->carrier, fields.carrier, true);
    memcpy(fields.accel, state->accel, sizeof(fields.accel));
    memcpy(fields.gyro, state->gyro, sizeof(fields.gyro));

    if (!ns2_motion_pdu40_build_high_rate(out, &fields)) return false;

    // Both PDU lengths advance the same controller clock. The next packet's
    // elapsed field is measured from this exact tick/sample boundary.
    anchor_to_entry(state, newest);
    state->emitted++;
    return true;
}

static void publish_carrier(ns2_ds5_motion40_t *state,
                            const uint8_t carrier_pdu[NS2_MOTION_PDU30_LENGTH],
                            const ns2_ds5_motion40_entry_t *latest,
                            uint16_t elapsed)
{
    memcpy(state->output, carrier_pdu, NS2_MOTION_PDU30_LENGTH);
    if (state->anchored) {
        // The shipping 0x1E is rebuilt for every physical DualSense sample,
        // where its own elapsed nibble is normally one. In mixed mode this PDU
        // is deliberately held to the genuine notification cadence, so its
        // elapsed nibble must describe the shared PDU boundary instead.
        if (elapsed < 1u) elapsed = 1u;
        if (elapsed > 15u) elapsed = 15u;
        const uint16_t timing =
            (uint16_t)((elapsed << 12) | (latest->tick & 0x0FFFu));
        state->output[0] = (uint8_t)timing;
        state->output[1] = (uint8_t)(timing >> 8);
    }
    state->output_length = NS2_MOTION_PDU30_LENGTH;
    anchor_to_entry(state, latest);
    state->carrier_frames++;
    if (state->carriers_since_40 < UINT8_MAX) state->carriers_since_40++;
}

bool ns2_ds5_motion40_select(ns2_ds5_motion40_t *state,
                             const uint8_t carrier_pdu[NS2_MOTION_PDU30_LENGTH],
                             uint8_t out[NS2_MOTION_PDU40_LENGTH],
                             uint8_t *out_length)
{
    if (!state || !carrier_pdu || !out || !out_length) return false;
    const ns2_ds5_motion40_entry_t *latest = latest_entry(state);
    if (!latest) {
        *out_length = 0u;
        return false;
    }

    bool fresh = false;
    if (!state->anchored || state->output_length == 0u) {
        publish_carrier(state, carrier_pdu, latest, 1u);
        fresh = true;
    } else {
        const uint16_t elapsed =
            (uint16_t)((latest->tick - state->last_pdu_tick) & 0x0FFFu);
        const bool want_40 = state->carriers_since_40 >= 3u;
        const uint16_t due = want_40 ? 7u : 6u;

        if (elapsed >= due) {
            if (want_40 && elapsed <= NS2_DS5_MOTION40_MAX_TICKS) {
                uint8_t pdu40[NS2_MOTION_PDU40_LENGTH];
                if (ns2_ds5_motion40_build(state, latest->us, pdu40)) {
                    memcpy(state->output, pdu40, sizeof(pdu40));
                    state->output_length = sizeof(pdu40);
                    state->carriers_since_40 = 0u;
                    fresh = true;
                } else {
                    // A coherent carrier is safer than inventing or repeating
                    // missing samples. It also re-anchors the next window.
                    state->fallback_carriers++;
                    publish_carrier(state, carrier_pdu, latest, elapsed);
                    fresh = true;
                }
            } else {
                publish_carrier(state, carrier_pdu, latest, elapsed);
                fresh = true;
            }
        } else {
            state->held_polls++;
        }
    }

    memcpy(out, state->output, state->output_length);
    *out_length = state->output_length;
    return fresh;
}
