#ifndef NS2_DS5_MOTION_H
#define NS2_DS5_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#include "switch_pro.h"

typedef enum {
    // Hardware-compatible bounded quaternion carrier for translated sources.
    // Components are numbered w/x/y/z, so identity is state 0. The genuine
    // carrier shares the cyclic chart order and paired sign-branch transition
    // law, but direct captures refute strict smallest-three as its exact
    // internal model. Keep that evidence boundary when changing this encoder.
    NS2_DS5_CARRIER_SWITCH2_WXYZ = 0,

    // Nintendo's Switch-1 DScale model retained as an explicit diagnostic
    // control: x/y/z/w state numbering, therefore identity is state 3.
    NS2_DS5_CARRIER_SWITCH1_DSCALE = 1,

    // Temporary compatibility path retained only for UART A/B testing of
    // the original fixed-state hypothesis.
    NS2_DS5_CARRIER_LEGACY_STATE0 = 2,
} ns2_ds5_motion_carrier_t;

// DualSense -> native Switch 2 report-0x09 motion translator.
//
// This state is owned by USB/core0. The normalized input snapshot has already
// crossed the report.c seqlock before it reaches this module, so no additional
// cross-core synchronization is needed here.
typedef struct {
    float quaternion[4];  // canonical [x, y, z, w]
    int32_t gyro_lp[3];   // normalized carrier gyro, << 6
    int32_t gyro_bias[3]; // runtime zero-rate estimate, << 6
    int32_t gyro_prev[3];
    int32_t gyro_jitter[3];
    int32_t gyro_corrected[3]; // raw de-biased sample used for integration
    int16_t accel[3];
    uint32_t last_update_us;
    uint32_t last_sensor_timestamp;
    uint32_t last_host_elapsed_us;
    uint32_t last_sensor_elapsed_us;
    uint32_t max_sensor_elapsed_us;
    uint32_t sensor_timestamp_fallbacks;
    uint32_t sensor_timestamp_invalid;
    uint32_t last_motion_sequence;
    uint32_t sequence_gaps;
    uint32_t integration_substeps;
    uint16_t imu_tick;
    uint16_t timing;
    uint32_t updates;
    uint32_t representation_rejects;
    uint16_t bias_warmup_samples;
    // Signed 1-based normalized input-axis selection for canonical X/Y/Z.
    // Example {-2,+1,+3}: X=-inputY, Y=+inputX, Z=+inputZ.
    int8_t gyro_map[3];
    ns2_ds5_motion_carrier_t carrier;
    // Current translated-carrier chart in wire w/x/y/z order. Genuine
    // hardware also retains its chart until a transmitted lane reaches the
    // boundary, but its full carrier is not exact strict smallest-three.
    uint8_t switch2_omitted;
    bool body_frame; // true: q' = q*w (body-local); false: q' = w*q
    bool bias_ready;
    bool sensor_timestamp_initialized;
    bool initialized;
    bool has_sample;
} ns2_ds5_motion_state_t;

void ns2_ds5_motion_reset(ns2_ds5_motion_state_t *state);

void ns2_ds5_motion_set_body_frame(ns2_ds5_motion_state_t *state,
                                   bool body_frame);
bool ns2_ds5_motion_set_gyro_map(ns2_ds5_motion_state_t *state,
                                const int8_t map[3]);
void ns2_ds5_motion_set_carrier(ns2_ds5_motion_state_t *state,
                                ns2_ds5_motion_carrier_t carrier);

// Consume the latest normalized DualSense sample. Calls faster than the
// translator's 250 Hz cadence are ignored; elapsed time, rather than call
// count, drives both timing and quaternion integration.
bool ns2_ds5_motion_update(ns2_ds5_motion_state_t *state,
                           const switch_pro_input_t *input,
                           uint32_t now_us);

// Build one native length-30 motion PDU. Returns false until a sample has been
// accepted or if the integrated quaternion cannot fit this bounded translated
// carrier. All four w/x/y/z chart states are supported.
bool ns2_ds5_motion_build(ns2_ds5_motion_state_t *state, uint8_t out[30]);

#endif  // NS2_DS5_MOTION_H
