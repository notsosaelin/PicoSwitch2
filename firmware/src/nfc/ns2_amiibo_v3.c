#include "ns2_amiibo_v3.h"

#include <string.h>

// NTAG I2C Plus 2K GET_VERSION reply (byte [4]=0x02 NTAG I2C family, [6]=0x15
// 2 KB size code). Distinct from NTAG215's {..0x02,0x01,0x00,0x11,0x03}.
static const uint8_t NS2_AMIIBO_V3_VERSION[8] = {
    0x00, 0x04, 0x04, 0x05, 0x02, 0x02, 0x15, 0x03};

static bool ns2_amiibo_v3_sector1_capability_valid(const uint8_t value[4])
{
    return value && value[0] == 0xA5u && value[1] == 0x00u &&
           value[2] != 0x00u && value[3] == 0x00u;
}

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

void ns2_amiibo_v3_sram_response(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    uint8_t out[NS2_AMIIBO_V3_SRAM_SIZE])
{
    if (!image || !out) return;
    memcpy(out, image + NS2_AMIIBO_V3_SRAM_OFFSET,
           NS2_AMIIBO_V3_SRAM_SIZE);
}

static uint16_t ns2_amiibo_v3_crc16_mcrf4xx(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8u; ++bit)
            crc = (uint16_t)((crc >> 1u) ^
                             ((crc & 1u) ? 0x8408u : 0u));
    }
    return crc;
}

bool ns2_amiibo_v3_sram_response_valid(
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    if (!image) return false;
    const uint8_t *sram = image + NS2_AMIIBO_V3_SRAM_OFFSET;
    const uint16_t expected = ns2_amiibo_v3_crc16_mcrf4xx(
        sram, NS2_AMIIBO_V3_SRAM_DATA_SIZE);
    const uint16_t stored =
        (uint16_t)(((uint16_t)sram[NS2_AMIIBO_V3_SRAM_DATA_SIZE] << 8u) |
                   sram[NS2_AMIIBO_V3_SRAM_DATA_SIZE + 1u]);
    return stored == expected;
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

bool ns2_amiibo_v3_build_sector_read_result(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    const uint8_t signature[NS2_AMIIBO_V3_SIGNATURE_SIZE],
    const uint8_t *request, size_t request_size,
    uint8_t *out, size_t out_capacity, size_t *out_size)
{
    // The genuine 0x1E request captured on 2026-07-28 has a fixed six-byte
    // reserved tail after a variable count of three-byte sector ranges.
    if (!image || !request || !out || !out_size ||
        request_size < 17u || request_size > 23u ||
        memcmp(request + 2u, image, 7u) != 0 ||
        request[9] != 0x01u)
        return false;

    const uint8_t range_count = request[10];
    if (range_count == 0u || range_count > 2u)
        return false;
    const size_t ranges_end = 11u + (size_t)range_count * 3u;
    if (ranges_end + 6u != request_size)
        return false;
    for (size_t i = ranges_end; i < request_size; ++i) {
        if (request[i] != 0u)
            return false;
    }

    size_t result_size = NS2_AMIIBO_V3_SECTOR_READ_PREFIX_SIZE;
    for (uint8_t i = 0; i < range_count; ++i) {
        const uint8_t sector = request[11u + (size_t)i * 3u];
        const uint8_t first = request[12u + (size_t)i * 3u];
        const uint8_t last = request[13u + (size_t)i * 3u];
        if (sector > 1u || last < first)
            return false;
        const size_t length = ((size_t)last - first + 1u) * 4u;
        const size_t address =
            (size_t)sector * 0x400u + (size_t)first * 4u;
        if (address + length > NS2_AMIIBO_V3_SIZE ||
            result_size + length > out_capacity)
            return false;
        result_size += length;
    }

    memset(out, 0, NS2_AMIIBO_V3_SECTOR_READ_PREFIX_SIZE);
    out[0] = 0x15u;
    out[4] = 0x01u;
    out[5] = 0x02u;
    out[7] = 0x07u;
    memcpy(out + 8u, image, 7u);
    out[18] = 0x06u;
    if (signature)
        memcpy(out + 19u, signature, NS2_AMIIBO_V3_SIGNATURE_SIZE);
    memcpy(out + 51u, request + 10u, request_size - 10u);

    static const uint8_t initial_sector1_capability[4] = {
        0xA5u, 0x00u, 0x01u, 0x00u,
    };
    size_t cursor = NS2_AMIIBO_V3_SECTOR_READ_PREFIX_SIZE;
    for (uint8_t i = 0; i < range_count; ++i) {
        const uint8_t sector = request[11u + (size_t)i * 3u];
        const uint8_t first = request[12u + (size_t)i * 3u];
        const uint8_t last = request[13u + (size_t)i * 3u];
        const size_t length = ((size_t)last - first + 1u) * 4u;
        const size_t address =
            (size_t)sector * 0x400u + (size_t)first * 4u;
        memcpy(out + cursor, image + address, length);
        // Air Riders sector reads contain eight sector-0 pages followed by a
        // 25-page sector-1 range: one chip-managed capability page and 24
        // application-data pages. The first page is allocation-relative
        // (Kirby 00, King Dedede 64), not globally fixed at sector-1 page 0.
        // Portable first-use dumps leave that page zero, so synthesize the
        // captured generation-1 value only for this exact descriptor shape.
        const bool air_riders_capability_range =
            range_count == 2u && i == 1u && sector == 1u &&
            request[11u] == 0u &&
            (uint16_t)request[13u] - request[12u] + 1u == 8u &&
            (uint16_t)last - first + 1u == 25u;
        if (air_riders_capability_range &&
            !ns2_amiibo_v3_sector1_capability_valid(out + cursor)) {
            memcpy(out + cursor, initial_sector1_capability,
                   NS2_AMIIBO_V3_SECTOR1_CAPABILITY_SIZE);
        }
        cursor += length;
    }

    *out_size = result_size;
    return true;
}
