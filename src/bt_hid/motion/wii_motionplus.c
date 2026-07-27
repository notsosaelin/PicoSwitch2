#include "wii_motionplus.h"

#include <string.h>

// Frame layout (docs/bluetooth/wii-motion.md §6.4):
//
//  [0] Yaw   Speed <7:0>
//  [1] Roll  Speed <7:0>
//  [2] Pitch Speed <7:0>
//  [3] Yaw   Speed <13:8> | yaw_slow(bit1)  | pitch_slow(bit0)
//  [4] Roll  Speed <13:8> | roll_slow(bit1) | ext_connected(bit0)
//  [5] Pitch Speed <13:8> | is_mp_data(bit1)| zero(bit0)
//
// The pitch slow bit deliberately lives in byte [3], not byte [5].
#define MP_BIT_IS_MP_DATA 0x02u
#define MP_BIT_SLOW       0x02u
#define MP_BIT_PITCH_SLOW 0x01u
#define MP_BIT_EXT_CONN   0x01u

bool wii_mp_is_motionplus_frame(const uint8_t ext[WII_MP_FRAME_SIZE])
{
    if (!ext) return false;
    return (ext[5] & MP_BIT_IS_MP_DATA) != 0u;
}

// The high 6 bits of each speed live in the upper 6 bits of bytes [3]/[4]/[5].
// `(b << 6) & 0xff00` is the kernel's compact way of writing `(b >> 2) << 8`:
// it drops the two flag bits and lands the remainder at 13:8.
static int32_t mp_assemble(uint8_t low, uint8_t high_and_flags)
{
    uint16_t v = (uint16_t)low;
    v |= (uint16_t)(((uint16_t)high_and_flags << 6) & 0xFF00u);
    return (int32_t)v - WII_MP_ZERO_RATE;
}

bool wii_mp_decode(const uint8_t ext[WII_MP_FRAME_SIZE], wii_mp_sample_t *out)
{
    if (!ext || !out) return false;
    if (!wii_mp_is_motionplus_frame(ext)) return false;

    out->yaw   = mp_assemble(ext[0], ext[3]);
    out->roll  = mp_assemble(ext[1], ext[4]);
    out->pitch = mp_assemble(ext[2], ext[5]);

    out->yaw_slow   = (ext[3] & MP_BIT_SLOW) != 0u;
    out->roll_slow  = (ext[4] & MP_BIT_SLOW) != 0u;
    out->pitch_slow = (ext[3] & MP_BIT_PITCH_SLOW) != 0u;   // note: byte [3]
    out->ext_connected = (ext[4] & MP_BIT_EXT_CONN) != 0u;
    return true;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// CRC32 (IEEE, reflected) over the two 14-byte spans that exclude the checksum
// fields themselves: offsets 0x00-0x0D and 0x10-0x1D (§6.7). The result is split
// with the high half stored at 0x0E and the low half at 0x1E.
static uint32_t mp_crc32(const uint8_t *data, size_t len, uint32_t crc)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

static void mp_parse_block(const uint8_t *p, wii_mp_cal_block_t *b)
{
    b->yaw_zero    = be16(p + 0);
    b->roll_zero   = be16(p + 2);
    b->pitch_zero  = be16(p + 4);
    b->yaw_scale   = be16(p + 6);
    b->roll_scale  = be16(p + 8);
    b->pitch_scale = be16(p + 10);
    b->degrees_div_6 = p[12];
}

bool wii_mp_parse_calibration(const uint8_t raw[WII_MP_CAL_SIZE],
                              wii_mp_cal_t *out)
{
    if (!raw || !out) return false;
    memset(out, 0, sizeof(*out));
    mp_parse_block(raw + 0x00, &out->fast);
    mp_parse_block(raw + 0x10, &out->slow);

    uint32_t crc = mp_crc32(raw + 0x00, 14u, 0xFFFFFFFFu);
    crc = mp_crc32(raw + 0x10, 14u, crc) ^ 0xFFFFFFFFu;

    const uint16_t stored_hi = be16(raw + 0x0E);
    const uint16_t stored_lo = be16(raw + 0x1E);
    const uint32_t stored = ((uint32_t)stored_hi << 16) | stored_lo;

    out->valid = (crc == stored);
    return out->valid;
}

int32_t wii_mp_axis_centi_dps(int32_t raw_centred, bool is_slow,
                              uint16_t zero, uint16_t scale,
                              uint8_t degrees_div_6, bool cal_valid)
{
    if (cal_valid && degrees_div_6 != 0u) {
        // Calibration is a two-point pair in 16-bit space while the sample is
        // 14-bit, so meet them by shifting the calibration right by 2 (§6.8).
        const int32_t zero14  = (int32_t)zero  >> 2;
        const int32_t scale14 = (int32_t)scale >> 2;
        const int32_t span = scale14 - zero14;   // sign encodes axis polarity
        if (span != 0) {
            const int32_t degrees = (int32_t)degrees_div_6 * 6;
            // raw_centred is already (raw - 8192); the calibration zero is
            // expressed in absolute 14-bit space, so restore the offset first.
            const int32_t rel = (raw_centred + WII_MP_ZERO_RATE) - zero14;
            return (rel * degrees * 100) / span;
        }
    }

    // Fallback: fixed nominal scales (§6.9). 8192 counts map to full scale.
    const int32_t full = is_slow ? WII_MP_FALLBACK_SLOW_DPS
                                 : WII_MP_FALLBACK_FAST_DPS;
    return (raw_centred * full * 100) / WII_MP_ZERO_RATE;
}

void wii_mp_sample_centi_dps(const wii_mp_sample_t *s, const wii_mp_cal_t *cal,
                             int32_t out_yaw_roll_pitch[3])
{
    if (!s || !out_yaw_roll_pitch) return;
    const bool valid = cal && cal->valid;

    // Each axis independently selects the slow or fast block for THIS sample --
    // yaw may legitimately use fast while pitch uses slow in the same frame.
    const wii_mp_cal_block_t *yb = NULL, *rb = NULL, *pb = NULL;
    if (valid) {
        yb = s->yaw_slow   ? &cal->slow : &cal->fast;
        rb = s->roll_slow  ? &cal->slow : &cal->fast;
        pb = s->pitch_slow ? &cal->slow : &cal->fast;
    }

    out_yaw_roll_pitch[0] = wii_mp_axis_centi_dps(
        s->yaw, s->yaw_slow,
        yb ? yb->yaw_zero : 0, yb ? yb->yaw_scale : 0,
        yb ? yb->degrees_div_6 : 0, valid);
    out_yaw_roll_pitch[1] = wii_mp_axis_centi_dps(
        s->roll, s->roll_slow,
        rb ? rb->roll_zero : 0, rb ? rb->roll_scale : 0,
        rb ? rb->degrees_div_6 : 0, valid);
    out_yaw_roll_pitch[2] = wii_mp_axis_centi_dps(
        s->pitch, s->pitch_slow,
        pb ? pb->pitch_zero : 0, pb ? pb->pitch_scale : 0,
        pb ? pb->degrees_div_6 : 0, valid);
}
