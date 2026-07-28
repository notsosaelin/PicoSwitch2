#include "ns2_amiibo_v3_write.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_v3(uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    for (size_t i = 0; i < NS2_AMIIBO_V3_SIZE; ++i)
        image[i] = (uint8_t)(i * 17u + 9u);
    image[0] = 0x04u;
    image[7] = 0x00u;
    image[8] = 0x44u;
    memset(image + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET, 0, 4u);
}

static void make_device_command(
    uint8_t data[NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE],
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    memset(data, 0, NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE);
    data[0] = 0xD0u;
    data[1] = 0x07u;
    memcpy(data + 2u, image, 7u);
    data[9] = 0x01u;
    data[10] = 0x01u;
}

static void make_write(
    uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE],
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    memset(staging, 0, NS2_NFC_WRITE_STAGING_SIZE);
    staging[0] = 0xD0u;
    staging[1] = 0x07u;
    memcpy(staging + 2u, image, 7u);
    // Captured v3 write header: D0 07 + UID + 01 06 01 04 FFFFFFFF +
    // four static-lock bytes + record count.
    staging[9] = 0x01u;
    staging[10] = 0x06u;
    staging[11] = 0x01u;
    staging[12] = 0x04u;
    memset(staging + 13u, 0xFF, 4u);
    staging[17] = 0xA5u;
    staging[18] = 0x00u;
    staging[19] = 0x01u;
    staging[20] = 0x00u;
    staging[21] = 3u;

    // This is the exact record layout observed in both genuine v3 writes:
    // page 0x05 / 32 bytes, page 0x30 / 240 bytes, page 0x6C / 152
    // bytes. The last byte lands at 0x247.
    size_t cursor = 22u;
    staging[cursor++] = 0x05u;
    staging[cursor++] = 32u;
    for (size_t i = 0; i < 32u; ++i)
        staging[cursor++] = (uint8_t)(0x20u + i);
    staging[cursor++] = 0x30u;
    staging[cursor++] = 240u;
    for (size_t i = 0; i < 240u; ++i)
        staging[cursor++] = (uint8_t)(0x40u + i);
    staging[cursor++] = 0x6Cu;
    staging[cursor++] = 152u;
    for (size_t i = 0; i < 152u; ++i)
        staging[cursor++] = (uint8_t)(0x80u + i);
    assert(cursor == 452u);
}

static void make_extended_clear(
    uint8_t staging[NS2_AMIIBO_V3_EXTENDED_MAX_SIZE],
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    memset(staging, 0, NS2_AMIIBO_V3_EXTENDED_MAX_SIZE);
    staging[0] = 0x88u; // 5000 ms, little-endian
    staging[1] = 0x13u;
    memcpy(staging + 2u, image, 7u);
    staging[9] = 0x01u;
    staging[10] = 0x06u;
    staging[22] = 0x02u;
    staging[23] = 0x00u;
    staging[24] = 0x92u;
    staging[25] = 0xF0u;
    for (size_t i = 0; i < 0xF0u; ++i)
        staging[26u + i] = (uint8_t)(0x20u + i);
    staging[266] = 0x00u;
    staging[267] = 0xCEu;
    staging[268] = 0x50u;
    for (size_t i = 0; i < 0x50u; ++i)
        staging[269u + i] = (uint8_t)(0x80u + i);
}

static bool range_nonzero(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    size_t offset, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (image[offset + i] != 0u) return true;
    }
    return false;
}

static void make_extended_update_at(
    uint8_t staging[NS2_AMIIBO_V3_EXTENDED_MAX_SIZE],
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    uint8_t sector0_page, uint8_t sector1_capability_page)
{
    memset(staging, 0, NS2_AMIIBO_V3_EXTENDED_MAX_SIZE);
    staging[0] = 0x88u;
    staging[1] = 0x13u;
    memcpy(staging + 2u, image, 7u);
    staging[9] = 0x01u;
    staging[10] = 0x06u;
    static const uint8_t header[] = {
        0x01, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF,
    };
    memcpy(staging + 11u, header, sizeof(header));
    staging[13] = sector1_capability_page;
    staging[18] = 0xA5u;
    staging[19] = 0x00u;
    const size_t capability_offset =
        0x400u + (size_t)sector1_capability_page * 4u;
    staging[20] =
        (uint8_t)(image[capability_offset + 2u] + 1u);
    if (staging[20] == 1u &&
        (range_nonzero(image, (size_t)sector0_page * 4u, 0x20u) ||
         range_nonzero(image, capability_offset + 4u, 0x60u)))
        staging[20] = 2u;
    staging[21] = 0x00u;
    staging[22] = 0x03u;

    size_t cursor = 23u;
    staging[cursor++] = 0x00u;
    staging[cursor++] = 0x04u;
    staging[cursor++] = 0x04u;
    staging[cursor++] = 0x00u;
    staging[cursor++] = 0x00u;
    staging[cursor++] = 0x03u;
    staging[cursor++] = 0x00u;

    staging[cursor++] = 0x00u;
    staging[cursor++] = sector0_page;
    staging[cursor++] = 0x20u;
    for (size_t i = 0; i < 0x20u; ++i)
        staging[cursor++] = (uint8_t)(0x40u + i);

    staging[cursor++] = 0x01u;
    staging[cursor++] = (uint8_t)(sector1_capability_page + 1u);
    staging[cursor++] = 0x60u;
    for (size_t i = 0; i < 0x60u; ++i)
        staging[cursor++] = (uint8_t)(0xA0u + i);
    assert(cursor == 164u);
}

static void make_extended_update(
    uint8_t staging[NS2_AMIIBO_V3_EXTENDED_MAX_SIZE],
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    make_extended_update_at(staging, image, 0x92u, 0x00u);
}

static void stage_complete_write(
    ns2_virtual_nfc_write_t *write,
    const uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE])
{
    ns2_virtual_nfc_write_begin(write);
    for (size_t offset = 0; offset < NS2_NFC_WRITE_STAGING_SIZE;
         offset += 76u) {
        size_t size = NS2_NFC_WRITE_STAGING_SIZE - offset;
        if (size > 76u) size = 76u;
        assert(ns2_virtual_nfc_write_chunk(
                   write, offset, staging + offset, size) ==
               NS2_VIRTUAL_NFC_OK);
    }
}

static void stage_complete_extended(
    ns2_virtual_nfc_write_t *write,
    const uint8_t staging[NS2_AMIIBO_V3_EXTENDED_MAX_SIZE],
    size_t total_size)
{
    ns2_virtual_nfc_write_begin(write);
    for (size_t offset = 0; offset < total_size; offset += 76u) {
        size_t size = total_size - offset;
        if (size > 76u) size = 76u;
        assert(ns2_virtual_nfc_write_chunk(
                   write, offset, staging + offset, size) ==
               NS2_VIRTUAL_NFC_OK);
    }
}

int main(void)
{
    uint8_t image[NS2_AMIIBO_V3_SIZE];
    uint8_t staging[NS2_NFC_WRITE_STAGING_SIZE];
    uint8_t extended[NS2_AMIIBO_V3_EXTENDED_MAX_SIZE];
    uint8_t device[NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE];
    make_v3(image);
    make_write(staging, image);
    make_extended_clear(extended, image);
    make_device_command(device, image);

    assert(ns2_amiibo_v3_is_device_command(
        device, sizeof(device), image));
    assert(!ns2_amiibo_v3_is_write_start(
        device, sizeof(device), image));
    assert(ns2_amiibo_v3_is_write_start(
        staging, 76u, image));
    assert(!ns2_amiibo_v3_is_device_command(
        staging, 76u, image));
    assert(ns2_amiibo_v3_is_extended_start(
        extended, 76u, image));
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) ==
           NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE);
    assert(!ns2_amiibo_v3_is_write_start(
        extended, 76u, image));

    // Timeout bytes are values, not an operation marker.
    staging[0] = 0xB8u;
    staging[1] = 0x0Bu;
    assert(ns2_amiibo_v3_is_write_start(staging, 76u, image));
    staging[0] = 0xD0u;
    staging[1] = 0x07u;

    ns2_virtual_nfc_write_t write;
    ns2_virtual_nfc_write_init(&write);
    stage_complete_write(&write, staging);

    uint8_t before[NS2_AMIIBO_V3_SIZE];
    memcpy(before, image, sizeof(before));
    uint8_t records = 0;
    uint16_t data_bytes = 0;
    assert(ns2_amiibo_v3_write_commit(
               &write, image, &records, &data_bytes) ==
           NS2_VIRTUAL_NFC_OK);
    assert(records == 3u && data_bytes == 424u);
    assert(memcmp(image, before, 16u) == 0);
    assert(memcmp(image + 16u, staging + 17u, 4u) == 0);
    assert(memcmp(image + 20u, staging + 24u, 32u) == 0);
    assert(memcmp(image + 0xC0u, staging + 58u, 240u) == 0);
    assert(memcmp(image + 0x1B0u, staging + 300u, 152u) == 0);
    assert(memcmp(image + NS2_AMIIBO_V3_WRITE_END,
                  before + NS2_AMIIBO_V3_WRITE_END,
                  NS2_AMIIBO_V3_SIZE - NS2_AMIIBO_V3_WRITE_END) == 0);

    // The first 0x20 operation is a sector-aware clear/initialise transaction.
    make_v3(image);
    make_extended_clear(extended, image);
    memcpy(before, image, sizeof(before));
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE);
    records = 0;
    data_bytes = 0;
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE,
               &records, &data_bytes) ==
           NS2_VIRTUAL_NFC_OK);
    assert(!write.active);
    assert(records == 2u && data_bytes == 0x140u);
    assert(memcmp(image, before, NS2_AMIIBO_V3_WRITE_END) == 0);
    assert(memcmp(image + 0x248u, extended + 26u, 0xF0u) == 0);
    assert(memcmp(image + 0x338u, extended + 269u, 0x50u) == 0);
    assert(memcmp(image + 0x388u, before + 0x388u,
                  sizeof(image) - 0x388u) == 0);

    // King Dedede proves allocations are envelope-driven rather than fixed:
    // sector-0 page B2, sector-1 capability page 64, data page 65. Recreate
    // the genuine first-use zero state after the copy test above.
    const size_t dedede_sector0_offset = 0xB2u * 4u;
    const size_t dedede_capability_offset = 0x400u + 0x64u * 4u;
    memset(image + dedede_sector0_offset, 0, 0x20u);
    memset(image + dedede_capability_offset, 0, 4u + 0x60u);
    make_extended_update_at(extended, image, 0xB2u, 0x64u);
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) ==
           NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    memcpy(before, image, sizeof(before));
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE,
               &records, &data_bytes) ==
           NS2_VIRTUAL_NFC_OK);
    assert(records == 3u && data_bytes == 132u);
    assert(image[16] == before[16] && image[17] == before[17]);
    assert(image[18] == 0x03u && image[19] == 0x00u);
    assert(memcmp(image + dedede_capability_offset,
                  extended + 18u, 4u) == 0);
    assert(memcmp(image + dedede_sector0_offset,
                  extended + 33u, 0x20u) == 0);
    assert(memcmp(image + dedede_capability_offset + 4u,
                  extended + 68u, 0x60u) == 0);
    // Dynamic Dedede storage must not alias Kirby's sector-1 page zero.
    assert(memcmp(image + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET,
                  before + NS2_AMIIBO_V3_SECTOR1_CAPABILITY_OFFSET,
                  4u) == 0);

    // The envelope carries the next allocation-relative capability generation.
    // It advanced
    // from A5 00 01 00 to A5 00 02 00 on genuine hardware independently of
    // sector-0 page 4.
    make_extended_update_at(extended, image, 0xB2u, 0x64u);
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) ==
           NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    extended[20]++;
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) == 0u);
    extended[20]--;
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE,
               &records, &data_bytes) == NS2_VIRTUAL_NFC_OK);
    assert(image[dedede_capability_offset + 2u] == 0x02u);

    // The next genuine cycle repeated the same independent transition:
    // page 0 advanced 02 -> 03 while sector-0 page 4 advanced 04 -> 05.
    // Deliberately make page 4 unrelated and prove classification follows the
    // retained sector-1 generation instead.
    image[16] = 0xA5u;
    image[17] = 0x00u;
    image[18] = 0x7Eu;
    image[19] = 0x00u;
    make_extended_update_at(extended, image, 0xB2u, 0x64u);
    assert(extended[20] == 0x03u);
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) ==
           NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);

    // Future figures remain identity-agnostic: several safe page allocations
    // classify from the self-described records without adding per-rider code.
    static const struct {
        uint8_t sector0_page;
        uint8_t sector1_capability_page;
    } future_allocations[] = {
        {0x92u, 0x00u},
        {0xA2u, 0x20u},
        {0xB2u, 0x64u},
        {0xD2u, 0xC0u},
    };
    for (size_t i = 0;
         i < sizeof(future_allocations) / sizeof(future_allocations[0]);
         ++i) {
        make_v3(image);
        const uint8_t sector0_page = future_allocations[i].sector0_page;
        const uint8_t capability_page =
            future_allocations[i].sector1_capability_page;
        const size_t sector0_offset = (size_t)sector0_page * 4u;
        const size_t capability_offset =
            0x400u + (size_t)capability_page * 4u;
        memset(image + sector0_offset, 0, 0x20u);
        memset(image + capability_offset, 0, 4u + 0x60u);
        make_extended_update_at(
            extended, image, sector0_page, capability_page);
        assert(ns2_amiibo_v3_extended_expected_size(
                   extended, 76u, image) ==
               NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    }

    // Self-description is still fail-closed: data must follow its capability
    // page and both records must stay inside the proven writable bounds.
    make_v3(image);
    memset(image + 0x2C8u, 0, 0x20u);
    memset(image + 0x590u, 0, 4u + 0x60u);
    make_extended_update_at(extended, image, 0xB2u, 0x64u);
    extended[66] = 0x66u;
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) == 0u);
    make_extended_update_at(extended, image, 0xB2u, 0x64u);
    extended[31] = 0xE0u;
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) == 0u);
    make_extended_update_at(extended, image, 0xB2u, 0x64u);
    extended[13] = 0xE8u;
    extended[66] = 0xE9u;
    assert(ns2_amiibo_v3_extended_expected_size(
               extended, 76u, image) == 0u);

    // The clear-stage ordinary checkpoint must retain presentation so the
    // console can begin the update stage. The update-stage checkpoint and an
    // abandoned/expired clear use the normal auto-eject. Unsigned timing must
    // remain correct across the 32-bit millisecond wrap.
    ns2_amiibo_v3_extended_sequence_t sequence;
    ns2_amiibo_v3_extended_sequence_reset(&sequence);
    assert(!ns2_amiibo_v3_extended_sequence_continue_after_write(
        &sequence, 100u));
    ns2_amiibo_v3_extended_sequence_note_commit(
        &sequence, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE, 100u);
    assert(ns2_amiibo_v3_extended_sequence_continue_after_write(
        &sequence, 5099u));
    assert(!ns2_amiibo_v3_extended_sequence_continue_after_write(
        &sequence, 5100u));
    ns2_amiibo_v3_extended_sequence_note_commit(
        &sequence, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE, 6000u);
    assert(!ns2_amiibo_v3_extended_sequence_continue_after_write(
        &sequence, 6001u));
    ns2_amiibo_v3_extended_sequence_note_commit(
        &sequence, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE, 0xFFFFFF00u);
    assert(ns2_amiibo_v3_extended_sequence_continue_after_write(
        &sequence, 0x00000020u));
    assert(!ns2_amiibo_v3_extended_sequence_continue_after_write(
        &sequence, 0x00001288u));

    make_extended_clear(extended, image);
    ns2_virtual_nfc_write_begin(&write);
    assert(ns2_virtual_nfc_write_chunk(
               &write, 0u, extended,
               NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE - 1u) ==
           NS2_VIRTUAL_NFC_OK);
    assert(ns2_virtual_nfc_write_chunk(
               &write, 400u, extended, 1u) ==
           NS2_VIRTUAL_NFC_OK);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE,
               NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_INCOMPLETE);

    memcpy(before, image, sizeof(before));
    make_extended_clear(extended, image);
    extended[2] ^= 1u;
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE,
               NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_UID);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_extended_clear(extended, image);
    extended[24] = 0x91u;
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE,
               NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_HEADER);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_extended_update(extended, image);
    extended[65] = 0x02u;
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE,
               NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_HEADER);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_extended_update(extended, image);
    extended[166] = 1u;
    stage_complete_extended(
        &write, extended, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE);
    assert(ns2_amiibo_v3_extended_commit(
               &write, image, NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE,
               NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA);
    assert(memcmp(image, before, sizeof(image)) == 0);

    // Conflicting retries fail before commit and do not reduce coverage.
    make_v3(image);
    make_write(staging, image);
    stage_complete_write(&write, staging);
    uint8_t conflict = (uint8_t)(staging[100] ^ 1u);
    assert(ns2_virtual_nfc_write_chunk(&write, 100u, &conflict, 1u) ==
           NS2_VIRTUAL_NFC_ERROR_CONFLICT);

    // Incomplete, wrong-UID, protected-page, out-of-range, and trailing-data
    // transactions all leave the image byte-identical.
    make_v3(image);
    memcpy(before, image, sizeof(before));
    ns2_virtual_nfc_write_begin(&write);
    assert(ns2_virtual_nfc_write_chunk(
               &write, 0u, staging, 76u) == NS2_VIRTUAL_NFC_OK);
    assert(ns2_amiibo_v3_write_commit(&write, image, NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_INCOMPLETE);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_write(staging, image);
    staging[2] ^= 1u;
    stage_complete_write(&write, staging);
    assert(ns2_amiibo_v3_write_commit(&write, image, NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_UID);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_write(staging, image);
    staging[22] = 0x04u;
    stage_complete_write(&write, staging);
    assert(ns2_amiibo_v3_write_commit(&write, image, NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_RECORD);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_write(staging, image);
    staging[298] = 0x92u;
    stage_complete_write(&write, staging);
    assert(ns2_amiibo_v3_write_commit(&write, image, NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_RECORD);
    assert(memcmp(image, before, sizeof(image)) == 0);

    make_write(staging, image);
    staging[453] = 1u;
    stage_complete_write(&write, staging);
    assert(ns2_amiibo_v3_write_commit(&write, image, NULL, NULL) ==
           NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA);
    assert(memcmp(image, before, sizeof(image)) == 0);

    puts("ns2_amiibo_v3_write: all tests passed");
    return 0;
}
