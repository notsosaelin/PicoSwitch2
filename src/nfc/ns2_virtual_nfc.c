#include "ns2_virtual_nfc.h"

#include <string.h>

static void write_u16le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static bool coverage_get(const ns2_virtual_nfc_write_t *write, size_t index)
{
    return (write->coverage[index / 8u] &
            (uint8_t)(1u << (index % 8u))) != 0;
}

static void coverage_set(ns2_virtual_nfc_write_t *write, size_t index)
{
    write->coverage[index / 8u] |=
        (uint8_t)(1u << (index % 8u));
}

static void uid_from_raw(const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
                         uint8_t uid[7])
{
    uid[0] = raw[0];
    uid[1] = raw[1];
    uid[2] = raw[2];
    uid[3] = raw[4];
    uid[4] = raw[5];
    uid[5] = raw[6];
    uid[6] = raw[7];
}

static ns2_virtual_nfc_result_t build_operation_prefix(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    const uint8_t operation_metadata[NS2_NFC_OPERATION_METADATA_SIZE],
    uint8_t out[60])
{
    if (!raw || !signature || !operation_metadata || !out)
        return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    if (virtual_amiibo_validate_raw(raw, VIRTUAL_AMIIBO_RAW_SIZE) !=
        VIRTUAL_AMIIBO_OK)
        return NS2_VIRTUAL_NFC_ERROR_RAW_TAG;

    memset(out, 0, 60);
    out[0] = 0x04;
    out[4] = 0x01;
    out[5] = 0x02;
    out[6] = 0x00;

    uint8_t uid[7];
    uid_from_raw(raw, uid);
    out[7] = 0x07;
    memcpy(out + 8, uid, sizeof(uid));
    memcpy(out + 19, signature, VIRTUAL_AMIIBO_SIGNATURE_SIZE);
    memcpy(out + 51, operation_metadata,
           NS2_NFC_OPERATION_METADATA_SIZE);
    return NS2_VIRTUAL_NFC_OK;
}

const char *ns2_virtual_nfc_result_string(ns2_virtual_nfc_result_t result)
{
    switch (result) {
        case NS2_VIRTUAL_NFC_OK: return "ok";
        case NS2_VIRTUAL_NFC_ERROR_ARGUMENT: return "bad argument";
        case NS2_VIRTUAL_NFC_ERROR_RAW_TAG: return "invalid NTAG215 image";
        case NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE: return "write not active";
        case NS2_VIRTUAL_NFC_ERROR_RANGE: return "write chunk out of range";
        case NS2_VIRTUAL_NFC_ERROR_CONFLICT:
            return "conflicting repeated write chunk";
        case NS2_VIRTUAL_NFC_ERROR_INCOMPLETE:
            return "write staging incomplete";
        case NS2_VIRTUAL_NFC_ERROR_HEADER:
            return "invalid write staging header";
        case NS2_VIRTUAL_NFC_ERROR_UID:
            return "write UID does not match active tag";
        case NS2_VIRTUAL_NFC_ERROR_RECORD_COUNT:
            return "invalid write record count";
        case NS2_VIRTUAL_NFC_ERROR_RECORD:
            return "invalid write record";
        case NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA:
            return "nonzero data follows write records";
        default: return "unknown error";
    }
}

void ns2_virtual_nfc_build_status(bool tag_present, const uint8_t uid[7],
                                  uint8_t out[NS2_NFC_STATUS_PAYLOAD_SIZE])
{
    if (!out) return;
    memset(out, 0, NS2_NFC_STATUS_PAYLOAD_SIZE);
    if (!tag_present || !uid) {
        out[0] = 0x07;
        out[1] = 0x41;
        return;
    }

    out[0] = 0x09;
    out[1] = 0x00;
    out[4] = 0x01;
    out[5] = 0x01;
    out[6] = 0x02;
    out[8] = 0x07;
    memcpy(out + 9, uid, 7);
}

ns2_virtual_nfc_result_t ns2_virtual_nfc_build_read_buffer(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    const uint8_t operation_metadata[NS2_NFC_OPERATION_METADATA_SIZE],
    uint8_t out[NS2_NFC_READ_BUFFER_SIZE])
{
    memset(out, 0, NS2_NFC_READ_BUFFER_SIZE);
    const ns2_virtual_nfc_result_t result =
        build_operation_prefix(raw, signature, operation_metadata, out);
    if (result != NS2_VIRTUAL_NFC_OK) return result;
    memcpy(out + 60, raw, VIRTUAL_AMIIBO_RAW_SIZE);
    return NS2_VIRTUAL_NFC_OK;
}

ns2_virtual_nfc_result_t ns2_virtual_nfc_build_write_prep_buffer(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    const uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    const uint8_t operation_metadata[NS2_NFC_OPERATION_METADATA_SIZE],
    uint8_t out[NS2_NFC_WRITE_PREP_BUFFER_SIZE])
{
    if (!out) return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    memset(out, 0, NS2_NFC_WRITE_PREP_BUFFER_SIZE);
    const ns2_virtual_nfc_result_t result =
        build_operation_prefix(raw, signature, operation_metadata, out);
    if (result != NS2_VIRTUAL_NFC_OK) return result;
    memcpy(out + 60, raw + 12, 4);
    return NS2_VIRTUAL_NFC_OK;
}

ns2_virtual_nfc_result_t ns2_virtual_nfc_build_buffer_chunk(
    const uint8_t *buffer, size_t buffer_size, uint16_t offset,
    uint8_t out[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE], size_t *out_size)
{
    if (!buffer || !out || !out_size)
        return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    if (buffer_size == 0 || offset >= buffer_size)
        return NS2_VIRTUAL_NFC_ERROR_RANGE;

    size_t length = buffer_size - offset;
    if (length > NS2_NFC_READ_CHUNK_DATA_SIZE)
        length = NS2_NFC_READ_CHUNK_DATA_SIZE;

    out[0] = (offset + length == buffer_size) ? 1u : 0u;
    write_u16le(out + 1, (uint16_t)length);
    memcpy(out + NS2_NFC_READ_CHUNK_HEADER_SIZE, buffer + offset, length);
    *out_size = NS2_NFC_READ_CHUNK_HEADER_SIZE + length;
    return NS2_VIRTUAL_NFC_OK;
}

ns2_virtual_nfc_result_t ns2_virtual_nfc_build_read_chunk(
    const uint8_t buffer[NS2_NFC_READ_BUFFER_SIZE], uint16_t offset,
    uint8_t out[NS2_NFC_READ_CHUNK_PAYLOAD_SIZE], size_t *out_size)
{
    return ns2_virtual_nfc_build_buffer_chunk(
        buffer, NS2_NFC_READ_BUFFER_SIZE, offset, out, out_size);
}

void ns2_virtual_nfc_write_init(ns2_virtual_nfc_write_t *write)
{
    if (write) memset(write, 0, sizeof(*write));
}

void ns2_virtual_nfc_write_begin(ns2_virtual_nfc_write_t *write)
{
    if (!write) return;
    memset(write, 0, sizeof(*write));
    write->active = true;
}

void ns2_virtual_nfc_write_cancel(ns2_virtual_nfc_write_t *write)
{
    if (!write) return;
    write->active = false;
    write->received = 0;
    memset(write->coverage, 0, sizeof(write->coverage));
}

ns2_virtual_nfc_result_t ns2_virtual_nfc_write_chunk(
    ns2_virtual_nfc_write_t *write, size_t offset,
    const uint8_t *data, size_t size)
{
    if (!write || (!data && size != 0))
        return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    if (!write->active) return NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE;
    if (offset > NS2_NFC_WRITE_STAGING_SIZE ||
        size > NS2_NFC_WRITE_STAGING_SIZE - offset)
        return NS2_VIRTUAL_NFC_ERROR_RANGE;

    for (size_t i = 0; i < size; ++i) {
        const size_t index = offset + i;
        if (coverage_get(write, index) && write->bytes[index] != data[i])
            return NS2_VIRTUAL_NFC_ERROR_CONFLICT;
    }
    for (size_t i = 0; i < size; ++i) {
        const size_t index = offset + i;
        if (!coverage_get(write, index)) {
            coverage_set(write, index);
            write->received++;
        }
        write->bytes[index] = data[i];
    }
    return NS2_VIRTUAL_NFC_OK;
}

ns2_virtual_nfc_result_t ns2_virtual_nfc_write_commit(
    ns2_virtual_nfc_write_t *write,
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint8_t *record_count_out, uint16_t *data_bytes_out)
{
    if (!write || !raw) return NS2_VIRTUAL_NFC_ERROR_ARGUMENT;
    if (!write->active) return NS2_VIRTUAL_NFC_ERROR_NOT_ACTIVE;
    if (write->received != NS2_NFC_WRITE_STAGING_SIZE)
        return NS2_VIRTUAL_NFC_ERROR_INCOMPLETE;
    if (virtual_amiibo_validate_raw(raw, VIRTUAL_AMIIBO_RAW_SIZE) !=
        VIRTUAL_AMIIBO_OK)
        return NS2_VIRTUAL_NFC_ERROR_RAW_TAG;
    if (write->bytes[0] != 0xD0 || write->bytes[1] != 0x07)
        return NS2_VIRTUAL_NFC_ERROR_HEADER;

    uint8_t uid[7];
    uid_from_raw(raw, uid);
    if (memcmp(write->bytes + 2, uid, sizeof(uid)) != 0)
        return NS2_VIRTUAL_NFC_ERROR_UID;

    const uint8_t record_count = write->bytes[21];
    if (record_count == 0 || record_count > 16)
        return NS2_VIRTUAL_NFC_ERROR_RECORD_COUNT;

    uint8_t candidate[VIRTUAL_AMIIBO_RAW_SIZE];
    memcpy(candidate, raw, sizeof(candidate));
    size_t cursor = 22;
    uint16_t data_bytes = 0;
    for (uint8_t record = 0; record < record_count; ++record) {
        if (cursor + 2 > NS2_NFC_WRITE_STAGING_SIZE)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        const uint8_t page = write->bytes[cursor++];
        const uint8_t length = write->bytes[cursor++];
        if (page == 0 || length == 0 ||
            cursor + length > NS2_NFC_WRITE_STAGING_SIZE)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;

        const size_t address = (size_t)page * 4u;
        // Preserve UID/manufacturer pages and the NTAG215 configuration area.
        // Captured writes target pages 5 through 129 (raw bytes 20..519).
        if (address < 20 || address + length > 520)
            return NS2_VIRTUAL_NFC_ERROR_RECORD;
        memcpy(candidate + address, write->bytes + cursor, length);
        cursor += length;
        data_bytes = (uint16_t)(data_bytes + length);
    }
    for (; cursor < NS2_NFC_WRITE_STAGING_SIZE; ++cursor) {
        if (write->bytes[cursor] != 0)
            return NS2_VIRTUAL_NFC_ERROR_TRAILING_DATA;
    }

    // The staging descriptor carries the final static-lock bytes separately
    // from the temporary transaction state.
    memcpy(candidate + 16, write->bytes + 17, 4);
    memcpy(raw, candidate, sizeof(candidate));
    write->active = false;
    if (record_count_out) *record_count_out = record_count;
    if (data_bytes_out) *data_bytes_out = data_bytes;
    return NS2_VIRTUAL_NFC_OK;
}
