#include "virtual_amiibo.h"

#include <string.h>

static bool coverage_get(const virtual_amiibo_t *tag, size_t index)
{
    return (tag->upload_coverage[index / 8u] &
            (uint8_t)(1u << (index % 8u))) != 0;
}

static void coverage_set(virtual_amiibo_t *tag, size_t index)
{
    tag->upload_coverage[index / 8u] |=
        (uint8_t)(1u << (index % 8u));
}

void virtual_amiibo_init(virtual_amiibo_t *tag)
{
    if (tag) memset(tag, 0, sizeof(*tag));
}

bool virtual_amiibo_supported_size(size_t size)
{
    return size == VIRTUAL_AMIIBO_RAW_SIZE ||
           size == VIRTUAL_AMIIBO_EXTENDED_SIZE;
}

uint32_t virtual_amiibo_crc32(const uint8_t *data, size_t size)
{
    if (!data && size != 0) return 0;

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

virtual_amiibo_result_t virtual_amiibo_validate_raw(const uint8_t *raw,
                                                     size_t size)
{
    if (!raw) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (size != VIRTUAL_AMIIBO_RAW_SIZE)
        return VIRTUAL_AMIIBO_ERROR_SIZE;

    const uint8_t bcc0 =
        (uint8_t)(0x88u ^ raw[0] ^ raw[1] ^ raw[2]);
    const uint8_t bcc1 =
        (uint8_t)(raw[4] ^ raw[5] ^ raw[6] ^ raw[7]);
    if (raw[3] != bcc0 || raw[8] != bcc1)
        return VIRTUAL_AMIIBO_ERROR_BCC;
    return VIRTUAL_AMIIBO_OK;
}

const char *virtual_amiibo_result_string(virtual_amiibo_result_t result)
{
    switch (result) {
        case VIRTUAL_AMIIBO_OK: return "ok";
        case VIRTUAL_AMIIBO_ERROR_ARGUMENT: return "bad argument";
        case VIRTUAL_AMIIBO_ERROR_SIZE: return "expected 540 or 572 bytes";
        case VIRTUAL_AMIIBO_ERROR_BCC: return "invalid NTAG215 UID/BCC";
        case VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE: return "no matching upload";
        case VIRTUAL_AMIIBO_ERROR_RANGE: return "chunk outside upload";
        case VIRTUAL_AMIIBO_ERROR_CONFLICT: return "conflicting repeated chunk";
        case VIRTUAL_AMIIBO_ERROR_INCOMPLETE: return "upload incomplete";
        case VIRTUAL_AMIIBO_ERROR_CRC: return "CRC32 mismatch";
        case VIRTUAL_AMIIBO_ERROR_DIRTY:
            return "save modified amiibo before replacing it";
        case VIRTUAL_AMIIBO_ERROR_NOT_LOADED: return "no amiibo loaded";
        default: return "unknown error";
    }
}

virtual_amiibo_result_t virtual_amiibo_load(virtual_amiibo_t *tag,
                                             const uint8_t *data,
                                             size_t size,
                                             bool persisted)
{
    if (!tag || !data) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!virtual_amiibo_supported_size(size))
        return VIRTUAL_AMIIBO_ERROR_SIZE;

    virtual_amiibo_result_t valid =
        virtual_amiibo_validate_raw(data, VIRTUAL_AMIIBO_RAW_SIZE);
    if (valid != VIRTUAL_AMIIBO_OK) return valid;

    memcpy(tag->raw, data, VIRTUAL_AMIIBO_RAW_SIZE);
    memcpy(tag->clean_raw, data, VIRTUAL_AMIIBO_RAW_SIZE);
    memset(tag->signature, 0, sizeof(tag->signature));
    tag->has_originality_signature =
        size == VIRTUAL_AMIIBO_EXTENDED_SIZE;
    if (tag->has_originality_signature) {
        memcpy(tag->signature, data + VIRTUAL_AMIIBO_RAW_SIZE,
               VIRTUAL_AMIIBO_SIGNATURE_SIZE);
    }
    tag->source_size = (uint16_t)size;
    tag->loaded = true;
    tag->dirty = false;
    tag->persisted = persisted;
    tag->upload_active = false;
    tag->upload_size = 0;
    tag->upload_received = 0;
    tag->has_used_copy = false;
    tag->using_used_copy = false;
    memset(tag->upload_coverage, 0, sizeof(tag->upload_coverage));
    tag->generation++;
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_upload_begin(virtual_amiibo_t *tag,
                                                     size_t size,
                                                     uint32_t expected_crc)
{
    if (!tag) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!virtual_amiibo_supported_size(size))
        return VIRTUAL_AMIIBO_ERROR_SIZE;
    if (tag->loaded && tag->dirty)
        return VIRTUAL_AMIIBO_ERROR_DIRTY;

    memset(tag->upload, 0, sizeof(tag->upload));
    memset(tag->upload_coverage, 0, sizeof(tag->upload_coverage));
    tag->upload_expected_crc = expected_crc;
    tag->upload_size = (uint16_t)size;
    tag->upload_received = 0;
    tag->upload_active = true;
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_upload_chunk(virtual_amiibo_t *tag,
                                                     size_t offset,
                                                     const uint8_t *data,
                                                     size_t size)
{
    if (!tag || (!data && size != 0))
        return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->upload_active)
        return VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE;
    if (offset > tag->upload_size || size > tag->upload_size - offset)
        return VIRTUAL_AMIIBO_ERROR_RANGE;

    // Retries are idempotent, but a retry that changes an already received
    // byte aborts before modifying any part of the transaction.
    for (size_t i = 0; i < size; ++i) {
        const size_t index = offset + i;
        if (coverage_get(tag, index) && tag->upload[index] != data[i])
            return VIRTUAL_AMIIBO_ERROR_CONFLICT;
    }
    for (size_t i = 0; i < size; ++i) {
        const size_t index = offset + i;
        if (!coverage_get(tag, index)) {
            coverage_set(tag, index);
            tag->upload_received++;
        }
        tag->upload[index] = data[i];
    }
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_upload_commit(virtual_amiibo_t *tag)
{
    if (!tag) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->upload_active)
        return VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE;
    if (tag->upload_received != tag->upload_size)
        return VIRTUAL_AMIIBO_ERROR_INCOMPLETE;
    if (virtual_amiibo_crc32(tag->upload, tag->upload_size) !=
        tag->upload_expected_crc)
        return VIRTUAL_AMIIBO_ERROR_CRC;

    const size_t size = tag->upload_size;
    virtual_amiibo_result_t result =
        virtual_amiibo_load(tag, tag->upload, size, false);
    if (result != VIRTUAL_AMIIBO_OK) return result;
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_upload_commit_used(
    virtual_amiibo_t *tag)
{
    if (!tag) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->loaded) return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    if (!tag->upload_active)
        return VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE;
    if (tag->upload_received != tag->upload_size)
        return VIRTUAL_AMIIBO_ERROR_INCOMPLETE;
    if (virtual_amiibo_crc32(tag->upload, tag->upload_size) !=
        tag->upload_expected_crc)
        return VIRTUAL_AMIIBO_ERROR_CRC;
    if (tag->upload_size != tag->source_size)
        return VIRTUAL_AMIIBO_ERROR_SIZE;

    virtual_amiibo_result_t valid =
        virtual_amiibo_validate_raw(tag->upload, VIRTUAL_AMIIBO_RAW_SIZE);
    if (valid != VIRTUAL_AMIIBO_OK) return valid;
    if (memcmp(tag->clean_raw, tag->upload, 16) != 0)
        return VIRTUAL_AMIIBO_ERROR_CONFLICT;
    if (tag->has_originality_signature &&
        memcmp(tag->signature, tag->upload + VIRTUAL_AMIIBO_RAW_SIZE,
               VIRTUAL_AMIIBO_SIGNATURE_SIZE) != 0)
        return VIRTUAL_AMIIBO_ERROR_CONFLICT;

    memcpy(tag->raw, tag->upload, VIRTUAL_AMIIBO_RAW_SIZE);
    tag->has_used_copy = true;
    tag->using_used_copy = true;
    tag->dirty = false;
    tag->persisted = false;
    tag->upload_active = false;
    tag->upload_size = 0;
    tag->upload_received = 0;
    memset(tag->upload_coverage, 0, sizeof(tag->upload_coverage));
    tag->generation++;
    return VIRTUAL_AMIIBO_OK;
}

void virtual_amiibo_upload_cancel(virtual_amiibo_t *tag)
{
    if (!tag) return;
    tag->upload_active = false;
    tag->upload_size = 0;
    tag->upload_received = 0;
    memset(tag->upload_coverage, 0, sizeof(tag->upload_coverage));
}

size_t virtual_amiibo_export_size(const virtual_amiibo_t *tag)
{
    return tag && tag->loaded ? tag->source_size : 0;
}

virtual_amiibo_result_t virtual_amiibo_read(const virtual_amiibo_t *tag,
                                             size_t offset,
                                             uint8_t *out,
                                             size_t size)
{
    return virtual_amiibo_read_copy(
        tag, tag && tag->has_used_copy, offset, out, size);
}

virtual_amiibo_result_t virtual_amiibo_read_copy(
    const virtual_amiibo_t *tag, bool used, size_t offset,
    uint8_t *out, size_t size)
{
    if (!tag || (!out && size != 0))
        return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->loaded) return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    if (used && !tag->has_used_copy)
        return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;

    const size_t total = virtual_amiibo_export_size(tag);
    if (offset > total || size > total - offset)
        return VIRTUAL_AMIIBO_ERROR_RANGE;
    if (size == 0) return VIRTUAL_AMIIBO_OK;

    if (offset < VIRTUAL_AMIIBO_RAW_SIZE) {
        size_t raw_size = VIRTUAL_AMIIBO_RAW_SIZE - offset;
        if (raw_size > size) raw_size = size;
        const uint8_t *raw = used ? tag->raw : tag->clean_raw;
        memcpy(out, raw + offset, raw_size);
        out += raw_size;
        offset += raw_size;
        size -= raw_size;
    }
    if (size != 0) {
        const size_t signature_offset = offset - VIRTUAL_AMIIBO_RAW_SIZE;
        memcpy(out, tag->signature + signature_offset, size);
    }
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_copy_active_raw(
    const virtual_amiibo_t *tag, uint8_t out[VIRTUAL_AMIIBO_RAW_SIZE])
{
    if (!tag || !out) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->loaded) return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    memcpy(out, tag->using_used_copy && tag->has_used_copy
                    ? tag->raw : tag->clean_raw,
           VIRTUAL_AMIIBO_RAW_SIZE);
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_select_used(
    virtual_amiibo_t *tag, bool used)
{
    if (!tag) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->loaded) return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    if (used && !tag->has_used_copy)
        return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    if (tag->using_used_copy == used)
        return VIRTUAL_AMIIBO_OK;
    tag->using_used_copy = used;
    tag->persisted = false;
    tag->generation++;
    return VIRTUAL_AMIIBO_OK;
}

void virtual_amiibo_uid(const virtual_amiibo_t *tag, uint8_t uid[7])
{
    if (!uid) return;
    memset(uid, 0, 7);
    if (!tag || !tag->loaded) return;
    uid[0] = tag->clean_raw[0];
    uid[1] = tag->clean_raw[1];
    uid[2] = tag->clean_raw[2];
    uid[3] = tag->clean_raw[4];
    uid[4] = tag->clean_raw[5];
    uid[5] = tag->clean_raw[6];
    uid[6] = tag->clean_raw[7];
}

void virtual_amiibo_mark_dirty(virtual_amiibo_t *tag)
{
    if (!tag || !tag->loaded) return;
    tag->dirty = true;
    tag->persisted = false;
    tag->generation++;
}

void virtual_amiibo_acknowledge_download(virtual_amiibo_t *tag)
{
    if (tag && tag->loaded && tag->dirty) {
        tag->dirty = false;
        tag->persisted = false;
        tag->generation++;
    }
}

virtual_amiibo_result_t virtual_amiibo_apply_console_write(
    virtual_amiibo_t *tag,
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint32_t expected_generation)
{
    if (!tag || !raw) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (!tag->loaded) return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    if (tag->generation != expected_generation)
        return VIRTUAL_AMIIBO_ERROR_CONFLICT;
    virtual_amiibo_result_t valid =
        virtual_amiibo_validate_raw(raw, VIRTUAL_AMIIBO_RAW_SIZE);
    if (valid != VIRTUAL_AMIIBO_OK) return valid;

    // The console write codec owns only mutable NTAG contents. Reject an
    // accidental UID/manufacturer-page replacement even if its BCC happens to
    // be internally consistent.
    const uint8_t *active =
        tag->using_used_copy && tag->has_used_copy
            ? tag->raw : tag->clean_raw;
    if (memcmp(active, raw, 16) != 0)
        return VIRTUAL_AMIIBO_ERROR_CONFLICT;

    memcpy(tag->raw, raw, VIRTUAL_AMIIBO_RAW_SIZE);
    tag->has_used_copy = true;
    tag->using_used_copy = true;
    tag->dirty = true;
    tag->persisted = false;
    tag->generation++;
    return VIRTUAL_AMIIBO_OK;
}
