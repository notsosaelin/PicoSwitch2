#include "ds5_motion_calibration.h"

#include <limits.h>
#include <string.h>

// Linux hid-playstation normalizes DualSense gyro to 1024 counts/(degree/s),
// but that range requires a 32-bit input axis. PicoSwitch2's common carrier is
// int16_t over +/-2000 degree/s, or 32768/2000 = 16.384 counts/(degree/s).
#define DS5_GYRO_CARRIER_RANGE 32768
#define DS5_GYRO_RANGE_DPS     2000
#define DS5_ACCEL_COUNTS_PER_G 8192

static int16_t read_le16s(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t crc32_raw(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                  (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc;
}

static uint32_t feature_crc32(const uint8_t *report, size_t length_without_crc)
{
    const uint8_t seed = 0xA3;
    uint32_t crc = crc32_raw(0xFFFFFFFFu, &seed, 1);
    return ~crc32_raw(crc, report, length_without_crc);
}

static int32_t abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

static int16_t clamp_i16(int64_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

void ds5_motion_calibration_reset(ds5_motion_calibration_t *calibration)
{
    if (!calibration) return;
    memset(calibration, 0, sizeof(*calibration));
    for (unsigned axis = 0; axis < 3; ++axis) {
        calibration->gyro[axis].sens_numer = 1;
        calibration->gyro[axis].sens_denom = 1;
        calibration->accel[axis].sens_numer = 1;
        calibration->accel[axis].sens_denom = 1;
    }
}

bool ds5_motion_calibration_parse(ds5_motion_calibration_t *calibration,
                                  const uint8_t *report, size_t length)
{
    if (!calibration || !report ||
        length != DS5_MOTION_CALIBRATION_REPORT_LEN ||
        report[0] != DS5_MOTION_CALIBRATION_REPORT_ID)
        return false;

    uint32_t expected_crc = read_le32(&report[length - 4]);
    if (feature_crc32(report, length - 4) != expected_crc)
        return false;

    ds5_motion_calibration_t parsed;
    ds5_motion_calibration_reset(&parsed);

    const int32_t gyro_bias[3] = {
        read_le16s(&report[1]), read_le16s(&report[3]), read_le16s(&report[5])
    };
    const int32_t gyro_plus[3] = {
        read_le16s(&report[7]), read_le16s(&report[11]), read_le16s(&report[15])
    };
    const int32_t gyro_minus[3] = {
        read_le16s(&report[9]), read_le16s(&report[13]), read_le16s(&report[17])
    };
    const int32_t speed_2x =
        (int32_t)read_le16s(&report[19]) + read_le16s(&report[21]);

    for (unsigned axis = 0; axis < 3; ++axis) {
        int32_t denom = abs_i32(gyro_plus[axis] - gyro_bias[axis]) +
                        abs_i32(gyro_minus[axis] - gyro_bias[axis]);
        if (speed_2x > 0 && denom > 0) {
            // physical dps = speed_2x * raw / denom. Scale directly into the
            // int16 carrier instead of producing Linux's int32 1024-count/dps
            // value and overflowing it during a later narrowing conversion.
            parsed.gyro[axis].sens_numer =
                (int64_t)speed_2x * DS5_GYRO_CARRIER_RANGE;
            parsed.gyro[axis].sens_denom = denom * DS5_GYRO_RANGE_DPS;
            parsed.gyro[axis].valid = true;
        }
    }

    for (unsigned axis = 0; axis < 3; ++axis) {
        size_t offset = 23u + axis * 4u;
        int32_t plus = read_le16s(&report[offset]);
        int32_t minus = read_le16s(&report[offset + 2]);
        int32_t range_2g = plus - minus;
        if (range_2g != 0) {
            parsed.accel[axis].bias = plus - range_2g / 2;
            parsed.accel[axis].sens_numer = 2 * DS5_ACCEL_COUNTS_PER_G;
            parsed.accel[axis].sens_denom = range_2g;
            parsed.accel[axis].valid = true;
        }
    }

    parsed.report_valid = true;
    *calibration = parsed;
    return true;
}

void ds5_motion_calibration_apply(const ds5_motion_calibration_t *calibration,
                                  const int16_t raw_gyro[3],
                                  const int16_t raw_accel[3],
                                  int16_t gyro_out[3],
                                  int16_t accel_out[3])
{
    if (!raw_gyro || !raw_accel || !gyro_out || !accel_out) return;

    for (unsigned axis = 0; axis < 3; ++axis) {
        const ds5_motion_axis_calibration_t *g =
            calibration ? &calibration->gyro[axis] : NULL;
        const ds5_motion_axis_calibration_t *a =
            calibration ? &calibration->accel[axis] : NULL;

        int64_t gyro = raw_gyro[axis];
        if (g && g->valid && g->sens_denom != 0)
            gyro = (g->sens_numer * raw_gyro[axis]) / g->sens_denom;

        int64_t accel = raw_accel[axis];
        if (a && a->valid && a->sens_denom != 0)
            accel = (a->sens_numer *
                     ((int32_t)raw_accel[axis] - a->bias)) /
                    a->sens_denom;

        gyro_out[axis] = clamp_i16(gyro);
        accel_out[axis] = clamp_i16(accel);
    }
}
