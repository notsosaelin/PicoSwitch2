#include "ds5_motion_pair_capture.h"

#include <string.h>

#include "sw2_capture.h"

typedef struct {
    uint8_t valid;
    uint8_t calibration_state;
    uint32_t sequence;
    uint32_t captured_us;
    uint32_t sensor_timestamp;
    int16_t raw_gyro[3];
    int16_t raw_accel[3];
    int16_t calibrated_gyro[3];
    int16_t calibrated_accel[3];
} ds5_latest_motion_t;

// Both producer functions run in BTstack/core1 callback context, so the latest
// sample needs no inter-core lock. Only the ring shared with UART/core0 is
// protected, and the critical section contains one fixed-size structure copy.
static ds5_latest_motion_t s_latest;

void ds5_motion_pair_update_ds5(uint32_t captured_us,
                                uint32_t sensor_timestamp,
                                const int16_t raw_gyro[3],
                                const int16_t raw_accel[3],
                                const int16_t calibrated_gyro[3],
                                const int16_t calibrated_accel[3],
                                uint8_t calibration_state)
{
    if (!raw_gyro || !raw_accel || !calibrated_gyro || !calibrated_accel)
        return;
    s_latest.valid = 1;
    s_latest.calibration_state = calibration_state;
    s_latest.sequence++;
    s_latest.captured_us = captured_us;
    s_latest.sensor_timestamp = sensor_timestamp;
    memcpy(s_latest.raw_gyro, raw_gyro, sizeof(s_latest.raw_gyro));
    memcpy(s_latest.raw_accel, raw_accel, sizeof(s_latest.raw_accel));
    memcpy(s_latest.calibrated_gyro, calibrated_gyro,
           sizeof(s_latest.calibrated_gyro));
    memcpy(s_latest.calibrated_accel, calibrated_accel,
           sizeof(s_latest.calibrated_accel));
}

void ds5_motion_pair_record_native(uint32_t captured_us,
                                   const uint8_t *native,
                                   uint8_t native_length)
{
    if (!ds5_motion_pair_get_enabled() || !native ||
        (native_length != 0x1Eu && native_length != 0x28u))
        return;

    ds5_motion_pair_record_t record;
    memset(&record, 0, sizeof(record));
    record.native_us = captured_us;
    record.native_length = native_length;
    memcpy(record.native, native, native_length);
    record.ds5_valid = s_latest.valid;
    record.calibration_state = s_latest.calibration_state;
    record.ds5_sequence = s_latest.sequence;
    record.ds5_us = s_latest.captured_us;
    record.ds5_sensor_timestamp = s_latest.sensor_timestamp;
    memcpy(record.raw_gyro, s_latest.raw_gyro, sizeof(record.raw_gyro));
    memcpy(record.raw_accel, s_latest.raw_accel, sizeof(record.raw_accel));
    memcpy(record.calibrated_gyro, s_latest.calibrated_gyro,
           sizeof(record.calibrated_gyro));
    memcpy(record.calibrated_accel, s_latest.calibrated_accel,
           sizeof(record.calibrated_accel));

    _Static_assert(sizeof(record) <= SW2_CAP_MAX_DATA,
                   "paired motion record exceeds shared capture entry");
    sw2_capture_pair_record((const uint8_t *)&record, sizeof(record));
}

void ds5_motion_pair_set_enabled(bool enabled)
{
    sw2_capture_pair_set_enabled(enabled);
}

bool ds5_motion_pair_get_enabled(void)
{
    return sw2_capture_pair_get_enabled();
}

uint16_t ds5_motion_pair_buffered_count(void)
{
    return sw2_capture_buffered_count();
}

uint32_t ds5_motion_pair_dropped_count(void)
{
    return sw2_capture_dropped_count();
}

bool ds5_motion_pair_drain_one(ds5_motion_pair_record_t *out)
{
    if (!out) return false;
    sw2_cap_entry_t entry;
    if (!sw2_capture_drain_one(&entry) ||
        entry.kind != SW2_CAP_MOTION_PAIR ||
        entry.len != sizeof(*out) || entry.orig_len != sizeof(*out))
        return false;
    memcpy(out, entry.data, sizeof(*out));
    return true;
}
