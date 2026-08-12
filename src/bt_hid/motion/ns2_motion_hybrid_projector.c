#include "ns2_motion_hybrid_projector.h"

#include <limits.h>
#include <string.h>

#include "ns2_ds5_motion40.h"
#include "ns2_motion_hybrid.h"
#include "switch_pro.h"

#define HIGH_RATE_ACCEL_LIMIT 2097151
#define HIGH_RATE_GYRO_LIMIT  2097151

static int32_t read_le32(const uint8_t *data)
{
    const uint32_t value =
        (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return (int32_t)value;
}

static int32_t clamp_wire(int64_t value, int32_t limit, uint8_t *saturated)
{
    if (value > limit) {
        if (saturated) *saturated = 1u;
        return limit;
    }
    if (value < -(int64_t)limit - 1) {
        if (saturated) *saturated = 1u;
        return -limit - 1;
    }
    return (int32_t)value;
}

static int32_t round_div_signed(int64_t value, int64_t divisor)
{
    const int64_t half = divisor / 2;
    return (int32_t)((value >= 0 ? value + half : value - half) / divisor);
}

static uint16_t elapsed40(const uint8_t pdu[NS2_MOTION_PDU40_LENGTH])
{
    return (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
}

static void clear_samples(ns2_motion_hybrid_projector_t *state)
{
    state->head = 0u;
    state->filled = 0u;
    state->last_project_sequence = 0u;
}

void ns2_motion_hybrid_projector_reset(
    ns2_motion_hybrid_projector_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    ns2_ds5_motion_reset(&state->translator);
}

static bool build_carrier(ns2_motion_hybrid_projector_t *state,
                          uint32_t out[3])
{
    uint8_t pdu[NS2_MOTION_PDU30_LENGTH];
    return ns2_ds5_motion_build(&state->translator, pdu) &&
           ns2_motion_pdu30_get_orientation(pdu, out);
}

static bool build_carrier_pdu(ns2_motion_hybrid_projector_t *state,
                              uint8_t out[NS2_MOTION_PDU30_LENGTH])
{
    return ns2_ds5_motion_build(&state->translator, out);
}

static void retain_latest_as_sample(ns2_motion_hybrid_projector_t *state)
{
    if (!state->latest_valid || !state->pose_aligned ||
        !state->translator.bias_ready)
        return;
    ns2_motion_hybrid_sample_t sample = state->latest;
    for (unsigned axis = 0; axis < 3u; ++axis) {
        int32_t value = state->translator.gyro_corrected[axis];
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        sample.gyro[axis] = (int16_t)value;
    }
    if (!build_carrier(state, sample.carrier)) return;
    state->ring[state->head] = sample;
    state->head = (uint8_t)((state->head + 1u) %
                            NS2_MOTION_HYBRID_SAMPLE_RING);
    if (state->filled < NS2_MOTION_HYBRID_SAMPLE_RING) state->filled++;
}

void ns2_motion_hybrid_projector_push(
    ns2_motion_hybrid_projector_t *state, uint32_t captured_us,
    uint32_t sensor_timestamp, uint32_t sequence,
    const int16_t accel[3], const int16_t gyro[3],
    uint8_t calibration_state)
{
    if (!state || !accel || !gyro) return;

    if (state->calibration_state != calibration_state &&
        calibration_state == NS2_MOTION_HYBRID_CALIBRATION_READY) {
        // Never carry fallback/raw-calibration history into a live donor.
        ns2_ds5_motion_reset(&state->translator);
        clear_samples(state);
        state->pose_aligned = 0u;
    }
    state->calibration_state = calibration_state;
    state->latest_valid = 1u;
    state->latest.us = captured_us;
    state->latest.sensor_timestamp = sensor_timestamp;
    state->latest.sequence = sequence;
    memcpy(state->latest.accel, accel, sizeof(state->latest.accel));
    memcpy(state->latest.gyro, gyro, sizeof(state->latest.gyro));

    if (calibration_state != NS2_MOTION_HYBRID_CALIBRATION_READY) return;

    switch_pro_input_t input;
    memset(&input, 0, sizeof(input));
    input.has_motion = 1u;
    input.motion_source = SWITCH_MOTION_SOURCE_DUALSENSE;
    input.motion_sequence = sequence;
    input.motion_timestamp = sensor_timestamp;
    input.motion_timestamp_valid = 1u;
    memcpy(input.accel, accel, sizeof(input.accel));
    memcpy(input.gyro, gyro, sizeof(input.gyro));
    if (ns2_ds5_motion_update(&state->translator, &input, captured_us))
        retain_latest_as_sample(state);
}

static bool gravity_pose_matches(
    const ns2_motion_hybrid_projector_t *state,
    const uint8_t genuine[NS2_MOTION_PDU30_LENGTH])
{
    int32_t native[3];
    int32_t donor[3];
    int64_t native_norm = 0;
    int64_t donor_norm = 0;
    int64_t dot = 0;
    for (unsigned axis = 0; axis < 3u; ++axis) {
        native[axis] = read_le32(&genuine[16u + axis * 4u]) / 65536;
        donor[axis] = state->latest.accel[axis];
        native_norm += (int64_t)native[axis] * native[axis];
        donor_norm += (int64_t)donor[axis] * donor[axis];
        dot += (int64_t)native[axis] * donor[axis];
    }
    if (native_norm < 2000ll * 2000ll || donor_norm < 2000ll * 2000ll ||
        dot <= 0)
        return false;
    // The two normalized gravity directions must agree within about 18 deg.
    if (dot * dot * 100ll < native_norm * donor_norm * 90ll) return false;
    // Gains differ slightly (the 0x1E output is calibrated to ~4310 counts/g),
    // but a wildly different magnitude means one controller was moving.
    if (native_norm * 2ll < donor_norm || donor_norm * 2ll < native_norm)
        return false;
    return true;
}

bool ns2_motion_hybrid_projector_observe_carrier(
    ns2_motion_hybrid_projector_t *state,
    const uint8_t genuine[NS2_MOTION_PDU30_LENGTH], uint32_t captured_us,
    ns2_motion_hybrid_live_reason_t *reason)
{
    if (!state || !genuine) return false;
    if (state->pose_aligned) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30;
        return true;
    }
    if (!state->latest_valid) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_WAIT_DS5;
        return false;
    }
    if (state->calibration_state != NS2_MOTION_HYBRID_CALIBRATION_READY) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_WAIT_CALIBRATION;
        return false;
    }
    if (!state->translator.bias_ready) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_WAIT_BIAS;
        return false;
    }
    if ((uint32_t)(captured_us - state->latest.us) >
        NS2_MOTION_HYBRID_MAX_DONOR_AGE_US) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_STALE_DONOR;
        return false;
    }
    if (!gravity_pose_matches(state, genuine)) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_WAIT_POSE;
        return false;
    }

    float quaternion[4];
    uint8_t chart = 0u;
    // This is deliberately only the validated translated-source/stable-state
    // approximation. Genuine transition packets can violate strict unit
    // retained energy; the decoder rejects those and alignment waits rather
    // than normalizing away evidence of Nintendo's private chart state.
    if (!ns2_motion_pdu30_get_quaternion(genuine, quaternion, &chart)) {
        if (reason) *reason = NS2_MOTION_HYBRID_LIVE_WAIT_POSE;
        return false;
    }
    memcpy(state->translator.quaternion, quaternion,
           sizeof(state->translator.quaternion));
    state->translator.switch2_omitted = chart;
    state->aligned_chart = chart;
    state->pose_aligned = 1u;
    clear_samples(state);
    retain_latest_as_sample(state);
    if (reason) *reason = NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30;
    return true;
}

static unsigned collect_window(const ns2_motion_hybrid_projector_t *state,
                               uint32_t start_us, uint32_t end_us,
                               uint8_t sorted[NS2_MOTION_HYBRID_SAMPLE_RING])
{
    const uint32_t span = end_us - start_us;
    unsigned count = 0u;
    for (unsigned i = 0; i < state->filled; ++i) {
        const uint32_t offset = state->ring[i].us - start_us;
        if (offset > span) continue;
        unsigned at = count;
        while (at > 0u &&
               (state->ring[sorted[at - 1u]].us - start_us) > offset) {
            sorted[at] = sorted[at - 1u];
            at--;
        }
        sorted[at] = (uint8_t)i;
        count++;
    }
    return count;
}

static const ns2_motion_hybrid_sample_t *nearest_sample(
    const ns2_motion_hybrid_projector_t *state, const uint8_t *sorted,
    unsigned count, uint32_t target_us)
{
    const ns2_motion_hybrid_sample_t *best = NULL;
    uint32_t best_distance = UINT32_MAX;
    for (unsigned i = 0; i < count; ++i) {
        const ns2_motion_hybrid_sample_t *sample = &state->ring[sorted[i]];
        const uint32_t distance = sample->us > target_us
            ? sample->us - target_us : target_us - sample->us;
        if (distance < best_distance) {
            best = sample;
            best_distance = distance;
        }
    }
    return best;
}

static const ns2_motion_hybrid_sample_t *nearest_ring_sample(
    const ns2_motion_hybrid_projector_t *state, uint32_t target_us)
{
    const ns2_motion_hybrid_sample_t *best = NULL;
    uint32_t best_distance = UINT32_MAX;
    for (unsigned i = 0; i < state->filled; ++i) {
        const ns2_motion_hybrid_sample_t *sample = &state->ring[i];
        const uint32_t distance = sample->us > target_us
            ? sample->us - target_us : target_us - sample->us;
        if (distance < best_distance) {
            best = sample;
            best_distance = distance;
        }
    }
    return best;
}

static uint16_t changed_bits(const uint8_t *left, const uint8_t *right,
                             uint8_t length)
{
    uint16_t count = 0u;
    for (uint8_t i = 0; i < length; ++i) {
        uint8_t value = left[i] ^ right[i];
        while (value) {
            count += value & 1u;
            value >>= 1;
        }
    }
    return count;
}

bool ns2_motion_hybrid_projector_project(
    ns2_motion_hybrid_projector_t *state, const uint8_t *base,
    uint8_t length, uint32_t captured_us, uint32_t groups, uint8_t *out,
    ns2_motion_hybrid_project_result_t *result)
{
    if (!state || !base || !out || !result) return false;
    memset(result, 0, sizeof(*result));
    result->requested_groups = groups;
    result->calibration_state = state->calibration_state;
    result->pose_aligned = state->pose_aligned;
    memcpy(out, base, length <= NS2_MOTION_PDU40_LENGTH
                          ? length : NS2_MOTION_PDU40_LENGTH);

    if (length == NS2_MOTION_PDU30_LENGTH) {
        ns2_motion_hybrid_live_reason_t reason =
            NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30;
        ns2_motion_hybrid_projector_observe_carrier(
            state, base, captured_us, &reason);
        result->calibration_state = state->calibration_state;
        result->pose_aligned = state->pose_aligned;
        if (!(groups & NS2_MOTION_HYBRID_PREFIX) ||
            !state->pose_aligned) {
            result->reason = reason;
            return false;
        }

        // A mode-3 prefix is not an independent packet-local field. Genuine
        // traffic interleaves it with length-0x1E absolute carriers. Replacing
        // the former while passing the latter through alternates two source
        // histories and makes even a sub-degree mismatch repeat as violent
        // camera motion. Once the genuine carrier has anchored the donor,
        // prefix ownership therefore spans BOTH PDU lengths.
        //
        // A short donor scheduling gap is represented by holding the most
        // recent donor orientation, exactly as the production 0x1E path holds
        // its last report. Falling back to the advancing genuine carrier here
        // would reintroduce the mixed-source discontinuity this path forbids.
        if (!state->latest_valid) {
            result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_DS5;
            return false;
        }
        result->ds5_age_us = captured_us - state->latest.us;
        result->ds5_sequence = state->latest.sequence;
        if (state->calibration_state !=
            NS2_MOTION_HYBRID_CALIBRATION_READY) {
            result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_CALIBRATION;
            return false;
        }
        if (!state->translator.bias_ready) {
            result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_BIAS;
            return false;
        }

        uint8_t donor[NS2_MOTION_PDU30_LENGTH];
        if (!build_carrier_pdu(state, donor)) {
            result->reason = NS2_MOTION_HYBRID_LIVE_DONOR_BUILD_FAILED;
            return false;
        }
        if (ns2_motion_hybrid_splice(
                base, donor, length, NS2_MOTION_HYBRID_PREFIX, out) !=
            NS2_MOTION_HYBRID_OK) {
            memcpy(out, base, length);
            result->reason = NS2_MOTION_HYBRID_LIVE_SPLICE_FAILED;
            return false;
        }
        result->reason = NS2_MOTION_HYBRID_LIVE_APPLIED;
        result->applied_groups = NS2_MOTION_HYBRID_PREFIX;
        result->changed_bits = changed_bits(base, out, length);
        return true;
    }
    const bool prefix_only = groups == NS2_MOTION_HYBRID_PREFIX;
    if (length != NS2_MOTION_PDU40_LENGTH ||
        (base[4] & 0x03u) != 3u ||
        (!prefix_only && elapsed40(base) > 10u)) {
        result->reason = NS2_MOTION_HYBRID_LIVE_UNSUPPORTED_LAYOUT;
        return false;
    }
    if (!state->latest_valid) {
        result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_DS5;
        return false;
    }
    result->ds5_age_us = captured_us - state->latest.us;
    result->ds5_sequence = state->latest.sequence;
    if (state->calibration_state != NS2_MOTION_HYBRID_CALIBRATION_READY) {
        result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_CALIBRATION;
        return false;
    }
    if (!state->translator.bias_ready) {
        result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_BIAS;
        return false;
    }
    if (!state->pose_aligned) {
        result->reason = NS2_MOTION_HYBRID_LIVE_WAIT_POSE;
        return false;
    }
    if (!prefix_only &&
        result->ds5_age_us > NS2_MOTION_HYBRID_MAX_DONOR_AGE_US) {
        result->reason = NS2_MOTION_HYBRID_LIVE_STALE_DONOR;
        return false;
    }
    if (!prefix_only &&
        state->last_project_sequence == state->latest.sequence) {
        result->reason = NS2_MOTION_HYBRID_LIVE_SOURCE_NOT_ADVANCED;
        return false;
    }

    const uint16_t elapsed = elapsed40(base);
    const uint32_t span_us = (uint32_t)elapsed * 1250u;
    if (span_us == 0u) {
        result->reason = NS2_MOTION_HYBRID_LIVE_NO_WINDOW;
        return false;
    }
    // Both publishers use the Pico clock. Anchor the donor window to the
    // genuine packet's arrival, not to the latest DS5 sample: otherwise a
    // 4-8 ms donor age silently shifts every substituted sample backward even
    // though the exported age makes the mismatch visible.
    const uint32_t end_us = captured_us;
    const uint32_t start_us = captured_us - span_us;
    uint8_t sorted[NS2_MOTION_HYBRID_SAMPLE_RING];
    const unsigned available = collect_window(state, start_us, end_us, sorted);
    if (!prefix_only && available < 2u) {
        result->reason = NS2_MOTION_HYBRID_LIVE_NO_WINDOW;
        return false;
    }

    const uint32_t prefix_us = start_us + 4u * 1250u;
    const ns2_motion_hybrid_sample_t *prefix = available
        ? nearest_sample(state, sorted, available, prefix_us)
        : nearest_ring_sample(state, prefix_us);
    if (!prefix && prefix_only) prefix = &state->latest;
    if (!prefix) {
        result->reason = NS2_MOTION_HYBRID_LIVE_NO_WINDOW;
        return false;
    }

    if (prefix_only) {
        int32_t carrier[3];
        ns2_ds5_motion40_prefix(
            prefix->carrier, carrier,
            elapsed <= NS2_MOTION40_HIGH_RATE_MAX_ELAPSED);
        uint8_t donor[NS2_MOTION_PDU40_LENGTH];
        memcpy(donor, base, sizeof(donor));
        if (!ns2_motion_pdu40_set_carrier(donor, carrier)) {
            result->reason = NS2_MOTION_HYBRID_LIVE_DONOR_BUILD_FAILED;
            return false;
        }
        if (ns2_motion_hybrid_splice(
                base, donor, length, NS2_MOTION_HYBRID_PREFIX, out) !=
            NS2_MOTION_HYBRID_OK) {
            memcpy(out, base, length);
            result->reason = NS2_MOTION_HYBRID_LIVE_SPLICE_FAILED;
            return false;
        }
        state->last_project_sequence = state->latest.sequence;
        result->reason = NS2_MOTION_HYBRID_LIVE_APPLIED;
        result->applied_groups = NS2_MOTION_HYBRID_PREFIX;
        result->changed_bits = changed_bits(base, out, length);
        return true;
    }

    ns2_motion40_high_rate_t fields;
    memset(&fields, 0, sizeof(fields));
    fields.tick = (uint16_t)(base[0] | ((uint16_t)(base[1] & 0x0Fu) << 8));
    fields.elapsed_ticks = elapsed;
    fields.packing_mode = 3u;
    fields.status = base[3];
    fields.tail_value = (uint16_t)base[38] | ((uint16_t)base[39] << 8);

    const ns2_motion_hybrid_sample_t *oldest = available
        ? &state->ring[sorted[0]] : &state->latest;
    const ns2_motion_hybrid_sample_t *newest = available
        ? &state->ring[sorted[available - 1u]] : &state->latest;
    const ns2_motion_hybrid_sample_t *accel_samples[2] = {oldest, newest};
    for (unsigned slot = 0; slot < 2u; ++slot) {
        for (unsigned axis = 0; axis < 3u; ++axis) {
            const int64_t scaled =
                (int64_t)accel_samples[slot]->accel[axis] *
                NS2_MOTION30_ACCEL_Q16_PER_COUNT;
            fields.accel[slot][axis] = clamp_wire(
                round_div_signed(scaled, 256), HIGH_RATE_ACCEL_LIMIT,
                &result->saturated_accel);
        }
    }

    int64_t gyro_area[3] = {0, 0, 0};
    uint32_t previous_us = start_us;
    for (unsigned i = 0; i < available; ++i) {
        const ns2_motion_hybrid_sample_t *sample = &state->ring[sorted[i]];
        uint32_t duration = sample->us - previous_us;
        if (duration > span_us) duration = 0u;
        for (unsigned axis = 0; axis < 3u; ++axis)
            gyro_area[axis] += (int64_t)sample->gyro[axis] * duration;
        previous_us = sample->us;
    }
    if (previous_us != end_us) {
        const uint32_t duration = end_us - previous_us;
        for (unsigned axis = 0; axis < 3u; ++axis)
            gyro_area[axis] += (int64_t)newest->gyro[axis] * duration;
    }
    for (unsigned axis = 0; axis < 3u; ++axis) {
        const int32_t mean = round_div_signed(gyro_area[axis], span_us);
        fields.gyro[0][axis] = clamp_wire(
            (int64_t)mean * 128, HIGH_RATE_GYRO_LIMIT,
            &result->saturated_gyro);
    }

    ns2_ds5_motion40_prefix(prefix->carrier, fields.carrier, true);

    uint8_t donor[NS2_MOTION_PDU40_LENGTH];
    if (!ns2_motion_pdu40_build_high_rate(donor, &fields)) {
        result->reason = NS2_MOTION_HYBRID_LIVE_DONOR_BUILD_FAILED;
        return false;
    }
    if (ns2_motion_hybrid_splice(base, donor, length, groups, out) !=
        NS2_MOTION_HYBRID_OK) {
        memcpy(out, base, length);
        result->reason = NS2_MOTION_HYBRID_LIVE_SPLICE_FAILED;
        return false;
    }

    state->last_project_sequence = state->latest.sequence;
    result->reason = NS2_MOTION_HYBRID_LIVE_APPLIED;
    result->applied_groups = groups;
    result->changed_bits = changed_bits(base, out, length);
    return true;
}

const char *ns2_motion_hybrid_live_reason_name(
    ns2_motion_hybrid_live_reason_t reason)
{
    switch (reason) {
        case NS2_MOTION_HYBRID_LIVE_APPLIED: return "applied";
        case NS2_MOTION_HYBRID_LIVE_GENUINE_CONTROL: return "genuine_control";
        case NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30: return "passthrough_0x1e";
        case NS2_MOTION_HYBRID_LIVE_WAIT_DS5: return "wait_ds5";
        case NS2_MOTION_HYBRID_LIVE_WAIT_CALIBRATION: return "wait_calibration";
        case NS2_MOTION_HYBRID_LIVE_WAIT_BIAS: return "wait_bias";
        case NS2_MOTION_HYBRID_LIVE_WAIT_POSE: return "wait_pose";
        case NS2_MOTION_HYBRID_LIVE_STALE_DONOR: return "stale_donor";
        case NS2_MOTION_HYBRID_LIVE_SOURCE_NOT_ADVANCED: return "source_not_advanced";
        case NS2_MOTION_HYBRID_LIVE_UNSUPPORTED_LAYOUT: return "unsupported_layout";
        case NS2_MOTION_HYBRID_LIVE_NO_WINDOW: return "no_window";
        case NS2_MOTION_HYBRID_LIVE_DONOR_BUILD_FAILED: return "donor_build_failed";
        case NS2_MOTION_HYBRID_LIVE_SPLICE_FAILED: return "splice_failed";
        default: return "unknown";
    }
}
