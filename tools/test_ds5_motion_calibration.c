#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ds5_motion_calibration.h"

static void put_le16(uint8_t *p, int16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)((uint16_t)value >> 8);
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

static void finish_crc(uint8_t report[DS5_MOTION_CALIBRATION_REPORT_LEN])
{
    const uint8_t seed = 0xA3;
    uint32_t crc = crc32_raw(0xFFFFFFFFu, &seed, 1);
    crc = ~crc32_raw(crc, report, DS5_MOTION_CALIBRATION_REPORT_LEN - 4);
    size_t offset = DS5_MOTION_CALIBRATION_REPORT_LEN - 4;
    report[offset + 0] = (uint8_t)crc;
    report[offset + 1] = (uint8_t)(crc >> 8);
    report[offset + 2] = (uint8_t)(crc >> 16);
    report[offset + 3] = (uint8_t)(crc >> 24);
}

static void make_identity_report(
    uint8_t report[DS5_MOTION_CALIBRATION_REPORT_LEN])
{
    memset(report, 0, DS5_MOTION_CALIBRATION_REPORT_LEN);
    report[0] = DS5_MOTION_CALIBRATION_REPORT_ID;

    // Gyro: speed_2x=2000 and each calibration span=32768 produces exactly
    // one PicoSwitch2 carrier count per raw controller count.
    const int16_t bias[3] = {100, -200, 50};
    for (unsigned axis = 0; axis < 3; ++axis) {
        put_le16(&report[1 + axis * 2], bias[axis]);
        put_le16(&report[7 + axis * 4], (int16_t)(bias[axis] + 16384));
        put_le16(&report[9 + axis * 4], (int16_t)(bias[axis] - 16384));
    }
    put_le16(&report[19], 1000);
    put_le16(&report[21], 1000);

    // Accel: +/-1g around a deliberately nonzero per-axis offset.
    const int16_t accel_bias[3] = {20, -35, 70};
    for (unsigned axis = 0; axis < 3; ++axis) {
        put_le16(&report[23 + axis * 4],
                 (int16_t)(accel_bias[axis] + 8192));
        put_le16(&report[25 + axis * 4],
                 (int16_t)(accel_bias[axis] - 8192));
    }
    finish_crc(report);
}

static void test_identity_scale_and_accel_bias(void)
{
    uint8_t report[DS5_MOTION_CALIBRATION_REPORT_LEN];
    ds5_motion_calibration_t calibration;
    make_identity_report(report);
    ds5_motion_calibration_reset(&calibration);
    assert(ds5_motion_calibration_parse(&calibration, report, sizeof(report)));
    assert(calibration.report_valid);

    const int16_t gyro[3] = {1234, -2345, 32767};
    const int16_t accel[3] = {8212, -8227, 70};
    int16_t gyro_out[3], accel_out[3];
    ds5_motion_calibration_apply(&calibration, gyro, accel,
                                 gyro_out, accel_out);
    // Linux deliberately uses gyro bias only to derive sensitivity; it does
    // not subtract it from live samples. Preserve that source-verified rule.
    assert(gyro_out[0] == 1234 && gyro_out[1] == -2345 &&
           gyro_out[2] == 32767);
    assert(accel_out[0] == 8192 && accel_out[1] == -8192 &&
           accel_out[2] == 0);
}

static void test_non_identity_scale_and_saturation(void)
{
    uint8_t report[DS5_MOTION_CALIBRATION_REPORT_LEN];
    ds5_motion_calibration_t calibration;
    make_identity_report(report);

    // Halve the X gyro calibration span, doubling its calibrated output.
    put_le16(&report[7], 100 + 8192);
    put_le16(&report[9], 100 - 8192);
    finish_crc(report);
    assert(ds5_motion_calibration_parse(&calibration, report, sizeof(report)));

    const int16_t gyro[3] = {1000, 0, 20000};
    const int16_t accel[3] = {0, 0, 0};
    int16_t gyro_out[3], accel_out[3];
    ds5_motion_calibration_apply(&calibration, gyro, accel,
                                 gyro_out, accel_out);
    assert(gyro_out[0] == 2000);
    assert(gyro_out[2] == 20000);

    const int16_t saturating[3] = {20000, 0, 0};
    ds5_motion_calibration_apply(&calibration, saturating, accel,
                                 gyro_out, accel_out);
    assert(gyro_out[0] == 32767);
}

static void test_invalid_framing_does_not_replace_calibration(void)
{
    uint8_t report[DS5_MOTION_CALIBRATION_REPORT_LEN];
    ds5_motion_calibration_t calibration;
    make_identity_report(report);
    ds5_motion_calibration_reset(&calibration);
    assert(ds5_motion_calibration_parse(&calibration, report, sizeof(report)));

    ds5_motion_calibration_t before = calibration;
    report[8] ^= 1;  // break the CRC
    assert(!ds5_motion_calibration_parse(&calibration, report, sizeof(report)));
    assert(memcmp(&calibration, &before, sizeof(calibration)) == 0);
    assert(!ds5_motion_calibration_parse(&calibration, report,
                                         sizeof(report) - 1));
    report[0] = 0x04;
    assert(!ds5_motion_calibration_parse(&calibration, report, sizeof(report)));
}

static void test_bad_axis_uses_native_fallback(void)
{
    uint8_t report[DS5_MOTION_CALIBRATION_REPORT_LEN];
    ds5_motion_calibration_t calibration;
    make_identity_report(report);

    // Zero X gyro denominator and Y accel denominator. Other axes remain
    // calibrated; only the malformed axes fall back to raw native units.
    put_le16(&report[7], 100);
    put_le16(&report[9], 100);
    put_le16(&report[27], -35);
    put_le16(&report[29], -35);
    finish_crc(report);
    assert(ds5_motion_calibration_parse(&calibration, report, sizeof(report)));
    assert(!calibration.gyro[0].valid && calibration.gyro[1].valid);
    assert(!calibration.accel[1].valid && calibration.accel[0].valid);

    const int16_t gyro[3] = {333, 444, 555};
    const int16_t accel[3] = {8212, 1234, 70};
    int16_t gyro_out[3], accel_out[3];
    ds5_motion_calibration_apply(&calibration, gyro, accel,
                                 gyro_out, accel_out);
    assert(gyro_out[0] == 333 && gyro_out[1] == 444);
    assert(accel_out[0] == 8192 && accel_out[1] == 1234);
}

int main(void)
{
    test_identity_scale_and_accel_bias();
    test_non_identity_scale_and_saturation();
    test_invalid_framing_does_not_replace_calibration();
    test_bad_axis_uses_native_fallback();
    puts("ds5_motion_calibration: all tests passed");
    return 0;
}
