#include "ns2_ds5_motion.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "ns2_motion_pdu.h"

#define NS2_DS5_MOTION_PERIOD_US       3800u
#define NS2_DS5_GYRO_FP_SHIFT          6
#define NS2_DS5_GYRO_JITTER_LIMIT      (6 << NS2_DS5_GYRO_FP_SHIFT)
#define NS2_DS5_GYRO_STILL_LIMIT       40
#define NS2_DS5_GYRO_WARMUP_LIMIT      40
#define NS2_DS5_GYRO_WARMUP_SAMPLES    32u
#define NS2_DS5_GYRO_COUNTS_PER_DPS    16.384f
#define NS2_DS5_DEG_TO_RAD             0.01745329251994329577f
#define NS2_DS5_INV_SQRT2              0.70710678118654752440f
#define NS2_DS5_SQRT2                  1.41421356237309504880f
#define NS2_DS5_SWITCH2_COMPONENT_MAX  0.5f
#define NS2_DS5_ACCEL_PDU_PER_COUNT    68963

// The original fixed-state experiment started from one genuine state-0 PDU.
// Keep it only for UART A/B comparison. The production candidate starts at
// identity, as Nintendo's Switch-1 DScale quaternion packer does.
static const float s_legacy_initial_quaternion[4] = {
    0.9207776592f, 0.0372730313f, -0.0250209301f, -0.3874960342f
};

static void reset_orientation(ns2_ds5_motion_state_t *state)
{
    if (state->carrier == NS2_DS5_CARRIER_LEGACY_STATE0) {
        memcpy(state->quaternion, s_legacy_initial_quaternion,
               sizeof(state->quaternion));
    } else {
        state->quaternion[0] = 0.0f;
        state->quaternion[1] = 0.0f;
        state->quaternion[2] = 0.0f;
        state->quaternion[3] = 1.0f;
    }
    state->representation_rejects = 0;
}

static int32_t clamp_i32(int64_t value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static void write_le32(uint8_t *out, int32_t value)
{
    const uint32_t u = (uint32_t)value;
    out[0] = (uint8_t)u;
    out[1] = (uint8_t)(u >> 8);
    out[2] = (uint8_t)(u >> 16);
    out[3] = (uint8_t)(u >> 24);
}

static uint32_t encode_u26(float value)
{
    const float scaled =
        ((value * NS2_DS5_SQRT2) + 1.0f) * 0.5f *
        (float)NS2_MOTION_ORIENTATION_MASK;
    return (uint32_t)(scaled + 0.5f);
}

static uint32_t encode_u24(float value)
{
    const float scaled =
        ((value * NS2_DS5_SQRT2) + 1.0f) * 0.5f * 16777215.0f;
    return (uint32_t)(scaled + 0.5f);
}

// Switch 2's three live-orientation slots have unequal numeric widths, but
// share the usual smallest-three +/-1/sqrt(2) physical range. The centered
// wire value is the physical quaternion component divided by sqrt(2):
//   G0 = (component / sqrt(2) + 0.5) * 2^26
//   G1 = (component / sqrt(2) + 0.5) * 2^25
//   G2 = (component / sqrt(2) + 0.5) * 2^24, plus state in bits 25:24.
// This is independently confirmed by the paired DS5/Pro2 pitch capture:
// decoding without sqrt(2) implies an impossible ~24.2 counts/(deg/s);
// restoring it yields ~16.9, within 3.3% of the calibrated 16.384 carrier.
static uint32_t encode_switch2_g0(float value)
{
    const float scaled =
        (value * NS2_DS5_INV_SQRT2 + 0.5f) * 67108864.0f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 67108863.0f) return 0x03FFFFFFu;
    return (uint32_t)(scaled + 0.5f);
}

static uint32_t encode_switch2_g1(float value)
{
    const float scaled =
        (value * NS2_DS5_INV_SQRT2 + 0.5f) * 33554432.0f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 33554431.0f) return 0x01FFFFFFu;
    return (uint32_t)(scaled + 0.5f);
}

static uint32_t encode_switch2_g2(float value)
{
    const float scaled =
        (value * NS2_DS5_INV_SQRT2 + 0.5f) * 16777216.0f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 16777215.0f) return 0x00FFFFFFu;
    return (uint32_t)(scaled + 0.5f);
}

static void quaternion_normalize_near_unit(float q[4])
{
    const float norm_sq =
        q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (norm_sq <= 0.0f) return;

    // Every Euler step starts from a unit quaternion and is capped at 16 ms,
    // so norm_sq remains close to 1. Two Newton-Raphson refinements from
    // y0=1 compute 1/sqrt(norm_sq) to much better precision than the Switch 2
    // wire quantization, without calling the SDK sqrtf routine. That routine
    // contends with the RAM-resident Opus encoder on the other core.
    float inverse_norm = 1.5f - 0.5f * norm_sq;
    inverse_norm *=
        1.5f - 0.5f * norm_sq * inverse_norm * inverse_norm;
    q[0] *= inverse_norm;
    q[1] *= inverse_norm;
    q[2] *= inverse_norm;
    q[3] *= inverse_norm;
}

static void quaternion_left_integrate(float q[4], const float omega[3],
                                      float dt_seconds)
{
    // First-order integration of q' = 0.5 * omega_quat * q. At the fixed
    // ~250 Hz update rate this avoids three trig calls in the audio-sensitive
    // build while normalization removes the small Euler length error.
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];
    const float ox = omega[0];
    const float oy = omega[1];
    const float oz = omega[2];
    const float half_dt = 0.5f * dt_seconds;

    q[0] += half_dt * (ox * w + oy * z - oz * y);
    q[1] += half_dt * (-ox * z + oy * w + oz * x);
    q[2] += half_dt * (ox * y - oy * x + oz * w);
    q[3] += half_dt * (-ox * x - oy * y - oz * z);

    quaternion_normalize_near_unit(q);
}

static void quaternion_right_integrate(float q[4], const float omega[3],
                                       float dt_seconds)
{
    // Body-frame rate integration: q' = 0.5 * q * omega_quat.
    // A controller IMU measures angular velocity in its own local frame, so
    // this preserves axis meaning even when the carrier starts at a genuine
    // non-identity quaternion.
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];
    const float ox = omega[0];
    const float oy = omega[1];
    const float oz = omega[2];
    const float half_dt = 0.5f * dt_seconds;

    q[0] += half_dt * (w * ox + y * oz - z * oy);
    q[1] += half_dt * (w * oy - x * oz + z * ox);
    q[2] += half_dt * (w * oz + x * oy - y * ox);
    q[3] += half_dt * (-x * ox - y * oy - z * oz);

    quaternion_normalize_near_unit(q);
}

void ns2_ds5_motion_reset(ns2_ds5_motion_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->carrier = NS2_DS5_CARRIER_SWITCH2_WXYZ;
    reset_orientation(state);
    // ns2_seam has already converted DualSense sensor axes into the verified
    // Pro Controller 2 physical frame (X=pitch, Y=roll, Z=yaw). Do not apply a
    // second permutation here; the former {-2,+1,+3} map was compensating for
    // an incorrect xyzw packet-layout hypothesis.
    state->gyro_map[0] = 1;
    state->gyro_map[1] = 2;
    state->gyro_map[2] = 3;
    state->body_frame = true;
}

void ns2_ds5_motion_set_body_frame(ns2_ds5_motion_state_t *state,
                                   bool body_frame)
{
    if (state) state->body_frame = body_frame;
}

bool ns2_ds5_motion_set_gyro_map(ns2_ds5_motion_state_t *state,
                                const int8_t map[3])
{
    if (!state || !map) return false;
    uint8_t used = 0;
    for (unsigned i = 0; i < 3; ++i) {
        int value = map[i];
        int axis = value < 0 ? -value : value;
        if (axis < 1 || axis > 3) return false;
        uint8_t bit = (uint8_t)(1u << (axis - 1));
        if (used & bit) return false;
        used |= bit;
    }
    memcpy(state->gyro_map, map, sizeof(state->gyro_map));
    return true;
}

void ns2_ds5_motion_set_carrier(ns2_ds5_motion_state_t *state,
                                ns2_ds5_motion_carrier_t carrier)
{
    if (!state) return;
    state->carrier = carrier;
    reset_orientation(state);
}

bool ns2_ds5_motion_update(ns2_ds5_motion_state_t *state,
                           const switch_pro_input_t *input,
                           uint32_t now_us)
{
    if (!state || !input || !input->has_motion) return false;

    if (!state->initialized) {
        state->initialized = true;
        state->has_sample = true;
        state->last_update_us = now_us;
        memcpy(state->accel, input->accel, sizeof(state->accel));
        for (unsigned axis = 0; axis < 3; ++axis) {
            state->gyro_lp[axis] =
                (int32_t)input->gyro[axis] << NS2_DS5_GYRO_FP_SHIFT;
            // A controller can deliver a large startup transient in its first
            // full motion report. Never declare that one sample to be zero:
            // doing so permanently subtracts the transient at rest and causes
            // nonstop rotation. The bounded stillness acquisition below owns
            // initial bias instead.
            state->gyro_bias[axis] = 0;
            state->gyro_prev[axis] = input->gyro[axis];
        }
        return true;
    }

    uint32_t elapsed_us = now_us - state->last_update_us;
    if (elapsed_us < NS2_DS5_MOTION_PERIOD_US) return false;
    state->last_update_us = now_us;
    if (elapsed_us > 16000u) elapsed_us = 16000u;
    if (elapsed_us < 500u) elapsed_us = 500u;

    bool steady = true;
    for (unsigned axis = 0; axis < 3; ++axis) {
        int32_t delta = (int32_t)input->gyro[axis] -
                        state->gyro_prev[axis];
        state->gyro_prev[axis] = input->gyro[axis];
        if (delta < 0) delta = -delta;
        state->gyro_jitter[axis] +=
            ((delta << NS2_DS5_GYRO_FP_SHIFT) -
             state->gyro_jitter[axis]) >> 3;
        if (state->gyro_jitter[axis] > NS2_DS5_GYRO_JITTER_LIMIT)
            steady = false;
    }

    int32_t corrected[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        state->gyro_lp[axis] +=
            (((int32_t)input->gyro[axis] << NS2_DS5_GYRO_FP_SHIFT) -
             state->gyro_lp[axis]) >> 2;
        const int32_t before_bias =
            (state->gyro_lp[axis] - state->gyro_bias[axis]) >>
            NS2_DS5_GYRO_FP_SHIFT;
        if (before_bias > NS2_DS5_GYRO_STILL_LIMIT ||
            before_bias < -NS2_DS5_GYRO_STILL_LIMIT)
            steady = false;
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (steady)
            state->gyro_bias[axis] +=
                (state->gyro_lp[axis] - state->gyro_bias[axis]) >> 8;
        corrected[axis] =
            (state->gyro_lp[axis] - state->gyro_bias[axis]) >>
            NS2_DS5_GYRO_FP_SHIFT;
    }

    if (!state->bias_ready) {
        bool warmup_still = true;
        for (unsigned axis = 0; axis < 3; ++axis) {
            int32_t raw = input->gyro[axis];
            if (raw < 0) raw = -raw;
            if (raw > NS2_DS5_GYRO_WARMUP_LIMIT ||
                state->gyro_jitter[axis] >
                    (NS2_DS5_GYRO_WARMUP_LIMIT <<
                     NS2_DS5_GYRO_FP_SHIFT)) {
                warmup_still = false;
            }
        }
        if (warmup_still) {
            state->bias_warmup_samples++;
            for (unsigned axis = 0; axis < 3; ++axis) {
                // Follow the settling low-pass value during acquisition. No
                // orientation is integrated until 32 consecutive plausible
                // still samples have made this reference trustworthy.
                state->gyro_bias[axis] = state->gyro_lp[axis];
                corrected[axis] = 0;
            }
            if (state->bias_warmup_samples >=
                NS2_DS5_GYRO_WARMUP_SAMPLES) {
                state->bias_ready = true;
            }
        } else {
            state->bias_warmup_samples = 0;
            memset(state->gyro_bias, 0, sizeof(state->gyro_bias));
            memset(corrected, 0, sizeof(corrected));
        }
    }

    // ns2_seam has already transformed DualSense axes to the normalized
    // Pro2 carrier frame: [-DS5-X, +DS5-Z, +DS5-Y]. The signed map is runtime
    // selectable over UART for diagnostics, but its production default is the
    // identity because the seam has already done the physical remount.
    const float scale =
        NS2_DS5_DEG_TO_RAD / NS2_DS5_GYRO_COUNTS_PER_DPS;
    float omega[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        const int selection = state->gyro_map[axis];
        const int source = (selection < 0 ? -selection : selection) - 1;
        const float sign = selection < 0 ? -1.0f : 1.0f;
        omega[axis] = sign * (float)corrected[source] * scale;
    }
    if (state->bias_ready) {
        if (state->body_frame)
            quaternion_right_integrate(state->quaternion, omega,
                                       (float)elapsed_us / 1000000.0f);
        else
            quaternion_left_integrate(state->quaternion, omega,
                                      (float)elapsed_us / 1000000.0f);
    }

    uint8_t count = (uint8_t)((elapsed_us + 625u) / 1250u);
    if (count < 1u) count = 1u;
    if (count > 15u) count = 15u;
    state->imu_tick = (uint16_t)((state->imu_tick + count) & 0x0FFFu);
    state->timing =
        (uint16_t)(((uint16_t)count << 12) | state->imu_tick);
    memcpy(state->accel, input->accel, sizeof(state->accel));
    state->updates++;
    return true;
}

bool ns2_ds5_motion_build(ns2_ds5_motion_state_t *state, uint8_t out[30])
{
    if (!state || !out || !state->has_sample) return false;

    memset(out, 0, NS2_MOTION_PDU30_LENGTH);
    out[0] = (uint8_t)state->timing;
    out[1] = (uint8_t)(state->timing >> 8);
    out[2] = 0x00;
    out[3] = 0x0C;  // genuine nominal temperature

    uint32_t orientation[3];
    if (state->carrier == NS2_DS5_CARRIER_LEGACY_STATE0) {
        // The legacy capture hid canonical X. Normalize q/-q at serialization
        // time instead of mutating the integrated orientation whenever X
        // crosses zero.
        const float sign = state->quaternion[0] < 0.0f ? -1.0f : 1.0f;
        const float qy = state->quaternion[1] * sign;
        const float qz = state->quaternion[2] * sign;
        const float qw = state->quaternion[3] * sign;
        if (qy < -NS2_DS5_INV_SQRT2 || qy > NS2_DS5_INV_SQRT2 ||
            qz < -NS2_DS5_INV_SQRT2 || qz > NS2_DS5_INV_SQRT2 ||
            qw < -NS2_DS5_INV_SQRT2 || qw > NS2_DS5_INV_SQRT2) {
            state->representation_rejects++;
            return false;
        }

        // Original fixed-state hypothesis retained for diagnostic comparison:
        // canonical [x,y,z,w] = [hidden,G2,G0,G1].
        orientation[0] = encode_u26(qz);
        orientation[1] = encode_u26(qw);
        orientation[2] = encode_u24(qy);
    } else if (state->carrier == NS2_DS5_CARRIER_SWITCH1_DSCALE) {
        // Switch 1 DScale diagnostic control:
        // state x/y/z/w; slots cyclically follow the omitted component.
        unsigned max_index = 0;
        for (unsigned i = 1; i < 4; ++i) {
            if (fabsf(state->quaternion[i]) >
                fabsf(state->quaternion[max_index]))
                max_index = i;
        }
        const float sign =
            state->quaternion[max_index] < 0.0f ? -1.0f : 1.0f;
        const float c0 =
            state->quaternion[(max_index + 1u) & 3u] * sign;
        const float c1 =
            state->quaternion[(max_index + 2u) & 3u] * sign;
        const float c2 =
            state->quaternion[(max_index + 3u) & 3u] * sign;
        if (c0 < -NS2_DS5_INV_SQRT2 || c0 > NS2_DS5_INV_SQRT2 ||
            c1 < -NS2_DS5_INV_SQRT2 || c1 > NS2_DS5_INV_SQRT2 ||
            c2 < -NS2_DS5_INV_SQRT2 || c2 > NS2_DS5_INV_SQRT2) {
            // The selected maximum guarantees this cannot happen for a unit
            // quaternion. Keep the counter as a hard invariant diagnostic.
            state->representation_rejects++;
            return false;
        }
        orientation[0] = encode_u26(c0);
        orientation[1] = encode_u26(c1);
        orientation[2] =
            ((uint32_t)max_index << 24) | encode_u24(c2);
    } else {
        // State 0 is fully established: it hides W and serializes X/Y/Z.
        // Keep q/-q canonicalized to positive W. Genuine adjacent captures
        // prove state 3 is a coordinate-basis rebase ([z,-x,-y]), not the
        // former cyclic "hide Z" hypothesis. States 1/2 and the controller's
        // exact transition policy remain incomplete, so production stays in
        // the proven state-0 chart and fails closed at its 90-degree boundary.
        const float sign =
            state->quaternion[3] < 0.0f ? -1.0f : 1.0f;
        const float c0 = state->quaternion[0] * sign;
        const float c1 = state->quaternion[1] * sign;
        const float c2 = state->quaternion[2] * sign;
        if (c0 < -NS2_DS5_INV_SQRT2 ||
            c0 > NS2_DS5_INV_SQRT2 ||
            c1 < -NS2_DS5_INV_SQRT2 ||
            c1 > NS2_DS5_INV_SQRT2 ||
            c2 < -NS2_DS5_INV_SQRT2 ||
            c2 > NS2_DS5_INV_SQRT2) {
            state->representation_rejects++;
            return false;
        }
        orientation[0] = encode_switch2_g0(c0);
        orientation[1] = encode_switch2_g1(c1);
        orientation[2] = encode_switch2_g2(c2);
    }
    if (!ns2_motion_pdu30_set_orientation(out, orientation))
        return false;

    for (unsigned axis = 0; axis < 3; ++axis) {
        const int32_t accel = clamp_i32(
            (int64_t)state->accel[axis] * NS2_DS5_ACCEL_PDU_PER_COUNT);
        write_le32(&out[16 + axis * 4], accel);
    }
    // Retain the genuine stationary template's constant tail. Byte 28 carries
    // a slowly changing auxiliary value on some firmware streams, but is not
    // part of orientation or acceleration; zero is a genuine observed value.
    out[28] = 0x00;
    out[29] = 0x02;
    return true;
}
