#ifndef NS2_MOTION_HYBRID_LIVE_H
#define NS2_MOTION_HYBRID_LIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "ns2_motion_hybrid_projector.h"

typedef enum {
    NS2_MOTION_HYBRID_MODE_OFF = 0,
    NS2_MOTION_HYBRID_MODE_GENUINE,
    NS2_MOTION_HYBRID_MODE_ACCEL,
    NS2_MOTION_HYBRID_MODE_GYRO,
    NS2_MOTION_HYBRID_MODE_PREFIX,
    NS2_MOTION_HYBRID_MODE_IMU,
    NS2_MOTION_HYBRID_MODE_ALL,
    NS2_MOTION_HYBRID_MODE_COUNT,
} ns2_motion_hybrid_mode_t;

#define NS2_MOTION_HYBRID_REASON_COUNT \
    (NS2_MOTION_HYBRID_LIVE_SPLICE_FAILED + 1u)

typedef struct {
    uint32_t native_us;
    uint32_t ds5_age_us;
    uint32_t ds5_sequence;
    uint32_t requested_groups;
    uint16_t changed_bits;
    uint8_t length;
    uint8_t mode;
    uint8_t reason;
    uint8_t calibration_state;
    uint8_t pose_aligned;
    uint8_t reserved;
    uint8_t base[40];
    uint8_t output_xor[40];
} ns2_motion_hybrid_capture_record_t;

typedef struct {
    uint8_t requested_mode;
    uint8_t active_mode;
    uint8_t pose_aligned;
    uint8_t calibration_state;
    uint8_t last_reason;
    uint8_t last_length;
    uint16_t last_changed_bits;
    uint32_t last_groups;
    uint32_t last_ds5_age_us;
    uint32_t last_ds5_sequence;
    uint32_t source_last_us;
    uint32_t source_sequence;
    uint8_t source_calibration_state;
    uint8_t source_reserved[3];
    uint32_t native_packets;
    uint32_t hybrid_packets;
    uint32_t genuine_controls;
    uint32_t fallback_packets;
    uint32_t saturated_accel;
    uint32_t saturated_gyro;
    uint32_t reasons[NS2_MOTION_HYBRID_REASON_COUNT];
} ns2_motion_hybrid_live_diag_t;

// Core1 DS5 decoder publication. Raw Sony axes are normalized here through
// the established motion seam; provenance is the call site in ds5_bt.
void ns2_motion_hybrid_live_update_ds5(
    uint32_t captured_us, uint32_t sensor_timestamp,
    const int16_t calibrated_gyro[3], const int16_t calibrated_accel[3],
    uint8_t calibration_state);

// Core1 genuine-PDU hook. `out` always starts as the exact base. Returns true
// only when a non-off mode authored a mode-matched output snapshot; failures
// are represented by a byte-identical genuine fallback and a reason code.
bool ns2_motion_hybrid_live_project_native(
    uint32_t captured_us, const uint8_t *base, uint8_t length,
    uint8_t out[40], uint8_t *mode, uint8_t *reason,
    uint32_t *groups);

// Immediately make the next USB selection ignore any retained hybrid. The
// projector itself resets on core1 at the next source event.
bool ns2_motion_hybrid_live_set_mode(uint8_t mode);
uint8_t ns2_motion_hybrid_live_get_mode(void);
const char *ns2_motion_hybrid_mode_name(uint8_t mode);
void ns2_motion_hybrid_live_source_disconnected(void);

void ns2_motion_hybrid_live_get_diag(ns2_motion_hybrid_live_diag_t *out);

void ns2_motion_hybrid_live_capture_set_enabled(bool enabled);
bool ns2_motion_hybrid_live_capture_get_enabled(void);
uint16_t ns2_motion_hybrid_live_capture_count(void);
uint32_t ns2_motion_hybrid_live_capture_dropped(void);
bool ns2_motion_hybrid_live_capture_drain(
    ns2_motion_hybrid_capture_record_t *out);

#endif  // NS2_MOTION_HYBRID_LIVE_H
