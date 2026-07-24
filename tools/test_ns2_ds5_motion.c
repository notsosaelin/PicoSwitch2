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
    if (swap_state == 0u) {
        wire[0] = hidden;
        wire[1] = component[0];
        wire[2] = component[1];
        wire[3] = component[2];
    } else if (swap_state == 3u) {
        // Proven by both directions of a genuine adjacent 3<->0 boundary:
        // state 3 [G0,G1,G2] = [z,-x,-y]. This is a coordinate-basis
        // rebase, not the former cyclic "hide z" interpretation.
        wire[0] = hidden;
        wire[1] = -component[1];
        wire[2] = -component[2];
        wire[3] = component[0];
    } else {
        // Retain the historical candidate for the two states for which this
        // host helper has no adjacent genuine transition yet.
        for (unsigned i = 0; i < 3; ++i)
            wire[(swap_state + i + 1u) & 3u] = component[i];
        wire[swap_state] = hidden;
    }
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
    // sw2_native_passthrough_live_2026-07-21.jsonl. The old cyclic decoder
    // reports false 59-61 degree jumps; the state-3 basis rebase preserves
    // pose to within the real 7.5-15 ms of motion between packets.
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
              transition_q_before, transition_q_after) < 1.25f,
          "genuine state 3->0 rebase is pose-continuous");

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
              transition_q_before, transition_q_after) < 1.0f,
          "genuine state 0->3 rebase is pose-continuous");

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
               startup_state.gyro_bias[0]) >> 6) <= 1 &&
          abs((startup_state.gyro_lp[1] -
               startup_state.gyro_bias[1]) >> 6) <= 1 &&
          abs((startup_state.gyro_lp[2] -
               startup_state.gyro_bias[2]) >> 6) <= 1,
          "settled startup residual is zero-rate");
    check(fabsf(startup_state.quaternion[0]) < 0.0001f &&
          fabsf(startup_state.quaternion[1]) < 0.0001f &&
          fabsf(startup_state.quaternion[2]) < 0.0001f &&
          startup_state.quaternion[3] > 0.9999f,
          "startup transient cannot rotate the carrier");

    // Switch 2 uses an MSBSwapState rebase at its +/-0.5 component boundary.
    // Until that transform is proven, the encoder must fail closed instead of
    // emitting the old Switch-1-scaled corruption.
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
    bool rejected_boundary = false;
    for (unsigned i = 0; i < 500; ++i) {
        now += 4000u;
        ns2_ds5_motion_update(&state, &input, now);
        if (!ns2_ds5_motion_build(&state, pdu)) {
            rejected_boundary = true;
            break;
        }
    }
    check(rejected_boundary,
          "unproven Switch 2 rebase boundary fails closed");
    check(state.representation_rejects == 1u,
          "rebase boundary rejection is diagnosed exactly once");

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
