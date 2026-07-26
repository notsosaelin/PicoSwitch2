// Host test for the NTAG I2C 2K (figure v3) amiibo data model.
#include "ns2_amiibo_v3.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_v3(uint8_t img[NS2_AMIIBO_V3_SIZE])
{
    for (size_t i = 0; i < NS2_AMIIBO_V3_SIZE; ++i)
        img[i] = (uint8_t)(i * 31u + 5u);
    img[0] = 0x04;   // NXP manufacturer byte
    img[7] = 0x00;   // internal
    img[8] = 0x44;   // internal
}

int main(void)
{
    uint8_t img[NS2_AMIIBO_V3_SIZE];
    make_v3(img);

    // Validation
    assert(ns2_amiibo_v3_valid(img, NS2_AMIIBO_V3_SIZE));
    assert(!ns2_amiibo_v3_valid(img, 540));            // wrong size
    assert(!ns2_amiibo_v3_valid(NULL, NS2_AMIIBO_V3_SIZE));
    uint8_t bad[NS2_AMIIBO_V3_SIZE];
    make_v3(bad); bad[0] = 0x05;                        // wrong manufacturer
    assert(!ns2_amiibo_v3_valid(bad, NS2_AMIIBO_V3_SIZE));
    make_v3(bad); bad[8] = 0x00;                        // wrong internal byte
    assert(!ns2_amiibo_v3_valid(bad, NS2_AMIIBO_V3_SIZE));

    // UID is 7 contiguous bytes (no BCC interleave)
    uint8_t uid[7];
    ns2_amiibo_v3_uid(img, uid);
    assert(memcmp(uid, img, 7) == 0);

    // Identity block at 0x54
    uint8_t id[8];
    ns2_amiibo_v3_identity(img, id);
    assert(memcmp(id, img + 0x54, 8) == 0);

    // GET_VERSION reply matches the NTAG I2C 2K signature
    uint8_t ver[8];
    ns2_amiibo_v3_version(ver);
    const uint8_t expect[8] = {0x00, 0x04, 0x04, 0x05, 0x02, 0x02, 0x15, 0x03};
    assert(memcmp(ver, expect, 8) == 0);

    // Bounded reads: sector*256+page addressing -> linear byte offset
    uint8_t buf[16];
    assert(ns2_amiibo_v3_read(img, 0, buf, 16));
    assert(memcmp(buf, img, 16) == 0);
    // last page of sector 1: page 0xFF in sector 1 -> full page (0x1FF)
    size_t last = (1u * 256u + 0xFFu) * 4u; // 0x7FC
    assert(last + 4 <= NS2_AMIIBO_V3_SIZE);
    assert(ns2_amiibo_v3_read(img, last, buf, 4));
    assert(memcmp(buf, img + last, 4) == 0);
    // out-of-range rejected, no partial copy
    assert(!ns2_amiibo_v3_read(img, NS2_AMIIBO_V3_SIZE - 3, buf, 4));
    assert(!ns2_amiibo_v3_read(img, NS2_AMIIBO_V3_SIZE + 4, buf, 4));

    puts("ns2_amiibo_v3: all tests passed");
    return 0;
}
