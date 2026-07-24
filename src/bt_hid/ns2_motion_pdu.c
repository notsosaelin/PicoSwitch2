#include "ns2_motion_pdu.h"

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
