#ifndef DS5_MOTION_CALIBRATION_H
#define DS5_MOTION_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS5_MOTION_CALIBRATION_REPORT_ID 0x05u
#define DS5_MOTION_CALIBRATION_REPORT_LEN 41u

typedef struct {
    int32_t bias;
    int64_t sens_numer;
    int32_t sens_denom;
    bool valid;
} ds5_motion_axis_calibration_t;

typedef struct {
    ds5_motion_axis_calibration_t gyro[3];
    ds5_motion_axis_calibration_t accel[3];
    bool report_valid;
} ds5_motion_calibration_t;

// Reset to the native DualSense carrier. Raw DS5 gyro is already approximately
// 16.384 counts/(degree/s), and raw acceleration is 8192 counts/g, matching the
// int16_t convention used by PicoSwitch2's input_event_t.
void ds5_motion_calibration_reset(ds5_motion_calibration_t *calibration);

// Parse and CRC-check the Bluetooth feature report returned by GET_REPORT 0x05.
// The report includes its report ID at byte zero and its little-endian CRC32 in
// the final four bytes. Returns false without changing `calibration` when its
// framing or CRC is invalid. A correctly framed report can still contain an
// invalid individual denominator; that axis safely retains native scaling.
bool ds5_motion_calibration_parse(ds5_motion_calibration_t *calibration,
                                  const uint8_t *report, size_t length);

// Convert one raw DualSense sample into PicoSwitch2's int16 motion carrier.
// Calibration is applied independently per axis, with saturating output.
void ds5_motion_calibration_apply(const ds5_motion_calibration_t *calibration,
                                  const int16_t raw_gyro[3],
                                  const int16_t raw_accel[3],
                                  int16_t gyro_out[3],
                                  int16_t accel_out[3]);

#endif  // DS5_MOTION_CALIBRATION_H
