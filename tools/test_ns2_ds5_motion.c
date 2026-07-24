#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ns2_ds5_motion.h"
#include "ns2_motion_pdu.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int32_t read_le32s(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] |
                     ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) |
                     ((uint32_t)p[3] << 24));
}

static float decode_switch2_g0(uint32_t value)
{
    return ((float)value / 67108864.0f - 0.5f) *
           1.41421356237309504880f;
}

static float decode_switch2_g1(uint32_t value)
{
    return ((float)value / 33554432.0f - 0.5f) *
           1.41421356237309504880f;
}

static float decode_switch2_g2(uint32_t value)
{
    return ((float)value / 16777216.0f - 0.5f) *
           1.41421356237309504880f;
}

static void decode_switch2_quaternion(const uint8_t pdu[30], float q[4])
{
    uint32_t orientation[3];
    ns2_motion_pdu30_get_orientation(pdu, orientation);
    const unsigned swap_state = orientation[2] >> 24;
    const float component[3] = {
        decode_switch2_g0(orientation[0]),
        decode_switch2_g1(orientation[1]),
        decode_switch2_g2(orientation[2] & 0x00FFFFFFu),
    };
    const float hidden_sq =
        1.0f - component[0] * component[0] -
        component[1] * component[1] -
        component[2] * component[2];
    const float hidden = sqrtf(hidden_sq > 0.0f ? hidden_sq : 0.0f);
    float wire[4] = {0}; // w, x, y, z
    wire[swap_state] = hidden;
    for (unsigned i = 0; i < 3; ++i)
        wire[(swap_state + i + 1u) & 3u] = component[i];
    q[0] = wire[1];
    q[1] = wire[2];
    q[2] = wire[3];
    q[3] = wire[0];
}

static void quaternion_conjugate(const float q[4], float out[4])
{
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

static void quaternion_multiply(const float a[4], const float b[4],
                                float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] +
             a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] +
             a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] -
             a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] -
             a[1] * b[1] - a[2] * b[2];
}

static float quaternion_angle_degrees(const float a[4], const float b[4])
{
    float dot = fabsf(a[0] * b[0] + a[1] * b[1] +
                      a[2] * b[2] + a[3] * b[3]);
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * acosf(dot) * 57.295779513082320876f;
}

static void quaternion_normalize(float q[4])
{
    const float norm = sqrtf(
        q[0] * q[0] + q[1] * q[1] +
        q[2] * q[2] + q[3] * q[3]);
    if (norm <= 0.0f) return;
    for (unsigned i = 0; i < 4; ++i)
        q[i] /= norm;
}

static unsigned expected_switch2_state(const float q[4])
{
    const float wire[4] = {q[3], q[0], q[1], q[2]}; // w,x,y,z
    unsigned state = 0;
    for (unsigned i = 1; i < 4; ++i) {
        if (fabsf(wire[i]) > fabsf(wire[state]))
            state = i;
    }
    return state;
}

static void quaternion_axis_angle(unsigned axis, float degrees, float q[4])
{
    memset(q, 0, sizeof(float) * 4u);
    const float half_radians =
        degrees * 0.5f * 0.01745329251994329577f;
    q[axis] = sinf(half_radians);
    q[3] = cosf(half_radians);
}

int main(void)
{
    ns2_ds5_motion_state_t state;
    switch_pro_input_t input;
    uint8_t pdu[30];
    uint32_t now;
    memset(&input, 0, sizeof(input));
    input.has_motion = 1;
    input.accel[2] = 4096;

    ns2_ds5_motion_reset(&state);
    check(state.carrier == NS2_DS5_CARRIER_SWITCH2_WXYZ,
          "Switch 2 wxyz carrier is the default");
    check(state.body_frame, "body-frame integration is the safe default");
    check(state.gyro_map[0] == 1 && state.gyro_map[1] == 2 &&
          state.gyro_map[2] == 3, "default signed gyro map retained");
    const int8_t duplicate_map[3] = {1, 1, 3};
    check(!ns2_ds5_motion_set_gyro_map(&state, duplicate_map),
          "duplicate source axes are rejected");
    check(ns2_ds5_motion_update(&state, &input, 1000u),
          "first sample initializes");
    check(ns2_ds5_motion_build(&state, pdu),
          "initialized state builds");
    check((pdu[4] & 0x03u) == 0u,
          "Switch 2 identity uses state 0 for dominant W");
    check(read_le32s(&pdu[16]) == 0, "accel X zero");
    check(read_le32s(&pdu[20]) == 0, "accel Y zero");
    check(read_le32s(&pdu[24]) == 282472448,
          "one g uses genuine stationary scale");

    float before[4];
    float after[4];
    decode_switch2_quaternion(pdu, before);
    check(fabsf(before[0]) < 0.0001f &&
          fabsf(before[1]) < 0.0001f &&
          fabsf(before[2]) < 0.0001f &&
          before[3] > 0.9999f,
          "identity quaternion round-trips through Switch 2 fields");

    // Exact round-trip of one genuine state-0 Pro Controller 2 carrier.
    const uint32_t genuine_orientation[3] = {
        0x01DF54D6u, 0x00D1DF88u, 0x00916EEEu
    };
    const float genuine_x = decode_switch2_g0(genuine_orientation[0]);
    const float genuine_y = decode_switch2_g1(genuine_orientation[1]);
    const float genuine_z = decode_switch2_g2(genuine_orientation[2]);
    state.quaternion[0] = genuine_x;
    state.quaternion[1] = genuine_y;
    state.quaternion[2] = genuine_z;
    state.quaternion[3] = sqrtf(
        1.0f - genuine_x * genuine_x -
        genuine_y * genuine_y - genuine_z * genuine_z);
    check(ns2_ds5_motion_build(&state, pdu),
          "genuine state-0 quaternion rebuilds");
    uint32_t rebuilt[3];
    check(ns2_motion_pdu30_get_orientation(pdu, rebuilt),
          "rebuilt genuine carrier decodes");
    check(rebuilt[0] == genuine_orientation[0] &&
          rebuilt[1] == genuine_orientation[1] &&
          rebuilt[2] == genuine_orientation[2],
          "genuine state-0 orientation round-trips bit exactly");

    // These are the two adjacent genuine 0x1e transition pairs already in
    // sw2_native_passthrough_live_2026-07-21.jsonl. In w/x/y/z order they are
    // ordinary smallest-three state changes: state 3 omits Z and carries
    // W/X/Y; state 0 omits W and carries X/Y/Z. The omitted components cross
    // near 1/sqrt(2), which is why the former swap-based interpretation looked
    // almost continuous despite assigning the components incorrectly.
    const uint32_t genuine_3_to_0_before[3] = {
        0x00003AAAu, 0x0105D274u, 0x0363E784u
    };
    const uint32_t genuine_3_to_0_after[3] = {
        0x01F48328u, 0x01384FBAu, 0x000147FCu
    };
    uint8_t transition_before[30] = {0};
    uint8_t transition_after[30] = {0};
    float transition_q_before[4];
    float transition_q_after[4];
    ns2_motion_pdu30_set_orientation(
        transition_before, genuine_3_to_0_before);
    ns2_motion_pdu30_set_orientation(
        transition_after, genuine_3_to_0_after);
    decode_switch2_quaternion(transition_before, transition_q_before);
    decode_switch2_quaternion(transition_after, transition_q_after);
    check(quaternion_angle_degrees(
              transition_q_before, transition_q_after) < 2.5f,
          "genuine state 3->0 smallest-three transition is continuous");

    const uint32_t genuine_0_to_3_before[3] = {
        0x020EA1D4u, 0x01238C94u, 0x0001B5FAu
    };
    const uint32_t genuine_0_to_3_after[3] = {
        0x00080C80u, 0x00F88657u, 0x036E3053u
    };
    memset(transition_before, 0, sizeof(transition_before));
    memset(transition_after, 0, sizeof(transition_after));
    ns2_motion_pdu30_set_orientation(
        transition_before, genuine_0_to_3_before);
    ns2_motion_pdu30_set_orientation(
        transition_after, genuine_0_to_3_after);
    decode_switch2_quaternion(transition_before, transition_q_before);
    decode_switch2_quaternion(transition_after, transition_q_after);
    check(quaternion_angle_degrees(
              transition_q_before, transition_q_after) < 2.5f,
          "genuine state 0->3 smallest-three transition is continuous");

    // Independent high-motion Pro Controller 2 transition preserved in the
    // research dump. It changes state 0 (hidden W) to state 1 (hidden X).
    // The packets are not a controlled stationary pair, so the looser bound
    // includes their real intervening motion; it is still an independent
    // check of the w/x/y/z state numbering and cyclic component order.
    const uint32_t research_0_to_1_before[3] = {
        0x03EEBB78u, 0x01598804u, 0x00A73722u
    };
    const uint32_t research_0_to_1_after[3] = {
        0x02B09B44u, 0x015678FCu, 0x01FE6887u
    };
    memset(transition_before, 0, sizeof(transition_before));
    memset(transition_after, 0, sizeof(transition_after));
    ns2_motion_pdu30_set_orientation(
        transition_before, research_0_to_1_before);
    ns2_motion_pdu30_set_orientation(
        transition_after, research_0_to_1_after);
    decode_switch2_quaternion(transition_before, transition_q_before);
    decode_switch2_quaternion(transition_after, transition_q_after);
    check(quaternion_angle_degrees(
              transition_q_before, transition_q_after) < 10.0f,
          "independent state 0->1 transition matches wxyz DScale order");

    // Restore identity, then establish startup bias from a bounded still
    // window before testing motion.
    ns2_ds5_motion_set_carrier(
        &state, NS2_DS5_CARRIER_SWITCH2_WXYZ);
    now = 5000u;
    for (unsigned i = 0; i < 40; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&state, &input, now);
    }
    check(state.bias_ready, "stationary warmup acquires gyro bias");

    // The normalized seam maps positive DS5 pitch to positive input X.
    // The default identity map preserves that as canonical X.
    input.gyro[0] = 328;  // about +20 dps DualSense pitch
    for (unsigned i = 0; i < 125; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&state, &input, now);
    }
    check(ns2_ds5_motion_build(&state, pdu),
          "small body-frame pitch remains representable");
    decode_switch2_quaternion(pdu, after);
    float before_inverse[4];
    float relative[4];
    quaternion_conjugate(before, before_inverse);
    quaternion_multiply(before_inverse, after, relative);
    check(relative[0] > 0.04f,
          "positive DS5 pitch produces positive body-frame canonical-X delta");
    check(state.updates == 165u,
          "warmup and 250 Hz motion updates counted exactly");
    check(state.representation_rejects == 0u,
          "ordinary pitch does not hit representation boundary");
    check(pdu[29] == 0x02u, "genuine constant tail retained");

    // A real DualSense can emit a large one-report transient while its IMU
    // stream starts. Reproduce the exact failure shape seen over UART: the
    // old code declared this first report to be zero, then continuously
    // integrated its inverse after the controller settled.
    ns2_ds5_motion_state_t startup_state;
    switch_pro_input_t startup_input;
    memset(&startup_input, 0, sizeof(startup_input));
    startup_input.has_motion = 1;
    startup_input.accel[2] = 4096;
    startup_input.gyro[0] = -1948;
    startup_input.gyro[1] = 382;
    startup_input.gyro[2] = 1077;
    ns2_ds5_motion_reset(&startup_state);
    check(ns2_ds5_motion_update(&startup_state, &startup_input, 1000u),
          "startup transient initializes without integrating");
    check(startup_state.gyro_bias[0] == 0 &&
          startup_state.gyro_bias[1] == 0 &&
          startup_state.gyro_bias[2] == 0,
          "first transient is never captured as zero-rate bias");

    startup_input.gyro[0] = -8;
    startup_input.gyro[1] = -8;
    startup_input.gyro[2] = 11;
    now = 1000u;
    for (unsigned i = 0; i < 128; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&startup_state, &startup_input, now);
    }
    check(startup_state.bias_ready,
          "settled reports acquire bias after a startup transient");
    check(abs((startup_state.gyro_lp[0] -
               startup_state.gyro_bias[0]) / 64) <= 1 &&
          abs((startup_state.gyro_lp[1] -
               startup_state.gyro_bias[1]) / 64) <= 1 &&
          abs((startup_state.gyro_lp[2] -
               startup_state.gyro_bias[2]) / 64) <= 1,
          "settled startup residual is zero-rate");
    check(fabsf(startup_state.quaternion[0]) < 0.0001f &&
          fabsf(startup_state.quaternion[1]) < 0.0001f &&
          fabsf(startup_state.quaternion[2]) < 0.0001f &&
          startup_state.quaternion[3] > 0.9999f,
          "startup transient cannot rotate the carrier");

    // Temperature and supply changes can move a stationary DualSense zero
    // point by a few raw counts after startup. Reproduce the live UART defect:
    // the old fixed-point >>8 tracker stalled at this small delta and drifted
    // several degrees in seconds. The tracker must converge below one raw
    // count without integrating a meaningful orientation change.
    ns2_ds5_motion_state_t drift_state;
    switch_pro_input_t drift_input;
    memset(&drift_input, 0, sizeof(drift_input));
    drift_input.has_motion = 1;
    drift_input.accel[2] = 4096;
    ns2_ds5_motion_reset(&drift_state);
    now = 1000u;
    check(ns2_ds5_motion_update(&drift_state, &drift_input, now),
          "bias-step test initializes");
    for (unsigned i = 0; i < 40; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&drift_state, &drift_input, now);
    }
    check(drift_state.bias_ready,
          "bias-step test completes still warmup");
    drift_input.gyro[0] = 3;
    drift_input.gyro[1] = -3;
    drift_input.gyro[2] = 2;
    for (unsigned i = 0; i < 3000; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&drift_state, &drift_input, now);
    }
    check(abs((drift_state.gyro_lp[0] -
               drift_state.gyro_bias[0]) / 64) == 0 &&
          abs((drift_state.gyro_lp[1] -
               drift_state.gyro_bias[1]) / 64) == 0 &&
          abs((drift_state.gyro_lp[2] -
               drift_state.gyro_bias[2]) / 64) == 0,
          "small stationary bias step converges below one raw count");
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    check(quaternion_angle_degrees(
              identity, drift_state.quaternion) < 0.25f,
          "small stationary bias step does not create visible drift");

    // Bluetooth notification arrival time is not the sensor sampling clock.
    // Four ordinary reports and one delayed report spanning the same authored
    // DualSense interval must integrate the same angle, including across the
    // controller timestamp's u32 wrap. This specifically guards the former
    // 16 ms host-time clamp that discarded rapid-motion angular area.
    ns2_ds5_motion_state_t regular_time_state;
    ns2_ds5_motion_state_t delayed_time_state;
    switch_pro_input_t regular_time_input;
    switch_pro_input_t delayed_time_input;
    memset(&regular_time_input, 0, sizeof(regular_time_input));
    regular_time_input.has_motion = 1;
    regular_time_input.motion_timestamp_valid = 1;
    regular_time_input.motion_timestamp = 0xFFFFF000u;
    regular_time_input.motion_sequence = 1u;
    regular_time_input.accel[2] = 4096;
    delayed_time_input = regular_time_input;
    ns2_ds5_motion_reset(&regular_time_state);
    ns2_ds5_motion_reset(&delayed_time_state);
    now = 1000u;
    check(ns2_ds5_motion_update(
              &regular_time_state, &regular_time_input, now),
          "sensor-clock regular path initializes");
    check(ns2_ds5_motion_update(
              &delayed_time_state, &delayed_time_input, now),
          "sensor-clock delayed path initializes");
    for (unsigned i = 0; i < 40; ++i) {
        regular_time_input.motion_sequence++;
        delayed_time_input.motion_sequence++;
        regular_time_input.motion_timestamp += 12000u; // 4 ms
        delayed_time_input.motion_timestamp += 12000u;
        now += 4000u;
        ns2_ds5_motion_update(
            &regular_time_state, &regular_time_input, now);
        ns2_ds5_motion_update(
            &delayed_time_state, &delayed_time_input, now);
    }
    check(regular_time_state.bias_ready &&
          delayed_time_state.bias_ready,
          "sensor-clock paths complete bias warmup");
    check(regular_time_state.last_sensor_timestamp ==
              regular_time_input.motion_timestamp &&
          regular_time_input.motion_timestamp < 0xFFFFF000u,
          "DualSense sensor timestamp wraps without losing its baseline");

    regular_time_input.gyro[2] = 32767; // approximately 2000 dps
    delayed_time_input.gyro[2] = 32767;
    const uint32_t interval_ticks = 22500u; // 7.5 ms at 3 ticks/us
    for (unsigned i = 0; i < 4; ++i) {
        regular_time_input.motion_sequence++;
        regular_time_input.motion_timestamp += interval_ticks;
        now += 11000u + i * 3000u; // deliberately jittered host delivery
        ns2_ds5_motion_update(
            &regular_time_state, &regular_time_input, now);
    }
    delayed_time_input.motion_sequence += 4u;
    delayed_time_input.motion_timestamp += 4u * interval_ticks;
    // Host delivery says only 4 ms; the authored sensor interval says 30 ms.
    ns2_ds5_motion_update(
        &delayed_time_state, &delayed_time_input, now + 4000u);

    check(quaternion_angle_degrees(
              regular_time_state.quaternion,
              delayed_time_state.quaternion) < 0.05f,
          "delayed sensor report preserves full high-rate angular area");
    check(delayed_time_state.last_sensor_elapsed_us == 30000u,
          "DualSense 0.33 us clock converts a 30 ms report gap exactly");
    check(delayed_time_state.max_sensor_elapsed_us == 30000u,
          "sensor-clock diagnostics retain the maximum gap");
    check(delayed_time_state.sequence_gaps == 3u,
          "physical report sequence gap is diagnosed");
    check(delayed_time_state.integration_substeps >= 8u,
          "large sensor interval is integrated in bounded substeps");
    check(delayed_time_state.sensor_timestamp_invalid == 0u &&
          delayed_time_state.sensor_timestamp_fallbacks == 0u,
          "valid wrapping sensor clock never falls back to host time");
    check(delayed_time_state.representation_rejects == 0u,
          "rapid delayed motion remains a valid quaternion carrier");

    // Exercise a real integrated path through the W<->Z omission boundary.
    // The old fixed-state encoder failed here and caused the console to hold
    // the previous orientation. The four-state encoder must remain continuous
    // across more than one full turn.
    ns2_ds5_motion_reset(&state);
    memset(&input, 0, sizeof(input));
    input.has_motion = 1;
    check(ns2_ds5_motion_update(&state, &input, 1000u),
          "transition test initializes");
    now = 1000u;
    for (unsigned i = 0; i < 40; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&state, &input, now);
    }
    check(state.bias_ready, "transition test completes bias warmup");
    input.gyro[2] = 3277;  // about 200 dps on canonical Z
    bool transition_builds = true;
    uint8_t integrated_state_mask = 0;
    float max_integrated_step = 0.0f;
    float previous_decoded[4];
    check(ns2_ds5_motion_build(&state, pdu),
          "transition path has an initial packet");
    decode_switch2_quaternion(pdu, previous_decoded);
    for (unsigned i = 0; i < 500; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&state, &input, now);
        if (!ns2_ds5_motion_build(&state, pdu)) {
            transition_builds = false;
            break;
        }
        integrated_state_mask |=
            (uint8_t)(1u << (pdu[4] & 0x03u));
        float decoded[4];
        decode_switch2_quaternion(pdu, decoded);
        const float step =
            quaternion_angle_degrees(previous_decoded, decoded);
        if (step > max_integrated_step) max_integrated_step = step;
        memcpy(previous_decoded, decoded, sizeof(previous_decoded));
    }
    check(transition_builds,
          "integrated full turn builds across every representation boundary");
    check((integrated_state_mask & 0x09u) == 0x09u,
          "integrated yaw uses both hidden-W and hidden-Z states");
    check(max_integrated_step < 2.0f,
          "integrated state transition has no orientation jump");
    check(state.representation_rejects == 0u,
          "integrated full turn has no representation rejects");

    // Exhaustively exercise arbitrary normalized quaternions, including both
    // signs of every possible omitted component. A valid retained state must
    // persist until its boundary is reached; then the emitted state must move
    // to the largest component. Decoding must reproduce the original rotation
    // modulo the q/-q equivalence.
    ns2_ds5_motion_state_t packing_state;
    ns2_ds5_motion_reset(&packing_state);
    packing_state.has_sample = true;

    // Re-encode the genuine state-3 sides of both observed boundaries while
    // explicitly retaining state 3. G0 is 26-bit but the embedded encoder
    // intentionally uses 24-bit IEEE float math, so allow its final two wire
    // LSBs to quantize while requiring the state and physical carrier to stay
    // unchanged.
    const uint32_t *genuine_state3_packets[2] = {
        genuine_3_to_0_before, genuine_0_to_3_after
    };
    uint32_t genuine_roundtrip_failures = 0;
    for (unsigned i = 0; i < 2; ++i) {
        memset(transition_before, 0, sizeof(transition_before));
        ns2_motion_pdu30_set_orientation(
            transition_before, genuine_state3_packets[i]);
        decode_switch2_quaternion(
            transition_before, packing_state.quaternion);
        packing_state.switch2_omitted = 3u;
        if (!ns2_ds5_motion_build(&packing_state, pdu)) {
            genuine_roundtrip_failures++;
            continue;
        }
        uint32_t roundtrip[3];
        ns2_motion_pdu30_get_orientation(pdu, roundtrip);
        for (unsigned field = 0; field < 3; ++field) {
            const uint32_t original = genuine_state3_packets[i][field];
            const uint32_t delta =
                roundtrip[field] > original
                    ? roundtrip[field] - original
                    : original - roundtrip[field];
            const uint32_t tolerance = field == 0 ? 2u : 1u;
            if (delta > tolerance)
                genuine_roundtrip_failures++;
        }
    }
    check(genuine_roundtrip_failures == 0u,
          "genuine state-3 carriers round-trip within float wire precision");

    packing_state.switch2_omitted = 0u;
    uint32_t random_state_mask = 0;
    uint32_t random_failures = 0;
    float max_random_error = 0.0f;
    uint32_t random = 0x6D2B79F5u;
    for (unsigned sample = 0; sample < 16384; ++sample) {
        float source[4];
        for (unsigned component = 0; component < 4; ++component) {
            random = random * 1664525u + 1013904223u;
            source[component] =
                (float)(random >> 8) / 8388607.5f - 1.0f;
        }
        quaternion_normalize(source);
        memcpy(packing_state.quaternion, source, sizeof(source));

        unsigned expected_state = packing_state.switch2_omitted;
        const float wire[4] = {
            source[3], source[0], source[1], source[2]
        };
        bool expected_transition = false;
        for (unsigned component = 0; component < 4; ++component) {
            if (component != expected_state &&
                fabsf(wire[component]) > 0.70710678118654752440f) {
                expected_transition = true;
                break;
            }
        }
        if (expected_transition)
            expected_state = expected_switch2_state(source);

        if (!ns2_ds5_motion_build(&packing_state, pdu)) {
            random_failures++;
            continue;
        }
        const unsigned emitted_state = pdu[4] & 0x03u;
        random_state_mask |= 1u << emitted_state;
        if (emitted_state != expected_state)
            random_failures++;
        float decoded[4];
        decode_switch2_quaternion(pdu, decoded);
        const float error = quaternion_angle_degrees(source, decoded);
        if (error > max_random_error) max_random_error = error;
        if (error > 0.1f) random_failures++;
    }
    check(random_state_mask == 0x0Fu,
          "random quaternion coverage reaches all four omission states");
    check(random_failures == 0u,
          "all random quaternions round-trip through the selected state");
    check(max_random_error < 0.1f,
          "random quaternion quantization error stays sub-tenth-degree");
    check(packing_state.representation_rejects == 0u,
          "random quaternion coverage has no representation rejects");

    // Sweep two complete turns about each principal axis. This crosses each
    // exact +/-90-degree tie in both directions and verifies q/-q handling at
    // 360 degrees without relying on a physical controller.
    uint32_t axis_failures = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        packing_state.switch2_omitted = 0u;
        uint8_t axis_state_mask = 0;
        float previous[4];
        bool have_previous = false;
        for (unsigned step_index = 0; step_index <= 1440; ++step_index) {
            float source[4];
            quaternion_axis_angle(axis, (float)step_index * 0.5f, source);
            memcpy(packing_state.quaternion, source, sizeof(source));
            if (!ns2_ds5_motion_build(&packing_state, pdu)) {
                axis_failures++;
                continue;
            }
            axis_state_mask |=
                (uint8_t)(1u << (pdu[4] & 0x03u));
            float decoded[4];
            decode_switch2_quaternion(pdu, decoded);
            if (quaternion_angle_degrees(source, decoded) > 0.1f)
                axis_failures++;
            if (have_previous &&
                quaternion_angle_degrees(previous, decoded) > 0.6f)
                axis_failures++;
            memcpy(previous, decoded, sizeof(previous));
            have_previous = true;
        }
        const uint8_t expected_mask =
            (uint8_t)((1u << 0) | (1u << (axis + 1u)));
        if (axis_state_mask != expected_mask)
            axis_failures++;
    }
    check(axis_failures == 0u,
          "principal-axis +/-360 sweeps are continuous across every tie");

    // Reproduce the reported difficult pose: hold a steep pitch while yawing
    // through two complete turns. This path drives compound quaternion states
    // while remaining entirely deterministic.
    float pitch[4];
    quaternion_axis_angle(0, 80.0f, pitch);
    packing_state.switch2_omitted = 0u;
    uint32_t compound_failures = 0;
    for (int degrees = -360; degrees <= 360; ++degrees) {
        float yaw[4];
        float source[4];
        quaternion_axis_angle(2, (float)degrees, yaw);
        quaternion_multiply(pitch, yaw, source);
        quaternion_normalize(source);
        memcpy(packing_state.quaternion, source, sizeof(source));
        if (!ns2_ds5_motion_build(&packing_state, pdu)) {
            compound_failures++;
            continue;
        }
        float decoded[4];
        decode_switch2_quaternion(pdu, decoded);
        if (quaternion_angle_degrees(source, decoded) > 0.1f)
            compound_failures++;
    }
    check(compound_failures == 0u,
          "high-pitch yaw sweep retains full range and continuity");
    check(packing_state.representation_rejects == 0u,
          "all synthetic paths finish with zero representation rejects");

    ns2_ds5_motion_set_carrier(
        &state, NS2_DS5_CARRIER_SWITCH1_DSCALE);
    check(state.carrier == NS2_DS5_CARRIER_SWITCH1_DSCALE,
          "Switch 1 DScale remains selectable as a diagnostic control");
    memset(&input, 0, sizeof(input));
    input.has_motion = 1;
    check(ns2_ds5_motion_update(&state, &input, now + 4000u),
          "Switch 1 control initializes");
    check(ns2_ds5_motion_build(&state, pdu),
          "Switch 1 control builds");
    check((pdu[4] & 0x03u) == 3u,
          "Switch 1 DScale identity uses state 3");

    ns2_ds5_motion_set_carrier(
        &state, NS2_DS5_CARRIER_LEGACY_STATE0);
    check(state.carrier == NS2_DS5_CARRIER_LEGACY_STATE0,
          "legacy carrier remains selectable for UART comparison");
    check(fabsf(state.quaternion[0] - 0.9207776592f) < 0.0001f,
          "legacy carrier restores its captured orientation");

    if (failures) return 1;
    puts("ns2_ds5_motion: all tests passed");
    return 0;
}
