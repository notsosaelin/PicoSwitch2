/*
 * Wii MotionPlus (RVL-035, and the integrated silicon in RVL-CNT-01-TR) gyro
 * decode and calibration.
 *
 * Deliberately transport-free and free of Pico SDK dependencies so every
 * byte-level rule can be exercised on the host (tools/test_wii_motionplus.c)
 * before any of it runs against real hardware -- the same split used by
 * ns2_virtual_nfc and usb_mode_cycle.
 *
 * Protocol reference: docs/bluetooth/wii-motion.md §6. Field layout is Confirmed
 * there by two independent implementations agreeing bit-for-bit (the Linux
 * hid-wiimote parser and Dolphin's packed bitfields).
 */
#ifndef WII_MOTIONPLUS_H
#define WII_MOTIONPLUS_H

#include <stdbool.h>
#include <stdint.h>

#define WII_MP_FRAME_SIZE 6u
#define WII_MP_CAL_SIZE   32u

// Zero-rate raw value: the sensor reports mid-scale of its 14-bit range when it
// is not rotating (§1.2).
#define WII_MP_ZERO_RATE 8192

// Uncalibrated fallback full scales (§6.9). The absolute figures are disputed
// across sources by up to 35% (§6.10) -- only the ~4.5x ratio is Confirmed -- so
// these are a degraded path, adequate for detecting motion but not for
// integrating orientation. Always prefer the per-unit calibration block.
#define WII_MP_FALLBACK_SLOW_DPS 440
#define WII_MP_FALLBACK_FAST_DPS 2000

// One decoded MotionPlus sample, in the sensor's own naming (§6.11).
typedef struct {
    int32_t yaw;      // raw 14-bit, zero-centred (raw - 8192)
    int32_t roll;
    int32_t pitch;
    bool yaw_slow;    // true = slow (high sensitivity) range for THIS sample
    bool roll_slow;
    bool pitch_slow;
    bool ext_connected;   // an extension is plugged into the MotionPlus
} wii_mp_sample_t;

// One half of the calibration block; the block carries a fast and a slow copy.
typedef struct {
    uint16_t yaw_zero,  roll_zero,  pitch_zero;    // 16-bit space
    uint16_t yaw_scale, roll_scale, pitch_scale;   // 16-bit space
    uint8_t  degrees_div_6;                        // deg/s at the scale point / 6
} wii_mp_cal_block_t;

typedef struct {
    wii_mp_cal_block_t fast;
    wii_mp_cal_block_t slow;
    bool valid;       // false until a block passes its checksum
} wii_mp_cal_t;

/*
 * True when this 6-byte extension frame carries MotionPlus data rather than a
 * passed-through extension frame. Bit 1 of byte [5] is the only discriminator;
 * it must be checked on every frame because a failed extension read makes the
 * MotionPlus emit its own data instead of alternating strictly (§7.1).
 */
bool wii_mp_is_motionplus_frame(const uint8_t ext[WII_MP_FRAME_SIZE]);

/*
 * Decode a MotionPlus frame. Returns false if `ext` is not MotionPlus data.
 *
 * Note the non-obvious cross-byte placement of the slow bits: the PITCH slow bit
 * lives in byte [3] alongside YAW's high bits, not with pitch's own high bits.
 * Getting that wrong yields an implementation where two axes behave and one goes
 * haywire only at speed (§6.4).
 */
bool wii_mp_decode(const uint8_t ext[WII_MP_FRAME_SIZE], wii_mp_sample_t *out);

/*
 * Parse the 32-byte calibration block read from 0xA60020 (§6.7). All 16-bit
 * fields are big-endian. Returns false if the CRC32 check fails, in which case
 * `out->valid` is false and the caller must use the fallback scales.
 */
bool wii_mp_parse_calibration(const uint8_t raw[WII_MP_CAL_SIZE],
                              wii_mp_cal_t *out);

/*
 * Convert one axis to deg/s x 100 (centi-dps), avoiding floating point on the
 * hot path. `cal` may be NULL or invalid, in which case the fallback scales are
 * used.
 *
 * The calibration is two-point in a 16-bit space while the sample is 14-bit, so
 * the calibration values are shifted right by 2 to meet it (§6.8). `scale` may be
 * numerically LESS than `zero` -- the sign of (scale - zero) encodes axis
 * polarity and must not be absolute-valued.
 */
int32_t wii_mp_axis_centi_dps(int32_t raw_centred, bool is_slow,
                              uint16_t zero, uint16_t scale,
                              uint8_t degrees_div_6, bool cal_valid);

/*
 * Full conversion of a decoded sample into centi-dps per axis, selecting the
 * slow or fast calibration block per axis independently (§6.8).
 * Output order is [yaw, roll, pitch] -- the sensor's own order.
 */
void wii_mp_sample_centi_dps(const wii_mp_sample_t *s, const wii_mp_cal_t *cal,
                             int32_t out_yaw_roll_pitch[3]);

/*
 * Passthrough modes (§7). In 0x05/0x07 the MotionPlus and the downstream
 * extension share one 6-byte window on alternating frames, and the MotionPlus
 * relocates some of the extension's bits to make room for its own flags.
 */
typedef enum {
    WII_MP_PASSTHROUGH_NONE = 0,
    WII_MP_PASSTHROUGH_NUNCHUK,   // activation mode 0x05
    WII_MP_PASSTHROUGH_CLASSIC,   // activation mode 0x07
} wii_mp_passthrough_t;

/*
 * Undo that relocation in place, so an ordinary extension decoder sees a normal
 * frame (§7.2). Call only on frames where wii_mp_is_motionplus_frame() is false.
 *
 * The three accelerometer LSBs (Nunchuk) or the left-stick axis LSBs (Classic)
 * are destroyed by the MotionPlus and cannot be recovered; they are refilled
 * from the next-most-significant bit, which is what Dolphin does.
 */
void wii_mp_passthrough_restore(uint8_t data[6], wii_mp_passthrough_t mode);

#endif  // WII_MOTIONPLUS_H
