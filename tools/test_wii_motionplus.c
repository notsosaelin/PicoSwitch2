/*
 * Host tests for the Wii MotionPlus decode (src/bt_hid/motion/wii_motionplus.c).
 *
 * Every expectation is taken from docs/bluetooth/wii-motion.md §6, whose field
 * layout is Confirmed there by two independent implementations agreeing
 * bit-for-bit (Linux hid-wiimote and Dolphin). The point of these tests is to
 * pin the traps that section calls out -- above all the cross-byte slow bits,
 * where the PITCH slow bit lives in byte [3] rather than with pitch's own high
 * bits, which is the classic "two axes work, one goes wrong only at speed" bug.
 *
 * Build: part of the host-tests target. Run: build/host-tests/test_wii_motionplus
 */
#include "wii_motionplus.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
} while (0)

// Build a frame from raw 14-bit values and flags, mirroring §6.4 exactly.
static void make_frame(uint8_t out[6], uint16_t yaw, uint16_t roll, uint16_t pitch,
                       bool yaw_slow, bool roll_slow, bool pitch_slow,
                       bool ext_connected, bool is_mp)
{
    out[0] = (uint8_t)(yaw   & 0xFF);
    out[1] = (uint8_t)(roll  & 0xFF);
    out[2] = (uint8_t)(pitch & 0xFF);
    out[3] = (uint8_t)(((yaw   >> 8) & 0x3F) << 2);
    out[4] = (uint8_t)(((roll  >> 8) & 0x3F) << 2);
    out[5] = (uint8_t)(((pitch >> 8) & 0x3F) << 2);
    if (yaw_slow)      out[3] |= 0x02;
    if (pitch_slow)    out[3] |= 0x01;   // NOTE: byte [3], not [5]
    if (roll_slow)     out[4] |= 0x02;
    if (ext_connected) out[4] |= 0x01;
    if (is_mp)         out[5] |= 0x02;
}

static void test_frame_discrimination(void)
{
    uint8_t f[6];
    make_frame(f, 8192, 8192, 8192, false, false, false, false, true);
    CHECK(wii_mp_is_motionplus_frame(f), "is_mp_data set must be recognised");

    make_frame(f, 8192, 8192, 8192, false, false, false, false, false);
    CHECK(!wii_mp_is_motionplus_frame(f),
          "a passthrough extension frame must not be taken for MotionPlus data");

    wii_mp_sample_t s;
    CHECK(!wii_mp_decode(f, &s), "decode must refuse a non-MotionPlus frame");
}

static void test_axis_assembly(void)
{
    uint8_t f[6];
    wii_mp_sample_t s;

    // Zero rate: the sensor reports mid-scale when not rotating.
    make_frame(f, 8192, 8192, 8192, false, false, false, false, true);
    CHECK(wii_mp_decode(f, &s), "decode of a valid frame");
    CHECK(s.yaw == 0 && s.roll == 0 && s.pitch == 0,
          "8192 must decode to zero-centred 0 (got %d/%d/%d)", s.yaw, s.roll, s.pitch);

    // Distinct values per axis prove no cross-wiring between the three lanes.
    make_frame(f, 8192 + 1000, 8192 - 2000, 8192 + 3000,
               false, false, false, false, true);
    CHECK(wii_mp_decode(f, &s), "decode of distinct axes");
    CHECK(s.yaw   ==  1000, "yaw assembly (got %d)",   s.yaw);
    CHECK(s.roll  == -2000, "roll assembly (got %d)",  s.roll);
    CHECK(s.pitch ==  3000, "pitch assembly (got %d)", s.pitch);

    // Full 14-bit span must survive the 6-bit high nibble packing.
    make_frame(f, 16383, 0, 8192, false, false, false, false, true);
    CHECK(wii_mp_decode(f, &s), "decode at the 14-bit endpoints");
    CHECK(s.yaw  ==  16383 - 8192, "yaw at max (got %d)", s.yaw);
    CHECK(s.roll == -8192,         "roll at min (got %d)", s.roll);
}

// The trap from §6.4: pitch's slow bit is in byte [3], with yaw's high bits.
static void test_slow_bits_cross_byte(void)
{
    uint8_t f[6];
    wii_mp_sample_t s;

    make_frame(f, 8192, 8192, 8192, false, false, true, false, true);
    CHECK(wii_mp_decode(f, &s), "decode with pitch slow");
    CHECK(s.pitch_slow, "pitch slow bit must be read from byte [3] bit 0");
    CHECK(!s.yaw_slow && !s.roll_slow, "only pitch should be slow here");

    make_frame(f, 8192, 8192, 8192, true, false, false, false, true);
    CHECK(wii_mp_decode(f, &s), "decode with yaw slow");
    CHECK(s.yaw_slow && !s.pitch_slow && !s.roll_slow,
          "yaw slow must not be confused with pitch slow (same byte)");

    make_frame(f, 8192, 8192, 8192, false, true, false, true, true);
    CHECK(wii_mp_decode(f, &s), "decode with roll slow + extension");
    CHECK(s.roll_slow, "roll slow bit is byte [4] bit 1");
    CHECK(s.ext_connected, "ext-connected is byte [4] bit 0");
}

// A slow sample must convert to a smaller rate than the same raw counts read as
// fast -- the ~4.5x ratio is the one Confirmed part of the scale story (§6.10).
static void test_dual_range_ratio(void)
{
    const int32_t raw = 4096;   // half of full deflection
    int32_t slow = wii_mp_axis_centi_dps(raw, true,  0, 0, 0, false);
    int32_t fast = wii_mp_axis_centi_dps(raw, false, 0, 0, 0, false);
    CHECK(slow != 0 && fast != 0, "fallback conversion must produce a rate");
    CHECK(fast > slow, "fast range must yield the larger rate (slow=%d fast=%d)",
          slow, fast);
    const int32_t ratio_x100 = (fast * 100) / slow;
    CHECK(ratio_x100 > 400 && ratio_x100 < 500,
          "fast/slow ratio should be ~4.5x (got %d.%02d)",
          ratio_x100 / 100, ratio_x100 % 100);
}

// Sign must follow the raw deflection in the fallback path.
static void test_fallback_signs(void)
{
    CHECK(wii_mp_axis_centi_dps( 1000, false, 0, 0, 0, false) > 0,
          "positive deflection must give positive rate");
    CHECK(wii_mp_axis_centi_dps(-1000, false, 0, 0, 0, false) < 0,
          "negative deflection must give negative rate");
    CHECK(wii_mp_axis_centi_dps(0, false, 0, 0, 0, false) == 0,
          "zero deflection must give zero rate");
}

static uint32_t crc32_ref(const uint8_t *d, size_t n, uint32_t crc)
{
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

static void test_calibration_parse_and_crc(void)
{
    uint8_t blk[32];
    memset(blk, 0, sizeof(blk));
    // fast block: zero 0x2000, scale 0x2000+0x4400, 270 deg/s at the scale point
    const uint16_t zero = 0x2000, scale = 0x6400;
    for (int i = 0; i < 3; i++) {
        blk[0x00 + i*2] = (uint8_t)(zero >> 8);  blk[0x01 + i*2] = (uint8_t)zero;
        blk[0x06 + i*2] = (uint8_t)(scale >> 8); blk[0x07 + i*2] = (uint8_t)scale;
        blk[0x10 + i*2] = (uint8_t)(zero >> 8);  blk[0x11 + i*2] = (uint8_t)zero;
        blk[0x16 + i*2] = (uint8_t)(scale >> 8); blk[0x17 + i*2] = (uint8_t)scale;
    }
    blk[0x0C] = 45;   // 45 * 6 = 270 deg/s
    blk[0x1C] = 45;

    uint32_t crc = crc32_ref(blk + 0x00, 14, 0xFFFFFFFFu);
    crc = crc32_ref(blk + 0x10, 14, crc) ^ 0xFFFFFFFFu;
    blk[0x0E] = (uint8_t)(crc >> 24); blk[0x0F] = (uint8_t)(crc >> 16);
    blk[0x1E] = (uint8_t)(crc >> 8);  blk[0x1F] = (uint8_t)crc;

    wii_mp_cal_t cal;
    CHECK(wii_mp_parse_calibration(blk, &cal), "a well-formed block must verify");
    CHECK(cal.valid, "valid flag must be set");
    CHECK(cal.fast.yaw_zero == zero, "big-endian zero parse (got 0x%04X)",
          cal.fast.yaw_zero);
    CHECK(cal.fast.yaw_scale == scale, "big-endian scale parse (got 0x%04X)",
          cal.fast.yaw_scale);
    CHECK(cal.fast.degrees_div_6 == 45, "degrees_div_6 parse");

    // A corrupt block must be rejected rather than silently calibrating garbage.
    blk[3] ^= 0xFF;
    CHECK(!wii_mp_parse_calibration(blk, &cal), "corrupt block must fail CRC");
    CHECK(!cal.valid, "valid flag must be clear after a failed CRC");
}

// (scale - zero) may be negative; its sign encodes axis polarity and must not be
// absolute-valued (§6.8).
static void test_calibrated_polarity(void)
{
    // scale above zero -> positive deflection yields positive rate
    int32_t pos = wii_mp_axis_centi_dps(1000, false, 0x2000, 0x6400, 45, true);
    // scale below zero -> the same deflection must invert
    int32_t neg = wii_mp_axis_centi_dps(1000, false, 0x2000, 0x0000, 45, true);
    CHECK(pos > 0, "scale > zero must keep polarity (got %d)", pos);
    CHECK(neg < 0, "scale < zero must invert polarity (got %d)", neg);
}

static void test_per_axis_block_selection(void)
{
    // Distinct degrees_div_6 per block makes the selection observable.
    wii_mp_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.valid = true;
    cal.fast.yaw_zero = cal.fast.roll_zero = cal.fast.pitch_zero = 0x2000;
    cal.fast.yaw_scale = cal.fast.roll_scale = cal.fast.pitch_scale = 0x6400;
    cal.fast.degrees_div_6 = 200;                 // 1200 deg/s
    cal.slow = cal.fast;
    cal.slow.degrees_div_6 = 45;                  // 270 deg/s

    wii_mp_sample_t s;
    memset(&s, 0, sizeof(s));
    s.yaw = s.roll = s.pitch = 1000;
    s.yaw_slow = false; s.roll_slow = false; s.pitch_slow = true;

    int32_t out[3];
    wii_mp_sample_centi_dps(&s, &cal, out);
    CHECK(out[0] == out[1], "yaw and roll both used the fast block");
    CHECK(out[2] != out[0],
          "pitch used the slow block independently (yaw=%d pitch=%d)",
          out[0], out[2]);
    CHECK(out[2] < out[0], "the slow block must yield the smaller rate");
}

int main(void)
{
    test_frame_discrimination();
    test_axis_assembly();
    test_slow_bits_cross_byte();
    test_dual_range_ratio();
    test_fallback_signs();
    test_calibration_parse_and_crc();
    test_calibrated_polarity();
    test_per_axis_block_selection();

    if (failures) {
        printf("wii_motionplus: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("wii_motionplus: all tests passed\n");
    return 0;
}
