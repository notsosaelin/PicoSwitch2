#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns2_ds5_motion.h"
#include "ns2_ds5_motion40.h"
#include "ns2_motion_pdu.h"

// Deterministic physical input for the sequence-level 0x1e/0x28 coherence
// test. This is deliberately a fixture PRODUCER, not an oracle: the Python
// validator independently reconstructs the commanded trajectory, decodes the
// emitted wire packets, and compares every representation in physical units.

#define SAMPLE_US 1250u
#define SAMPLE_COUNT 432u
#define COUNTS_PER_G 4096.0
#define COUNTS_PER_DPS 16.4
#define DEG_TO_RAD 0.01745329251994329576923690768489

static void quaternion_normalize(double q[4])
{
    const double norm = sqrt(q[0] * q[0] + q[1] * q[1] +
                             q[2] * q[2] + q[3] * q[3]);
    if (norm == 0.0) return;
    for (unsigned axis = 0; axis < 4u; ++axis) q[axis] /= norm;
}

// Right-multiply XYZW orientation by an exact body-frame axis-angle delta.
// The production translator uses small Euler substeps; this exact integrator
// is an independent physical reference whose error should stay tiny at 800 Hz.
static void integrate_truth(double q[4], const int16_t gyro[3])
{
    const double dt = (double)SAMPLE_US / 1000000.0;
    const double wx = (double)gyro[0] / COUNTS_PER_DPS * DEG_TO_RAD;
    const double wy = (double)gyro[1] / COUNTS_PER_DPS * DEG_TO_RAD;
    const double wz = (double)gyro[2] / COUNTS_PER_DPS * DEG_TO_RAD;
    const double speed = sqrt(wx * wx + wy * wy + wz * wz);
    if (speed == 0.0) return;

    const double half = 0.5 * speed * dt;
    const double scale = sin(half) / speed;
    const double dx = wx * scale;
    const double dy = wy * scale;
    const double dz = wz * scale;
    const double dw = cos(half);
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = w * dx + x * dw + y * dz - z * dy;
    q[1] = w * dy - x * dz + y * dw + z * dx;
    q[2] = w * dz + x * dy - y * dx + z * dw;
    q[3] = w * dw - x * dx - y * dy - z * dz;
    quaternion_normalize(q);
}

// Rotate world gravity into the controller body frame: q^-1 * g * q.
static void gravity_body(const double q[4], int16_t accel[3])
{
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double gx = 2.0 * (x * z - w * y);
    const double gy = 2.0 * (y * z + w * x);
    const double gz = 1.0 - 2.0 * (x * x + y * y);
    const double value[3] = {gx * COUNTS_PER_G,
                             gy * COUNTS_PER_G,
                             gz * COUNTS_PER_G};
    for (unsigned axis = 0; axis < 3u; ++axis)
        accel[axis] = (int16_t)llround(value[axis]);
}

static void commanded_gyro(unsigned sample, int16_t gyro[3])
{
    gyro[0] = gyro[1] = gyro[2] = 0;
    // Forty-eight still samples make the production bias acquisition explicit.
    // The following three non-collinear plateaus exercise scale, axis order,
    // signs, and prefix epoch without crossing a carrier chart boundary.
    if (sample >= 48u && sample < 144u) {
        gyro[0] = 820;    // +50 dps
        gyro[1] = -410;   // -25 dps
        gyro[2] = 1230;   // +75 dps
    } else if (sample >= 144u && sample < 240u) {
        gyro[0] = -574;   // -35 dps
        gyro[1] = 984;    // +60 dps
        gyro[2] = 656;    // +40 dps
    } else if (sample >= 240u && sample < 336u) {
        gyro[0] = 410;    // +25 dps
        gyro[1] = 328;    // +20 dps
        gyro[2] = -1312;  // -80 dps
    }
}

static void print_hex(const uint8_t *bytes, unsigned length)
{
    for (unsigned i = 0; i < length; ++i) printf("%02X", bytes[i]);
}

int main(void)
{
    ns2_ds5_motion_state_t motion30;
    ns2_ds5_motion40_t motion40;
    switch_pro_input_t input;
    double truth[4] = {0.0, 0.0, 0.0, 1.0};
    uint8_t carrier[NS2_MOTION_PDU30_LENGTH];

    ns2_ds5_motion_reset(&motion30);
    ns2_ds5_motion40_reset(&motion40);
    memset(&input, 0, sizeof(input));
    input.has_motion = 1u;
    input.motion_source = SWITCH_MOTION_SOURCE_DUALSENSE;
    input.motion_timestamp_valid = 1u;

    for (unsigned sample = 0; sample < SAMPLE_COUNT; ++sample) {
        const uint32_t now_us = (sample + 1u) * SAMPLE_US;
        commanded_gyro(sample, input.gyro);
        if (sample != 0u) integrate_truth(truth, input.gyro);
        gravity_body(truth, input.accel);
        input.motion_sequence = sample + 1u;
        input.motion_timestamp = (sample + 1u) * SAMPLE_US * 3u;

        if (!ns2_ds5_motion_update(&motion30, &input, now_us)) continue;
        if (!ns2_ds5_motion_build(&motion30, carrier)) continue;

        uint32_t carrier_raw[3];
        if (!ns2_motion_pdu30_get_orientation(carrier, carrier_raw)) return 2;
        int16_t corrected[3];
        for (unsigned axis = 0; axis < 3u; ++axis)
            corrected[axis] = (int16_t)motion30.gyro_corrected[axis];
        ns2_ds5_motion40_sample(&motion40, input.accel, corrected,
                                carrier_raw, motion30.timing, now_us);

        printf("{\"kind\":\"sample\",\"index\":%u,\"us\":%lu,"
               "\"tick\":%u,\"bias_ready\":%s,"
               "\"gyro\":[%d,%d,%d],\"accel\":[%d,%d,%d],"
               "\"q_xyzw\":[%.12g,%.12g,%.12g,%.12g],"
               "\"truth_xyzw\":[%.12g,%.12g,%.12g,%.12g],"
               "\"carrier\":\"",
               sample, (unsigned long)now_us, motion30.timing & 0x0FFFu,
               motion30.bias_ready ? "true" : "false",
               corrected[0], corrected[1], corrected[2],
               input.accel[0], input.accel[1], input.accel[2],
               motion30.quaternion[0], motion30.quaternion[1],
               motion30.quaternion[2], motion30.quaternion[3],
               truth[0], truth[1], truth[2], truth[3]);
        print_hex(carrier, sizeof(carrier));
        puts("\"}");

        uint8_t selected[NS2_MOTION_PDU40_LENGTH];
        uint8_t selected_length = 0u;
        if (ns2_ds5_motion40_select(&motion40, carrier, selected,
                                    &selected_length)) {
            printf("{\"kind\":\"pdu\",\"sample_index\":%u,"
                   "\"length\":%u,\"payload\":\"",
                   sample, selected_length);
            print_hex(selected, selected_length);
            puts("\"}");
        }
    }

    printf("{\"kind\":\"summary\",\"samples\":%u,"
           "\"carriers\":%lu,\"batches\":%lu,\"held\":%lu,"
           "\"fallbacks\":%lu,\"starved\":%lu,"
           "\"overlong\":%lu,\"sat_accel\":%lu,"
           "\"sat_gyro\":%lu}\n",
           SAMPLE_COUNT, (unsigned long)motion40.carrier_frames,
           (unsigned long)motion40.emitted,
           (unsigned long)motion40.held_polls,
           (unsigned long)motion40.fallback_carriers,
           (unsigned long)motion40.skipped_no_samples,
           (unsigned long)motion40.skipped_overlong,
           (unsigned long)motion40.saturated_accel,
           (unsigned long)motion40.saturated_gyro);
    return 0;
}
