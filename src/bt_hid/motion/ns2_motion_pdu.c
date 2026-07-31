#include "ns2_motion_pdu.h"

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    const uint32_t sign = 1u << (bits - 1u);
    return (value & sign)
        ? (int32_t)((int64_t)value - ((int64_t)1 << bits))
        : (int32_t)value;
}

bool ns2_motion_pdu30_get_orientation(const uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                                      uint32_t out[3])
{
    if (!pdu || !out) return false;

    out[0] = (uint32_t)pdu[5] |
             ((uint32_t)pdu[6] << 8) |
             ((uint32_t)pdu[7] << 16) |
             ((uint32_t)(pdu[8] & 0x03u) << 24);
    out[1] = (uint32_t)pdu[9] |
             ((uint32_t)pdu[10] << 8) |
             ((uint32_t)pdu[11] << 16) |
             ((uint32_t)(pdu[12] & 0x03u) << 24);
    out[2] = (uint32_t)pdu[13] |
             ((uint32_t)pdu[14] << 8) |
             ((uint32_t)pdu[15] << 16) |
             ((uint32_t)(pdu[4] & 0x03u) << 24);
    return true;
}

bool ns2_motion_pdu30_set_orientation(uint8_t pdu[NS2_MOTION_PDU30_LENGTH],
                                      const uint32_t values[3])
{
    if (!pdu || !values) return false;

    const uint32_t g0 = values[0] & NS2_MOTION_ORIENTATION_MASK;
    const uint32_t g1 = values[1] & NS2_MOTION_ORIENTATION_MASK;
    const uint32_t g2 = values[2] & NS2_MOTION_ORIENTATION_MASK;

    pdu[5] = (uint8_t)g0;
    pdu[6] = (uint8_t)(g0 >> 8);
    pdu[7] = (uint8_t)(g0 >> 16);
    pdu[8] = (uint8_t)((pdu[8] & 0xFCu) | ((g0 >> 24) & 0x03u));

    pdu[9] = (uint8_t)g1;
    pdu[10] = (uint8_t)(g1 >> 8);
    pdu[11] = (uint8_t)(g1 >> 16);
    pdu[12] = (uint8_t)((pdu[12] & 0xFCu) | ((g1 >> 24) & 0x03u));

    pdu[13] = (uint8_t)g2;
    pdu[14] = (uint8_t)(g2 >> 8);
    pdu[15] = (uint8_t)(g2 >> 16);
    pdu[4] = (uint8_t)((pdu[4] & 0xFCu) | ((g2 >> 24) & 0x03u));
    return true;
}

bool ns2_motion_pdu40_get_reference(
    const uint8_t pdu[NS2_MOTION_PDU40_LENGTH], int32_t out[3])
{
    if (!pdu || !out) return false;

    const uint32_t g6 =
        ((uint32_t)(pdu[32] & 0x03u) << 20) |
        ((uint32_t)pdu[31] << 12) |
        ((((uint32_t)pdu[30] << 8) | pdu[29]) >> 4);
    const uint32_t g7 =
        ((uint32_t)(pdu[35] & 0x03u) << 20) |
        ((uint32_t)pdu[34] << 12) |
        ((((uint32_t)pdu[33] << 8) | pdu[32]) >> 4);
    const uint32_t g8 =
        ((uint32_t)pdu[37] << 12) |
        ((((uint32_t)pdu[36] << 8) | pdu[35]) >> 4);

    out[0] = sign_extend(g6, 22);
    out[1] = sign_extend(g7, 22);
    out[2] = sign_extend(g8, 20);
    return true;
}

bool ns2_motion_pdu40_set_reference(
    uint8_t pdu[NS2_MOTION_PDU40_LENGTH], const int32_t values[3])
{
    if (!pdu || !values) return false;
    if (values[0] < NS2_MOTION_REFERENCE_G6_MIN ||
        values[0] > NS2_MOTION_REFERENCE_G6_MAX ||
        values[1] < NS2_MOTION_REFERENCE_G7_MIN ||
        values[1] > NS2_MOTION_REFERENCE_G7_MAX ||
        values[2] < NS2_MOTION_REFERENCE_G8_MIN ||
        values[2] > NS2_MOTION_REFERENCE_G8_MAX)
        return false;

    const uint32_t g6 = (uint32_t)values[0] & 0x003FFFFFu;
    const uint32_t g7 = (uint32_t)values[1] & 0x003FFFFFu;
    const uint32_t g8 = (uint32_t)values[2] & 0x000FFFFFu;

    pdu[29] = (uint8_t)((pdu[29] & 0x0Fu) | ((g6 & 0x0Fu) << 4));
    pdu[30] = (uint8_t)(g6 >> 4);
    pdu[31] = (uint8_t)(g6 >> 12);
    pdu[32] = (uint8_t)((pdu[32] & 0x0Cu) |
                        ((g6 >> 20) & 0x03u) |
                        ((g7 & 0x0Fu) << 4));
    pdu[33] = (uint8_t)(g7 >> 4);
    pdu[34] = (uint8_t)(g7 >> 12);
    pdu[35] = (uint8_t)((pdu[35] & 0x0Cu) |
                        ((g7 >> 20) & 0x03u) |
                        ((g8 & 0x0Fu) << 4));
    pdu[36] = (uint8_t)(g8 >> 4);
    pdu[37] = (uint8_t)(g8 >> 12);
    return true;
}

// ---------------------------------------------------------------------------
// Length-0x28 catch-up packer
// ---------------------------------------------------------------------------

// Catch-up field map, in payload bit offsets (payload = pdu[4..39], 288 bits).
// Mirrors ns2_motion_reference.decode_motion40 exactly; the fixture test holds
// both implementations to the same genuine packets.
#define MOTION40_PAYLOAD_BYTES 36u
#define MOTION40_PAYLOAD_BITS  288u

static const uint16_t k_catchup_accel_offset[3] = {68u, 158u, 245u};
static const uint8_t  k_catchup_accel_width[3]  = {14u, 13u, 14u};
static const uint16_t k_catchup_gyro_offset[2]  = {110u, 197u};
static const uint8_t  k_catchup_gyro_width[2]   = {16u, 16u};
static const uint16_t k_catchup_carrier_offset[3] = {2u, 24u, 45u};
static const uint8_t  k_catchup_carrier_width[3]  = {22u, 21u, 23u};
#define MOTION40_CATCHUP_TAIL_OFFSET 287u

// LSB-first within each byte, little-endian across the payload. A bit at a
// time is plainly correct and costs nothing: ~200 bits per packet at 50 Hz.
static void payload_put(uint8_t *payload, unsigned offset, unsigned width,
                        uint32_t value)
{
    for (unsigned i = 0; i < width; ++i) {
        if (value & (1u << i)) {
            const unsigned bit = offset + i;
            payload[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
        }
    }
}

static bool fits_signed(int32_t value, unsigned width)
{
    const int32_t limit = (int32_t)1 << (width - 1u);
    return value >= -limit && value < limit;
}

static bool payload_put_vector(uint8_t *payload, unsigned offset,
                               unsigned width, const int32_t vector[3])
{
    for (unsigned axis = 0; axis < 3u; ++axis) {
        if (!fits_signed(vector[axis], width)) return false;
    }
    for (unsigned axis = 0; axis < 3u; ++axis) {
        const uint32_t mask = (width >= 32u) ? 0xFFFFFFFFu
                                             : ((1u << width) - 1u);
        payload_put(payload, offset + axis * width, width,
                    (uint32_t)vector[axis] & mask);
    }
    return true;
}

bool ns2_motion_pdu40_build_catchup(uint8_t pdu[NS2_MOTION_PDU40_LENGTH],
                                    const ns2_motion40_catchup_t *fields)
{
    if (!pdu || !fields) return false;
    if (fields->tick > 0x0FFFu) return false;
    if (fields->elapsed_ticks > 0x0FFFu) return false;
    // Elapsed selects the layout. Emitting catch-up fields under an elapsed
    // count the decoder would read as a different layout produces a packet
    // that decodes cleanly into the wrong fields, so refuse instead.
    if (fields->elapsed_ticks < NS2_MOTION40_CATCHUP_MIN_ELAPSED) return false;
    if (fields->packing_mode > 3u) return false;
    if (fields->tail_bit > 1u) return false;

    for (unsigned lane = 0; lane < 3u; ++lane) {
        if (!fits_signed(fields->carrier[lane], k_catchup_carrier_width[lane]))
            return false;
    }
    for (unsigned slot = 0; slot < 3u; ++slot) {
        for (unsigned axis = 0; axis < 3u; ++axis) {
            if (!fits_signed(fields->accel[slot][axis],
                             k_catchup_accel_width[slot]))
                return false;
        }
    }
    for (unsigned slot = 0; slot < 2u; ++slot) {
        for (unsigned axis = 0; axis < 3u; ++axis) {
            if (!fits_signed(fields->gyro[slot][axis],
                             k_catchup_gyro_width[slot]))
                return false;
        }
    }

    // Every field validated: build into a scratch payload so a late failure
    // cannot leave the caller's buffer half-written.
    uint8_t payload[MOTION40_PAYLOAD_BYTES];
    for (unsigned i = 0; i < MOTION40_PAYLOAD_BYTES; ++i) payload[i] = 0u;

    payload_put(payload, 0u, 2u, fields->packing_mode);
    for (unsigned lane = 0; lane < 3u; ++lane) {
        const unsigned width = k_catchup_carrier_width[lane];
        const uint32_t mask = (1u << width) - 1u;
        payload_put(payload, k_catchup_carrier_offset[lane], width,
                    (uint32_t)fields->carrier[lane] & mask);
    }
    for (unsigned slot = 0; slot < 3u; ++slot) {
        if (!payload_put_vector(payload, k_catchup_accel_offset[slot],
                                k_catchup_accel_width[slot],
                                fields->accel[slot]))
            return false;
    }
    for (unsigned slot = 0; slot < 2u; ++slot) {
        if (!payload_put_vector(payload, k_catchup_gyro_offset[slot],
                                k_catchup_gyro_width[slot],
                                fields->gyro[slot]))
            return false;
    }
    payload_put(payload, MOTION40_CATCHUP_TAIL_OFFSET, 1u, fields->tail_bit);

    const uint8_t status = fields->status ? fields->status
                                          : NS2_MOTION40_STATUS_CATCHUP;
    // Preamble: 12-bit tick, then the 12-bit elapsed count split across the
    // high nibble of byte 1 and all of byte 2, then the layout/status byte.
    pdu[0] = (uint8_t)(fields->tick & 0xFFu);
    pdu[1] = (uint8_t)(((fields->tick >> 8) & 0x0Fu) |
                       ((fields->elapsed_ticks & 0x0Fu) << 4));
    pdu[2] = (uint8_t)((fields->elapsed_ticks >> 4) & 0xFFu);
    pdu[3] = status;
    for (unsigned i = 0; i < MOTION40_PAYLOAD_BYTES; ++i)
        pdu[4u + i] = payload[i];
    return true;
}
