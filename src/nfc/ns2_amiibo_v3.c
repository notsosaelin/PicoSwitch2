#include "ns2_amiibo_v3.h"

#include <string.h>

// NTAG I2C Plus 2K GET_VERSION reply (byte [4]=0x02 NTAG I2C family, [6]=0x15
// 2 KB size code). Distinct from NTAG215's {..0x02,0x01,0x00,0x11,0x03}.
static const uint8_t NS2_AMIIBO_V3_VERSION[8] = {
    0x00, 0x04, 0x04, 0x05, 0x02, 0x02, 0x15, 0x03};

bool ns2_amiibo_v3_valid(const uint8_t *bytes, size_t len)
{
    if (!bytes || len != NS2_AMIIBO_V3_SIZE) return false;
    // UID is 7 contiguous bytes; byte[7]/[8] are the chip's internal 0x00/0x44,
    // not the NTAG215 BCC interleave. A genuine dump starts with the 0x04 NXP
    // manufacturer byte.
    return bytes[0] == 0x04 && bytes[7] == 0x00 && bytes[8] == 0x44;
}

void ns2_amiibo_v3_uid(const uint8_t image[NS2_AMIIBO_V3_SIZE], uint8_t uid[7])
{
    if (!image || !uid) return;
    memcpy(uid, image, 7);
}

void ns2_amiibo_v3_identity(const uint8_t image[NS2_AMIIBO_V3_SIZE],
                            uint8_t id[8])
{
    if (!image || !id) return;
    memcpy(id, image + 0x54, 8);
}

void ns2_amiibo_v3_version(uint8_t out[8])
{
    if (!out) return;
    memcpy(out, NS2_AMIIBO_V3_VERSION, sizeof(NS2_AMIIBO_V3_VERSION));
}

bool ns2_amiibo_v3_read(const uint8_t image[NS2_AMIIBO_V3_SIZE],
                        size_t offset, uint8_t *out, size_t len)
{
    if (!image || !out) return false;
    if (offset > NS2_AMIIBO_V3_SIZE || len > NS2_AMIIBO_V3_SIZE - offset)
        return false;
    memcpy(out, image + offset, len);
    return true;
}
