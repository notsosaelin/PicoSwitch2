#include "ns2_amiibo_v3_write.h"

#include <string.h>

static bool command_identity_matches(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    return data && image && size >= 11u &&
           memcmp(data + 2u, image, 7u) == 0 &&
           data[9] == 0x01u;
}

static bool sector1_capability_valid(const uint8_t value[4])
{
    return value && value[0] == 0xA5u && value[1] == 0x00u &&
           value[2] != 0x00u && value[3] == 0x00u;
}

static bool image_range_nonzero(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    size_t offset, size_t size)
{
    if (!image || offset > NS2_AMIIBO_V3_SIZE ||
        size > NS2_AMIIBO_V3_SIZE - offset)
        return false;
    for (size_t i = 0; i < size; ++i) {
        if (image[offset + i] != 0u)
            return true;
    }
    return false;
}

static bool extended_update_layout(
    const uint8_t *data, size_t size,
    uint8_t *sector0_page_out, uint8_t *sector1_capability_page_out)
{
    // The update is self-describing. Kirby uses sector-0 page 92 and
    // sector-1 capability/data pages 00/01. King Dedede uses B2 and 64/65.
    // Validate the shared shape and safe address bounds instead of encoding
    // any rider identity or one captured allocation.
    if (!data || size < 68u ||
        data[22] != 0x03u ||
        data[23] != 0x00u || data[24] != 0x04u ||
        data[25] != 0x04u ||
        data[30] != 0x00u || data[32] != 0x20u ||
        data[65] != 0x01u || data[67] != 0x60u)
        return false;

    const uint8_t sector0_page = data[31];
    const uint8_t sector1_capability_page = data[13];
    const uint16_t sector1_data_page =
        (uint16_t)sector1_capability_page + 1u;

    // The 355-byte operation clears sector-0 pages 92-E1. The 32-byte
    // application record must remain wholly inside that proven writable
    // window. The sector-1 capability page plus 96-byte data record must fit
    // wholly inside the 256-page second sector.
    if (sector0_page < 0x92u ||
        (uint16_t)sector0_page + 7u > 0xE1u ||
        sector1_data_page > 0xFFu ||
        sector1_data_page + 23u > 0xFFu ||
        data[66] != (uint8_t)sector1_data_page)
        return false;

    if (sector0_page_out) *sector0_page_out = sector0_page;
    if (sector1_capability_page_out)
        *sector1_capability_page_out = sector1_capability_page;
    return true;
}

static uint8_t current_sector1_capability_generation(
    const uint8_t image[NS2_AMIIBO_V3_SIZE],
    uint8_t sector0_page, uint8_t sector1_capability_page)
{
    const size_t capability_offset =
        0x400u + (size_t)sector1_capability_page * 4u;
    const uint8_t *stored =
        image + capability_offset;
    if (sector1_capability_valid(stored))
        return stored[2];

    // Existing 2048-byte ecosystem dumps leave the chip-managed page zero.
    // A dump with no data in this operation's own allocation is first-use. A
    // dump with records but no retained capability page predates our support
    // and uses the hardware-confirmed generation-1 fallback.
    const size_t sector0_offset = (size_t)sector0_page * 4u;
    const size_t sector1_data_offset = capability_offset + 4u;
    return image_range_nonzero(image, sector0_offset, 0x20u) ||
           image_range_nonzero(image, sector1_data_offset, 0x60u)
        ? 1u : 0u;
}

bool ns2_amiibo_v3_is_device_command(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    return size == NS2_AMIIBO_V3_DEVICE_COMMAND_SIZE &&
           command_identity_matches(data, size, image) &&
           data[10] == 0x01u;
}

bool ns2_amiibo_v3_is_write_start(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    // Byte 21 is the record count, so require the complete fixed header before
    // accepting a chunk as the start of a mutable-image transaction.
    return size >= 22u && command_identity_matches(data, size, image) &&
           data[10] == 0x06u &&
           data[21] >= 1u && data[21] <= 16u;
}

bool ns2_amiibo_v3_is_extended_start(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    return ns2_amiibo_v3_extended_expected_size(data, size, image) != 0u;
}

size_t ns2_amiibo_v3_extended_expected_size(
    const uint8_t *data, size_t size,
    const uint8_t image[NS2_AMIIBO_V3_SIZE])
{
    if (size < 26u || !command_identity_matches(data, size, image) ||
        data[10] != 0x06u)
        return 0u;

    // Genuine clear/initialise envelope:
    //   common identity | 11 zero header bytes | count 02 |
    //   sector 00, page 92, length F0 | ...
    static const uint8_t clear_header[11] = {0};
    if (memcmp(data + 11u, clear_header, sizeof(clear_header)) == 0 &&
        data[22] == 0x02u &&
        data[23] == 0x00u && data[24] == 0x92u && data[25] == 0xF0u)
        return NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE;

    // Genuine update envelope. Byte 13 selects the chip-managed capability
    // page in sector 1. The explicit 96-byte record begins at the following
    // page. Kirby uses 00/01; King Dedede uses 64/65. The sector-0 record page
    // also varies (92 vs B2), so both allocations come from the validated
    // record envelope rather than the rider identity.
    static const uint8_t update_header_prefix[2] = {
        0x01, 0x01,
    };
    static const uint8_t update_header_suffix[4] = {
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    uint8_t sector0_page = 0u;
    uint8_t sector1_capability_page = 0u;
    if (!extended_update_layout(
            data, size, &sector0_page, &sector1_capability_page))
        return 0u;

    const uint8_t *next_capability = data + 18u;
    const uint8_t expected_generation =
        (uint8_t)(current_sector1_capability_generation(
                      image, sector0_page, sector1_capability_page) + 1u);
    if (memcmp(data + 11u, update_header_prefix,
               sizeof(update_header_prefix)) == 0 &&
        memcmp(data + 14u, update_header_suffix,
               sizeof(update_header_suffix)) == 0 &&
        sector1_capability_valid(next_capability) &&
        next_capability[2] == expected_generation)
        return NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE;

    return 0u;
}

ns2_virtual_nfc_result_t ns2_amiibo_v3_write_commit(
    ns2_virtual_nfc_write_t *write,
    uint8_t image[NS2_AMIIBO_V3_SIZE],
    uint8_t *record_count_out, uint16_t *data_bytes_out)
{
    if (!write || !image) return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    if (!write->active) return NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE;
    if (write->received != NS2_NFC_WRITE_STAGING_SIZE)
        return NS2_VIRTUAL_NFC_ERROR_INCOMPLETE;
    if (!ns2_amiibo_v3_valid(image, NS2_AMIIBO_V3_SIZE))
        return NS2_VIRTUAL_NFC_ERROR_RAW_TAG;
    if (!ns2_amiibo_v3_is_write_start(
            write->bytes, sizeof(write->bytes), image))
        return memcmp(write->bytes + 2u, image, 7u) != 0
            ? NS2_VIRTUAL_NFC_ERROR_UID
            : NS2_VIRTUAL_NFC_ERROR_HEADER;

    const uint8_t record_count = write->bytes[21];
    if (record_count == 0u || record_count > 16u)
        return NS2_VIRTUAL_NFC_ERROR_RECORD_COUNT;

    // Validate first. Applying in a second pass preserves the same all-or-
    // nothing guarantee as the NTAG215 codec without allocating a second
    // 2048-byte image on the Pico's core-0 stack.
    size_t cursor = 22u;
    uint16_t data_bytes = 0u;
    for (uint8_t record = 0; record < record_count; ++record) {
        if (cursor + 2u > NS2_NFC_WRITE_STAGING_SIZE)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        const uint8_t page = write->bytes[cursor++];
        const uint8_t length = write->bytes[cursor++];
        if (page == 0u || length == 0u ||
            cursor + length > NS2_NFC_WRITE_STAGING_SIZE)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;

        const size_t address = (size_t)page * 4u;
        // Preserve UID/manufacturer pages and everything past the captured v3
        // encrypted-body write range. Machine SRAM is updated by the separate
        // 0x14/0x21 device-command exchange, not this commit.
        if (address < 20u ||
            address + length > NS2_AMIIBO_V3_WRITE_END)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        cursor += length;
        data_bytes = (uint16_t)(data_bytes + length);
    }
    for (size_t i = cursor; i < NS2_NFC_WRITE_STAGING_SIZE; ++i) {
        if (write->bytes[i] != 0u)
            return NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA;
    }

    memcpy(image + 16u, write->bytes + 17u, 4u);
    cursor = 22u;
    for (uint8_t record = 0; record < record_count; ++record) {
        const uint8_t page = write->bytes[cursor++];
        const uint8_t length = write->bytes[cursor++];
        memcpy(image + (size_t)page * 4u, write->bytes + cursor, length);
        cursor += length;
    }

    write->active = false;
    if (record_count_out) *record_count_out = record_count;
    if (data_bytes_out) *data_bytes_out = data_bytes;
    return NS2_VIRTUAL_NFC_OK;
}

typedef struct {
    uint8_t sector;
    uint8_t page;
    uint8_t length;
} ns2_v3_extended_record_t;

static const ns2_v3_extended_record_t clear_records[] = {
    {0x00u, 0x92u, 0xF0u},
    {0x00u, 0xCEu, 0x50u},
};

static const ns2_v3_extended_record_t update_control_record = {
    0x00u, 0x04u, 0x04u,
};

ns2_virtual_nfc_result_t ns2_amiibo_v3_extended_commit(
    ns2_virtual_nfc_write_t *write,
    uint8_t image[NS2_AMIIBO_V3_SIZE], size_t expected_size,
    uint8_t *record_count_out, uint16_t *data_bytes_out)
{
    if (!write || !image) return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    if (!write->active) return NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE;
    if (expected_size != NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE &&
        expected_size != NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE)
        return NS2_VIRTUAL_NFC_ERROR_HEADER;
    if (write->received != expected_size)
        return NS2_VIRTUAL_NFC_ERROR_INCOMPLETE;
    for (size_t i = 0; i < expected_size; ++i) {
        if ((write->coverage[i >> 3u] &
             (uint8_t)(1u << (i & 7u))) == 0u)
            return NS2_VIRTUAL_NFC_ERROR_INCOMPLETE;
    }
    if (!ns2_amiibo_v3_valid(image, NS2_AMIIBO_V3_SIZE))
        return NS2_VIRTUAL_NFC_ERROR_RAW_TAG;
    const size_t classified_size = ns2_amiibo_v3_extended_expected_size(
        write->bytes, expected_size, image);
    if (classified_size == 0u)
        return memcmp(write->bytes + 2u, image, 7u) != 0
            ? NS2_VIRTUAL_NFC_ERROR_UID
            : NS2_VIRTUAL_NFC_ERROR_HEADER;
    if (classified_size != expected_size)
        return NS2_VIRTUAL_NFC_ERROR_HEADER;

    ns2_v3_extended_record_t dynamic_update_records[3];
    const ns2_v3_extended_record_t *records = clear_records;
    uint8_t record_count = (uint8_t)(sizeof(clear_records) /
                                     sizeof(clear_records[0]));
    if (expected_size == NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE) {
        uint8_t sector0_page = 0u;
        uint8_t sector1_capability_page = 0u;
        if (!extended_update_layout(
                write->bytes, expected_size, &sector0_page,
                &sector1_capability_page))
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        dynamic_update_records[0] = update_control_record;
        dynamic_update_records[1] =
            (ns2_v3_extended_record_t){0x00u, sector0_page, 0x20u};
        dynamic_update_records[2] =
            (ns2_v3_extended_record_t){
                0x01u, (uint8_t)(sector1_capability_page + 1u), 0x60u};
        records = dynamic_update_records;
        record_count = 3u;
    }

    // Validate the exact capture-derived sector/page layout and all trailing
    // padding before modifying the image.
    size_t cursor = 23u;
    uint16_t data_bytes = 0u;
    for (uint8_t record = 0; record < record_count; ++record) {
        const ns2_v3_extended_record_t *expected = &records[record];
        if (cursor + 3u > expected_size ||
            write->bytes[cursor] != expected->sector ||
            write->bytes[cursor + 1u] != expected->page ||
            write->bytes[cursor + 2u] != expected->length)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        cursor += 3u;
        if (cursor + expected->length > expected_size)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;

        const size_t address =
            (size_t)expected->sector * 0x400u +
            (size_t)expected->page * 4u;
        if (address + expected->length > NS2_AMIIBO_V3_SIZE)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        cursor += expected->length;
        data_bytes = (uint16_t)(data_bytes + expected->length);
    }
    for (size_t i = cursor; i < expected_size; ++i) {
        if (write->bytes[i] != 0u)
            return NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA;
    }

    cursor = 23u;
    if (expected_size == NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE) {
        // The capability page is not present in the three-record envelope,
        // but genuine hardware advances the page selected by byte 13 to the
        // value in bytes 18-21. Retain it in the complete image so power
        // cycles and portal Sync preserve a coherent tag for any allocation.
        const size_t capability_offset =
            0x400u + (size_t)write->bytes[13] * 4u;
        memcpy(image + capability_offset,
               write->bytes + 18u,
               NS2_AMIIBO_V3_SECTOR1_CAPABILITY_SIZE);
    }
    for (uint8_t record = 0; record < record_count; ++record) {
        const ns2_v3_extended_record_t *expected = &records[record];
        cursor += 3u;
        const size_t address =
            (size_t)expected->sector * 0x400u +
            (size_t)expected->page * 4u;
        if (expected->sector == 0u && expected->page == 0x04u) {
            // NTAG capability-container bytes A5 00 are read-only. The genuine
            // tag retained them while accepting the final two bytes.
            memcpy(image + address + 2u, write->bytes + cursor + 2u, 2u);
        } else {
            memcpy(image + address, write->bytes + cursor, expected->length);
        }
        cursor += expected->length;
    }

    write->active = false;
    if (record_count_out) *record_count_out = record_count;
    if (data_bytes_out) *data_bytes_out = data_bytes;
    return NS2_VIRTUAL_NFC_OK;
}

void ns2_amiibo_v3_extended_sequence_reset(
    ns2_amiibo_v3_extended_sequence_t *sequence)
{
    if (!sequence) return;
    sequence->phase = NS2_AMIIBO_V3_EXTENDED_SEQUENCE_IDLE;
    sequence->deadline_ms = 0u;
}

void ns2_amiibo_v3_extended_sequence_note_commit(
    ns2_amiibo_v3_extended_sequence_t *sequence,
    size_t committed_size, uint32_t now_ms)
{
    if (!sequence) return;
    if (committed_size == NS2_AMIIBO_V3_EXTENDED_CLEAR_SIZE) {
        sequence->phase =
            NS2_AMIIBO_V3_EXTENDED_SEQUENCE_AWAIT_UPDATE;
        sequence->deadline_ms =
            now_ms + NS2_AMIIBO_V3_EXTENDED_SEQUENCE_TIMEOUT_MS;
    } else if (committed_size == NS2_AMIIBO_V3_EXTENDED_UPDATE_SIZE) {
        sequence->phase =
            NS2_AMIIBO_V3_EXTENDED_SEQUENCE_UPDATE_COMMITTED;
        sequence->deadline_ms = 0u;
    } else {
        ns2_amiibo_v3_extended_sequence_reset(sequence);
    }
}

void ns2_amiibo_v3_extended_sequence_expire(
    ns2_amiibo_v3_extended_sequence_t *sequence, uint32_t now_ms)
{
    if (!sequence ||
        sequence->phase !=
            NS2_AMIIBO_V3_EXTENDED_SEQUENCE_AWAIT_UPDATE)
        return;
    if ((int32_t)(now_ms - sequence->deadline_ms) >= 0)
        ns2_amiibo_v3_extended_sequence_reset(sequence);
}

bool ns2_amiibo_v3_extended_sequence_continue_after_write(
    ns2_amiibo_v3_extended_sequence_t *sequence, uint32_t now_ms)
{
    ns2_amiibo_v3_extended_sequence_expire(sequence, now_ms);
    return sequence &&
           sequence->phase ==
               NS2_AMIIBO_V3_EXTENDED_SEQUENCE_AWAIT_UPDATE;
}
