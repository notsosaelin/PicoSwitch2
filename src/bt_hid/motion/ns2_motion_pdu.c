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
