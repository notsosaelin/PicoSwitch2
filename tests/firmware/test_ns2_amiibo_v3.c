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

    // Complete SRAM device response. This is deliberately the genuine
    // 2026-07-27 response: its CRC-16/MCRF4XX is 0x7AC4. The result builder
    // must carry all 64 bytes, not only the first 32 plus a captured CRC.
    const uint8_t genuine_sram[NS2_AMIIBO_V3_SRAM_SIZE] = {
        0x02, 0x00, 0x73, 0x2A, 0xB4, 0x1C, 0x4A, 0xC2,
        0x91, 0xB9, 0xA5, 0x98, 0x3C, 0x03, 0x94, 0x00,
        0xC9, 0x00, 0x0A, 0x50, 0x42, 0x34, 0x57, 0x31,
        0x37, 0x20, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7A, 0xC4};
    memcpy(img + NS2_AMIIBO_V3_SRAM_OFFSET, genuine_sram,
           sizeof(genuine_sram));
    uint8_t sram[NS2_AMIIBO_V3_SRAM_SIZE];
    memset(sram, 0xA5, sizeof(sram));
    ns2_amiibo_v3_sram_response(img, sram);
    assert(memcmp(sram, genuine_sram, sizeof(sram)) == 0);
    assert(ns2_amiibo_v3_sram_response_valid(img));
    img[NS2_AMIIBO_V3_SRAM_OFFSET + 32u] ^= 0x01u;
    assert(!ns2_amiibo_v3_sram_response_valid(img));
    img[NS2_AMIIBO_V3_SRAM_OFFSET + 32u] ^= 0x01u;
    img[NS2_AMIIBO_V3_SRAM_OFFSET + 63u] ^= 0x01u;
    assert(!ns2_amiibo_v3_sram_response_valid(img));
    img[NS2_AMIIBO_V3_SRAM_OFFSET + 63u] ^= 0x01u;

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

    // Genuine post-write sector read:
    //   sector 0 pages 92-99 (32 bytes)
    //   sector 1 pages 00-18 (100 bytes)
    // The result is a 64-byte prefix plus those 132 data bytes. Sector 1 page
    // zero is read-only chip metadata and is synthesized rather than read from
    // the ecosystem dump's zero-filled slot.
    uint8_t signature[NS2_AMIIBO_V3_SIGNATURE_SIZE];
    for (size_t i = 0; i < sizeof(signature); ++i)
        signature[i] = (uint8_t)(0x80u + i);
    uint8_t request[23] = {
        0xD0, 0x07,
        0, 0, 0, 0, 0, 0, 0,
        0x01, 0x02,
        0x00, 0x92, 0x99,
        0x01, 0x00, 0x18,
        0, 0, 0, 0, 0, 0,
    };
    memcpy(request + 2, img, 7);
    uint8_t result[NS2_AMIIBO_V3_SECTOR_READ_MAX_SIZE];
    size_t result_size = 0;
    assert(ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, sizeof(result), &result_size));
    assert(result_size == 196u);
    assert(result[0] == 0x15u);
    assert(result[4] == 0x01u && result[5] == 0x02u);
    assert(result[7] == 0x07u);
    assert(memcmp(result + 8u, img, 7u) == 0);
    assert(result[18] == 0x06u);
    assert(memcmp(result + 19u, signature, sizeof(signature)) == 0);
    assert(memcmp(result + 51u, request + 10u, 13u) == 0);
    assert(memcmp(result + 64u, img + 0x248u, 32u) == 0);
    const uint8_t sector1_page0[4] = {0xA5, 0x00, 0x01, 0x00};
    assert(memcmp(result + 96u, sector1_page0, 4u) == 0);
    assert(memcmp(result + 100u, img + 0x404u, 96u) == 0);

    // A later genuine read proved that this page advances to A5 00 02 00.
    // Once retained in the image, the dynamic value replaces the legacy
    // generation-1 fallback without disturbing the following page-1 bytes.
    const uint8_t sector1_page0_second[4] = {0xA5, 0x00, 0x02, 0x00};
    memcpy(img + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET,
           sector1_page0_second, sizeof(sector1_page0_second));
    assert(ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, sizeof(result), &result_size));
    assert(memcmp(result + 96u, sector1_page0_second, 4u) == 0);
    assert(memcmp(result + 100u, img + 0x404u, 96u) == 0);

    const uint8_t sector1_page0_third[4] = {0xA5, 0x00, 0x03, 0x00};
    memcpy(img + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET,
           sector1_page0_third, sizeof(sector1_page0_third));
    assert(ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, sizeof(result), &result_size));
    assert(memcmp(result + 96u, sector1_page0_third, 4u) == 0);
    memset(img + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET, 0, 4u);

    // King Dedede uses the same descriptor shape at different allocations:
    // sector-0 B2-B9 and sector-1 64-7C. The first page of the sector-1
    // range receives the same first-use fallback, then a retained dynamic
    // capability replaces it without any rider-specific lookup.
    uint8_t dedede_request[sizeof(request)];
    memcpy(dedede_request, request, sizeof(dedede_request));
    dedede_request[12] = 0xB2u;
    dedede_request[13] = 0xB9u;
    dedede_request[15] = 0x64u;
    dedede_request[16] = 0x7Cu;
    for (size_t i = 0; i < 0x20u; ++i)
        img[0x2C8u + i] = (uint8_t)(0x30u + i);
    memset(img + 0x590u, 0, 4u);
    for (size_t i = 0; i < 0x60u; ++i)
        img[0x594u + i] = (uint8_t)(0x90u + i);
    assert(ns2_amiibo_v3_build_sector_read_result(
        img, signature, dedede_request, sizeof(dedede_request),
        result, sizeof(result), &result_size));
    assert(result_size == 196u);
    assert(memcmp(result + 64u, img + 0x2C8u, 0x20u) == 0);
    assert(memcmp(result + 96u, sector1_page0, 4u) == 0);
    assert(memcmp(result + 100u, img + 0x594u, 0x60u) == 0);

    const uint8_t dedede_capability_second[4] = {
        0xA5u, 0x00u, 0x02u, 0x00u,
    };
    memcpy(img + 0x590u, dedede_capability_second,
           sizeof(dedede_capability_second));
    assert(ns2_amiibo_v3_build_sector_read_result(
        img, signature, dedede_request, sizeof(dedede_request),
        result, sizeof(result), &result_size));
    assert(memcmp(result + 96u, dedede_capability_second, 4u) == 0);

    // No signature is represented by zeros, while malformed identity, range,
    // reserved bytes, and undersized output are rejected without a partial
    // success.
    memset(result, 0xCC, sizeof(result));
    assert(ns2_amiibo_v3_build_sector_read_result(
        img, NULL, request, sizeof(request),
        result, sizeof(result), &result_size));
    for (size_t i = 19u; i < 51u; ++i) assert(result[i] == 0u);
    request[2] ^= 1u;
    assert(!ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, sizeof(result), &result_size));
    request[2] ^= 1u;
    request[11] = 2u;
    assert(!ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, sizeof(result), &result_size));
    request[11] = 0u;
    request[17] = 1u;
    assert(!ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, sizeof(result), &result_size));
    request[17] = 0u;
    assert(!ns2_amiibo_v3_build_sector_read_result(
        img, signature, request, sizeof(request),
        result, 195u, &result_size));

    // Byte-exact fixture reconstructed from the genuine Pro Controller 2 reuse
    // capture, seq 74-83 in
    // genuine-kirby-warp-reuse-sub1e-usb-2026-07-28.jsonl.
    static const uint8_t genuine_uid[7] = {
        0x04, 0x90, 0x11, 0xCA, 0xDB, 0x1F, 0x90,
    };
    static const uint8_t genuine_signature[32] = {
        0x80, 0x92, 0x50, 0x07, 0xB8, 0x2D, 0x0E, 0x23,
        0xF0, 0xFD, 0xE4, 0x3D, 0x9D, 0xD2, 0xF1, 0x2A,
        0x4F, 0x6B, 0x75, 0x0D, 0xAC, 0xFC, 0xA3, 0xB5,
        0xD6, 0x84, 0x75, 0x47, 0xE8, 0x95, 0xC0, 0x86,
    };
    static const uint8_t genuine_request[23] = {
        0xD0, 0x07, 0x04, 0x90, 0x11, 0xCA, 0xDB, 0x1F,
        0x90, 0x01, 0x02, 0x00, 0x92, 0x99, 0x01, 0x00,
        0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t genuine_sector_data[132] = {
        0xC2, 0xA5, 0x82, 0x33, 0x16, 0x18, 0x65, 0xAF,
        0x18, 0xEC, 0x8C, 0x1F, 0x7C, 0x0F, 0x5F, 0x8F,
        0x44, 0x4E, 0x4F, 0xE2, 0x1B, 0xC0, 0x96, 0x8B,
        0x3E, 0xFD, 0x0A, 0x47, 0xBF, 0xDC, 0xB1, 0xAF,
        0xA5, 0x00, 0x01, 0x00, 0xA0, 0x7F, 0x98, 0x13,
        0x52, 0x8F, 0x9C, 0x9B, 0x62, 0x04, 0xDD, 0x4B,
        0xE5, 0x4B, 0x12, 0x46, 0x91, 0x31, 0x63, 0x91,
        0x93, 0xCD, 0x6D, 0xF6, 0xAC, 0xA5, 0x0A, 0x74,
        0x59, 0x11, 0xFD, 0xF3, 0xEC, 0x0D, 0xE2, 0xC9,
        0x90, 0x0B, 0x04, 0xF8, 0x6B, 0xD1, 0x85, 0x28,
        0x3A, 0x3A, 0x9F, 0x1D, 0xAD, 0x07, 0x75, 0x53,
        0x34, 0x4B, 0x73, 0x2C, 0xAC, 0x26, 0x22, 0x13,
        0xB5, 0xB0, 0x38, 0x50, 0xC8, 0xB4, 0xA7, 0xEA,
        0x9A, 0xAF, 0xF5, 0x95, 0x76, 0x58, 0x30, 0x62,
        0xF0, 0x18, 0xD9, 0x7C, 0xC4, 0x61, 0x1C, 0x14,
        0xA4, 0xC7, 0x4D, 0x44, 0x55, 0x03, 0xA6, 0x18,
        0xD4, 0xA2, 0x9D, 0x78,
    };
    uint8_t genuine_image[NS2_AMIIBO_V3_SIZE] = {0};
    memcpy(genuine_image, genuine_uid, sizeof(genuine_uid));
    genuine_image[7] = 0x00u;
    genuine_image[8] = 0x44u;
    memcpy(genuine_image + 0x248u, genuine_sector_data, 32u);
    memcpy(genuine_image + 0x404u, genuine_sector_data + 36u, 96u);
    assert(ns2_amiibo_v3_build_sector_read_result(
        genuine_image, genuine_signature,
        genuine_request, sizeof(genuine_request),
        result, sizeof(result), &result_size));
    assert(result_size == 196u);
    static const uint8_t genuine_prefix[64] = {
        0x15, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x07,
        0x04, 0x90, 0x11, 0xCA, 0xDB, 0x1F, 0x90, 0x00,
        0x00, 0x00, 0x06, 0x80, 0x92, 0x50, 0x07, 0xB8,
        0x2D, 0x0E, 0x23, 0xF0, 0xFD, 0xE4, 0x3D, 0x9D,
        0xD2, 0xF1, 0x2A, 0x4F, 0x6B, 0x75, 0x0D, 0xAC,
        0xFC, 0xA3, 0xB5, 0xD6, 0x84, 0x75, 0x47, 0xE8,
        0x95, 0xC0, 0x86, 0x02, 0x00, 0x92, 0x99, 0x01,
        0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    assert(memcmp(result, genuine_prefix, sizeof(genuine_prefix)) == 0);
    assert(memcmp(result + sizeof(genuine_prefix),
                  genuine_sector_data, sizeof(genuine_sector_data)) == 0);

    puts("ns2_amiibo_v3: all tests passed");
    return 0;
}
