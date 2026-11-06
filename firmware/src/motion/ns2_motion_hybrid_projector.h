#ifndef NS2_MOTION_HYBRID_PROJECTOR_H
#define NS2_MOTION_HYBRID_PROJECTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "ns2_ds5_motion.h"
#include "ns2_motion_pdu.h"

// Pure, transport-independent live fitment core. The firmware wrapper feeds
// it normalized DualSense samples and genuine controller-authored PDUs; host
// tests replay the same API without UART, Bluetooth, or USB dependencies.
#define NS2_MOTION_HYBRID_SAMPLE_RING 32u
#define NS2_MOTION_HYBRID_MAX_DONOR_AGE_US 20000u
#define NS2_MOTION_HYBRID_CALIBRATION_READY 2u

typedef enum {
    NS2_MOTION_HYBRID_LIVE_APPLIED = 0,
    NS2_MOTION_HYBRID_LIVE_GENUINE_CONTROL,
    NS2_MOTION_HYBRID_LIVE_PASSTHROUGH_30,
    NS2_MOTION_HYBRID_LIVE_WAIT_DS5,
    NS2_MOTION_HYBRID_LIVE_WAIT_CALIBRATION,
    NS2_MOTION_HYBRID_LIVE_WAIT_BIAS,
    NS2_MOTION_HYBRID_LIVE_WAIT_POSE,
    NS2_MOTION_HYBRID_LIVE_STALE_DONOR,
    NS2_MOTION_HYBRID_LIVE_SOURCE_NOT_ADVANCED,
    NS2_MOTION_HYBRID_LIVE_UNSUPPORTED_LAYOUT,
    NS2_MOTION_HYBRID_LIVE_NO_WINDOW,
    NS2_MOTION_HYBRID_LIVE_DONOR_BUILD_FAILED,
    NS2_MOTION_HYBRID_LIVE_SPLICE_FAILED,
} ns2_motion_hybrid_live_reason_t;

typedef struct {
    uint32_t us;
    uint32_t sequence;
    uint32_t sensor_timestamp;
    int16_t accel[3];
    int16_t gyro[3];
    uint32_t carrier[3];
} ns2_motion_hybrid_sample_t;

typedef struct {
    ns2_ds5_motion_state_t translator;
    ns2_motion_hybrid_sample_t ring[NS2_MOTION_HYBRID_SAMPLE_RING];
    ns2_motion_hybrid_sample_t latest;
    uint8_t head;
    uint8_t filled;
    uint8_t latest_valid;
    uint8_t calibration_state;
    uint8_t pose_aligned;
    uint8_t aligned_chart;
    uint32_t last_project_sequence;
} ns2_motion_hybrid_projector_t;

typedef struct {
    ns2_motion_hybrid_live_reason_t reason;
    uint32_t requested_groups;
    uint32_t applied_groups;
    uint32_t ds5_age_us;
    uint32_t ds5_sequence;
    uint16_t changed_bits;
    uint8_t calibration_state;
    uint8_t pose_aligned;
    uint8_t saturated_accel;
    uint8_t saturated_gyro;
} ns2_motion_hybrid_project_result_t;

void ns2_motion_hybrid_projector_reset(
    ns2_motion_hybrid_projector_t *state);

// Inputs are already in the Pro Controller 2 frame and shared interchange
// scale: acceleration 4096 counts/g, gyro approximately 16.4 counts/dps.
void ns2_motion_hybrid_projector_push(
    ns2_motion_hybrid_projector_t *state, uint32_t captured_us,
    uint32_t sensor_timestamp, uint32_t sequence,
    const int16_t accel[3], const int16_t gyro[3],
    uint8_t calibration_state);

// A genuine 0x1E carrier supplies the absolute chart anchor and a gravity
// pose check. Until this succeeds no physical donor group is substituted.
bool ns2_motion_hybrid_projector_observe_carrier(
    ns2_motion_hybrid_projector_t *state,
    const uint8_t genuine[NS2_MOTION_PDU30_LENGTH], uint32_t captured_us,
    ns2_motion_hybrid_live_reason_t *reason);

// Build a donor on the genuine packet's elapsed/layout boundary, then copy
// only requested groups. Prefix ownership is sequence-wide: when requested,
// it replaces the orientation carrier in both interleaved 0x1E and high-rate
// 0x28 PDUs so the console never alternates genuine and donor histories.
// `out` is initialized to `base` and remains genuine before ownership is
// acquired or on a hard construction failure.
bool ns2_motion_hybrid_projector_project(
    ns2_motion_hybrid_projector_t *state, const uint8_t *base,
    uint8_t length, uint32_t captured_us, uint32_t groups, uint8_t *out,
    ns2_motion_hybrid_project_result_t *result);

const char *ns2_motion_hybrid_live_reason_name(
    ns2_motion_hybrid_live_reason_t reason);

#endif  // NS2_MOTION_HYBRID_PROJECTOR_H
