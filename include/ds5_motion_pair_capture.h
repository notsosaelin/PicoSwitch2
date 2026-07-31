#ifndef DS5_MOTION_PAIR_CAPTURE_H
#define DS5_MOTION_PAIR_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "ds5_motion_chart_trigger.h"

#define DS5_MOTION_PAIR_NATIVE_MAX 40u
#define DS5_MOTION_PAIR_CAPACITY 127u

typedef struct {
    uint32_t native_us;
    uint8_t native_length;
    uint8_t native[DS5_MOTION_PAIR_NATIVE_MAX];

    uint8_t ds5_valid;
    uint8_t calibration_state;
    uint32_t ds5_sequence;
    uint32_t ds5_us;
    uint32_t ds5_sensor_timestamp;
    int16_t raw_gyro[3];
    int16_t raw_accel[3];
    int16_t calibrated_gyro[3];
    int16_t calibrated_accel[3];
} ds5_motion_pair_record_t;

// Core1 producer: update the latest DualSense sample. This never writes the
// capture ring; a paired record is created only when a genuine Pro2 native PDU
// arrives, giving every row one directly time-aligned oracle output.
void ds5_motion_pair_update_ds5(uint32_t captured_us,
                                uint32_t sensor_timestamp,
                                const int16_t raw_gyro[3],
                                const int16_t raw_accel[3],
                                const int16_t calibrated_gyro[3],
                                const int16_t calibrated_accel[3],
                                uint8_t calibration_state);

// Core1 producer: snapshot the latest DS5 sample beside an exact genuine Pro2
// native PDU. No-op unless explicitly enabled over UART.
void ds5_motion_pair_record_native(uint32_t captured_us,
                                   const uint8_t *native,
                                   uint8_t native_length);

// Core0/UART control and drain API.
void ds5_motion_pair_set_enabled(bool enabled);
void ds5_motion_pair_arm_chart_trigger(void);
void ds5_motion_pair_arm_chart_trigger_mask(uint8_t target_mask);
bool ds5_motion_pair_get_enabled(void);
uint16_t ds5_motion_pair_buffered_count(void);
uint32_t ds5_motion_pair_dropped_count(void);
bool ds5_motion_pair_drain_one(ds5_motion_pair_record_t *out);
void ds5_motion_pair_chart_status(ds5_motion_chart_trigger_t *out);

#endif  // DS5_MOTION_PAIR_CAPTURE_H
