#include "ns2_ds5_motion40.h"

#include <string.h>

// DualSense acceleration is 8192 counts/g against the Pro Controller 2's
// 4096 counts/g, so ordinary counts are the raw value halved. Both devices
// report gyro near 16.4 counts/dps (the DualSense at 16.384), so gyro passes
// through unscaled.
#define DS5_ACCEL_SHIFT 1

// Catch-up wire scaling, from ns2_motion_reference.WIRE_TO_COUNTS. Slot width
// alone does not determine these: the middle acceleration slot is
// half-resolution and both gyros sit at four times the ordinary scale.
//   accel slot 0 (14-bit): wire = counts
//   accel slot 1 (13-bit): wire = counts / 2
//   accel slot 2 (14-bit): wire = counts
//   gyro  slots  (16-bit): wire = counts * 4
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

void ns2_ds5_motion40_sample(ns2_ds5_motion40_t *state,
                             const int16_t accel[3], const int16_t gyro[3])
{
    if (!state || !accel || !gyro) return;

    if (state->accel_count < NS2_DS5_MOTION40_ACCEL_SLOTS) {
        const unsigned slot = state->accel_count;
        for (unsigned axis = 0; axis < 3u; ++axis) {
            const int32_t counts = (int32_t)accel[axis] >> DS5_ACCEL_SHIFT;
            // Slot 1 is half-resolution on the wire.
            const int32_t wire = (slot == 1u) ? (counts / 2) : counts;
            const int32_t limited = clamp32(wire, k_accel_limit[slot]);
            if (limited != wire) state->saturated_accel++;
            state->accel[slot][axis] = limited;
        }
        state->accel_count++;
    }

    if (state->gyro_count < NS2_DS5_MOTION40_GYRO_SLOTS) {
        const unsigned slot = state->gyro_count;
        for (unsigned axis = 0; axis < 3u; ++axis) {
            // Gyro sits at four times the ordinary scale on the wire. The
            // 16-bit slot therefore caps near +/-499 dps, well inside a
            // DualSense's range, so clamping is load-bearing rather than
            // defensive.
            const int32_t wire = (int32_t)gyro[axis] * 4;
            const int32_t limited = clamp32(wire, GYRO_WIRE_LIMIT);
            if (limited != wire) state->saturated_gyro++;
            state->gyro[slot][axis] = limited;
        }
        state->gyro_count++;
    }
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
        return false;
    }

    const uint32_t elapsed_us = now_us - state->last_emit_us;  // wraps cleanly
    const uint32_t elapsed_ticks = elapsed_us / NS2_DS5_MOTION40_TICK_US;
    if (elapsed_ticks < NS2_DS5_MOTION40_MIN_TICKS) return false;

    if (state->accel_count < NS2_DS5_MOTION40_ACCEL_SLOTS ||
        state->gyro_count < NS2_DS5_MOTION40_GYRO_SLOTS) {
        // The interval elapsed but the source did not supply enough samples.
        // Emitting anyway would mean inventing data; wait instead. A steady
        // rise here means the emit interval is too short for the source rate.
        state->skipped_no_samples++;
        return false;
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

    state->last_emit_us = now_us;
    state->accel_count = 0;
    state->gyro_count = 0;
    state->emitted++;
    return true;
}
